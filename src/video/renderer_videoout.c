// sceVideoOut presentation: YCbCr420_BT709 / NV12 (BGRA only if prefer_ycbcr=0).
#include "video.h"
#include "nv12_blit.h"
#include "../log.h"
#include "../orbis/video_out_c.h"
#include "../ui/ui_draw.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <emmintrin.h> /* SSE2 — Jaguar PS4 */

#define FB_COUNT_MAX 3
/* n=1 + convert~37ms = tearing (writes the front-buffer). Try n=2. */
#define FB_COUNT_YCC_ALLOC 2
#define FB_COUNT_BGRA 3
#define FB_ALIGN 0x200000 // 2 MiB
#define YCC_STRIDE_MUL 4

static int s_video = -1;
static void *s_fb[FB_COUNT_MAX];       /* VA Garlic (VideoOut / flip) */
static void *s_fb_cpu[FB_COUNT_MAX];   /* VA ONION alias (blit CPU); o = s_fb */
static int s_fb_count;     /* buffers used in flip / present */
static int s_fb_alloc;     /* buffers in the DirectMemory mapping */
static int s_fb_index;
static size_t s_fb_size;
static off_t s_dmem_off;
static size_t s_dmem_size;
static void *s_dmem_base;      /* Garlic map */
static void *s_dmem_cpu_base;  /* Onion alias map (nullable) */
static int s_width, s_height, s_pitch;
static int s_buf_h; /* height registered in VideoOut (1080); decoder may be 1088 */
static int s_use_bgra;
static int s_plugin_ok;
static int s_flip_logged;
static int s_flip_mode = ML_VIDEO_OUT_FLIP_VSYNC;
static int s_present_wb; /* 1 if blit writes to cacheable alias */
static int s_ycc_hrep4;  /* 1: expand ×4; 0: NV12 packed */
static int s_gray_left;  /* gray test frames at start */
static int s_last_flip_idx = -1;
static int s_prev_flip_idx = -1;
static int s_flip_wait_logs;
static uint64_t s_flip_stuck_since; /* 0 = not stuck; else now_us() when backlog began */
static uint64_t s_flip_stuck_logged;
static int s_show_stats;

/*
 * BGRA MT: workers claim row bands off an atomic counter, so a slow band does
 * not stall the frame the way a fixed half-split does. Pipeline: kick async →
 * Decode overlaps convert; finish=join+flip. Always Decode in decoder (do not
 * skip H.264 refs).
 */
typedef struct {
    uint8_t *dst;
    int dst_pitch;
    int dst_w;
    int dst_h;
    int src_w;
    int src_h;
    const uint8_t *y;
    const uint8_t *uv;
    int pitch_y;
    int pitch_uv;
    int nbands;
    const int *x_lut; /* dst-col -> src-col; NULL when src/dst sizes match */
} nv12_bgra_job_t;

#define BGRA_WORKERS_MAX 6
#define BGRA_WORKERS_DEFAULT 4
/* Even (4:2:0 pairs rows) and a multiple of 2 so band starts stay 64B-aligned. */
#define BGRA_BAND_ROWS 32
static pthread_t s_bgra_thr[BGRA_WORKERS_MAX];
static pthread_mutex_t s_bgra_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_bgra_cv = PTHREAD_COND_INITIALIZER;
static int s_bgra_workers = BGRA_WORKERS_DEFAULT;
static int s_bgra_worker_alive;
static int s_bgra_shutdown;
static int s_bgra_seq;         /* work generation; workers wake when it moves */
static int s_bgra_outstanding;
static uint64_t s_bgra_job_us; /* slowest participant ≈ wall convert approx */
static nv12_bgra_job_t s_bgra_job;
/* Own cache line: hammered with lock xadd by every worker, while s_bgra_job
 * right above is read on each claim. */
static int s_bgra_next_band __attribute__((aligned(64)));

/* -1 = pick from the framebuffer mapping, 0/1 = forced from moonlight.ini. */
static int s_bgra_nt_pref = -1;
static int s_bgra_nt;         /* resolved: 1 = streaming stores */

static int s_pipe_active;
static int s_pipe_fb_idx = -1;

static void bgra_worker_start(void);
static void bgra_worker_stop(void);
static void bgra_resolve_store_mode(void);
static void bgra_convert_kick(uint8_t *dst, int dst_pitch,
                              int src_w, int src_h, int dst_w, int dst_h,
                              const uint8_t *y, const uint8_t *uv,
                              int pitch_y, int pitch_uv, int main_helps);
static void bgra_convert_wait(void);

static void fill_solid_u32(uint8_t *dst, int pitch, int h, int width, uint32_t pix) {
    size_t ysz = (size_t)pitch * (size_t)h;
    size_t uvsz = ysz / 2u;
    for (int r = 0; r < h; r++) {
        uint32_t *row = (uint32_t *)(void *)(dst + (size_t)r * (size_t)pitch);
        for (int x = 0; x < width; x++)
            row[x] = pix;
    }
    memset(dst + ysz, 0x80, uvsz);
}

static void fill_ayuv_gray(uint8_t *dst, int pitch, int h, int width) {
    /* 0x80808080: gris en RGB y en YUV (U=V=Y=0x80). */
    fill_solid_u32(dst, pitch, h, width, 0x80808080u);
}

static int wait_flip_shown(int want_idx, int timeout_ms) {
    if (s_video < 0 || s_fb_count < 2 || want_idx < 0)
        return 0;
    for (int i = 0; i < timeout_ms; i++) {
        MlVideoOutFlipStatus st;
        memset(&st, 0, sizeof(st));
        if (sceVideoOutGetFlipStatus(s_video, &st) == 0) {
            if (st.currentBuffer == want_idx && st.numFlipPending == 0)
                return 1;
            /* pending=0 and cur still -1/other: sometimes the latch takes 1 vsync */
            if (st.numFlipPending == 0 && i > 2 && st.currentBuffer < 0)
                return 0;
        }
        sceKernelUsleep(1000);
    }
    return 0;
}

static uint64_t now_us(void) {
    return (uint64_t)sceKernelGetProcessTime();
}

int video_present_plugin_loaded(void) {
    FILE *f = fopen(ML_YCBCR_PLUGIN_MARKER, "r");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static void free_dmem(void) {
    /* Unmap VA before releasing physical pages; on this firmware the
     * VA mapping may survive Release and fragment VA space over a
     * long session. */
    if (s_dmem_base && s_dmem_size)
        sceKernelMunmap(s_dmem_base, s_dmem_size);
    if (s_dmem_base) {
        sceKernelReleaseDirectMemory(s_dmem_off, s_dmem_size);
        s_dmem_base = NULL;
        s_dmem_off = 0;
        s_dmem_size = 0;
    }
    s_dmem_cpu_base = NULL;
    s_present_wb = 0;
    for (int i = 0; i < FB_COUNT_MAX; i++) {
        s_fb[i] = NULL;
        s_fb_cpu[i] = NULL;
    }
    s_fb_size = 0;
    s_fb_count = 0;
    s_fb_alloc = 0;
}

static size_t nv12_size(int pitch, int h) {
    return (size_t)pitch * (size_t)h * 3u / 2u;
}

static size_t bgra_size(int w, int h) {
    return (size_t)w * (size_t)h * 4u;
}

/* alloc_count: DirectMemory pages. reg_count: those registered/flipped.
 * force_wc=1: WC_GARLIC only (YCbCr: DCE misreads ONION-mapped buffers → neon).
 * force_wc=0: try WB_GARLIC/ONION (BGRA / debug). */
static int alloc_dmem(size_t per_fb, int alloc_count, int reg_count, int force_wc) {
    free_dmem();
    if (alloc_count < 1 || alloc_count > FB_COUNT_MAX)
        return -1;
    if (reg_count < 1 || reg_count > alloc_count)
        return -1;
    s_fb_alloc = alloc_count;
    s_fb_count = reg_count;
    s_fb_size = (per_fb + FB_ALIGN - 1) & ~(size_t)(FB_ALIGN - 1);
    s_dmem_size = s_fb_size * (size_t)s_fb_alloc;
    s_dmem_size = (s_dmem_size + FB_ALIGN - 1) & ~(size_t)(FB_ALIGN - 1);

    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                           s_dmem_size, FB_ALIGN, ML_DMEM_TYPE_GARLIC,
                                           &s_dmem_off);
    if (rc < 0) {
        LOGE("present: AllocateDirectMemory => 0x%08x", (unsigned)rc);
        s_fb_count = s_fb_alloc = 0;
        return -1;
    }

    s_present_wb = 0;
    s_dmem_base = NULL;
    if (!force_wc) {
        static const struct { int type; const char *name; size_t align; } tries[] = {
            { ML_DMEM_TYPE_WB_GARLIC, "WB_GARLIC", FB_ALIGN },
            { ML_DMEM_TYPE_WB_GARLIC, "WB_GARLIC/16k", ML_DMEM_ALIGN },
            { ML_DMEM_TYPE_ONION,     "ONION",     FB_ALIGN },
            { ML_DMEM_TYPE_ONION,     "ONION/16k", ML_DMEM_ALIGN },
        };
        for (size_t t = 0; t < sizeof(tries) / sizeof(tries[0]); t++) {
            void *va = NULL;
            int m2 = sceKernelMapDirectMemory2(&va, s_dmem_size, tries[t].type,
                                               ML_DMEM_PROT_RW, 0, s_dmem_off,
                                               tries[t].align);
            if (m2 == 0 && va) {
                s_dmem_base = va;
                s_present_wb = 1;
                LOGI("present: dmem Map2 %s OK size=0x%zx (blit cacheable)",
                     tries[t].name, s_dmem_size);
                break;
            }
            LOGW("present: Map2 %s => 0x%08x", tries[t].name, (unsigned)m2);
        }
    }
    if (!s_dmem_base) {
        rc = sceKernelMapDirectMemory(&s_dmem_base, s_dmem_size, ML_DMEM_PROT_RW, 0,
                                      s_dmem_off, FB_ALIGN);
        if (rc < 0 || !s_dmem_base) {
            LOGE("present: MapDirectMemory WC => 0x%08x", (unsigned)rc);
            sceKernelReleaseDirectMemory(s_dmem_off, s_dmem_size);
            s_dmem_off = 0;
            s_fb_count = s_fb_alloc = 0;
            return -1;
        }
        LOGI("present: dmem WC_GARLIC size=0x%zx (DCE-coherent%s)",
             s_dmem_size, force_wc ? "; YCbCr force" : "");
    }

    s_dmem_cpu_base = s_dmem_base;
    for (int i = 0; i < s_fb_alloc; i++) {
        s_fb[i] = (uint8_t *)s_dmem_base + (size_t)i * s_fb_size;
        s_fb_cpu[i] = s_fb[i];
        memset(s_fb_cpu[i], 0x10, s_fb_size);
    }
    if (s_present_wb) {
        __asm__ volatile("sfence" ::: "memory");
        sceGnmFlushGarlic();
    }
    return 0;
}

