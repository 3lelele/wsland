// ReSharper disable All
#include <wlr/xwayland/xwayland.h>
#include "wlr/types/wlr_output_layout.h"
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>

#include "wsland/server.h"
#include "wsland/adapter.h"
#include "wsland/utils/log.h"


static char* window_fetch_title(wsland_window *window) {
    return window->xwayland->title;
}

static wsland_window* window_fetch_parent(struct wsland_window *window) {
    if (window->xwayland->parent) {
        return window->xwayland->parent->data;
    }
    return NULL;
}

static wsland_output *window_fetch_output(struct wsland_window *window) {
    int pos_x = window->tree->node.x < 0 ? 0 : window->tree->node.x;
    int pos_y = window->tree->node.y < 0 ? 0 : window->tree->node.y;

    if (window->xwayland->parent) {
        wsland_window *parent = window->xwayland->parent->data;
        if (parent) {
            pos_x = parent->tree->node.x < 0 ? 0 : parent->tree->node.x;
            pos_y = parent->tree->node.y < 0 ? 0 : parent->tree->node.y;
        }
    }

    struct wlr_output *wo = wlr_output_layout_output_at(window->server->output_layout, pos_x, pos_y);
    if (wo) {
        wsland_output *output = wo->data;
        if (output) { return output; }
    }

    return NULL;
}

static void window_activate(struct wsland_window *window, bool enabled) {
    wlr_xwayland_surface_activate(window->xwayland, enabled);
}

static void server_xwayland_request_move(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_move);
}

static void server_xwayland_map(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.map);

    window->tree = wlr_scene_subsurface_tree_create(
        &window->server->scene->tree, window->xwayland->surface
    );

    {
        wsland_output *output = window_fetch_output(window);
        if (output) {
            struct wlr_box bounds = {0};
            wlr_surface_get_extends(window->xwayland->surface, &bounds);

            int pos_x = output->monitor.x + (output->monitor.width - bounds.width) / 2;
            int pos_y = output->monitor.y + (output->monitor.height - bounds.height) / 2;
            wlr_scene_node_set_position(&window->tree->node, pos_x, pos_y);
        }
    }

    wl_list_insert(&window->server->windows, &window->server_link);
}

static void server_xwayland_unmap(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.unmap);

    wl_signal_emit(&window->server->events.wsland_window_destroy, window);
    wlr_scene_node_destroy(&window->tree->node);
    wl_list_remove(&window->server_link);
}

static void server_xwayland_associate(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.associate);

    window->events.map.notify = server_xwayland_map;
    wl_signal_add(&window->xwayland->surface->events.map, &window->events.map);
    window->events.unmap.notify = server_xwayland_unmap;
    wl_signal_add(&window->xwayland->surface->events.unmap, &window->events.unmap);
}

static void server_xwayland_destroy(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.destroy);

    wl_signal_emit(&window->server->events.wsland_window_destroy, window);
    wl_list_remove(&window->events.request_move.link);
    wl_list_remove(&window->events.destroy.link);
    wl_list_remove(&window->events.unmap.link);
    wl_list_remove(&window->events.map.link);
    free(window);
}

static void wsland_xwayland_ready(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.xwayland_ready);
    wlr_xwayland_set_seat(server->xwayland, server->seat);
}

wsland_window_handle wsland_xwayland_window_impl = {
    .window_fetch_title = window_fetch_title,
    .window_fetch_parent = window_fetch_parent,
    .window_fetch_output = window_fetch_output,
    .window_activate = window_activate,
};

static void wsland_xwayland_new_surface(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.new_xwayland_toplevel);

    wsland_window *window = calloc(1, sizeof(*window));
    window->handle = &wsland_xwayland_window_impl;
    window->server = server;
    window->xwayland = data;
    window->type = XWAYLAND;

    window->xwayland->data = window;

    window->events.request_move.notify = server_xwayland_request_move;
    wl_signal_add(&window->xwayland->events.request_move, &window->events.request_move);
    window->events.associate.notify = server_xwayland_associate;
    wl_signal_add(&window->xwayland->events.associate, &window->events.associate);
    window->events.destroy.notify = server_xwayland_destroy;
    wl_signal_add(&window->xwayland->events.destroy, &window->events.destroy);
}

void xwayland_event_init(wsland_server *server) {
    server->events.xwayland_ready.notify = wsland_xwayland_ready;
    wl_signal_add(&server->xwayland->events.ready, &server->events.xwayland_ready);
    server->events.new_xwayland_toplevel.notify = wsland_xwayland_new_surface;
    wl_signal_add(&server->xwayland->events.new_surface, &server->events.new_xwayland_toplevel);
}