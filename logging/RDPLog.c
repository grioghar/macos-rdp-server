#include "logging/RDPLog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#include <os/log.h>
#include <stdatomic.h>

static _Atomic(RDPLogLevel) g_level = RDP_LOG_INFO;

static const char *level_label[] = {
    [RDP_LOG_ERROR]   = "ERROR",
    [RDP_LOG_INFO]    = "INFO ",
    [RDP_LOG_VERBOSE] = "VERBO",
    [RDP_LOG_DEBUG]   = "DEBUG",
};

static const int syslog_priority[] = {
    [RDP_LOG_ERROR]   = LOG_ERR,
    [RDP_LOG_INFO]    = LOG_INFO,
    [RDP_LOG_VERBOSE] = LOG_DEBUG,
    [RDP_LOG_DEBUG]   = LOG_DEBUG,
};

void rdp_log_set_level(RDPLogLevel level) {
    atomic_store(&g_level, level);
}

RDPLogLevel rdp_log_get_level(void) {
    return atomic_load(&g_level);
}

int rdp_log_level_from_string(const char *s) {
    if (!s) return -1;
    if (strcasecmp(s, "error")   == 0) return RDP_LOG_ERROR;
    if (strcasecmp(s, "info")    == 0) return RDP_LOG_INFO;
    if (strcasecmp(s, "verbose") == 0) return RDP_LOG_VERBOSE;
    if (strcasecmp(s, "debug")   == 0) return RDP_LOG_DEBUG;
    return -1;
}

void rdp_log(RDPLogLevel level, const char *component,
             const char *file, int line, const char *fmt, ...) {
    if (level > atomic_load(&g_level)) return;

    va_list ap;
    va_start(ap, fmt);

    /* Format the message body first. */
    char body[1024];
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    /* Timestamp: HH:MM:SS.mmm */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    char timebuf[16];
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d.%03d",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
             (int)(ts.tv_nsec / 1000000));

    /* Basename of __FILE__ only — no full path noise in logs. */
    const char *basename = strrchr(file, '/');
    basename = basename ? basename + 1 : file;

    /* Emit to stderr (TTY / redirected log file). */
    if (level == RDP_LOG_DEBUG) {
        fprintf(stderr, "[%s] [%s] [%s] %s:%d — %s\n",
                timebuf, level_label[level], component, basename, line, body);
    } else {
        fprintf(stderr, "[%s] [%s] [%s] %s\n",
                timebuf, level_label[level], component, body);
    }

    /* Also emit to syslog / os_log so Console.app and log(1) see it. */
    syslog(syslog_priority[level], "[%s] %s", component, body);
}
