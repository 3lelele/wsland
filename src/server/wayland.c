// ReSharper disable All
#include <assert.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wlr/interfaces/wlr_output.h>

#include <wlr/util/edges.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xcursor_manager.h>

#include "wsland/server.h"
#include "wsland/adapter.h"
#include "wsland/utils/log.h"


static char* window_fetch_title(wsland_window *window) {
    return window->wayland->title;
}

static wsland_window *window_fetch_parent(wsland_window *window) {
    if (window->wayland->parent) {
        struct wlr_scene_tree *parent_tree = window->wayland->parent->base->data;
        if (parent_tree) {
            return parent_tree->node.data;
        }
    }
    return NULL;
}

static wsland_output *window_fetch_output(wsland_window *window) {
    int pos_x = window->tree->node.x < 0 ? 0 : window->tree->node.x;
    int pos_y = window->tree->node.y < 0 ? 0 : window->tree->node.y;

    if (window->wayland->parent) {
        struct wlr_scene_tree *parent_tree = window->wayland->parent->base->data;
        pos_x = parent_tree->node.x < 0 ? 0 : parent_tree->node.x;
        pos_y = parent_tree->node.y < 0 ? 0 : parent_tree->node.y;
    }

    struct wlr_output *wo = wlr_output_layout_output_at(window->server->output_layout, pos_x, pos_y);
    if (wo) {
        wsland_output *output = wo->data;
        if (output) { return output; }
    }

    return NULL;
}

static void window_focus(wsland_window *window) {
    if (!window) {
        return;
    }

    wsland_server *server = window->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface *surface = window->wayland->base->surface;
    if (prev_surface == surface) {
        return;
    }

    if (prev_surface) {
        struct wlr_xdg_toplevel *prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_toplevel != NULL) {
            wlr_xdg_toplevel_set_activated(prev_toplevel, false);
        }
    }

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    wlr_scene_node_raise_to_top(&window->tree->node);

    wl_list_remove(&window->server_link);
    wl_list_insert(&server->windows, &window->server_link);
    wlr_xdg_toplevel_set_activated(window->wayland, true);

    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(
            seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers
        );
    }

    wsland_window *temp;
    wl_list_for_each(temp, &window->children, parent_link) {
        window_focus(temp);
    }
}

static void window_activate(wsland_window *window, bool enabled) {
    wlr_xdg_toplevel_set_activated(window->wayland, enabled);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.map);
    wl_list_insert(&window->server->windows, &window->server_link);

    window_focus(window);

    {
        wsland_output *output = window_fetch_output(window);
        if (output) {
            struct wlr_box bounds = {0};
            wlr_surface_get_extends(window->wayland->base->surface, &bounds);

            int pos_x = output->monitor.x + (output->monitor.width - bounds.width) / 2;
            int pos_y = output->monitor.y + (output->monitor.height - bounds.height) / 2;
            wlr_scene_node_set_position(&window->tree->node, pos_x, pos_y);
        }
    }

    if (window->wayland->parent) {
        struct wlr_scene_tree *parent_tree = window->wayland->parent->base->data;
        if (parent_tree) {
            wsland_window *parent = parent_tree->node.data;
            if (parent) {
                wl_list_insert(&parent->children, &window->parent_link);
            }
        }
    }
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.unmap);

    if (window == window->server->grab.window) {
        window->server->handle->reset_cursor_mode(window->server);
    }

    wl_signal_emit(&window->server->events.wsland_window_destroy, window);
    wl_list_remove(&window->server_link);
    wl_list_remove(&window->parent_link);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.commit);

    if (window->wayland->base->initial_commit) {
        wlr_xdg_toplevel_set_size(window->wayland, 0, 0);
    }
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.destroy);

    wl_signal_emit(&window->server->events.wsland_window_destroy, window);
    wl_list_remove(&window->events.map.link);
    wl_list_remove(&window->events.unmap.link);
    wl_list_remove(&window->events.commit.link);
    wl_list_remove(&window->events.destroy.link);
    wl_list_remove(&window->events.request_move.link);
    wl_list_remove(&window->events.request_resize.link);
    wl_list_remove(&window->events.request_maximize.link);
    wl_list_remove(&window->events.request_fullscreen.link);
    wl_list_remove(&window->children);
    free(window);
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_move);
    struct wlr_xdg_toplevel_move_event *event = data;

    window->server->handle->begin_interactive(window, WSLAND_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;

    window->server->handle->begin_interactive(window, WSLAND_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_maximize);

    if (window->wayland->base->surface->mapped) {
        wsland_output *output = window_fetch_output(window);

        if (output) {
            if (window->wayland->current.maximized) {
                wlr_scene_node_set_position(&window->tree->node, window->before.x, window->before.y);
                wlr_xdg_toplevel_set_size(window->wayland, window->before.width, window->before.height);
            } else {
                window->before = (struct wlr_box){
                    window->tree->node.x,
                    window->tree->node.y,
                    window->wayland->base->current.geometry.width,
                    window->wayland->base->current.geometry.height,
                };

                wlr_scene_node_set_position(&window->tree->node, output->work_area.x, output->work_area.y);
                wlr_xdg_toplevel_set_size(window->wayland, output->work_area.width, output->work_area.height);
            }
            wlr_xdg_toplevel_set_maximized(window->wayland, !window->wayland->current.maximized);
        }
        wlr_xdg_surface_schedule_configure(window->wayland->base);
    }
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_fullscreen);

    if (window->wayland->base->surface->mapped) {
        wsland_output *output = window_fetch_output(window);

        if (output) {
            if (window->wayland->current.fullscreen) {
                wlr_scene_node_set_position(&window->tree->node, window->before.x, window->before.y);
                wlr_xdg_toplevel_set_size(window->wayland, window->before.width, window->before.height);
            } else {
                window->before = (struct wlr_box){
                    window->tree->node.x,
                    window->tree->node.y,
                    window->wayland->base->current.geometry.width,
                    window->wayland->base->current.geometry.height,
                };

                wlr_scene_node_set_position(&window->tree->node, output->monitor.x, output->monitor.y);
                wlr_xdg_toplevel_set_size(window->wayland, output->monitor.width, output->monitor.height);
            }
            wlr_xdg_toplevel_set_fullscreen(window->wayland, !window->wayland->current.fullscreen);
        }
        wlr_xdg_surface_schedule_configure(window->wayland->base);
    }
}

