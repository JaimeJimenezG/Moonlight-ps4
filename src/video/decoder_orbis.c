// Hardware H.264 decoder (libSceVideodec2) + YCbCr/NV12 presentation.
#include "video.h"
#include "nv12_blit.h"
#include "../log.h"
#include "../orbis/videodec2.h"
#include "../orbis/video_out_c.h"
#include "../gamestream/sps.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <emmintrin.h>
#include <smmintrin.h> /* MOVNTDQA — correct load from WC_GARLIC */

#define DMEM_ALIGN ML_DMEM_ALIGN
#define ERR_RESET_THRESHOLD 30
#define BOUNCE_WORKERS 4
/* Largest single H.264 AU we stage (1080p IDR at 20 Mbps fits in 4 MiB). */
#define AU_MAX_BYTES (4u * 1024u * 1024u)

static videodec2_api_t s_api;
static OrbisVideodec2Decoder s_dec;
static int s_width, s_height;
static int s_prefer_ycbcr = 1;

/* One framebuffer per frame Videodec2 may have in flight, carved out of a
 * single DirectMemory reservation and rotated per Decode. */
#define VD2_MAX_FB ORBIS_VIDEODEC2_MAX_PIPELINE_DEPTH

/*
 * AU (bitstream) buffers. Rotated like the framebuffers: with
 * decodePipelineDepth > 1 the Vdec worker may still be parsing AU(N) after
 * Decode() returned, so the next AU must not overwrite it. The Vdec CPU
 * worker reads this buffer during entropy parsing; on WC_GARLIC those are
 * uncached reads (~0.2-0.3 ms per KB measured on console: decode time grew
 * linearly with KB/frame), so ONION (cacheable, still GPU-visible) is the
 * default and WC_GARLIC is only kept as an INI fallback (dec_au_onion=false).
 */
static void *s_au_mem[VD2_MAX_FB];
static off_t s_au_off[VD2_MAX_FB];
static size_t s_au_cap[VD2_MAX_FB];
static int s_au_n = 1;
static int s_au_i;
static int s_au_type = ML_DMEM_TYPE_GARLIC;
static void *s_fb_mem;       /* base VA for Decode (WC or Map2 ONION) */
static void *s_fb_cpu;       /* cacheable CPU read alias (nullable) */
static off_t s_fb_off;
static size_t s_fb_size;     /* whole reservation */
static size_t s_fb_stride;   /* bytes per framebuffer */
static int s_fb_n = 1;       /* framebuffers in rotation */
static int s_fb_i;
static int s_fb_mem_type = ML_DMEM_TYPE_GARLIC;
static int s_fb_alias_ok;    /* 1 = bounce memcpy cacheable */

/* Tuning knobs from moonlight.ini, latched before dr_setup. */
static int s_tune_depth = 2;
static int s_tune_prio = ORBIS_VIDEODEC2_THREAD_PRIO_DEFAULT;
static int s_tune_au_onion = 1;
static int s_tune_fb_garlic;

static uint8_t *s_bounce[2]; /* ping-pong: convert reads N; bounce writes N+1 */
static size_t s_bounce_cap;
static int s_bounce_i;

static void *s_cpu_mem, *s_gpu_mem, *s_cpugpu_mem;
static off_t s_cpu_off, s_gpu_off, s_cpugpu_off;
static size_t s_cpu_sz, s_gpu_sz, s_cpugpu_sz;

static unsigned s_err_streak;
static unsigned s_submits;
static int s_logged_sample;
static int s_logged_au;
static uint64_t s_pts_base;
static uint64_t s_time_base_us;
static int s_have_pts_base;
static int s_pts_drift_logged;

static uint8_t s_sps_scratch[256];

typedef struct {
    void *dst;
    const void *src;
    size_t n;
} bounce_job_t;

/* Persistent pool for WC bounce (avoids pthread_create/join per frame). */
static pthread_t s_bounce_thr[BOUNCE_WORKERS];
static pthread_mutex_t s_bounce_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_bounce_cv = PTHREAD_COND_INITIALIZER;
static int s_bounce_alive;
static int s_bounce_job_seq;
static int s_bounce_done;
static bounce_job_t s_bounce_jobs[BOUNCE_WORKERS];
static int s_bounce_njobs;

static uint64_t now_us(void) {
    return (uint64_t)sceKernelGetProcessTime();
}

/* After GPU writes (WC): invalidate CPU lines of the WB alias before reading. */
static void cpu_cache_invalidate(void *p, size_t n) {
    if (!p || n == 0)
        return;
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63u;
    uintptr_t end = (uintptr_t)p + n;
    for (; a < end; a += 64u)
        _mm_clflush((void *)a);
    _mm_mfence();
}

/*
 * Generic memcpy over WC_GARLIC is ~50–70 ms (MOVDQU).
 * MOVNTDQA (SSE4.1) is the correct load for Write-Combining on Jaguar.
 * Normal store into bounce to leave Y/UV hot in convert's cache.
 */