static int register_ycbcr(int attr_w, int h, uint32_t pitch, uint32_t tiling) {
    MlVideoOutBufferAttribute attr;
    memset(&attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute(&attr,
                                  ML_VIDEO_OUT_PIXEL_YCBCR420_BT709,
                                  tiling,
                                  ML_VIDEO_OUT_ASPECT_16_9,
                                  (uint32_t)attr_w, (uint32_t)h, pitch);
    int rc = sceVideoOutRegisterBuffers(s_video, 0, (void *const *)s_fb, s_fb_count,
                                        &attr);
    LOGI("present: YCbCr try tile=%u pitch=%u attr=%dx%d n=%d => 0x%08x",
         tiling, pitch, attr_w, h, s_fb_count, (unsigned)rc);
    return rc;
}

static int register_bgra(int w, int h, uint32_t pitch) {
    MlVideoOutBufferAttribute attr;
    memset(&attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute(&attr,
                                  ML_VIDEO_OUT_PIXEL_B8G8R8A8,
                                  ML_VIDEO_OUT_TILING_LINEAR,
                                  ML_VIDEO_OUT_ASPECT_16_9,
                                  (uint32_t)w, (uint32_t)h, pitch);
    int rc = sceVideoOutRegisterBuffers(s_video, 0, (void *const *)s_fb, s_fb_count,
                                        &attr);
    LOGI("present: BGRA try pitch=%u %dx%d n=%d => 0x%08x",
         pitch, w, h, s_fb_count, (unsigned)rc);
    return rc;
}

/* OpenOrbis stub = jmp . (hang). Always Dlsym from the real SPRX. */
static void try_ycbcr_privilege(int video_handle) {
    typedef int32_t (*ycc1_fn)(int32_t);
    typedef int32_t (*sys_fn)(int32_t);
    typedef int32_t (*margins_fn)(int32_t, int32_t, int32_t);
    extern uint32_t sceKernelLoadStartModule(const char *, size_t, const void *,
                                             uint32_t, void *, void *);
    extern int32_t sceKernelDlsym(int32_t, const char *, void **);

    static const char *paths[] = {
        "/system/common/lib/libSceVideoOut.sprx",
        "libSceVideoOut.sprx",
        NULL,
    };
    int mod = -1;
    for (int i = 0; paths[i]; i++) {
        int rc = (int)sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, NULL);
        LOGI("present: LoadStartModule(%s) => 0x%08x", paths[i], (unsigned)rc);
        if (rc > 0) {
            mod = rc;
            break;
        }
    }
    if (mod < 0) {
        LOGW("present: VideoOut SPRX no cargable; privilege via plugin hook");
        return;
    }

    void *sym = NULL;
    if (sceKernelDlsym(mod, "sceVideoOutAddBufferYccPrivilege", &sym) != 0 || !sym) {
        LOGW("present: Dlsym AddBufferYccPrivilege failed");
        return;
    }
    int32_t ycc = ((ycc1_fn)sym)(video_handle);
    LOGI("present: AddBufferYccPrivilege(handle) => 0x%08x", (unsigned)ycc);

    void *sys = NULL;
    if (sceKernelDlsym(mod, "sceVideoOutSysUpdatePrivilege", &sys) == 0 && sys) {
        int32_t upd = ((sys_fn)sys)(video_handle);
        LOGI("present: SysUpdatePrivilege => 0x%08x", (unsigned)upd);
    }

    void *marg = NULL;
    if (sceKernelDlsym(mod, "sceVideoOutSetWindowModeMargins", &marg) == 0 && marg) {
        int32_t mrc = ((margins_fn)marg)(video_handle, 0, 0);
        LOGI("present: SetWindowModeMargins(0,0) => 0x%08x", (unsigned)mrc);
    }
}

/* 0.7.32: RegisterBuffers OK but SubmitFlip → 0x80290001 / cur=-1 (black screen). */
static int32_t probe_submit_flip(int buf_idx) {
    static const uint32_t modes[] = {
        ML_VIDEO_OUT_FLIP_VSYNC,
        2u, /* HSYNC / ASAP en algunos docs */
        0u,
    };
    int32_t last = -1;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        int32_t rc = sceVideoOutSubmitFlip(s_video, buf_idx, modes[i], 0);
        MlVideoOutFlipStatus st;
        memset(&st, 0, sizeof(st));
        (void)sceVideoOutGetFlipStatus(s_video, &st);
        LOGI("present: flip probe mode=%u idx=%d => 0x%08x pending=%d cur=%d",
             modes[i], buf_idx, (unsigned)rc, st.numFlipPending, st.currentBuffer);
        last = rc;
        if (rc == 0) {
            s_flip_mode = (int)modes[i];
            return 0;
        }
    }
    return last;
}

static int switch_to_bgra(int w, int h) {
    LOGW("present: YCbCr no usable → fallback BGRA");
    (void)sceVideoOutUnregisterBuffers(s_video, 0);
    /* Unregister after YCbCr leaves the port dirty (SLOT_OCCUPIED). Reopen. */
    if (s_video >= 0) {
        sceVideoOutClose(s_video);
        s_video = -1;
    }
    {
        int userId = ML_VIDEO_USER_MAIN;
        sceUserServiceGetInitialUser(&userId);
        s_video = sceVideoOutOpen(userId, ML_VIDEO_OUT_BUS_MAIN, 0, NULL);
        if (s_video < 0)
            s_video = sceVideoOutOpen(ML_VIDEO_USER_MAIN, ML_VIDEO_OUT_BUS_MAIN, 0, NULL);
        if (s_video < 0) {
            LOGE("present: reopen VideoOut failed 0x%08x", (unsigned)s_video);
            return -1;
        }
        sceVideoOutSetFlipRate(s_video, ML_VIDEO_OUT_FLIP_60HZ);
    }
    if (alloc_dmem(bgra_size(w, h), FB_COUNT_BGRA, FB_COUNT_BGRA, 0) != 0)
        return -1;
    int brc = register_bgra(w, h, (uint32_t)w);
    if (brc < 0) {
        LOGE("present: fallback BGRA RegisterBuffers 0x%08x", (unsigned)brc);
        return -1;
    }
    s_pitch = w * 4;
    s_buf_h = h;
    s_use_bgra = 1;
    s_fb_index = 0;
    s_last_flip_idx = -1;
    s_prev_flip_idx = -1;
    s_flip_stuck_since = 0;
    s_flip_stuck_logged = 0;
    s_flip_logged = 0;
    s_flip_wait_logs = 0;
    s_flip_mode = ML_VIDEO_OUT_FLIP_VSYNC;
    s_gray_left = 0;
    for (int i = 0; i < s_fb_count; i++)
        memset(s_fb_cpu[i], 0, s_fb_size);
    __asm__ volatile("sfence" ::: "memory");
    sceGnmFlushGarlic();
    int32_t frc = probe_submit_flip(0);
    if (frc != 0) {
        LOGE("present: BGRA SubmitFlip also fails 0x%08x", (unsigned)frc);
        return -1;
    }
    s_last_flip_idx = 0;
    nv12_blit_init();
    bgra_worker_start();
    LOGI("present_mode=BGRA (fallback) plugin=%s buf_h=%d n=%d wb=%d convert=sse2_mt",
         s_plugin_ok ? "OK" : "MISSING", s_buf_h, s_fb_count, s_present_wb);
    return 0;
}

static inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static void yuv_to_bgra(uint8_t *dst, int dst_pitch, int w, int h,
                        const uint8_t *y, const uint8_t *u, const uint8_t *v,
                        int pitch_y, int pitch_u, int pitch_v) {
    for (int row = 0; row < h; row++) {
        uint8_t *drow = dst + (size_t)row * (size_t)dst_pitch;
        const uint8_t *yrow = y + (size_t)row * (size_t)pitch_y;
        const uint8_t *urow = u + (size_t)(row / 2) * (size_t)pitch_u;
        const uint8_t *vrow = v + (size_t)(row / 2) * (size_t)pitch_v;
        for (int x = 0; x < w; x++) {
            int yy = yrow[x];
            int uu = (int)urow[x / 2] - 128;
            int vv = (int)vrow[x / 2] - 128;
            /* >>7 + coeffs ≤227: fit in int16 (avoids mullo 359/454 overflow). */
            int r = yy + ((179 * vv) >> 7);
            int g = yy - ((44 * uu + 92 * vv) >> 7);
            int b = yy + ((227 * uu) >> 7);
            drow[x * 4 + 0] = clamp_u8(b);
            drow[x * 4 + 1] = clamp_u8(g);
            drow[x * 4 + 2] = clamp_u8(r);
            drow[x * 4 + 3] = 0xFF;
        }
    }
}

/*
 * Nearest-neighbour horizontal LUT cache: streamed resolution stays fixed for
 * the whole session, so this is computed once and reused every frame. Shared
 * (read-only once built) by every BGRA worker thread.
 */
#define SCALE_LUT_MAX 1920
static int s_scale_lut[SCALE_LUT_MAX];
static int s_scale_lut_dst_w, s_scale_lut_src_w;

static const int *scale_lut_get(int dst_w, int src_w) {
    if (dst_w <= 0 || src_w <= 0 || dst_w > SCALE_LUT_MAX)
        return NULL;
    if (dst_w == s_scale_lut_dst_w && src_w == s_scale_lut_src_w)
        return s_scale_lut;
    for (int x = 0; x < dst_w; x++) {
        int sx = (int)(((int64_t)x * src_w) / dst_w);
        if (sx >= src_w)
            sx = src_w - 1;
        s_scale_lut[x] = sx;
    }
    s_scale_lut_dst_w = dst_w;
    s_scale_lut_src_w = src_w;
    return s_scale_lut;
}

static inline int scale_src_row(int dst_row, int dst_h, int src_h) {
    int sr = (int)(((int64_t)dst_row * src_h) / dst_h);
    if (sr >= src_h)
        sr = src_h - 1;
    return sr;
}

/* Stretch YUV420P (3 planes) to fill dst_w x dst_h — used when the decoded
 * frame size (src_w x src_h) does not match the registered VideoOut buffer
 * (e.g. streaming at 720p onto a 1080p output): without this the picture
 * only occupies the top-left src_w x src_h corner of the screen. */
static void yuv_to_bgra_scaled(uint8_t *dst, int dst_pitch, int dst_w, int dst_h,
                               const uint8_t *y, const uint8_t *u, const uint8_t *v,
                               int pitch_y, int pitch_u, int pitch_v,
                               int src_w, int src_h, const int *x_lut) {
    (void)src_w;
    for (int row = 0; row < dst_h; row++) {
        int src_row = scale_src_row(row, dst_h, src_h);
        int uv_row = src_row / 2;
        uint8_t *drow = dst + (size_t)row * (size_t)dst_pitch;
        const uint8_t *yrow = y + (size_t)src_row * (size_t)pitch_y;
        const uint8_t *urow = u + (size_t)uv_row * (size_t)pitch_u;
        const uint8_t *vrow = v + (size_t)uv_row * (size_t)pitch_v;
        for (int x = 0; x < dst_w; x++) {
            int sx = x_lut[x];
            int yy = yrow[sx];
            int uu = (int)urow[sx / 2] - 128;
            int vv = (int)vrow[sx / 2] - 128;
            int r = yy + ((179 * vv) >> 7);
            int g = yy - ((44 * uu + 92 * vv) >> 7);
            int b = yy + ((227 * uu) >> 7);
            drow[x * 4 + 0] = clamp_u8(b);
            drow[x * 4 + 1] = clamp_u8(g);
            drow[x * 4 + 2] = clamp_u8(r);
            drow[x * 4 + 3] = 0xFF;
        }
    }
}

/*
 * Store mode for the BGRA output. Non-temporal only pays off on WC_GARLIC:
 * on the ONION/WB alias the cache is what hides the coherent bus latency, and
 * bypassing it measured ~6x slower (8.3 ms vs 1.4 ms per 1080p frame).
 */
#define BGRA_ST_UNALIGNED 0
#define BGRA_ST_ALIGNED   1
#define BGRA_ST_NT        2

/* 8 px BGRA from Y (epi16) + precomputed chroma terms. */
static inline __attribute__((always_inline)) void
bgra_store8(uint8_t *dp, __m128i y16, __m128i rv, __m128i guv, __m128i bu,
            __m128i a255, const int st_mode) {
    __m128i r = _mm_add_epi16(y16, rv);
    __m128i g = _mm_sub_epi16(y16, guv);
    __m128i b = _mm_add_epi16(y16, bu);
    __m128i br = _mm_packus_epi16(b, r);
    __m128i ga = _mm_packus_epi16(g, a255);
    __m128i bg0 = _mm_unpacklo_epi8(br, ga);
    __m128i ra0 = _mm_unpackhi_epi8(br, ga);
    __m128i px0 = _mm_unpacklo_epi16(bg0, ra0);
    __m128i px1 = _mm_unpackhi_epi16(bg0, ra0);
    if (st_mode == BGRA_ST_NT) {
        /* Callers pair +0/+32 so full 64B lines are written. */
        _mm_stream_si128((__m128i *)(void *)dp, px0);
        _mm_stream_si128((__m128i *)(void *)(dp + 16), px1);
    } else if (st_mode == BGRA_ST_ALIGNED) {
        _mm_store_si128((__m128i *)(void *)dp, px0);
        _mm_store_si128((__m128i *)(void *)(dp + 16), px1);
    } else {
        _mm_storeu_si128((__m128i *)(void *)dp, px0);
        _mm_storeu_si128((__m128i *)(void *)(dp + 16), px1);
    }
}

static inline void nv12_to_bgra_px(uint8_t *dp, int yy, int uu, int vv) {
    dp[0] = clamp_u8(yy + ((227 * uu) >> 7));
    dp[1] = clamp_u8(yy - ((44 * uu + 92 * vv) >> 7));
    dp[2] = clamp_u8(yy + ((179 * vv) >> 7));
    dp[3] = 0xFF;
}

/*
 * 16 px/iter, 2 Y rows per UV row (4:2:0: chroma computed once per pair),
 * prefetch next row pair (Jaguar). Constant `st_mode` → compiler emits
 * a single store variant per instantiation.
 *
 * Coefs >>7 (179/44/92/227): ≈ BT.601 JPEG and |chroma|·coeff ≤ 28829 < 32768
 * → mullo_epi16 without wrap (>>8 path with 359/454 painted lilac as #ffc400).
 */
