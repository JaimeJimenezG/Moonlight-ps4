#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <direct.h>
#else
#  include <strings.h>
#  include <sys/stat.h>
#endif
#include <errno.h>

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#endif

#if defined(_WIN32)
#  define ml_strcasecmp _stricmp
#else
#  define ml_strcasecmp strcasecmp
#endif

void config_set_defaults(app_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    LiInitializeStreamConfiguration(&cfg->stream);
    cfg->stream.width = 1920;
    cfg->stream.height = 1080;
    cfg->stream.fps = 60;
    cfg->stream.bitrate = 20000;
    /* Moonlight default. Smaller payloads mean more packets/s for the same
     * bitrate and a proportionally smaller SO_RCVBUF request upstream. */
    cfg->stream.packetSize = 1392;
    cfg->stream.streamingRemotely = STREAM_CFG_LOCAL;
    cfg->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    cfg->stream.supportedVideoFormats = VIDEO_FORMAT_H264;
    cfg->stream.encryptionFlags = ENCFLG_NONE;
    cfg->stream.colorSpace = COLORSPACE_REC_709;
    cfg->stream.colorRange = COLOR_RANGE_LIMITED;
    cfg->stream.clientRefreshRateX100 = 5994;
    cfg->sops = true;
    cfg->local_audio = false;
    cfg->prefer_hw = true;
    cfg->videodec2_spike = false;
    cfg->prefer_ycbcr = false;
    cfg->enable_file_log = false;
    cfg->show_stats = false;
    cfg->dec_pipeline_depth = 2;
    cfg->dec_thread_prio = 700;
    cfg->slices_per_frame = 2;
    cfg->dec_au_onion = true;
    cfg->dec_fb_garlic = false;
    cfg->bgra_workers = 4;
    cfg->bgra_nt = -1;
    snprintf(cfg->app_name, sizeof(cfg->app_name), "Steam Big Picture");
}

void config_ensure_dir(const char *dir) {
#ifdef __ORBIS__
    // Create basic parent dirs if missing (/data).
    sceKernelMkdir("/data", 0777);
    sceKernelMkdir(dir, 0777);
#else
#  if defined(_WIN32)
    _mkdir(dir);
#  else
    mkdir(dir, 0755);
#  endif
#endif
    (void)errno;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static int parse_bool(const char *v) {
    return !ml_strcasecmp(v, "true") || !strcmp(v, "1") || !ml_strcasecmp(v, "yes");
}

int config_load(app_config_t *cfg, const char *dir) {
    char path[512];
    char line[512];

    config_set_defaults(cfg);
    LOGI("config_load: dir=%s", dir);

    // Compat: host.txt / debug_host.txt from phases 0-2
    snprintf(path, sizeof(path), "%s/host.txt", dir);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(cfg->host, sizeof(cfg->host), f))
            cfg->host[strcspn(cfg->host, " \r\n")] = '\0';
        fclose(f);
        LOGI("config: host.txt => '%s'", cfg->host);
    }
    snprintf(path, sizeof(path), "%s/debug_host.txt", dir);
    f = fopen(path, "r");
    if (f) {
        if (fgets(cfg->debug_host, sizeof(cfg->debug_host), f))
            cfg->debug_host[strcspn(cfg->debug_host, " \r\n")] = '\0';
        fclose(f);
        LOGI("config: debug_host.txt => '%s'", cfg->debug_host);
    }

    snprintf(path, sizeof(path), "%s/moonlight.ini", dir);
    f = fopen(path, "r");
    if (!f) {
        LOGW("config: no hay %s", path);
        return cfg->host[0] ? 0 : -1;
    }
    LOGI("config: leyendo %s", path);

    int cfg_rev = 1; /* files without the key predate the rev-2 decode defaults */
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';' || *p == '[')
            continue;
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        if (!strcmp(key, "cfg_rev"))
            cfg_rev = atoi(val);
        else if (!strcmp(key, "host"))
            snprintf(cfg->host, sizeof(cfg->host), "%s", val);
        else if (!strcmp(key, "app"))
            snprintf(cfg->app_name, sizeof(cfg->app_name), "%s", val);
        else if (!strcmp(key, "debug_host"))
            snprintf(cfg->debug_host, sizeof(cfg->debug_host), "%s", val);
        else if (!strcmp(key, "width"))
            cfg->stream.width = atoi(val);
        else if (!strcmp(key, "height"))
            cfg->stream.height = atoi(val);
        else if (!strcmp(key, "fps"))
            cfg->stream.fps = atoi(val);
        else if (!strcmp(key, "bitrate"))
            cfg->stream.bitrate = atoi(val);
        else if (!strcmp(key, "packet_size"))
            cfg->stream.packetSize = atoi(val);
        else if (!strcmp(key, "sops"))
            cfg->sops = parse_bool(val);
        else if (!strcmp(key, "local_audio"))
            cfg->local_audio = parse_bool(val);
        else if (!strcmp(key, "prefer_hw"))
            cfg->prefer_hw = parse_bool(val);
        else if (!strcmp(key, "videodec2_spike"))
            cfg->videodec2_spike = parse_bool(val);
        else if (!strcmp(key, "prefer_ycbcr"))
            cfg->prefer_ycbcr = parse_bool(val);
        else if (!strcmp(key, "enable_file_log"))
            cfg->enable_file_log = parse_bool(val);
        else if (!strcmp(key, "show_stats"))
            cfg->show_stats = parse_bool(val);
        else if (!strcmp(key, "dec_pipeline_depth"))
            cfg->dec_pipeline_depth = atoi(val);
        else if (!strcmp(key, "dec_thread_prio"))
            cfg->dec_thread_prio = atoi(val);
        else if (!strcmp(key, "slices_per_frame"))
            cfg->slices_per_frame = atoi(val);
        else if (!strcmp(key, "dec_au_onion"))
            cfg->dec_au_onion = parse_bool(val);
        else if (!strcmp(key, "dec_fb_garlic"))
            cfg->dec_fb_garlic = parse_bool(val);
        else if (!strcmp(key, "bgra_workers"))
            cfg->bgra_workers = atoi(val);
        else if (!strcmp(key, "bgra_nt"))
            cfg->bgra_nt = atoi(val);
        else
            LOGW("config: clave desconocida '%s'", key);
    }
    fclose(f);

    /*
     * One-shot migration: rev < 2 files were written when the decode-tuning
     * defaults were depth=1 + AU in WC_GARLIC, and config_save pins every
     * key, so a plain default change would never reach existing installs.
     * Rev 2 defaults (validated as the fast path): pipelined Decode (depth=2,
     * overlaps CPU parse with HW decode; +1 frame of latency) and the AU
     * bitstream in cacheable ONION. Explicit user overrides survive from
     * rev >= 2 onward (the file carries cfg_rev = 2 after the next save).
     */
    if (cfg_rev < 2) {
        LOGW("config: rev %d < 2; aplicando nuevos defaults de decode "
             "(depth %d→2, au_onion %d→1)",
             cfg_rev, cfg->dec_pipeline_depth, cfg->dec_au_onion ? 1 : 0);
        cfg->dec_pipeline_depth = 2;
        cfg->dec_au_onion = true;
    }

    LOGI("config: host=%s app=%s debug=%s %dx%d@%d br=%d pkt=%d hw=%d ycbcr=%d file_log=%d",
         cfg->host, cfg->app_name, cfg->debug_host,
         cfg->stream.width, cfg->stream.height, cfg->stream.fps, cfg->stream.bitrate,
         cfg->stream.packetSize, cfg->prefer_hw, cfg->prefer_ycbcr,
         cfg->enable_file_log);
    return 0;
}