__attribute__((target("sse4.1")))
static void wc_bounce_copy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n && (((uintptr_t)d | (uintptr_t)s) & 15u)) {
        *d++ = *s++;
        n--;
    }
    size_t n16 = n / 16u;
    __m128i *dd = (__m128i *)(void *)d;
    const __m128i *ss = (const __m128i *)(const void *)s;
    for (size_t i = 0; i < n16; i++) {
        __m128i v = _mm_stream_load_si128((__m128i *)(void *)(ss + i));
        _mm_store_si128(dd + i, v);
    }
    d += n16 * 16u;
    s += n16 * 16u;
    n -= n16 * 16u;
    while (n--)
        *d++ = *s++;
}

static void *bounce_worker_main(void *arg) {
    int id = (int)(uintptr_t)arg;
    int seen = 0;
    for (;;) {
        pthread_mutex_lock(&s_bounce_mtx);
        while (s_bounce_alive && s_bounce_job_seq == seen)
            pthread_cond_wait(&s_bounce_cv, &s_bounce_mtx);
        if (!s_bounce_alive) {
            pthread_mutex_unlock(&s_bounce_mtx);
            break;
        }
        seen = s_bounce_job_seq;
        bounce_job_t job = s_bounce_jobs[id];
        pthread_mutex_unlock(&s_bounce_mtx);

        if (job.n)
            wc_bounce_copy(job.dst, job.src, job.n);

        pthread_mutex_lock(&s_bounce_mtx);
        s_bounce_done++;
        if (s_bounce_done >= s_bounce_njobs)
            pthread_cond_broadcast(&s_bounce_cv);
        pthread_mutex_unlock(&s_bounce_mtx);
    }
    return NULL;
}

static void bounce_workers_start(void) {
    if (s_bounce_alive)
        return;
    s_bounce_alive = 1;
    s_bounce_job_seq = 0;
    s_bounce_done = 0;
    for (int i = 0; i < BOUNCE_WORKERS; i++) {
        if (pthread_create(&s_bounce_thr[i], NULL, bounce_worker_main,
                           (void *)(uintptr_t)i) != 0) {
            LOGW("orbis: bounce worker[%d] create failed", i);
            pthread_mutex_lock(&s_bounce_mtx);
            s_bounce_alive = 0;
            pthread_cond_broadcast(&s_bounce_cv);
            pthread_mutex_unlock(&s_bounce_mtx);
            for (int j = 0; j < i; j++)
                pthread_join(s_bounce_thr[j], NULL);
            return;
        }
    }
    LOGI("orbis: bounce WC pool %d threads", BOUNCE_WORKERS);
}

static void bounce_workers_stop(void) {
    if (!s_bounce_alive)
        return;
    pthread_mutex_lock(&s_bounce_mtx);
    s_bounce_alive = 0;
    pthread_cond_broadcast(&s_bounce_cv);
    pthread_mutex_unlock(&s_bounce_mtx);
    for (int i = 0; i < BOUNCE_WORKERS; i++)
        pthread_join(s_bounce_thr[i], NULL);
}

/* 4 chunks in parallel (persistent pool). ~2× scaling seen with 2 threads. */
static void wc_bounce_copy_mt(void *dst, const void *src, size_t n) {
    if (n < 256 * 1024 || !s_bounce_alive) {
        wc_bounce_copy(dst, src, n);
        return;
    }
    size_t chunk = (n / (size_t)BOUNCE_WORKERS) & ~(size_t)15u;
    if (chunk == 0) {
        wc_bounce_copy(dst, src, n);
        return;
    }
    pthread_mutex_lock(&s_bounce_mtx);
    s_bounce_njobs = BOUNCE_WORKERS;
    s_bounce_done = 0;
    size_t off = 0;
    for (int i = 0; i < BOUNCE_WORKERS; i++) {
        size_t len = (i == BOUNCE_WORKERS - 1) ? (n - off) : chunk;
        s_bounce_jobs[i].dst = (uint8_t *)dst + off;
        s_bounce_jobs[i].src = (const uint8_t *)src + off;
        s_bounce_jobs[i].n = len;
        off += len;
    }
    s_bounce_job_seq++;
    pthread_cond_broadcast(&s_bounce_cv);
    while (s_bounce_done < s_bounce_njobs)
        pthread_cond_wait(&s_bounce_cv, &s_bounce_mtx);
    pthread_mutex_unlock(&s_bounce_mtx);
}

