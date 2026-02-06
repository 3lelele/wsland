#include "wsland/utils/log.h"

enum wlr_log_importance to_wlr(wsland_log_level level) {
    switch (level) {
    case DEBUG:
        return WLR_DEBUG;
    case INFO:
        return WLR_INFO;
    case ERROR:
        return WLR_ERROR;
    }
    return WLR_SILENT;
}

DWORD to_freerdp(wsland_log_level level) {
    switch (level) {
    case DEBUG:
        return WLOG_DEBUG;
    case INFO:
        return WLOG_INFO;
    case ERROR:
        return WLOG_ERROR;
    }
    return WLOG_OFF;
}