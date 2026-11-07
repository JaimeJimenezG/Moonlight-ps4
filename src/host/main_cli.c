// Linux development CLI for phase 1: exercises the full gamestream layer
// against a real Sunshine/GFE server without needing the console.
//
//   moonlight-cli pair <host>            generate a PIN and pair
//   moonlight-cli list <host>            list apps on the server
//   moonlight-cli stream <host> [opts]   launch and dump video to a .h264 file
//   moonlight-cli quit <host>            close the active session
//
// Stream options: --app <name>, --res WxH, --fps N, --bitrate kbps,
//                 --time seconds, --out file.h264

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  define _WINSOCK_DEPRECATED_NO_WARNINGS
#  include <winsock2.h>
#  include <windows.h>
#  define ml_strcasecmp _stricmp
#else
#  include <unistd.h>
#  define ml_strcasecmp strcasecmp
#endif
#include <time.h>

#include <Limelight.h>

#include "../log.h"
#include "../gamestream/client.h"
#include "../gamestream/gs_http.h"
#include "../gamestream/gs_errors.h"

static client_identity_t g_id;
static gs_server_t g_server;

// ---------------------------------------------------------------------------
// Dump callbacks: video to .h264 file, audio counted only.
// ---------------------------------------------------------------------------

static FILE *g_video_out;
static long long g_video_bytes;
static int g_video_frames;
static int g_idr_frames;

static int dump_dr_setup(int videoFormat, int width, int height, int redrawRate,
                         void *context, int drFlags) {
    (void)context; (void)drFlags;
    LOGI("decoder: format=0x%x %dx%d@%d", videoFormat, width, height, redrawRate);
    return 0;
}

static int dump_dr_submit(PDECODE_UNIT du) {
    for (PLENTRY entry = du->bufferList; entry; entry = entry->next) {
        if (g_video_out)
            fwrite(entry->data, 1, entry->length, g_video_out);
        g_video_bytes += entry->length;
    }
    g_video_frames++;
    if (du->frameType == FRAME_TYPE_IDR)
        g_idr_frames++;
    return DR_OK;
}

static long long g_audio_samples;

static int dump_ar_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                        void *context, int arFlags) {
    (void)context; (void)arFlags; (void)audioConfiguration;
    LOGI("audio: %d Hz, %d channels", opusConfig->sampleRate, opusConfig->channelCount);
    return 0;
}

static void dump_ar_sample(char *sampleData, int sampleLength) {
    (void)sampleData; (void)sampleLength;
    g_audio_samples++;
}

// ---------------------------------------------------------------------------
// Connection callbacks
// ---------------------------------------------------------------------------

static void cl_stage_starting(int stage) {
    LOGI("connection: starting stage %s", LiGetStageName(stage));
}

static void cl_stage_failed(int stage, int errorCode) {
    LOGE("connection: stage %s failed (error %d)", LiGetStageName(stage), errorCode);
}

static void cl_connection_terminated(int errorCode) {
    LOGE("connection: terminated (error %d)", errorCode);
}

static void cl_log(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    log_vprintf(format, ap);
    va_end(ap);
}

// ---------------------------------------------------------------------------

static const char *config_dir(void) {
    static char dir[512];
#if defined(_WIN32)
    const char *local_app_data = getenv("LOCALAPPDATA");
    if (!local_app_data || !local_app_data[0])
        local_app_data = getenv("APPDATA");
    if (!local_app_data || !local_app_data[0])
        local_app_data = getenv("USERPROFILE");
    snprintf(dir, sizeof(dir), "%s/moonlight-ps4", local_app_data ? local_app_data : ".");
#else
    const char *home = getenv("HOME");
    snprintf(dir, sizeof(dir), "%s/.config/moonlight-ps4", home ? home : ".");
#endif
    return dir;
}

