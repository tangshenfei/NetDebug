#include "core/log.h"
#include "core/thread.h"
#include "platform/platform.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static FILE *g_file = NULL;
static int g_level = 1;
static nd_mutex g_lock;
static int g_lock_ready = 0;

void nd_log_init(const char *path)
{
    if (!g_lock_ready) {
        nd_mutex_init(&g_lock);
        g_lock_ready = 1;
    }
    if (path && path[0]) {
        g_file = fopen(path, "a");
        if (!g_file) {
            char why[256];
            fprintf(stderr, "warning: cannot open log file %s (%s)\n", path,
                    nd_sys_error(errno, why, sizeof(why)));
        }
    }
}

void nd_log_close(void)
{
    if (g_file) {
        fflush(g_file);
        fclose(g_file);
        g_file = NULL;
    }
    if (g_lock_ready) {
        nd_mutex_destroy(&g_lock);
        g_lock_ready = 0;
    }
}

void nd_log_set_level(int level)
{
    g_level = level;
}

static const char *level_tag(int level)
{
    switch (level) {
    case ND_LOG_DEBUG: return "D";
    case ND_LOG_INFO:  return "I";
    case ND_LOG_WARN:  return "W";
    case ND_LOG_ERROR: return "E";
    default:           return "?";
    }
}

void nd_log_msg(int level, const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    char msg[2048];
    char stamp[64];
    time_t now;
    struct tm tm_now;
    int n;

    if (level < g_level)
        return;

    now = time(NULL);
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    (void)file;
    (void)line;

    if (g_lock_ready)
        nd_mutex_lock(&g_lock);

    n = fprintf(stderr, "%s.%03u [%s] %s", stamp, (unsigned)(nd_monotonic_ms() % 1000),
                level_tag(level), msg);
    (void)n;
    fputc('\n', stderr);

    if (g_file) {
        fprintf(g_file, "%s.%03u [%s] %s\n", stamp, (unsigned)(nd_monotonic_ms() % 1000),
                level_tag(level), msg);
        fflush(g_file);
    }

    if (g_lock_ready)
        nd_mutex_unlock(&g_lock);
}
