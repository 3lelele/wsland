#include "wsland/freerdp.h"

static unsigned int disp_client_monitor_layout_change(DispServerContext *context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *display_control) {
    return CHANNEL_RC_OK;
}

bool ctx_disp_init(wsland_peer *peer) {
    DispServerContext *disp_ctx = disp_server_context_new(peer->vcm);
    if (!disp_ctx) {
        return false;
    }
    peer->ctx_server_disp = disp_ctx;

    disp_ctx->custom = peer;
    disp_ctx->MaxNumMonitors = RDP_MAX_MONITOR;
    disp_ctx->MaxMonitorAreaFactorA = DISPLAY_CONTROL_MAX_MONITOR_WIDTH;
    disp_ctx->MaxMonitorAreaFactorB = DISPLAY_CONTROL_MAX_MONITOR_HEIGHT;
    disp_ctx->DispMonitorLayout = disp_client_monitor_layout_change;
    if (disp_ctx->Open(disp_ctx) != CHANNEL_RC_OK) {
        return false;
    }

    if (disp_ctx->DisplayControlCaps(disp_ctx) != CHANNEL_RC_OK) {
        return false;
    }
    return true;
}