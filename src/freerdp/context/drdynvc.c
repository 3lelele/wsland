#define _DEFAULT_SOURCE

#include <unistd.h>

#include "wsland/freerdp.h"


static void rail_drdynvc_destroy(wsland_peer *peer) {
    DrdynvcServerContext *vc_ctx = peer->ctx_server_drdynvc;

    if (vc_ctx) {
        vc_ctx->Stop(vc_ctx);
        drdynvc_server_context_free(vc_ctx);
    }
}

bool ctx_drdynvc_init(wsland_peer *peer) { /* Open Dynamic virtual channel */
    peer->ctx_server_drdynvc = drdynvc_server_context_new(peer->vcm);
    if (!peer->ctx_server_drdynvc) {
        return false;
    }
    if (peer->ctx_server_drdynvc->Start(peer->ctx_server_drdynvc) != CHANNEL_RC_OK) {
        drdynvc_server_context_free(peer->ctx_server_drdynvc);
        return false;
    }

    /* Force Dynamic virtual channel to exchange caps */
    if (WTSVirtualChannelManagerGetDrdynvcState(peer->vcm) == DRDYNVC_STATE_NONE) {
        peer->peer->activated = TRUE;

        int waitRetry = 0; /* Wait reply to arrive from client */
        while (WTSVirtualChannelManagerGetDrdynvcState(peer->vcm) != DRDYNVC_STATE_READY) {
            if (++waitRetry > 10000) { /* timeout after 100 sec. */
                rail_drdynvc_destroy(peer);
                return FALSE;
            }
            usleep(10000); /* wait 0.01 sec. */
            peer->peer->CheckFileDescriptor(peer->peer);
            WTSVirtualChannelManagerCheckFileDescriptor(peer->vcm);
        }
    }

    return true;
}