static int alloc_dmem(size_t need, int32_t mem_type, void **ptr, off_t *off, size_t *cap) {
    need = (need + DMEM_ALIGN - 1) & ~(size_t)(DMEM_ALIGN - 1);
    if (need < DMEM_ALIGN)
        need = DMEM_ALIGN;
    if (*ptr && *cap >= need)
        return 0;
    if (*ptr) {
        /* AU size swings frame to frame with bitrate and IDRs. Overshoot on
         * regrow so it converges to the peak instead of re-mapping physical
         * memory every time a frame comes in bigger than the last. */
        need += need / 2u;
        need = (need + DMEM_ALIGN - 1) & ~(size_t)(DMEM_ALIGN - 1);
        /* Unmap VA before releasing physical pages; on this firmware the
         * VA mapping may survive Release and fragment VA space over a
         * long session. */
        if (*cap)
            sceKernelMunmap(*ptr, *cap);
        sceKernelReleaseDirectMemory(*off, *cap);
        *ptr = NULL;
        *off = 0;
        *cap = 0;
    }
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                           need, DMEM_ALIGN, mem_type, off);
    if (rc < 0)
        return rc;
    rc = sceKernelMapDirectMemory(ptr, need, ML_DMEM_PROT_RW, 0, *off, DMEM_ALIGN);
    if (rc < 0) {
        sceKernelReleaseDirectMemory(*off, need);
        *off = 0;
        return rc;
    }
    *cap = need;
    return 0;
}

/*
 * Decoder FB: first Allocate+Map2 ONION (like present BGRA) → bounce memcpy
 * fast. If Map2 fails: WC MapDirectMemory + bounce MOVNTDQA×4.
 * (Map2 on top of a WC MapDirectMemory fails with 0x80020010.)
 */
static int alloc_fb_decoder(size_t need, size_t align, int count) {
    if (align < DMEM_ALIGN)
        align = DMEM_ALIGN;
    if (count < 1)
        count = 1;
    if (count > VD2_MAX_FB)
        count = VD2_MAX_FB;
    need = (need + align - 1) & ~(align - 1);
    size_t total = need * (size_t)count;
    if (s_fb_mem) {
        if (s_fb_size)
            sceKernelMunmap(s_fb_mem, s_fb_size);
        sceKernelReleaseDirectMemory(s_fb_off, s_fb_size);
        s_fb_mem = NULL;
        s_fb_cpu = NULL;
        s_fb_off = 0;
        s_fb_size = 0;
        s_fb_alias_ok = 0;
    }
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                           total, align, ML_DMEM_TYPE_GARLIC, &s_fb_off);
    if (rc < 0)
        return rc;

    s_fb_size = total;
    s_fb_stride = need;
    s_fb_n = count;
    s_fb_i = 0;
    s_fb_mem = NULL;
    s_fb_cpu = NULL;
    s_fb_alias_ok = 0;
    s_fb_mem_type = ML_DMEM_TYPE_GARLIC;

    /* Path A: single Map2 ONION (same pattern as present). */
    if (!s_tune_fb_garlic) {
        struct { int type; const char *name; size_t al; } tries[] = {
            { ML_DMEM_TYPE_ONION,     "ONION",     align },
            { ML_DMEM_TYPE_ONION,     "ONION/16k", DMEM_ALIGN },
            { ML_DMEM_TYPE_WB_GARLIC, "WB_GARLIC", align },
            { ML_DMEM_TYPE_WB_GARLIC, "WB_GARLIC/16k", DMEM_ALIGN },
        };
        for (size_t t = 0; t < sizeof(tries) / sizeof(tries[0]); t++) {
            void *va = NULL;
            int m2 = sceKernelMapDirectMemory2(&va, total, tries[t].type,
                                               ML_DMEM_PROT_RW, 0, s_fb_off,
                                               tries[t].al);
            if (m2 == 0 && va) {
                s_fb_mem = va;
                s_fb_cpu = va;
                s_fb_alias_ok = 1;
                s_fb_mem_type = tries[t].type;
                LOGI("orbis: framebuf Map2 %s size=%zu x%d (Vdec+CPU mismo VA)",
                     tries[t].name, need, count);
                break;
            }
            LOGW("orbis: FB Map2 %s => 0x%08x", tries[t].name, (unsigned)m2);
        }
    }

    /* Path B: classic WC + bounce pool. */
    if (!s_fb_mem) {
        rc = sceKernelMapDirectMemory(&s_fb_mem, total, ML_DMEM_PROT_RW, 0,
                                      s_fb_off, align);
        if (rc < 0) {
            sceKernelReleaseDirectMemory(s_fb_off, total);
            s_fb_off = 0;
            s_fb_mem = NULL;
            s_fb_size = 0;
            return rc;
        }
        s_fb_mem_type = ML_DMEM_TYPE_GARLIC;
        LOGI("orbis: framebuf WC_GARLIC size=%zu x%d (Vdec OK; bounce WC mt)",
             need, count);
        bounce_workers_start();
    }

    if (s_bounce_cap < need) {
        free(s_bounce[0]);
        free(s_bounce[1]);
        s_bounce[0] = (uint8_t *)malloc(need);
        s_bounce[1] = (uint8_t *)malloc(need);
        s_bounce_cap = (s_bounce[0] && s_bounce[1]) ? need : 0;
        if (!s_bounce_cap) {
            free(s_bounce[0]);
            free(s_bounce[1]);
            s_bounce[0] = s_bounce[1] = NULL;
            LOGE("orbis: bounce malloc %zu failed", need);
            return -1;
        }
        s_bounce_i = 0;
        LOGI("orbis: bounce CPU x2 %zu (%s; pipe)", need,
             s_fb_alias_ok ? "Map2 cacheable memcpy" : "WC MOVNTDQA x4");
    }
    return 0;
}

