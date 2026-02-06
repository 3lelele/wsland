#ifndef WSLAND_UTILS_LOG_H
#define WSLAND_UTILS_LOG_H

#include <freerdp/log.h>
#include <wlr/util/log.h>

typedef enum wsland_log_type {
    CONFIG, SERVER, ADAPTER, FREERDP
} wsland_log_type;

typedef enum wsland_log_level {
    DEBUG, INFO, ERROR
} wsland_log_level;

enum wlr_log_importance to_wlr(wsland_log_level level);
DWORD to_freerdp(wsland_log_level level);

#define wsland_log(type, level, fmt, ...) \
switch (type) { \
    case CONFIG: \
        _wlr_log(to_wlr(level), "[%s:%d] [config] " fmt, _WLR_FILENAME, __LINE__, ##__VA_ARGS__); \
        break; \
    case SERVER: \
        _wlr_log(to_wlr(level), "[%s:%d] [server] " fmt, _WLR_FILENAME, __LINE__, ##__VA_ARGS__); \
        break; \
    case ADAPTER: \
        _wlr_log(to_wlr(level), "[%s:%d] [adapter] " fmt, _WLR_FILENAME, __LINE__, ##__VA_ARGS__); \
        break; \
    case FREERDP: \
        WLog_Print(WLog_Get("freerdp"), to_freerdp(level), fmt, ##__VA_ARGS__); \
        break; \
}
#endif