wsland_window_handle wsland_wayland_window_impl = {
    .window_fetch_title = window_fetch_title,
    .window_fetch_parent = window_fetch_parent,
    .window_fetch_output = window_fetch_output,
    .window_activate = window_activate,
    .window_focus = window_focus,
};

static void new_toplevel(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.new_wayland_toplevel);

    wsland_window *window = calloc(1, sizeof(*window));
    window->handle = &wsland_wayland_window_impl;
    window->server = server;
    window->wayland = data;
    window->type = WAYLAND;

    window->tree = wlr_scene_xdg_surface_create(
        &window->server->scene->tree, window->wayland->base
    );
    window->tree->node.data = window;
    window->wayland->base->data = window->tree;

    window->events.map.notify = xdg_toplevel_map;
    wl_signal_add(&window->wayland->base->surface->events.map, &window->events.map);
    window->events.unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&window->wayland->base->surface->events.unmap, &window->events.unmap);
    window->events.commit.notify = xdg_toplevel_commit;
    wl_signal_add(&window->wayland->base->surface->events.commit, &window->events.commit);

    window->events.destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&window->wayland->events.destroy, &window->events.destroy);

    window->events.request_move.notify = xdg_toplevel_request_move;
    wl_signal_add(&window->wayland->events.request_move, &window->events.request_move);
    window->events.request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&window->wayland->events.request_resize, &window->events.request_resize);
    window->events.request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&window->wayland->events.request_maximize, &window->events.request_maximize);
    window->events.request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&window->wayland->events.request_fullscreen, &window->events.request_fullscreen);

    wl_list_init(&window->server_link);
    wl_list_init(&window->parent_link);
    wl_list_init(&window->children);
}

static void xdg_popup_map(struct wl_listener *listener, void *data) {
    wsland_popup *popup = wl_container_of(listener, popup, events.map);

    struct wlr_box toplevel_space_box;
    wlr_surface_get_extends(popup->popup->parent, &toplevel_space_box);
    wlr_xdg_popup_unconstrain_from_box(popup->popup, &toplevel_space_box);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    wsland_popup *popup = wl_container_of(listener, popup, events.commit);

    if (popup->popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->popup->base);
    }
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    wsland_popup *popup = wl_container_of(listener, popup, events.destroy);

    wl_list_remove(&popup->events.map.link);
    wl_list_remove(&popup->events.commit.link);
    wl_list_remove(&popup->events.destroy.link);
    free(popup);
}

static void new_popup(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.new_wayland_popup);

    wsland_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        wsland_log(SERVER, ERROR, "failed to allocate wsland_popup");
        return;
    }

    popup->popup = data;

    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->popup->parent);
    assert(parent != NULL);
    struct wlr_scene_tree *parent_tree = parent->data;
    popup->popup->base->data = wlr_scene_xdg_surface_create(parent_tree, popup->popup->base);

    popup->events.map.notify = xdg_popup_map;
    wl_signal_add(&popup->popup->base->surface->events.map, &popup->events.map);

    popup->events.commit.notify = xdg_popup_commit;
    wl_signal_add(&popup->popup->base->surface->events.commit, &popup->events.commit);

    popup->events.destroy.notify = xdg_popup_destroy;
    wl_signal_add(&popup->popup->events.destroy, &popup->events.destroy);
}

void wayland_event_init(wsland_server *server) {
    server->events.new_wayland_toplevel.notify = new_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->events.new_wayland_toplevel);
    server->events.new_wayland_popup.notify = new_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->events.new_wayland_popup);
}