static void free_all_dmem(void) {
    bounce_workers_stop();
    nv12_blit_shutdown();
    for (int i = 0; i < VD2_MAX_FB; i++) {
        if (s_au_mem[i]) {
            if (s_au_cap[i])
                sceKernelMunmap(s_au_mem[i], s_au_cap[i]);
            sceKernelReleaseDirectMemory(s_au_off[i], s_au_cap[i]);
            s_au_mem[i] = NULL;
            s_au_off[i] = 0;
            s_au_cap[i] = 0;
        }
    }
    s_au_i = 0;
    if (s_fb_mem) {
        if (s_fb_size)
            sceKernelMunmap(s_fb_mem, s_fb_size);
        sceKernelReleaseDirectMemory(s_fb_off, s_fb_size);
        s_fb_mem = NULL;
        s_fb_cpu = NULL;
        s_fb_off = 0;
        s_fb_size = 0;
        s_fb_alias_ok = 0;
    }
    free(s_bounce[0]);
    free(s_bounce[1]);
    s_bounce[0] = s_bounce[1] = NULL;
    s_bounce_cap = 0;
    s_bounce_i = 0;
    if (s_cpu_mem) {
        if (s_cpu_sz)
            sceKernelMunmap(s_cpu_mem, s_cpu_sz);
        sceKernelReleaseDirectMemory(s_cpu_off, s_cpu_sz);
        s_cpu_mem = NULL;
    }
    if (s_gpu_mem) {
        if (s_gpu_sz)
            sceKernelMunmap(s_gpu_mem, s_gpu_sz);
        sceKernelReleaseDirectMemory(s_gpu_off, s_gpu_sz);
        s_gpu_mem = NULL;
    }
    if (s_cpugpu_mem) {
        if (s_cpugpu_sz)
            sceKernelMunmap(s_cpugpu_mem, s_cpugpu_sz);
        sceKernelReleaseDirectMemory(s_cpugpu_off, s_cpugpu_sz);
        s_cpugpu_mem = NULL;
    }
    s_cpu_off = s_gpu_off = s_cpugpu_off = 0;
    s_cpu_sz = s_gpu_sz = s_cpugpu_sz = 0;
}

static int query_decoder_mem(int w, int h, OrbisVideodec2DecoderMemoryInfo *dm) {
    /* Must match the CreateDecoder config exactly: memory sizes depend on
     * decodePipelineDepth (querying with depth=1 and creating with 2 would
     * under-allocate the decoder working areas). */
    OrbisVideodec2DecoderConfigInfo dc;
    videodec2_fill_decoder_config_ex(&dc, s_api.queue, w, h,
                                     s_tune_depth, s_tune_prio);

    memset(dm, 0, sizeof(*dm));
    dm->thisSize = sizeof(*dm);
    int rc = s_api.QueryDecoderMemoryInfo(&dc, dm);
    LOGI("orbis: QueryDecoderMemoryInfo %dx%d resType=%u prof=%u lvl=%u => 0x%08x "
         "cpu=%llu gpu=%llu cpuGpu=%llu fb=%llu",
         w, h, dc.resourceType, dc.profile, dc.maxLevel, (unsigned)rc,
         (unsigned long long)dm->cpuMemorySize,
         (unsigned long long)dm->gpuMemorySize,
         (unsigned long long)dm->cpuGpuMemorySize,
         (unsigned long long)dm->maxFrameBufferSize);
    return rc < 0 ? -1 : 0;
}

