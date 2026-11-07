#pragma once

#include <stdarg.h>

// Logging: UDP (nc -u -l -p 9999) and optional file.
// LOGN/log_notify: Orbis toast — use ONLY for user-facing events
// (version, ycbcr, title, pause, close).

void log_init(const char *udp_host, unsigned short udp_port);
void log_init_file(const char *path);
void log_close_file(void);
void log_set_file_enabled(int enable, const char *path);
void log_shutdown(void);

#if defined(_MSC_VER)
#  define ML_FORMAT_PRINTF(fmt_idx, args_idx)
#elif defined(__clang__) || defined(__GNUC__)
#  define ML_FORMAT_PRINTF(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#  define ML_FORMAT_PRINTF(fmt_idx, args_idx)
#endif

void log_printf(const char *fmt, ...) ML_FORMAT_PRINTF(1, 2);
void log_vprintf(const char *fmt, va_list ap);

void log_notify(const char *fmt, ...) ML_FORMAT_PRINTF(1, 2);

#define LOGI(fmt, ...) log_printf("[I] " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) log_printf("[W] " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) log_printf("[E] " fmt "\n", ##__VA_ARGS__)
#define LOGN(fmt, ...) log_notify(fmt, ##__VA_ARGS__)
