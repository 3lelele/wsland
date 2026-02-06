#ifndef WSLAND_ADAPTER_H
#define WSLAND_ADAPTER_H

#include "wsland/server.h"
#include "wsland/freerdp.h"

typedef struct wsland_cursor_data {
    int width, height;
    int hotspot_x, hotspot_y;
    bool dirty;

    void *data; uint32_t *format; size_t *stride;
} wsland_cursor_data;

typedef struct wsland_window_data {
    struct wlr_scene *scene;
    struct wlr_swapchain *chain;
    struct wlr_scene_tree *scene_tree;

    uint32_t parent_id;
    uint32_t window_id;
    uint32_t surface_id;

    bool need_create;
    bool need_update;
    bool need_show;
    bool need_hide;
    bool need_damage;

    bool update_title;
    bool update_visible;
    bool update_position;

    char *title;
    int pos_x, pos_y;
    int scale_w, scale_h;
    struct wlr_box visible;
    pixman_region32_t damage;
} wsland_window_data;

void wsland_adapter_destroy_window(wsland_toplevel *toplevel);

void wsland_adapter_create_keyboard_for_peer(wsland_peer *peer, rdpSettings *settings);
void wsland_adapter_create_output_for_peer(wsland_peer *peer, rdpMonitor *monitor);


typedef struct wsland_adapter_handle {
    void (*wsland_cursor_frame)(struct wl_listener *listener, void *data);
    void (*wsland_output_frame)(struct wl_listener *listener, void *data);

    void (*server_destroy_wsland_toplevel)(struct wl_listener *listener, void *data);
} wsland_adapter_handle;

typedef struct wsland_peer_mouse_event {
    wsland_peer *peer;

} wsland_adapter_mouse_event;

typedef struct wsland_adapter {
    struct {
        struct wl_listener wsland_cursor_frame;
        struct wl_listener wsland_output_frame;

        struct wl_listener wsland_toplevel_destroy;
    } events;

    wsland_server *server;
    wsland_freerdp *freerdp;
    wsland_adapter_handle *handle;
} wsland_adapter;

struct wl_event_loop *wsland_adapter_fetch_event_loop(wsland_adapter *adapter);
wsland_adapter_handle *wsland_adapter_handle_init(wsland_adapter *adapter);

void wsland_adapter_destroy(wsland_adapter *adapter);
wsland_adapter *wsland_adapter_create(wsland_server *server);
#endif
