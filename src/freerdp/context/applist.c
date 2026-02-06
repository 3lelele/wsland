// ReSharper disable All
#define _DEFAULT_SOURCE

#include <unistd.h>

#include "wsland/freerdp.h"
#include "wsland/utils/log.h"


static UINT applist_client_caps(RdpAppListServerContext *context, const RDPAPPLIST_CLIENT_CAPS_PDU *arg) {
    return CHANNEL_RC_OK;
}

bool ctx_applist_init(wsland_peer *peer) {
    if (peer->freerdp->rail_shell_name) {
        RdpAppListServerContext *applist_ctx = rdpapplist_server_context_new(peer->vcm);
        if (!applist_ctx) {
            return false;
        }
        peer->ctx_server_applist = applist_ctx;

        applist_ctx->custom = peer;
        applist_ctx->ApplicationListClientCaps = applist_client_caps;
        if (applist_ctx->Open(applist_ctx) != CHANNEL_RC_OK) {
            return false;
        }

        RDPAPPLIST_SERVER_CAPS_PDU app_list_caps = {0};
        wsland_log(FREERDP, DEBUG, "Server AppList caps version:%d", RDPAPPLIST_CHANNEL_VERSION);
        app_list_caps.version = RDPAPPLIST_CHANNEL_VERSION;
        wsland_log(FREERDP, DEBUG, "    appListProviderName:%s", peer->freerdp->rail_shell_name);
        if (!utf8_string_to_rail_string(peer->freerdp->rail_shell_name, &app_list_caps.appListProviderName)) {
            return false;
        }

        char *s = getenv("WSLG_SERVICE_ID");
        if (!s) {
            s = peer->freerdp->rail_shell_name;
        }
        wsland_log(FREERDP, DEBUG, "    appListProviderUniqueId:%s", s);
        if (!utf8_string_to_rail_string(s, &app_list_caps.appListProviderUniqueId)) {
            return false;
        }
        if (applist_ctx->ApplicationListCaps(applist_ctx, &app_list_caps) != CHANNEL_RC_OK) {
            return false;
        }
        free(app_list_caps.appListProviderName.string);
    }

    /* wait graphics channel (and optionally graphics redir channel) reponse from client */
    int waitRetry = 0;
    while (!peer->activation_graphics_completed || (peer->ctx_server_gfxredir && !peer->activation_graphics_redirection_completed)) {
        if (++waitRetry > 10000) { /* timeout after 100 sec. */
            return false;
        }
        usleep(10000); /* wait 0.01 sec. */
        peer->peer->CheckFileDescriptor(peer->peer);
        WTSVirtualChannelManagerCheckFileDescriptor(peer->vcm);
    }

    return true;
}