static int cmd_init(const char *host) {
    if (identity_init(&g_id, config_dir()) != GS_OK) {
        LOGE("identity: %s", gs_error);
        return -1;
    }
    http_init(&g_id, getenv("MOONLIGHT_VERBOSE") != NULL);
    int ret = gs_init(&g_server, &g_id, host, 47989, config_dir());
    if (ret != GS_OK) {
        LOGE("gs_init failed (%d): %s", ret, gs_error);
        return -1;
    }
    LOGI("server %s: version %s, paired=%d, current game=%d",
         host, g_server.appVersion, g_server.paired, g_server.currentGame);
    return 0;
}

static int cmd_pair(const char *host) {
    if (cmd_init(host) != 0)
        return 1;
    if (g_server.paired) {
        LOGI("already paired with %s", host);
        return 0;
    }

    unsigned char rnd[2];
    rng_fill(&g_id, rnd, sizeof(rnd));
    char pin[5];
    snprintf(pin, sizeof(pin), "%04u", (rnd[0] << 8 | rnd[1]) % 10000u);

    printf("\n  Pairing PIN: %s\n"
           "  Enter it in the Sunshine UI (https://%s:47990/pin)\n\n",
           pin, host);

    if (gs_pair(&g_server, pin) != GS_OK) {
        LOGE("pairing failed: %s", gs_error);
        return 1;
    }
    LOGI("successfully paired with %s", host);
    return 0;
}

static int cmd_list(const char *host) {
    if (cmd_init(host) != 0)
        return 1;
    if (!g_server.paired) {
        LOGE("not paired; run first: moonlight-cli pair %s", host);
        return 1;
    }
    app_entry_t *list = NULL;
    if (gs_applist(&g_server, &list) != GS_OK) {
        LOGE("applist failed: %s", gs_error);
        return 1;
    }
    printf("Apps available on %s:\n", host);
    for (app_entry_t *a = list; a; a = a->next)
        printf("  %6d  %s\n", a->id, a->name);
    xml_applist_free(list);
    return 0;
}

static int cmd_quit(const char *host) {
    if (cmd_init(host) != 0)
        return 1;
    if (gs_quit_app(&g_server) != GS_OK) {
        LOGE("cancel failed: %s", gs_error);
        return 1;
    }
    LOGI("session closed");
    return 0;
}