static inline __attribute__((always_inline)) void
nv12_to_bgra_impl(uint8_t *dst, int dst_pitch, int w, int h,
                  const uint8_t *y, const uint8_t *uv,
                  int pitch_y, int pitch_uv, const int st_mode) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i m00ff = _mm_set1_epi16(0x00FF);
    const __m128i c128 = _mm_set1_epi16(128);
    const __m128i c179 = _mm_set1_epi16(179);
    const __m128i c44 = _mm_set1_epi16(44);
    const __m128i c92 = _mm_set1_epi16(92);
    const __m128i c227 = _mm_set1_epi16(227);
    const __m128i a255 = _mm_set1_epi16(255);

    int row = 0;
    for (; row + 1 < h; row += 2) {
        uint8_t *drow0 = dst + (size_t)row * (size_t)dst_pitch;
        uint8_t *drow1 = drow0 + dst_pitch;
        const uint8_t *yrow0 = y + (size_t)row * (size_t)pitch_y;
        const uint8_t *yrow1 = yrow0 + pitch_y;
        const uint8_t *uvrow = uv + (size_t)(row / 2) * (size_t)pitch_uv;
        int x = 0;
        for (; x + 15 < w; x += 16) {
            _mm_prefetch((const char *)(yrow1 + pitch_y + x), _MM_HINT_T0);
            _mm_prefetch((const char *)(yrow1 + 2 * pitch_y + x), _MM_HINT_T0);
            _mm_prefetch((const char *)(uvrow + pitch_uv + x), _MM_HINT_T0);

            /* 16B interleaved UV = chroma for 16 px; deinterleave by mask/shift */
            __m128i uv8 = _mm_loadu_si128((const __m128i *)(const void *)(uvrow + x));
            __m128i u16 = _mm_sub_epi16(_mm_and_si128(uv8, m00ff), c128);
            __m128i v16 = _mm_sub_epi16(_mm_srli_epi16(uv8, 8), c128);
            __m128i rv = _mm_srai_epi16(_mm_mullo_epi16(v16, c179), 7);
            __m128i guv = _mm_srai_epi16(
                _mm_add_epi16(_mm_mullo_epi16(u16, c44),
                              _mm_mullo_epi16(v16, c92)), 7);
            __m128i bu = _mm_srai_epi16(_mm_mullo_epi16(u16, c227), 7);
            /* duplicate each chroma sample to its 2 px */
            __m128i rv_lo = _mm_unpacklo_epi16(rv, rv);
            __m128i rv_hi = _mm_unpackhi_epi16(rv, rv);
            __m128i guv_lo = _mm_unpacklo_epi16(guv, guv);
            __m128i guv_hi = _mm_unpackhi_epi16(guv, guv);
            __m128i bu_lo = _mm_unpacklo_epi16(bu, bu);
            __m128i bu_hi = _mm_unpackhi_epi16(bu, bu);

            __m128i y0 = _mm_loadu_si128((const __m128i *)(const void *)(yrow0 + x));
            __m128i y1 = _mm_loadu_si128((const __m128i *)(const void *)(yrow1 + x));
            bgra_store8(drow0 + (size_t)x * 4,
                        _mm_unpacklo_epi8(y0, zero), rv_lo, guv_lo, bu_lo, a255, st_mode);
            bgra_store8(drow0 + (size_t)x * 4 + 32,
                        _mm_unpackhi_epi8(y0, zero), rv_hi, guv_hi, bu_hi, a255, st_mode);
            bgra_store8(drow1 + (size_t)x * 4,
                        _mm_unpacklo_epi8(y1, zero), rv_lo, guv_lo, bu_lo, a255, st_mode);
            bgra_store8(drow1 + (size_t)x * 4 + 32,
                        _mm_unpackhi_epi8(y1, zero), rv_hi, guv_hi, bu_hi, a255, st_mode);
        }
        for (; x < w; x++) {
            int uu = (int)uvrow[(x / 2) * 2] - 128;
            int vv = (int)uvrow[(x / 2) * 2 + 1] - 128;
            nv12_to_bgra_px(drow0 + (size_t)x * 4, yrow0[x], uu, vv);
            nv12_to_bgra_px(drow1 + (size_t)x * 4, yrow1[x], uu, vv);
        }
    }
    /* fila suelta si h impar */
    for (; row < h; row++) {
        uint8_t *drow = dst + (size_t)row * (size_t)dst_pitch;
        const uint8_t *yrow = y + (size_t)row * (size_t)pitch_y;
        const uint8_t *uvrow = uv + (size_t)(row / 2) * (size_t)pitch_uv;
        for (int x = 0; x < w; x++) {
            int uu = (int)uvrow[(x / 2) * 2] - 128;
            int vv = (int)uvrow[(x / 2) * 2 + 1] - 128;
            nv12_to_bgra_px(drow + (size_t)x * 4, yrow[x], uu, vv);
        }
    }
}

static void nv12_to_bgra_rows(uint8_t *dst, int dst_pitch, int w,
                              int row0, int row1,
                              const uint8_t *y, const uint8_t *uv,
                              int pitch_y, int pitch_uv) {
    if (row1 <= row0)
        return;
    uint8_t *d = dst + (size_t)row0 * (size_t)dst_pitch;
    const uint8_t *yp = y + (size_t)row0 * (size_t)pitch_y;
    const uint8_t *uvp = uv + (size_t)(row0 / 2) * (size_t)pitch_uv;
    int h = row1 - row0;
    if ((((uintptr_t)d | (uintptr_t)(unsigned)dst_pitch) & 15u) != 0)
        nv12_to_bgra_impl(d, dst_pitch, w, h, yp, uvp, pitch_y, pitch_uv,
                          BGRA_ST_UNALIGNED);
    else if (s_bgra_nt)
        nv12_to_bgra_impl(d, dst_pitch, w, h, yp, uvp, pitch_y, pitch_uv,
                          BGRA_ST_NT);
    else
        nv12_to_bgra_impl(d, dst_pitch, w, h, yp, uvp, pitch_y, pitch_uv,
                          BGRA_ST_ALIGNED);
}

/* Scalar nearest-neighbour scale (NV12 src_w x src_h -> dst_w x dst_h), rows
 * [row0,row1) of the destination. Slower than the SIMD 1:1 path but only
 * runs when the streamed resolution differs from the VideoOut buffer. */
static void nv12_to_bgra_rows_scaled(uint8_t *dst, int dst_pitch, int dst_w,
                                     int row0, int row1, int dst_h,
                                     const uint8_t *y, const uint8_t *uv,
                                     int pitch_y, int pitch_uv,
                                     int src_h, const int *x_lut) {
    for (int row = row0; row < row1; row++) {
        int src_row = scale_src_row(row, dst_h, src_h);
        int uv_row = src_row / 2;
        uint8_t *drow = dst + (size_t)row * (size_t)dst_pitch;
        const uint8_t *yrow = y + (size_t)src_row * (size_t)pitch_y;
        const uint8_t *uvrow = uv + (size_t)uv_row * (size_t)pitch_uv;
        for (int x = 0; x < dst_w; x++) {
            int sx = x_lut[x];
            int uvx = (sx / 2) * 2;
            nv12_to_bgra_px(drow + (size_t)x * 4, yrow[sx],
                            (int)uvrow[uvx] - 128, (int)uvrow[uvx + 1] - 128);
        }
    }
}

/* Drain the band queue of the current job. Runs on workers and, when the
 * caller helps, on the calling thread too. */
static void bgra_run_bands(void) {
    const nv12_bgra_job_t *j = &s_bgra_job;
    for (;;) {
        int b = __atomic_fetch_add(&s_bgra_next_band, 1, __ATOMIC_RELAXED);
        if (b >= j->nbands)
            break;
        int row0 = b * BGRA_BAND_ROWS;
        int row1 = row0 + BGRA_BAND_ROWS;
        if (row1 > j->dst_h)
            row1 = j->dst_h;
        if (j->x_lut) {
            nv12_to_bgra_rows_scaled(j->dst, j->dst_pitch, j->dst_w, row0, row1,
                                     j->dst_h, j->y, j->uv, j->pitch_y, j->pitch_uv,
                                     j->src_h, j->x_lut);
        } else {
            nv12_to_bgra_rows(j->dst, j->dst_pitch, j->dst_w, row0, row1,
                              j->y, j->uv, j->pitch_y, j->pitch_uv);
        }
    }
    /* Flush the WC buffers of this thread's streaming stores. */
    _mm_sfence();
}

static void bgra_note_elapsed(uint64_t dt) {
    pthread_mutex_lock(&s_bgra_mtx);
    if (dt > s_bgra_job_us)
        s_bgra_job_us = dt;
    pthread_mutex_unlock(&s_bgra_mtx);
}

static void *bgra_worker_main(void *arg) {
    (void)arg;
    int seen = 0;
    for (;;) {
        pthread_mutex_lock(&s_bgra_mtx);
        while (s_bgra_seq == seen && !s_bgra_shutdown)
            pthread_cond_wait(&s_bgra_cv, &s_bgra_mtx);
        if (s_bgra_shutdown) {
            pthread_mutex_unlock(&s_bgra_mtx);
            break;
        }
        seen = s_bgra_seq;
        pthread_mutex_unlock(&s_bgra_mtx);

        uint64_t t0 = now_us();
        bgra_run_bands();
        uint64_t dt = now_us() - t0;

        pthread_mutex_lock(&s_bgra_mtx);
        if (dt > s_bgra_job_us)
            s_bgra_job_us = dt;
        if (--s_bgra_outstanding == 0)
            pthread_cond_broadcast(&s_bgra_cv);
        pthread_mutex_unlock(&s_bgra_mtx);
    }
    return NULL;
}

/* WC_GARLIC has no cache to hide the write latency, so streaming stores win
 * there. On the ONION/WB alias they lose badly; keep the cache in the path. */
static void bgra_resolve_store_mode(void) {
    s_bgra_nt = s_bgra_nt_pref >= 0 ? (s_bgra_nt_pref ? 1 : 0)
                                    : (s_present_wb ? 0 : 1);
}

void video_present_set_bgra_tuning(int workers, int nt_pref) {
    if (workers > 0) {
        if (workers > BGRA_WORKERS_MAX)
            workers = BGRA_WORKERS_MAX;
        s_bgra_workers = workers;
    }
    s_bgra_nt_pref = nt_pref;
    bgra_resolve_store_mode();
    LOGI("present: BGRA tuning workers=%d nt_pref=%d", s_bgra_workers, nt_pref);
}

