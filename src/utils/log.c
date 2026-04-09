#include <stdlib.h>
#include <string.h>

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

bool wsland_trace_runtime_enabled(void) {
    static int initialized = 0;
    static bool enabled = false;

    if (!initialized) {
        const char *value = getenv("WSLAND_TRACE_RUNTIME");
        if (value && (
            strcmp(value, "1") == 0 ||
            strcmp(value, "true") == 0 ||
            strcmp(value, "TRUE") == 0 ||
            strcmp(value, "yes") == 0 ||
            strcmp(value, "on") == 0
        )) {
            enabled = true;
        }
        initialized = 1;
    }

    return enabled;
}