static int alloc_decoder_memories(OrbisVideodec2DecoderMemoryInfo *dm) {
    /*
     * cpuMemory / cpuGpuMemory are the decoder's CPU working areas
     * (entropy parsing, DPB). On WC_GARLIC (CPU non-cacheable) Decode
     * took ~150ms/frame → overflow + IDR loop + corruption. ONION
     * (WB, CPU-cacheable + GPU-visible) is the correct type for these.
     * gpuMemory stays GARLIC (GPU work). checkMemoryType=false → skip validate.
     */
    if (dm->cpuMemorySize &&
        alloc_dmem((size_t)dm->cpuMemorySize, ML_DMEM_TYPE_ONION,
                   &s_cpu_mem, &s_cpu_off, &s_cpu_sz) < 0)
        return -1;
    dm->cpuMemory = s_cpu_mem;

    if (dm->gpuMemorySize &&
        alloc_dmem((size_t)dm->gpuMemorySize, ML_DMEM_TYPE_GARLIC,
                   &s_gpu_mem, &s_gpu_off, &s_gpu_sz) < 0)
        return -1;
    dm->gpuMemory = s_gpu_mem;

    if (dm->cpuGpuMemorySize &&
        alloc_dmem((size_t)dm->cpuGpuMemorySize, ML_DMEM_TYPE_ONION,
                   &s_cpugpu_mem, &s_cpugpu_off, &s_cpugpu_sz) < 0)
        return -1;
    dm->cpuGpuMemory = s_cpugpu_mem;

    LOGI("orbis: decoder mem cpu=ONION gpu=GARLIC cpuGpu=ONION "
         "(cpu=%llu gpu=%llu cpuGpu=%llu)",
         (unsigned long long)dm->cpuMemorySize,
         (unsigned long long)dm->gpuMemorySize,
         (unsigned long long)dm->cpuGpuMemorySize);

    if (dm->maxFrameBufferSize) {
        size_t align = dm->frameBufferAlignment ? dm->frameBufferAlignment : 0x100u;
        size_t need = (size_t)dm->maxFrameBufferSize;
        /* Two spares beyond the pipeline depth: one for the frame the async
         * BGRA convert is still reading, plus one of slack in case the
         * pipelined decoder returns its output with an extra call of delay
         * (the exact latency of depth>1 is not documented). */
        if (alloc_fb_decoder(need, align, s_tune_depth + 2) < 0)
            return -1;
        LOGI("orbis: framebuf type=%d stride=%zu n=%d total=%zu align=%zu",
             s_fb_mem_type, s_fb_stride, s_fb_n, s_fb_size, align);
        nv12_blit_bind_decoder_fb(s_fb_alias_ok && s_fb_cpu ? s_fb_cpu : s_fb_mem,
                                  s_fb_off, s_fb_size,
                                  s_fb_alias_ok ? ML_DMEM_TYPE_ONION : s_fb_mem_type);
    }

    s_au_type = s_tune_au_onion ? ML_DMEM_TYPE_ONION : ML_DMEM_TYPE_GARLIC;
    s_au_n = s_fb_n > 0 ? s_fb_n : 2;
    if (s_au_n > VD2_MAX_FB)
        s_au_n = VD2_MAX_FB;
    s_au_i = 0;
    for (int i = 0; i < s_au_n; i++) {
        if (alloc_dmem(1024 * 1024, s_au_type,
                       &s_au_mem[i], &s_au_off[i], &s_au_cap[i]) < 0) {
            LOGE("orbis: AU prealloc[%d] failed (type=%d)", i, s_au_type);
            return -1;
        }
    }
    LOGI("orbis: AU %s prealloc %zu x%d",
         s_tune_au_onion ? "ONION" : "Garlic", s_au_cap[0], s_au_n);
    return 0;
}

static void log_nal_types(const uint8_t *buf, int len) {
    char types[128];
    int tpos = 0;
    int count = 0;
    for (int i = 0; i + 4 < len && count < 8; ) {
        int sc = 0;
        if (i + 3 < len && buf[i] == 0 && buf[i + 1] == 0 &&
            buf[i + 2] == 0 && buf[i + 3] == 1)
            sc = 4;
        else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
            sc = 3;
        if (!sc || i + sc >= len) {
            i++;
            continue;
        }
        int nt = buf[i + sc] & 0x1f;
        tpos += snprintf(types + tpos, sizeof(types) - (size_t)tpos,
                         "%s%d", count ? "," : "", nt);
        count++;
        i += sc + 1;
    }
    LOGI("orbis: primer AU NAL types=[%s] size=%d head=%02x%02x%02x%02x%02x%02x",
         types, len,
         len > 0 ? buf[0] : 0, len > 1 ? buf[1] : 0, len > 2 ? buf[2] : 0,
         len > 3 ? buf[3] : 0, len > 4 ? buf[4] : 0, len > 5 ? buf[5] : 0);
}