static void bgra_worker_start(void) {
    if (s_bgra_worker_alive)
        return;
    s_bgra_shutdown = 0;
    s_bgra_outstanding = 0;
    s_bgra_seq = 0;
    s_bgra_next_band = 0;
    s_bgra_job.nbands = 0;
    int want = s_bgra_workers;
    int ok = 0;
    for (int i = 0; i < want; i++) {
        if (pthread_create(&s_bgra_thr[i], NULL, bgra_worker_main, NULL) == 0)
            ok++;
    }
    if (ok == want) {
        s_bgra_worker_alive = 1;
    } else {
        s_bgra_shutdown = 1;
        pthread_cond_broadcast(&s_bgra_cv);
        for (int i = 0; i < ok; i++)
            pthread_join(s_bgra_thr[i], NULL);
        s_bgra_shutdown = 0;
        LOGW("present: BGRA workers create failed (%d/%d); single-thread",
             ok, want);
    }
}

static void bgra_worker_stop(void) {
    if (!s_bgra_worker_alive)
        return;
    pthread_mutex_lock(&s_bgra_mtx);
    s_bgra_shutdown = 1;
    pthread_cond_broadcast(&s_bgra_cv);
    pthread_mutex_unlock(&s_bgra_mtx);
    for (int i = 0; i < s_bgra_workers; i++)
        pthread_join(s_bgra_thr[i], NULL);
    s_bgra_worker_alive = 0;
    s_bgra_shutdown = 0;
    s_bgra_outstanding = 0;
}

/* src_w/src_h: decoded frame size. dst_w/dst_h: VideoOut buffer size (always
 * the display's registered resolution). When they differ the frame is
 * nearest-neighbour scaled to fill the whole buffer instead of only writing
 * its top-left src_w x src_h corner. */
static void bgra_convert_kick(uint8_t *dst, int dst_pitch,
                              int src_w, int src_h, int dst_w, int dst_h,
                              const uint8_t *y, const uint8_t *uv,
                              int pitch_y, int pitch_uv, int main_helps) {
    const int *lut = (src_w == dst_w && src_h == dst_h)
                          ? NULL
                          : scale_lut_get(dst_w, src_w);
    /* LUT unavailable (dst_w > SCALE_LUT_MAX): fall back to 1:1 clipped copy
     * rather than reading out of bounds. */
    if (!lut && (src_w != dst_w || src_h != dst_h)) {
        dst_w = src_w < dst_w ? src_w : dst_w;
        dst_h = src_h < dst_h ? src_h : dst_h;
    }

    if (!s_bgra_worker_alive || dst_h < 2) {
        uint64_t t0 = now_us();
        if (lut)
            nv12_to_bgra_rows_scaled(dst, dst_pitch, dst_w, 0, dst_h, dst_h,
                                     y, uv, pitch_y, pitch_uv, src_h, lut);
        else
            nv12_to_bgra_rows(dst, dst_pitch, dst_w, 0, dst_h, y, uv, pitch_y, pitch_uv);
        _mm_sfence();
        s_bgra_job_us = now_us() - t0;
        return;
    }
    pthread_mutex_lock(&s_bgra_mtx);
    s_bgra_job_us = 0;
    s_bgra_job.dst = dst;
    s_bgra_job.dst_pitch = dst_pitch;
    s_bgra_job.dst_w = dst_w;
    s_bgra_job.dst_h = dst_h;
    s_bgra_job.src_w = src_w;
    s_bgra_job.src_h = src_h;
    s_bgra_job.y = y;
    s_bgra_job.uv = uv;
    s_bgra_job.pitch_y = pitch_y;
    s_bgra_job.pitch_uv = pitch_uv;
    s_bgra_job.x_lut = lut;
    s_bgra_job.nbands = (dst_h + BGRA_BAND_ROWS - 1) / BGRA_BAND_ROWS;
    __atomic_store_n(&s_bgra_next_band, 0, __ATOMIC_RELAXED);
    s_bgra_outstanding = s_bgra_workers;
    s_bgra_seq++;
    pthread_cond_broadcast(&s_bgra_cv);
    pthread_mutex_unlock(&s_bgra_mtx);

    if (main_helps) {
        /* Sync path: the caller would only sit blocked otherwise. */
        uint64_t t0 = now_us();
        bgra_run_bands();
        bgra_note_elapsed(now_us() - t0);
    }
}

static void bgra_convert_wait(void) {
    if (!s_bgra_worker_alive)
        return;
    pthread_mutex_lock(&s_bgra_mtx);
    while (s_bgra_outstanding > 0)
        pthread_cond_wait(&s_bgra_cv, &s_bgra_mtx);
    pthread_mutex_unlock(&s_bgra_mtx);
}

static void nv12_to_bgra(uint8_t *dst, int dst_pitch,
                         int src_w, int src_h, int dst_w, int dst_h,
                         const uint8_t *y, const uint8_t *uv,
                         int pitch_y, int pitch_uv) {
    bgra_convert_kick(dst, dst_pitch, src_w, src_h, dst_w, dst_h, y, uv, pitch_y, pitch_uv, 1);
    bgra_convert_wait();
}

static void pack_yuv420p_to_nv12(uint8_t *dst, int dst_pitch, int w, int h,
                                 const uint8_t *y, const uint8_t *u, const uint8_t *v,
                                 int pitch_y, int pitch_u, int pitch_v) {
    for (int row = 0; row < h; row++) {
        memcpy(dst + (size_t)row * (size_t)dst_pitch, y + (size_t)row * (size_t)pitch_y, (size_t)w);
    }
    uint8_t *dst_uv = dst + (size_t)dst_pitch * (size_t)h;
    int uv_h = h / 2;
    for (int row = 0; row < uv_h; row++) {
        uint8_t *drow = dst_uv + (size_t)row * (size_t)dst_pitch;
        const uint8_t *urow = u + (size_t)row * (size_t)pitch_u;
        const uint8_t *vrow = v + (size_t)row * (size_t)pitch_v;
        for (int x = 0; x < w / 2; x++) {
            drow[x * 2 + 0] = urow[x];
            drow[x * 2 + 1] = vrow[x];
        }
    }
}