static int cmd_stream(const char *host, int argc, char **argv) {
    const char *app_name = "Desktop";
    const char *out_path = "moonlight-dump.h264";
    int width = 1280, height = 720, fps = 60, bitrate = 10000, seconds = 20;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--app") && i + 1 < argc)
            app_name = argv[++i];
        else if (!strcmp(argv[i], "--res") && i + 1 < argc)
            sscanf(argv[++i], "%dx%d", &width, &height);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
            fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc)
            bitrate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--time") && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_path = argv[++i];
    }

    if (cmd_init(host) != 0)
        return 1;
    if (!g_server.paired) {
        LOGE("not paired; run first: moonlight-cli pair %s", host);
        return 1;
    }

    // Look up app by name (or use its id if numeric).
    int app_id = atoi(app_name);
    if (app_id == 0) {
        app_entry_t *list = NULL;
        if (gs_applist(&g_server, &list) != GS_OK) {
            LOGE("applist failed: %s", gs_error);
            return 1;
        }
        for (app_entry_t *a = list; a; a = a->next) {
            if (!ml_strcasecmp(a->name, app_name)) {
                app_id = a->id;
                break;
            }
        }
        xml_applist_free(list);
        if (app_id == 0) {
            LOGE("app '%s' not found on server", app_name);
            return 1;
        }
    }

    STREAM_CONFIGURATION config;
    LiInitializeStreamConfiguration(&config);
    config.width = width;
    config.height = height;
    config.fps = fps;
    config.bitrate = bitrate;
    config.packetSize = 1392;
    config.streamingRemotely = STREAM_CFG_LOCAL;
    config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    config.supportedVideoFormats = VIDEO_FORMAT_H264;

    LOGI("launching app %d at %dx%d@%d, %d kbps, %d s", app_id, width, height,
         fps, bitrate, seconds);

    if (gs_start_app(&g_server, &config, app_id, true, false, 0) != GS_OK) {
        LOGE("launch failed: %s", gs_error);
        return 1;
    }

    g_video_out = fopen(out_path, "wb");
    if (!g_video_out) {
        LOGE("could not open %s", out_path);
        return 1;
    }

    CONNECTION_LISTENER_CALLBACKS clCallbacks;
    LiInitializeConnectionCallbacks(&clCallbacks);
    clCallbacks.stageStarting = cl_stage_starting;
    clCallbacks.stageFailed = cl_stage_failed;
    clCallbacks.connectionTerminated = cl_connection_terminated;
    clCallbacks.logMessage = cl_log;

    DECODER_RENDERER_CALLBACKS drCallbacks;
    LiInitializeVideoCallbacks(&drCallbacks);
    drCallbacks.setup = dump_dr_setup;
    drCallbacks.submitDecodeUnit = dump_dr_submit;
    drCallbacks.capabilities = CAPABILITY_DIRECT_SUBMIT;

    AUDIO_RENDERER_CALLBACKS arCallbacks;
    LiInitializeAudioCallbacks(&arCallbacks);
    arCallbacks.init = dump_ar_init;
    arCallbacks.decodeAndPlaySample = dump_ar_sample;
    arCallbacks.capabilities = CAPABILITY_DIRECT_SUBMIT;

    int ret = LiStartConnection(&g_server.serverInfo, &config, &clCallbacks,
                                &drCallbacks, &arCallbacks, NULL, 0, NULL, 0);
    if (ret != 0) {
        LOGE("LiStartConnection failed: %d", ret);
        fclose(g_video_out);
        gs_quit_app(&g_server);
        return 1;
    }

    LOGI("streaming active; dumping video to %s", out_path);
    int last_frames = 0;
    for (int t = 0; t < seconds; t++) {
#if defined(_WIN32)
        Sleep(1000);
#else
        sleep(1);
#endif
        LOGI("  t=%2ds  frames=%d (+%d)  IDR=%d  video=%.1f MB  audio=%lld packets",
             t + 1, g_video_frames, g_video_frames - last_frames, g_idr_frames,
             g_video_bytes / 1048576.0, g_audio_samples);
        last_frames = g_video_frames;
    }

    LiStopConnection();
    fclose(g_video_out);
    g_video_out = NULL;

    LOGI("summary: %d frames in %d s (%.1f effective fps), %.1f MB",
         g_video_frames, seconds, (double)g_video_frames / seconds,
         g_video_bytes / 1048576.0);
    LOGI("check the video with: ffplay -f h264 %s", out_path);

    gs_quit_app(&g_server);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage: moonlight-cli <command> <host> [options]\n"
            "  pair <host>              generate a PIN and pair\n"
            "  list <host>              list apps\n"
            "  stream <host> [--app N] [--res WxH] [--fps N] [--bitrate kbps]\n"
            "                [--time s] [--out f.h264]\n"
            "  quit <host>              close the active session\n");
}

int main(int argc, char **argv) {
#if defined(_WIN32)
    {
        WSADATA wsa;
        // mbedTLS, moonlight-common-c and ENet need winsock initialised once
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "WSAStartup failed\n");
            return 2;
        }
    }
#endif
    log_init(NULL, 0);

    if (argc < 3) {
        usage();
        return 2;
    }

    const char *cmd = argv[1];
    const char *host = argv[2];

    if (!strcmp(cmd, "pair"))
        return cmd_pair(host);
    if (!strcmp(cmd, "list"))
        return cmd_list(host);
    if (!strcmp(cmd, "stream"))
        return cmd_stream(host, argc - 3, argv + 3);
    if (!strcmp(cmd, "quit"))
        return cmd_quit(host);

    usage();
    return 2;
}