static int dr_setup(int videoFormat, int width, int height, int redrawRate,
                    void *context, int drFlags) {
    (void)drFlags; (void)redrawRate;
    if (context)
        s_prefer_ycbcr = *(int *)context ? 1 : 0;

    if (!(videoFormat & VIDEO_FORMAT_MASK_H264)) {
        LOGE("orbis: only H.264 (format 0x%x)", videoFormat);
        return -1;
    }

    s_width = width;
    s_height = height;
    s_err_streak = 0;
    s_submits = 0;
    s_logged_sample = 0;
    s_logged_au = 0;
    s_pts_base = 0;
    s_time_base_us = 0;
    s_have_pts_base = 0;
    s_pts_drift_logged = 0;
    video_reset_stats();
    gs_sps_init(width, height);

    if (videodec2_load(&s_api) != 0)
        return -1;

    /* Present BEFORE CreateDecoder (validated stability) and BEFORE creating
     * the compute queue: if YCbCr hard-fails we left the queue alive and
     * suspending the app triggered CPU_FAULT_SUBMITDONE_TIMEOUT. */
    if (video_present_init(width, height, s_prefer_ycbcr) != 0) {
        videodec2_unload(&s_api);
        return -1;
    }

    if (videodec2_ensure_compute_queue(&s_api) < 0) {
        video_present_shutdown();
        videodec2_unload(&s_api);
        return -1;
    }

    OrbisVideodec2DecoderMemoryInfo dm;
    if (query_decoder_mem(width, height, &dm) != 0) {
        LOGE("orbis: QueryDecoderMemoryInfo failed");
        video_present_shutdown();
        videodec2_unload(&s_api);
        return -1;
    }
    if (alloc_decoder_memories(&dm) != 0) {
        LOGE("orbis: failed alloc decoder memories");
        free_all_dmem();
        video_present_shutdown();
        videodec2_unload(&s_api);
        return -1;
    }

    OrbisVideodec2DecoderConfigInfo dc;
    videodec2_fill_decoder_config_ex(&dc, s_api.queue, width, height,
                                     s_tune_depth, s_tune_prio);

    int rc = s_api.CreateDecoder(&dc, &dm, &s_dec);
    if (rc < 0) {
        LOGE("orbis: CreateDecoder 0x%08x", (unsigned)rc);
        free_all_dmem();
        video_present_shutdown();
        videodec2_unload(&s_api);
        return -1;
    }

    LOGI("orbis: Videodec2 H.264 listo %dx%d dpb=4 hmax=%d depth=%d prio=%d "
         "fb_n=%d au=%s blit=%s",
         width, height, (height + 15) & ~15, s_tune_depth, s_tune_prio, s_fb_n,
         s_tune_au_onion ? "ONION" : "Garlic", nv12_blit_mode_name());
    return DR_OK;
}

static void dr_cleanup(void) {
    if (video_present_is_bgra())
        (void)video_present_bgra_pipe_finish();
    if (s_dec && s_api.DeleteDecoder)
        s_api.DeleteDecoder(s_dec);
    s_dec = NULL;
    free_all_dmem();
    video_present_shutdown();
    videodec2_unload(&s_api);
    memset(&s_api, 0, sizeof(s_api));
}