int video_present_init(int w, int h, int prefer_ycbcr) {
    /*
     * NEVER call sceVideoOutClose on the hot path: with pending flips
     * (cur=-1) Close hangs → eternal black on stream exit or launch.
     * Always reuse the BGRA present if it already exists.
     */
    if (s_video >= 0 && s_use_bgra && !prefer_ycbcr) {
        LOGI("present: reuse VideoOut BGRA %dx%d (requested %dx%d)",
             s_width, s_buf_h, w, h);
        s_flip_logged = 0;
        s_flip_wait_logs = 0;
        s_prev_flip_idx = -1;
        s_flip_stuck_since = 0;
        s_flip_stuck_logged = 0;
        s_gray_left = 0;
        s_pipe_active = 0;
        s_pipe_fb_idx = -1;
        bgra_resolve_store_mode();
        bgra_worker_start();
        return 0;
    }
    if (s_video >= 0) {
        /* YCbCr or other mode: soft-detach without Close; force BGRA reuse below. */
        LOGW("present: VideoOut already open (non-BGRA); soft-detach and BGRA");
        video_present_shutdown(); /* soft: no Close */
        if (s_video >= 0 && s_use_bgra) {
            bgra_worker_start();
            return 0;
        }
    }

    s_width = w;
    s_height = h;
    s_pitch = w;
    s_buf_h = h;
    s_use_bgra = 0;
    s_plugin_ok = video_present_plugin_loaded();

    int userId = ML_VIDEO_USER_MAIN;
    sceUserServiceGetInitialUser(&userId);

    /* MAIN bus only — SOCIAL/LIVE → INVALID_VALUE en pruebas previas. */
    s_video = sceVideoOutOpen(userId, ML_VIDEO_OUT_BUS_MAIN, 0, NULL);
    if (s_video < 0)
        s_video = sceVideoOutOpen(ML_VIDEO_USER_MAIN, ML_VIDEO_OUT_BUS_MAIN, 0, NULL);
    if (s_video < 0) {
        LOGE("present: sceVideoOutOpen failed: 0x%08x", s_video);
        return -1;
    }
    sceVideoOutSetFlipRate(s_video, ML_VIDEO_OUT_FLIP_60HZ);

    if (!prefer_ycbcr)
        goto bgra_debug;

    LOGI("present: plugin=%s — resolving YCC privilege via Dlsym",
         s_plugin_ok ? "OK" : "MISSING");

    /* Privilege on a fresh port. Do not register BGRA first: Unregister does not
     * fully clear → 0x80290010 SLOT_OCCUPIED / 0x80290009 RESOURCE_BUSY and the
     * YCbCr ioctl is never called (vo_diag post-kpatch). */
    try_ycbcr_privilege(s_video);

    int reg_h = (h == 1088) ? 1080 : h;
    int rc = -1;

    /*
     * hrep4: DCE reads bpp×4 (pitch attr 1920 → stride 7680). YYYY+UV planar.
     * n=2 + plugin 1.63 forces addrs ioctl. Experimental format — prefer_ycbcr=0.
     */
    int stride4 = w * YCC_STRIDE_MUL;
    size_t need = nv12_size(stride4, reg_h > 1088 ? reg_h : 1088);
    if (alloc_dmem(need, FB_COUNT_YCC_ALLOC, FB_COUNT_YCC_ALLOC, 1 /* force WC */) != 0)
        return -1;

    LOGI("present: dmem alloc=%d fb_size=0x%zx total=0x%zx wb=%d stride4=%d",
         s_fb_alloc, s_fb_size, s_dmem_size, s_present_wb, stride4);
    for (int i = 0; i < s_fb_alloc; i++)
        LOGI("present: fb[%d] va=%p phys_off=0x%llx", i, s_fb[i],
             (unsigned long long)(s_dmem_off + (off_t)i * (off_t)s_fb_size));

    typedef struct {
        uint32_t tile;
        int h;
        int attr_w;
        uint32_t attr_pitch;
        int blit_pitch;
        int hrep4;
        const char *tag;
    } ycbcr_try_t;
    ycbcr_try_t tries[] = {
        { ML_VIDEO_OUT_TILING_LINEAR, reg_h, w, (uint32_t)w, stride4, 1, "hrep4" },
        { ML_VIDEO_OUT_TILING_TILE,   reg_h, w, (uint32_t)w, stride4, 1, "hrep4tile" },
    };

    s_ycc_hrep4 = 1;
    rc = -1;
    /* Prefer n=2 (no tearing); fall back to n=1 if kernel rejects. */
    int n_try[] = { s_fb_alloc, 1 };
    for (size_t ni = 0; ni < sizeof(n_try) / sizeof(n_try[0]) && rc != 0; ni++) {
        s_fb_count = n_try[ni];
        for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
            (void)sceVideoOutUnregisterBuffers(s_video, 0);
            rc = register_ycbcr(tries[i].attr_w, tries[i].h, tries[i].attr_pitch,
                                tries[i].tile);
            if (rc == 0) {
                s_pitch = tries[i].blit_pitch;
                s_buf_h = tries[i].h;
                s_ycc_hrep4 = tries[i].hrep4;
                LOGI("present: YCbCr OK tag=%s attr=%dx%d pitch=%u blit=%d hrep4=%d tile=%u n=%d",
                     tries[i].tag, tries[i].attr_w, tries[i].h, tries[i].attr_pitch,
                     s_pitch, s_ycc_hrep4, tries[i].tile, s_fb_count);
                break;
            }
        }
    }

    if (rc != 0) {
        LOGE("present: YCbCr RegisterBuffers failed (last 0x%08x); fallback BGRA",
             (unsigned)rc);
        (void)sceVideoOutUnregisterBuffers(s_video, 0);
        if (switch_to_bgra(w, h) != 0) {
            LOGI("present_mode=FAIL plugin=%s", s_plugin_ok ? "OK" : "MISSING");
            video_present_shutdown();
            return -1;
        }
        return 0;
    }

    s_use_bgra = 0;
    s_fb_index = 0;
    s_last_flip_idx = -1;
    s_prev_flip_idx = -1;
    s_flip_stuck_since = 0;
    s_flip_stuck_logged = 0;
    s_flip_logged = 0;
    s_flip_wait_logs = 0;
    s_flip_mode = ML_VIDEO_OUT_FLIP_VSYNC;
    s_gray_left = 2;
    {
        for (int i = 0; i < s_fb_count; i++)
            fill_solid_u32((uint8_t *)s_fb_cpu[i], s_pitch, s_buf_h, w, 0x80808080u);
        __asm__ volatile("sfence" ::: "memory");
        sceGnmFlushGarlic();
    }

    int32_t flip_rc = probe_submit_flip(0);
    if (flip_rc != 0) {
        LOGW("present: YCbCr Register OK but SubmitFlip 0x%08x (cur=-1 typical)",
             (unsigned)flip_rc);
        if (switch_to_bgra(w, h) != 0) {
            video_present_shutdown();
            return -1;
        }
        return 0;
    }
    s_last_flip_idx = 0;
    s_fb_index = 0;

    nv12_blit_init();
    LOGI("present: YCbCr420_BT709 OK %dx%d pitch=%d buf_h=%d n=%d flip_mode=%d",
         w, s_buf_h, s_pitch, s_buf_h, s_fb_count, s_flip_mode);
    LOGI("present_mode=YCbCr plugin=%s buf_h=%d n=%d blit=%s hrep4=%d wb=%d",
         s_plugin_ok ? "OK" : "MISSING", s_buf_h, s_fb_count,
         nv12_blit_mode_name(), s_ycc_hrep4, s_present_wb);
    return 0;

bgra_debug:
    LOGI("present: prefer_ycbcr=0 → BGRA (colores OK; convert CPU)");
    if (alloc_dmem(bgra_size(w, h), FB_COUNT_BGRA, FB_COUNT_BGRA, 0) != 0) {
        video_present_shutdown();
        return -1;
    }
    {
        int brc = register_bgra(w, h, (uint32_t)w);
        if (brc < 0) {
            LOGE("present: FATAL BGRA failed rc=0x%08x", (unsigned)brc);
            video_present_shutdown();
            return -1;
        }
    }
    s_pitch = w * 4;
    s_buf_h = h;
    s_use_bgra = 1;
    s_fb_index = 0;
    s_last_flip_idx = -1;
    s_prev_flip_idx = -1;
    s_flip_stuck_since = 0;
    s_flip_stuck_logged = 0;
    s_flip_logged = 0;
    s_flip_wait_logs = 0;
    s_flip_mode = ML_VIDEO_OUT_FLIP_VSYNC;
    s_gray_left = 0;
    for (int i = 0; i < s_fb_count; i++) {
        memset(s_fb_cpu[i], 0, s_fb_size);
        __asm__ volatile("sfence" ::: "memory");
    }
    sceGnmFlushGarlic();
    (void)probe_submit_flip(0);
    s_last_flip_idx = 0;
    s_fb_index = 0;
    nv12_blit_init();
    bgra_resolve_store_mode();
    bgra_worker_start();
    LOGI("present_mode=BGRA plugin=%s buf_h=%d n=%d wb=%d convert=sse2_mt "
         "workers=%d nt=%d",
         s_plugin_ok ? "OK" : "MISSING", s_buf_h, s_fb_count, s_present_wb,
         s_bgra_workers, s_bgra_nt);
    return 0;
}

void video_present_shutdown(void) {
    /*
     * Soft-detach: stop workers/pipe but do NOT close VideoOut or free dmem.
     * Close hangs with pending flips; menu/stream reuse the same port.
     */
    LOGI("present: soft-shutdown (without Close)");
    if (s_pipe_active) {
        bgra_convert_wait();
        s_pipe_active = 0;
        s_pipe_fb_idx = -1;
    }
    bgra_worker_stop();
    nv12_blit_shutdown();
    s_flip_logged = 0;
    /* s_video / dmem / s_use_bgra / size are preserved */
}

int video_present_is_bgra(void) {
    return s_use_bgra;
}

static int pick_free_fb(int *out_shown) {
    MlVideoOutFlipStatus st0;
    memset(&st0, 0, sizeof(st0));
    (void)sceVideoOutGetFlipStatus(s_video, &st0);
    int shown = st0.currentBuffer;
    if (out_shown)
        *out_shown = shown;
    int next = -1;
    if (s_fb_count > 1) {
        for (int i = 0; i < s_fb_count; i++) {
            int cand = (s_fb_index + 1 + i) % s_fb_count;
            if (shown >= 0 && cand == shown)
                continue;
            /* Exclude every in-flight flip, not just the most recent: with
             * n=3 and pending=2 both queued buffers must be avoided.
             * prev is only still in flight when >=2 flips are queued;
             * with pending==1 it is already retired (shown check covers it). */
            if (st0.numFlipPending > 0 && cand == s_last_flip_idx)
                continue;
            if (st0.numFlipPending >= 2 && cand == s_prev_flip_idx)
                continue;
            if (s_pipe_active && cand == s_pipe_fb_idx)
                continue;
            next = cand;
            break;
        }
    } else {
        next = 0;
    }
    return next;
}

void video_set_show_stats(int enable) {
    s_show_stats = enable ? 1 : 0;
}

/* Colors 0xAARRGGBB (BGRA LE); the output plane ignores alpha (see ui_menu.c). */
#define STATS_COL_BG      0xFF141B24u
#define STATS_COL_ACCENT  0xFF3FA7FFu
#define STATS_COL_TEXT    0xFFE8ECF0u
#define STATS_COL_DIM     0xFF8794A2u
#define STATS_COL_GOOD    0xFF6FF08Fu
#define STATS_COL_WARN    0xFFFFB03Fu
#define STATS_COL_BAD     0xFFFF4D4Du
#define STATS_COL_GRAPH_BG 0xFF0B0F14u

