#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

static FILE           *g_log_fp    = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

int log_init(const char *path)
{
    g_log_fp = fopen(path, "a");
    if (!g_log_fp) return -1;
    return 0;
}

void log_close(void)
{
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
}

void log_msg(const char *level, const char *fmt, ...)
{
    if (!g_log_fp) return;

    pthread_mutex_lock(&g_log_mutex);

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_info);

    fprintf(g_log_fp, "[%s] [%-5s] [pid=%d] ", ts, level, (int)getpid());

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log_fp, fmt, ap);
    va_end(ap);

    fputc('\n', g_log_fp);
    fflush(g_log_fp);

    pthread_mutex_unlock(&g_log_mutex);
}