static int dr_submit(PDECODE_UNIT du) {
    if (!s_dec)
        return DR_OK;

    int total = 0;
    for (PLENTRY e = du->bufferList; e; e = e->next)
        total += e->length;
    if (total <= 0)
        return DR_OK;

    /*
     * Always Decode (H.264 chain / refs). Only skip bounce+present if the
     * flip queue is truly backed up or PTS is late — otherwise: tearing and
     * "low bitrate". Do NOT use video_present_should_drop() here: it counts
     * the pipelined convert as busy, and with the async pipe that is the
     * steady state (it would spuriously skip frames).
     */
    int skip_present = 0;
    if (video_present_flip_backlogged())
        skip_present = 1;

    if (du->presentationTimeUs) {
        if (!s_have_pts_base) {
            s_pts_base = du->presentationTimeUs;
            s_time_base_us = now_us();
            s_have_pts_base = 1;
        } else {
            /*
             * Host PTS clock vs PS4 ProcessTime drift ~30-100 ppm.
             * Over 1800 s that is 54-180 ms > old 33 ms threshold, after
             * which EVERY frame set skip_present=1 forever (1 fps cliff,
             * restart fixes via dr_setup reset). Fix: gentle re-anchor on
             * small drift, drop only on large (real stall); plus a hard
             * rebase every 5 min so residual error cannot accumulate.
             */
            uint64_t now = now_us();
            int64_t expected = (int64_t)s_time_base_us +
                               (int64_t)(du->presentationTimeUs - s_pts_base);
            int64_t drift = (int64_t)now - expected;
            if (drift > 500000 || drift < -500000) {
                /* PTS discontinuity (host timestamp jump), not clock skew:
                 * re-anchor immediately instead of dropping until the
                 * periodic rebase. */
                s_pts_base = du->presentationTimeUs;
                s_time_base_us = now;
            } else if (drift > 80000) {
                /* Late only: frame arrived after its PTS + margin (real
                 * stall). Early frames (drift < 0, host ahead) still
                 * present — dropping them would cut fps for no gain. */
                skip_present = 1;
                /* Log once per session so the UDP log shows PTS vs
                 * flip-queue drops. */
                if (!s_pts_drift_logged) {
                    s_pts_drift_logged = 1;
                    LOGW("orbis: PTS drift %lld us (>80ms) -> skip_present; rebase in %llu ms",
                         (long long)drift,
                         (unsigned long long)(300000000ull -
                                              (now - s_time_base_us)) / 1000ull);
                }
            } else if (drift > 16000 || drift < -16000) {
                s_time_base_us =
                    (uint64_t)((int64_t)s_time_base_us + drift / 2);
            }
            if (now - s_time_base_us > 300000000ull) {
                LOGI("orbis: PTS rebase after %llu s (drift %lld us)",
                     (unsigned long long)(now - s_time_base_us) / 1000000ull,
                     (long long)drift);
                s_pts_base = du->presentationTimeUs;
                s_time_base_us = now;
            }
        }
    }

    /* Next AU slot: with depth > 1 the Vdec worker may still be parsing the
     * previous AU after Decode() returned, so never reuse it right away. */
    s_au_i = (s_au_i + 1) % (s_au_n > 0 ? s_au_n : 1);
    uint8_t *au = NULL;

    /* SPS fixup can slightly lengthen the NAL; keep slack. */
    size_t au_need = (size_t)total + 128;
    /*
     * Cap AU growth: without a cap a bitrate spike would Release+Allocate
     * ever-larger DirectMemory (fragmenting VA); oversize AUs ask for an
     * IDR instead.
     */
    if (au_need > AU_MAX_BYTES) {
        LOGW("orbis: AU %zu > cap %u; IDR", au_need, AU_MAX_BYTES);
        return DR_NEED_IDR;
    }
    if (alloc_dmem(au_need, s_au_type, &s_au_mem[s_au_i], &s_au_off[s_au_i],
                   &s_au_cap[s_au_i]) < 0) {
        LOGE("orbis: AU alloc %zu failed (type=%d)", au_need, s_au_type);
        return DR_NEED_IDR;
    }
    au = (uint8_t *)s_au_mem[s_au_i];

    uint64_t tau0 = now_us();
    int off = 0;
    for (PLENTRY e = du->bufferList; e; e = e->next) {
        if (e->bufferType == BUFFER_TYPE_SPS) {
            uint32_t sps_off = 0;
            gs_sps_fix(e, GS_SPS_BITSTREAM_FIXUP, s_sps_scratch, &sps_off);
            memcpy(au + off, s_sps_scratch, sps_off);
            off += (int)sps_off;
        } else {
            memcpy(au + off, e->data, e->length);
            off += e->length;
        }
    }
    total = off;
    video_stats_add_au(now_us() - tau0, (unsigned long long)total);

    if (!s_logged_au)
        log_nal_types(au, total);

    uint64_t t0 = now_us();

    OrbisVideodec2InputData in;
    memset(&in, 0, sizeof(in));
    in.thisSize = sizeof(in);
    in.auData = au;
    in.auSize = (uint64_t)total;
    in.ptsData = du->presentationTimeUs;
    in.dtsData = du->presentationTimeUs;

    OrbisVideodec2FrameBuffer fb;
    memset(&fb, 0, sizeof(fb));
    fb.thisSize = sizeof(fb);
    fb.frameBuffer = (uint8_t *)s_fb_mem + (size_t)s_fb_i * s_fb_stride;
    fb.frameBufferSize = s_fb_stride;
    s_fb_i = (s_fb_i + 1) % s_fb_n;

    OrbisVideodec2OutputInfo out;
    memset(&out, 0, sizeof(out));
    out.thisSize = sizeof(out);

    int rc = s_api.Decode(s_dec, &in, &fb, &out);
    uint64_t t1 = now_us();

    if (rc < 0) {
        if (!s_logged_au) {
            LOGE("orbis: Decode 0x%08x", (unsigned)rc);
            s_logged_au = 1;
        }
        return DR_NEED_IDR;
    }
    s_logged_au = 1;

    s_submits++;
    video_stats_add_decode(t1 - t0);

    if (!out.isValid || !out.frameBuffer) {
        if (out.isErrorFrame) {
            s_err_streak++;
            if (s_err_streak >= ERR_RESET_THRESHOLD) {
                LOGW("orbis: %u isErrorFrame seguidos; Reset+IDR", s_err_streak);
                s_api.Reset(s_dec);
                s_err_streak = 0;
                return DR_NEED_IDR;
            }
        }
        return DR_OK;
    }

    s_err_streak = 0;

    /*
     * Decode already ran (possible overlap with prior frame convert).
     * If no FB: still finish the pending one (free/present) and drop.
     */
    if (skip_present) {
        if (video_present_is_bgra())
            (void)video_present_bgra_pipe_finish();
        video_stats_add(0, 0, 0, 1);
        return DR_OK;
    }

    sceGnmFlushGarlic();

    const uint8_t *src = (const uint8_t *)out.frameBuffer;
    /* If cacheable alias exists, read from there (same phys as Vdec WC). */
    if (s_fb_alias_ok && s_fb_cpu && s_fb_mem && src) {
        ptrdiff_t off = (const uint8_t *)src - (const uint8_t *)s_fb_mem;
        if (off >= 0 && (size_t)off < s_fb_size)
            src = (const uint8_t *)s_fb_cpu + off;
        else
            src = (const uint8_t *)s_fb_cpu;
    }
    int pitch_y = (int)out.framePitchInBytes ? (int)out.framePitchInBytes : (int)out.framePitch;
    int pitch_uv = pitch_y;
    int w = (int)out.frameWidth ? (int)out.frameWidth : s_width;
    int h = (int)out.frameHeight ? (int)out.frameHeight : s_height;
    size_t nbytes = (size_t)pitch_y * (size_t)h * 3u / 2u;
    if (nbytes > s_fb_stride)
        nbytes = s_fb_stride;
    /*
     * Videodec2 pads the output to macroblock height (1080 → 1088). Those
     * extra rows are garbage, not picture: crop them for present instead of
     * letting the renderer "scale" 1088→1080. The mismatch used to force the
     * scalar LUT convert path on every frame (~8.3 ms vs ~1.4 ms SIMD 1:1)
     * and squashed 8 padding rows into the image. w/h keep the FB layout
     * (UV plane offset and cache invalidate need the padded height).
     */
    int disp_w = (s_width > 0 && w > s_width) ? s_width : w;
    int disp_h = (s_height > 0 && h > s_height) ? s_height : h;

    int use_pipe = video_present_is_bgra();
    uint8_t *dst_bounce = NULL;
    uint64_t tc0 = now_us();
    if (s_fb_alias_ok) {
        /*
         * FB already cacheable: invalidate + convert directly.
         * Avoids ~3 MiB/frame memcpy (~1.9 ms) and pipe contention.
         */
        cpu_cache_invalidate((void *)(uintptr_t)src, nbytes);
    } else if (s_bounce[0] && nbytes > 0 && nbytes <= s_bounce_cap) {
        dst_bounce = s_bounce[s_bounce_i];
        wc_bounce_copy_mt(dst_bounce, src, nbytes);
        src = dst_bounce;
    }
    uint64_t tc1 = now_us();

    if (!s_logged_sample && h > 0) {
        int mid = h / 2;
        int ymid_off = mid * pitch_y;
        int uv_off = pitch_y * h;
        LOGI("orbis: sample y0=%02x%02x%02x%02x ymid=%02x%02x uv0=%02x%02x%02x%02x",
             src[0], src[1], src[2], src[3],
             src[ymid_off], src[ymid_off + 1],
             src[uv_off], src[uv_off + 1], src[uv_off + 2], src[uv_off + 3]);
        LOGI("orbis: out fmt=0x%x %ux%u pitchB=%u bounce=%d alias=%d (once)",
             out.frameFormat, out.frameWidth, out.frameHeight, out.framePitchInBytes,
             dst_bounce ? 1 : 0, s_fb_alias_ok);
        s_logged_sample = 1;
    }

    if (use_pipe) {
        /* Join+flip of previous (its convert overlapped this Decode), then
         * kick this frame's convert async so the next Decode overlaps it.
         * With the cacheable alias the workers read the decoder FB directly:
         * safe because fb_n = depth+2, so the FB being converted is not
         * handed back to Decode until after the next pipe_finish. */
        (void)video_present_bgra_pipe_finish();
        const uint8_t *uv = src + (size_t)pitch_y * (size_t)h;
        (void)video_present_bgra_pipe_kick(src, uv, pitch_y, pitch_uv,
                                           disp_w, disp_h);
        if (dst_bounce)
            s_bounce_i ^= 1;
    } else {
        /* YCbCr blit path: keep the padded h (nv12 layout). */
        video_present_frame_nv12_copy(src, nbytes, pitch_y, pitch_uv, w, h);
    }
    if (tc1 > tc0)
        video_stats_add_bounce(tc1 - tc0);

    return DR_OK;
}

