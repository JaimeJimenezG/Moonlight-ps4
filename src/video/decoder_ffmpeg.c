// Software H.264 decoder (FFmpeg) + YCbCr/NV12 presentation.
//
// Flujo: submitDecodeUnit -> avcodec_send/receive -> NV12/YUV420P -> renderer.

#include "video.h"
#include "../log.h"

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __ORBIS__
#include "../orbis/video_out_c.h"
#endif

static AVCodecContext *s_ctx;
static AVPacket *s_pkt;
static AVFrame *s_frame;
static uint8_t *s_nal_buf;
static int s_nal_cap;
static int s_width, s_height;
static int s_prefer_ycbcr = 1;
static uint64_t s_pts_base;
static uint64_t s_time_base_us;
static int s_have_pts_base;

static uint64_t now_us(void) {
#ifdef __ORBIS__
    return (uint64_t)sceKernelGetProcessTime();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

static enum AVPixelFormat pick_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts) {
    (void)ctx;
    const enum AVPixelFormat *p;
    for (p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_NV12)
            return AV_PIX_FMT_NV12;
    }
    for (p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_YUV420P)
            return AV_PIX_FMT_YUV420P;
    }
    return fmts[0];
}

static int dr_setup(int videoFormat, int width, int height, int redrawRate,
                    void *context, int drFlags) {
    (void)drFlags; (void)redrawRate;
    if (context)
        s_prefer_ycbcr = *(int *)context ? 1 : 0;

    if (!(videoFormat & VIDEO_FORMAT_MASK_H264)) {
        LOGE("video: only H.264 supported (format 0x%x)", videoFormat);
        return -1;
    }

    s_width = width;
    s_height = height;
    s_pts_base = 0;
    s_time_base_us = 0;
    s_have_pts_base = 0;
    video_reset_stats();

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        LOGE("video: decoder H.264 no encontrado en FFmpeg");
        return -1;
    }
    s_ctx = avcodec_alloc_context3(codec);
    if (!s_ctx)
        return -1;

    s_ctx->width = width;
    s_ctx->height = height;
    s_ctx->thread_count = 4;
    s_ctx->thread_type = FF_THREAD_SLICE;
    s_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    s_ctx->get_format = pick_format;

    if (avcodec_open2(s_ctx, codec, NULL) < 0) {
        LOGE("video: avcodec_open2 failed");
        avcodec_free_context(&s_ctx);
        return -1;
    }

    s_pkt = av_packet_alloc();
    s_frame = av_frame_alloc();
    if (!s_pkt || !s_frame)
        return -1;

#ifdef __ORBIS__
    if (video_present_init(width, height, s_prefer_ycbcr) != 0)
        return -1;
#endif

    LOGI("video: FFmpeg H.264 listo %dx%d (slice threads=%d, pix=%s)",
         width, height, s_ctx->thread_count,
         av_get_pix_fmt_name(s_ctx->pix_fmt));
    return DR_OK;
}

static void dr_cleanup(void) {
#ifdef __ORBIS__
    video_present_shutdown();
#endif
    if (s_frame) { av_frame_free(&s_frame); s_frame = NULL; }
    if (s_pkt) { av_packet_free(&s_pkt); s_pkt = NULL; }
    if (s_ctx) { avcodec_free_context(&s_ctx); s_ctx = NULL; }
    free(s_nal_buf);
    s_nal_buf = NULL;
    s_nal_cap = 0;
}

static int ensure_nal_buf(int need) {
    if (need <= s_nal_cap)
        return 0;
    int cap = s_nal_cap ? s_nal_cap : 256 * 1024;
    while (cap < need)
        cap *= 2;
    uint8_t *p = realloc(s_nal_buf, (size_t)cap);
    if (!p)
        return -1;
    s_nal_buf = p;
    s_nal_cap = cap;
    return 0;
}

static int dr_submit(PDECODE_UNIT du) {
    if (!s_ctx)
        return DR_OK;

    int total = 0;
    for (PLENTRY e = du->bufferList; e; e = e->next)
        total += e->length;
    if (total <= 0)
        return DR_OK;

    /* Always decode; skip present only if late / flip busy.
     * Same PTS drift re-anchor as decoder_orbis: host vs local clock skew
     * (~30-100 ppm) would otherwise trip the 33 ms threshold permanently
     * after ~30 min (1 fps cliff). */
    int skip_present = 0;
    if (du->presentationTimeUs) {
        if (!s_have_pts_base) {
            s_pts_base = du->presentationTimeUs;
            s_time_base_us = now_us();
            s_have_pts_base = 1;
        } else {
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
                /* Late only; early frames still present. */
                skip_present = 1;
            } else if (drift > 16000 || drift < -16000) {
                s_time_base_us =
                    (uint64_t)((int64_t)s_time_base_us + drift / 2);
            }
            if (now - s_time_base_us > 300000000ull) {
                s_pts_base = du->presentationTimeUs;
                s_time_base_us = now;
            }
        }
    }

    if (ensure_nal_buf(total) != 0)
        return DR_NEED_IDR;

    int off = 0;
    for (PLENTRY e = du->bufferList; e; e = e->next) {
        memcpy(s_nal_buf + off, e->data, e->length);
        off += e->length;
    }

    uint64_t t0 = now_us();

    if (av_new_packet(s_pkt, total) < 0)
        return DR_NEED_IDR;
    memcpy(s_pkt->data, s_nal_buf, (size_t)total);
    s_pkt->pts = (int64_t)(du->presentationTimeUs / 1000);

    int ret = avcodec_send_packet(s_ctx, s_pkt);
    av_packet_unref(s_pkt);
    if (ret < 0)
        return DR_NEED_IDR;

    while (avcodec_receive_frame(s_ctx, s_frame) == 0) {
        uint64_t t1 = now_us();

#ifdef __ORBIS__
        video_frame_format_t fmt = VIDEO_FRAME_YUV420P;
        const uint8_t *u = s_frame->data[1];
        const uint8_t *v = s_frame->data[2];
        int pitch_uv = s_frame->linesize[1];

        if (s_frame->format == AV_PIX_FMT_NV12) {
            fmt = VIDEO_FRAME_NV12;
            u = NULL;
            v = s_frame->data[1];
            pitch_uv = s_frame->linesize[1];
        }

        video_stats_add_decode(t1 - t0);
        if (skip_present || video_present_should_drop()) {
            video_stats_add(0, 0, 0, 1);
        } else {
            video_present_frame(s_frame->data[0], u, v,
                                s_frame->linesize[0], pitch_uv,
                                s_width, s_height, fmt);
        }
#else
        (void)t1;
        video_stats_add(t1 - t0, 0, 0, 0);
#endif
        t0 = now_us();
    }

    return DR_OK;
}

DECODER_RENDERER_CALLBACKS video_callbacks_ffmpeg = {
    .setup = dr_setup,
    .cleanup = dr_cleanup,
    .submitDecodeUnit = dr_submit,
    .capabilities = CAPABILITY_SLICES_PER_FRAME(4),
};