/* Perf overlay: FPS/decode/convert/present/KB-per-frame + a frame-time
 * timeline (bar per recent frame; green/amber/red vs. 60/30 fps budgets).
 * Draws directly onto the BGRA backbuffer before it is flushed/flipped, so
 * it is redrawn fresh every frame (no need to erase it beforehand). */
static void draw_stats_overlay(uint8_t *dst) {
    video_live_stats_t live;
    video_stats_get_live(&live);

    ui_surface_t s = { dst, s_pitch, s_width, s_buf_h };

    const int scale = 2;
    const int line_h = 8 * scale + 6;
    const int graph_w = 240, graph_h = 56;
    const int px = 24, py = 24;
    const int pad = 12;
    const int pw = graph_w + pad * 2;
    const int ph = pad + line_h * 4 + 8 + graph_h + pad;

    ui_rect(&s, px, py, pw, ph, STATS_COL_BG);
    ui_rect(&s, px, py, pw, 3, STATS_COL_ACCENT);

    char line[64];
    int ty = py + pad;
    int tx = px + pad;

    snprintf(line, sizeof(line), "FPS %.1f", (double)live.fps);
    ui_text(&s, tx, ty, scale, STATS_COL_GOOD, line);
    ty += line_h;

    snprintf(line, sizeof(line), "DEC %.1fms  CONV %.1fms",
             (double)live.decode_ms, (double)live.convert_ms);
    ui_text(&s, tx, ty, scale, STATS_COL_TEXT, line);
    ty += line_h;

    snprintf(line, sizeof(line), "PRES %.1fms  TOT %.1fms", (double)live.present_ms,
             (double)(live.decode_ms + live.convert_ms + live.present_ms));
    ui_text(&s, tx, ty, scale, STATS_COL_TEXT, line);
    ty += line_h;

    snprintf(line, sizeof(line), "%.0f KB/frame", (double)live.kb_per_frame);
    ui_text(&s, tx, ty, scale, STATS_COL_DIM, line);
    ty += line_h + 8;

    /* Frame-time timeline: oldest on the left, most recent on the right. */
    int gx = tx, gy = ty;
    ui_rect(&s, gx, gy, graph_w, graph_h, STATS_COL_GRAPH_BG);

    float max_ms = 16.7f; /* floor at 60fps so a perfectly smooth feed still shows some bar */
    for (int i = 0; i < live.frame_count; i++)
        if (live.frame_ms[i] > max_ms)
            max_ms = live.frame_ms[i];

    const int bar_w = 2, gap = 1;
    int nbars = graph_w / (bar_w + gap);
    if (nbars > live.frame_count)
        nbars = live.frame_count;
    int x0 = gx + graph_w - nbars * (bar_w + gap);
    for (int i = 0; i < nbars; i++) {
        int src_idx = live.frame_count - nbars + i;
        float ms = live.frame_ms[src_idx];
        int h = (int)(ms / max_ms * graph_h + 0.5f);
        if (h > graph_h) h = graph_h;
        if (h < 1) h = 1;
        uint32_t col = STATS_COL_GOOD;
        if (ms > 33.3f)
            col = STATS_COL_BAD; /* below 30fps budget */
        else if (ms > 16.7f)
            col = STATS_COL_WARN; /* below 60fps budget */
        ui_rect(&s, x0 + i * (bar_w + gap), gy + graph_h - h, bar_w, h, col);
    }
}

static void present_submit_flip(int next, uint8_t *dst, uint64_t convert_us) {
    if (s_show_stats && s_use_bgra)
        draw_stats_overlay(dst);

    __asm__ volatile("sfence" ::: "memory");
    sceGnmFlushGarlic();

    uint64_t t1 = now_us();
    int32_t flip_rc = sceVideoOutSubmitFlip(s_video, next, (uint32_t)s_flip_mode, 0);
    if (flip_rc == 0) {
        s_prev_flip_idx = s_last_flip_idx;
        s_fb_index = next;
        s_last_flip_idx = next;
    }

    int shown_ok = 1;
    if (!s_use_bgra && s_fb_count > 1 && flip_rc == 0)
        shown_ok = wait_flip_shown(next, 32);

    uint64_t t2 = now_us();

    if (!s_flip_logged) {
        s_flip_logged = 1;
        MlVideoOutFlipStatus st;
        memset(&st, 0, sizeof(st));
        (void)sceVideoOutGetFlipStatus(s_video, &st);
        size_t ysz = (size_t)s_pitch * (size_t)s_buf_h;
        LOGI("present: flip rc=0x%08x idx=%d mode=%d pending=%d cur=%d wait=%d bgra=%d",
             (unsigned)flip_rc, next, s_flip_mode, st.numFlipPending, st.currentBuffer,
             shown_ok, s_use_bgra);
        LOGI("present: dst y0=%02x%02x uv0=%02x%02x pitch=%d hrep4=%d",
             dst[0], dst[1], dst[ysz], dst[ysz + 1], s_pitch, s_ycc_hrep4);
    } else if (flip_rc != 0) {
        LOGW("present: SubmitFlip failed 0x%08x idx=%d", (unsigned)flip_rc, next);
    } else if (!shown_ok && s_fb_count > 1 && s_flip_wait_logs < 8) {
        s_flip_wait_logs++;
        MlVideoOutFlipStatus st;
        memset(&st, 0, sizeof(st));
        (void)sceVideoOutGetFlipStatus(s_video, &st);
        LOGW("present: flip wait timeout want=%d cur=%d pending=%d",
             next, st.currentBuffer, st.numFlipPending);
    }

    video_stats_add(0, convert_us, t2 - t1, 0);
    (void)t1;
}

/* Shared flip-queue watchdog: tracks how long the queue has been at/over
 * threshold with currentBuffer==-1 (driver wedged). After 500 ms stuck it
 * logs (throttled to 5 s) and lets one probe frame through so vsync can
 * retire the queue instead of dropping forever (1 fps cliff). */
static int flip_queue_full(int count_pipe) {
    if (s_video < 0 || s_fb_count < 1)
        return 0;
    MlVideoOutFlipStatus st;
    memset(&st, 0, sizeof(st));
    if (sceVideoOutGetFlipStatus(s_video, &st) != 0)
        return 0;
    int pending = st.numFlipPending;
    if (st.numGpuFlipPending > pending)
        pending = st.numGpuFlipPending;
    int busy = pending + ((count_pipe && s_pipe_active) ? 1 : 0);
    int threshold = (s_fb_count > 1) ? (s_fb_count - 1) : 1;
    if (busy < threshold) {
        s_flip_stuck_since = 0;
        return 0;
    }
    uint64_t now = now_us();
    if (!s_flip_stuck_since)
        s_flip_stuck_since = now;
    if (st.currentBuffer == -1 && now - s_flip_stuck_since > 500000ull) {
        if (now - s_flip_stuck_logged > 5000000ull) {
            s_flip_stuck_logged = now;
            LOGW("present: flip queue stuck %llu ms pending=%d gpu=%d cur=%d "
                 "-> probe frame",
                 (unsigned long long)(now - s_flip_stuck_since) / 1000ull,
                 st.numFlipPending, st.numGpuFlipPending, st.currentBuffer);
        }
        /* Probe: restart the window so we present one frame, then go back
         * to dropping for another 500 ms if the driver is still wedged. */
        s_flip_stuck_since = now;
        return 0;
    }
    return 1;
}

int video_present_should_drop(void) {
    if (s_video < 0 || s_fb_count < 1)
        return 0;
    /* Active pipe occupies 1 FB in convert; counts as extra busy. */
    return flip_queue_full(1);
}

/* Real flip backlog only (does not count the in-flight pipe convert). The
 * decoder polls this before Decode, when the previous frame's convert is
 * legitimately still running; counting it (should_drop) would skip most
 * frames in steady state. */
int video_present_flip_backlogged(void) {
    return flip_queue_full(0);
}

int video_present_bgra_pipe_finish(void) {
    if (!s_pipe_active || s_pipe_fb_idx < 0)
        return 0;
    bgra_convert_wait();
    uint8_t *dst = (uint8_t *)s_fb_cpu[s_pipe_fb_idx];
    int next = s_pipe_fb_idx;
    uint64_t convert_us = s_bgra_job_us;
    s_pipe_active = 0;
    s_pipe_fb_idx = -1;
    present_submit_flip(next, dst, convert_us);
    return 0;
}

int video_present_bgra_pipe_kick(const uint8_t *y, const uint8_t *uv,
                                 int pitch_y, int pitch_uv, int w, int h) {
    if (s_video < 0 || !y || !s_use_bgra || s_fb_count < 1)
        return -1;
    if (s_pipe_active)
        (void)video_present_bgra_pipe_finish();
    if (video_present_should_drop()) {
        video_stats_add(0, 0, 0, 1);
        return 0;
    }

    int next = pick_free_fb(NULL);
    if (next < 0) {
        video_stats_add(0, 0, 0, 1);
        return 0;
    }

    uint8_t *dst = (uint8_t *)s_fb_cpu[next];
    const uint8_t *src_uv = uv ? uv : y + (size_t)pitch_y * (size_t)h;
    int src_uv_pitch = pitch_uv ? pitch_uv : pitch_y;

    /* Scale src (w x h, decoded) to fill the registered VideoOut buffer
     * (s_width x s_buf_h, the actual display resolution) instead of only
     * writing the top-left w x h corner. */
    bgra_convert_kick(dst, s_pitch, w, h, s_width, s_buf_h,
                      y, src_uv, pitch_y, src_uv_pitch, 0);
    s_pipe_fb_idx = next;
    s_pipe_active = 1;
    return 0;
}