void video_orbis_set_tuning(int pipeline_depth, int thread_prio,
                            int au_onion, int fb_garlic) {
    if (pipeline_depth < 1)
        pipeline_depth = 1;
    if (pipeline_depth > ORBIS_VIDEODEC2_MAX_PIPELINE_DEPTH - 1)
        pipeline_depth = ORBIS_VIDEODEC2_MAX_PIPELINE_DEPTH - 1;
    s_tune_depth = pipeline_depth;
    s_tune_prio = thread_prio > 0 ? thread_prio : ORBIS_VIDEODEC2_THREAD_PRIO_DEFAULT;
    s_tune_au_onion = au_onion ? 1 : 0;
    s_tune_fb_garlic = fb_garlic ? 1 : 0;
    LOGI("orbis: tuning depth=%d prio=%d au=%s fb=%s",
         s_tune_depth, s_tune_prio, s_tune_au_onion ? "ONION" : "Garlic",
         s_tune_fb_garlic ? "WC_GARLIC" : "auto");
}

DECODER_RENDERER_CALLBACKS video_callbacks_orbis = {
    .setup = dr_setup,
    .cleanup = dr_cleanup,
    .submitDecodeUnit = dr_submit,
    .capabilities = CAPABILITY_SLICES_PER_FRAME(2),
};
