#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

typedef enum {
    RDP_LOG_ERROR   = 0,
    RDP_LOG_INFO    = 1,
    RDP_LOG_VERBOSE = 2,
    RDP_LOG_DEBUG   = 3,
} RDPLogLevel;

/* Set the active log level. Messages at or below this level are emitted.
   Default: RDP_LOG_INFO. */
void rdp_log_set_level(RDPLogLevel level);
RDPLogLevel rdp_log_get_level(void);

/* Parse a string ("error","info","verbose","debug") — returns -1 on failure. */
int rdp_log_level_from_string(const char *s);

/* Core log function — component is a short tag like "peer", "capture", "audio". */
void rdp_log(RDPLogLevel level, const char *component,
             const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* Convenience macros — component name is set per translation unit via
   RDP_LOG_COMPONENT before including this header, defaulting to "rdp". */
#ifndef RDP_LOG_COMPONENT
#define RDP_LOG_COMPONENT "rdp"
#endif

#define rdp_error(fmt, ...)   rdp_log(RDP_LOG_ERROR,   RDP_LOG_COMPONENT, \
                                       __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define rdp_info(fmt, ...)    rdp_log(RDP_LOG_INFO,    RDP_LOG_COMPONENT, \
                                       __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define rdp_verbose(fmt, ...) rdp_log(RDP_LOG_VERBOSE, RDP_LOG_COMPONENT, \
                                       __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define rdp_debug(fmt, ...)   rdp_log(RDP_LOG_DEBUG,   RDP_LOG_COMPONENT, \
                                       __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