int video_present_frame(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                        int pitch_y, int pitch_uv, int w, int h,
                        video_frame_format_t fmt) {
    if (s_video < 0 || !y || s_fb_count < 1)
        return -1;
    if (s_pipe_active)
        (void)video_present_bgra_pipe_finish();
    if (video_present_should_drop()) {
        video_stats_add(0, 0, 0, 1);
        return 0;
    }

    int shown = -1;
    int next = pick_free_fb(&shown);
    if (next < 0) {
        video_stats_add(0, 0, 0, 1);
        return 0;
    }

    uint8_t *dst = (uint8_t *)s_fb_cpu[next];
    uint64_t t0 = now_us();

    if (s_gray_left > 0 && !s_use_bgra) {
        fill_solid_u32(dst, s_pitch, s_buf_h, w, 0x80808080u);
        s_gray_left--;
        if (s_gray_left == 1)
            LOGI("present: gray 0x80808080 idx=%d shown=%d n=%d",
                 next, shown, s_fb_count);
    } else if (fmt == VIDEO_FRAME_NV12) {
        const uint8_t *src_uv = v ? v : y + (size_t)pitch_y * (size_t)h;
        int src_uv_pitch = pitch_uv ? pitch_uv : pitch_y;
        if (s_use_bgra) {
            /* Scale to fill the whole registered buffer (display resolution)
             * instead of clipping to min(h, s_buf_h): otherwise streaming at
             * e.g. 720p onto a 1080p output only fills the top-left corner. */
            nv12_to_bgra(dst, s_pitch, w, h, s_width, s_buf_h, y, src_uv, pitch_y, src_uv_pitch);
        } else {
            nv12_blit_copy(dst, s_pitch, s_buf_h, y, pitch_y, h, w, s_ycc_hrep4);
            (void)src_uv;
            (void)src_uv_pitch;
        }
    } else {
        if (s_use_bgra) {
            if (w == s_width && h == s_buf_h) {
                yuv_to_bgra(dst, s_pitch, w, h, y, u, v, pitch_y, pitch_uv, pitch_uv);
            } else {
                const int *lut = scale_lut_get(s_width, w);
                if (lut) {
                    yuv_to_bgra_scaled(dst, s_pitch, s_width, s_buf_h, y, u, v,
                                       pitch_y, pitch_uv, pitch_uv, w, h, lut);
                } else {
                    int rows = h < s_buf_h ? h : s_buf_h;
                    int cols = w < s_width ? w : s_width;
                    yuv_to_bgra(dst, s_pitch, cols, rows, y, u, v, pitch_y, pitch_uv, pitch_uv);
                }
            }
        } else {
            int rows = h < s_buf_h ? h : s_buf_h;
            pack_yuv420p_to_nv12(dst, s_pitch, w, rows, y, u, v, pitch_y, pitch_uv, pitch_uv);
        }
    }

    uint64_t convert_us = s_use_bgra && s_bgra_job_us ? s_bgra_job_us : (now_us() - t0);
    present_submit_flip(next, dst, convert_us);
    return 0;
}

int video_present_frame_nv12_copy(const void *src, size_t src_size,
                                  int pitch_y, int pitch_uv, int w, int h) {
    if (!src)
        return -1;
    size_t need = nv12_size(pitch_y, h);
    if (src_size < need)
        return -1;
    (void)pitch_uv;

    const uint8_t *y = (const uint8_t *)src;
    const uint8_t *uv = y + (size_t)pitch_y * (size_t)h;
    return video_present_frame(y, NULL, uv, pitch_y, pitch_y, w, h, VIDEO_FRAME_NV12);
}

/* ------------------------------------------------------------------------
 * UI: BGRA present for the menu.
 * ALWAYS draw to a CPU staging buffer then memcpy → free backbuffer + flip.
 * This way the on-screen buffer is never cleared (cause of flicker / black).
 * ---------------------------------------------------------------------- */

int video_ui_begin(int w, int h) {
    if (s_video >= 0 && s_use_bgra) {
        bgra_worker_stop();
        LOGI("video_ui: reuse present %dx%d (requested %dx%d)",
             s_width, s_buf_h, w, h);
        return 0;
    }
    if (s_video >= 0) {
        /* Soft-detach; if still open in BGRA, reuse. */
        video_present_shutdown();
        if (s_video >= 0 && s_use_bgra) {
            bgra_worker_stop();
            return 0;
        }
    }
    if (video_present_init(w, h, 0 /* BGRA */) != 0)
        return -1;
    if (!s_use_bgra) {
        LOGE("video_ui: did not end up in BGRA mode");
        return -1;
    }
    bgra_worker_stop();
    LOGI("video_ui: begin %dx%d n=%d pitch=%d", w, h, s_fb_count, s_pitch);
    return 0;
}

/* Wait for a backbuffer that is neither on-screen nor in flight. */
static int ui_wait_backbuffer(int timeout_ms) {
    for (int t = 0; t < timeout_ms; t++) {
        MlVideoOutFlipStatus st;
        memset(&st, 0, sizeof(st));
        if (sceVideoOutGetFlipStatus(s_video, &st) != 0) {
            sceKernelUsleep(1000);
            continue;
        }
        for (int i = 0; i < s_fb_count; i++) {
            int cand = (s_fb_index + 1 + i) % s_fb_count;
            if (st.currentBuffer >= 0 && cand == st.currentBuffer)
                continue;
            if (st.numFlipPending > 0 && cand == s_last_flip_idx)
                continue;
            if (st.numFlipPending >= 2 && cand == s_prev_flip_idx)
                continue;
            return cand;
        }
        sceKernelUsleep(1000);
    }
    return -1;
}

int video_ui_get_fb(uint8_t **bgra, int *pitch_bytes, int *idx) {
    if (s_video < 0 || !s_use_bgra || s_fb_count < 1)
        return -1;
    int next = ui_wait_backbuffer(32);
    if (next < 0)
        return -1;
    *bgra = (uint8_t *)s_fb_cpu[next];
    *pitch_bytes = s_pitch;
    *idx = next;
    return 0;
}

int video_ui_flip(int idx) {
    if (s_video < 0 || idx < 0 || idx >= s_fb_count)
        return -1;
    __asm__ volatile("sfence" ::: "memory");
    sceGnmFlushGarlic();
    /* Mode ASAP (2): VSYNC may block if pending does not drop (cur=-1 typical). */
    int32_t rc = sceVideoOutSubmitFlip(s_video, idx, 2u, 0);
    if (rc != 0)
        rc = sceVideoOutSubmitFlip(s_video, idx, (uint32_t)s_flip_mode, 0);
    if (rc == 0) {
        s_prev_flip_idx = s_last_flip_idx;
        s_fb_index = idx;
        s_last_flip_idx = idx;
    }
    return rc;
}

/*
 * Present a full BGRA frame from staging (pitch in bytes).
 * Atomic copy to backbuffer → flip. Preferred for the menu.
 */
int video_ui_present(const uint8_t *src, int src_pitch) {
    if (!src || s_video < 0 || !s_use_bgra || s_fb_count < 1)
        return -1;

    /* If the flip queue is full, do not block: drop this frame. */
    {
        MlVideoOutFlipStatus st;
        memset(&st, 0, sizeof(st));
        if (sceVideoOutGetFlipStatus(s_video, &st) == 0 &&
            st.numFlipPending >= s_fb_count - 1) {
            return -1;
        }
    }

    int next = ui_wait_backbuffer(16);
    if (next < 0)
        return -1;

    uint8_t *dst = (uint8_t *)s_fb_cpu[next];
    int rows = s_buf_h;
    size_t row_bytes = (size_t)s_width * 4u;
    if (src_pitch == s_pitch && (size_t)src_pitch == row_bytes) {
        memcpy(dst, src, row_bytes * (size_t)rows);
    } else {
        for (int r = 0; r < rows; r++) {
            memcpy(dst + (size_t)r * (size_t)s_pitch,
                   src + (size_t)r * (size_t)src_pitch, row_bytes);
        }
    }
    return video_ui_flip(next);
}

void video_ui_end(void) {
    /*
     * Do NOT close VideoOut here: sceVideoOutClose/Unregister with pending
     * flips (cur=-1) hangs the process → eternal black after LAUNCH.
     * The decoder reuses the same BGRA present in video_present_init.
     * If size/mode mismatch, present_init shuts down there.
     */
    LOGI("video_ui: end (VideoOut stays open for the stream)");
    bgra_worker_stop();
}