int config_save(const app_config_t *cfg, const char *dir) {
    char path[512];
    config_ensure_dir(dir);
    snprintf(path, sizeof(path), "%s/moonlight.ini", dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        LOGE("config: could not write %s", path);
        return -1;
    }
    fprintf(f,
            "# moonlight-ps4\n"
            "cfg_rev = 2\n"
            "host = %s\n"
            "app = %s\n"
            "debug_host = %s\n"
            "width = %d\n"
            "height = %d\n"
            "fps = %d\n"
            "bitrate = %d\n"
            "packet_size = %d\n"
            "sops = %s\n"
            "local_audio = %s\n"
            "prefer_hw = %s\n"
            "videodec2_spike = %s\n"
            "prefer_ycbcr = %s\n"
            "enable_file_log = %s\n"
            "show_stats = %s\n"
            "dec_pipeline_depth = %d\n"
            "dec_thread_prio = %d\n"
            "slices_per_frame = %d\n"
            "dec_au_onion = %s\n"
            "dec_fb_garlic = %s\n"
            "bgra_workers = %d\n"
            "bgra_nt = %d\n",
            cfg->host, cfg->app_name, cfg->debug_host,
            cfg->stream.width, cfg->stream.height, cfg->stream.fps, cfg->stream.bitrate,
            cfg->stream.packetSize,
            cfg->sops ? "true" : "false",
            cfg->local_audio ? "true" : "false",
            cfg->prefer_hw ? "true" : "false",
            cfg->videodec2_spike ? "true" : "false",
            cfg->prefer_ycbcr ? "true" : "false",
            cfg->enable_file_log ? "true" : "false",
            cfg->show_stats ? "true" : "false",
            cfg->dec_pipeline_depth, cfg->dec_thread_prio, cfg->slices_per_frame,
            cfg->dec_au_onion ? "true" : "false",
            cfg->dec_fb_garlic ? "true" : "false",
            cfg->bgra_workers, cfg->bgra_nt);
    fclose(f);
    return 0;
}
