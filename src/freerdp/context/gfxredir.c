#include "wsland/freerdp.h"


static UINT gfxredir_client_graphics_redirection_legacy_caps(GfxRedirServerContext *context, const GFXREDIR_LEGACY_CAPS_PDU *redirectionCaps) {
    return CHANNEL_RC_OK;
}

static UINT gfxredir_client_graphics_redirection_caps_advertise(GfxRedirServerContext *context, const GFXREDIR_CAPS_ADVERTISE_PDU *redirectionCaps) {
    return CHANNEL_RC_OK;
}

static UINT gfxredir_client_present_buffer_ack(GfxRedirServerContext *context, const GFXREDIR_PRESENT_BUFFER_ACK_PDU *presentAck) {
    return CHANNEL_RC_OK;
}

bool ctx_gfxredir_init(wsland_peer *peer) {
    GfxRedirServerContext *redir_ctx = gfxredir_server_context_new(peer->vcm);
    if (!redir_ctx) {
        return false;
    }
    peer->ctx_server_gfxredir = redir_ctx;
    redir_ctx->custom = peer;
    redir_ctx->GraphicsRedirectionLegacyCaps = gfxredir_client_graphics_redirection_legacy_caps;
    redir_ctx->GraphicsRedirectionCapsAdvertise = gfxredir_client_graphics_redirection_caps_advertise;
    redir_ctx->PresentBufferAck = gfxredir_client_present_buffer_ack;
    if (redir_ctx->Open(redir_ctx) != CHANNEL_RC_OK) {
        return false;
    }

    return true;
}