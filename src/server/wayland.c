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


static wsland_output *wsland_output_fetch(struct wsland_window *window) {
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

static void reset_cursor_mode(wsland_server *server) {
    if (server->cache_cursor.restore) {
        server->cache_cursor.restore = false;

        if (server->cache_cursor.surface) {
            wlr_cursor_set_surface(
                server->cursor,
                server->cache_cursor.surface,
                server->cache_cursor.s_hotspot_x,
                server->cache_cursor.s_hotspot_y
            );
        } else {
            wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
        }
    }
    server->move.mode = WSLAND_CURSOR_PASSTHROUGH;
    server->grab.window = NULL;
}

static void output_frame(struct wl_listener *listener, void *data) {
    wsland_output *output = wl_container_of(listener, output, events.frame);

    pixman_region32_init(&output->pending_commit_damage);
    pixman_region32_copy(&output->pending_commit_damage, &output->scene_output->pending_commit_damage);
    pixman_region32_translate(&output->pending_commit_damage, output->scene_output->x, output->scene_output->y);
    if (wlr_scene_output_commit(output->scene_output, NULL)) {
        wl_signal_emit(&output->server->events.wsland_window_frame, output);
        pixman_region32_fini(&output->pending_commit_damage);
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    wsland_output *output = wl_container_of(listener, output, events.destroy);

    wlr_scene_output_destroy(output->scene_output);
    wl_list_remove(&output->events.destroy.link);
    wl_list_remove(&output->events.frame.link);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.new_output);
    wsland_output *output = data;
    output->server = server;

    wlr_output_init_render(&output->output, server->allocator, server->renderer);

    output->events.frame.notify = output_frame;
    wl_signal_add(&output->output.events.frame, &output->events.frame);
    output->events.destroy.notify = output_destroy;
    wl_signal_add(&output->output.events.destroy, &output->events.destroy);
    wl_list_insert(&server->outputs, &output->server_link);

    output->scene_output = wlr_scene_output_create(server->scene, &output->output);
    struct wlr_output_layout_output *layout_output = wlr_output_layout_add_auto(server->output_layout, &output->output);
    wlr_scene_output_layout_add_output(server->scene_layout, layout_output, output->scene_output);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_commit_state(&output->output, &state);
    wlr_output_state_finish(&state);

    if (server->config->command) {
        if (fork() == 0) {
            execl("/bin/sh", "/bin/sh", "-c", server->config->command, (void*)NULL);
        }
    }
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    wsland_keyboard *keyboard = wl_container_of(listener, keyboard, events.modifiers);

    wlr_seat_set_keyboard(keyboard->server->seat, &keyboard->keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->keyboard.modifiers);
}

static void server_window_focus(wsland_window *window) {
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
        server_window_focus(temp);
    }
}

static void server_window_activate(wsland_window *window, bool enabled) {
    wlr_xdg_toplevel_set_activated(window->wayland, enabled);
}

static wsland_window *server_window_fetch_parent(wsland_window *window) {
    if (window->wayland->parent) {
        struct wlr_scene_tree *parent_tree = window->wayland->parent->base->data;
        if (parent_tree) {
            return parent_tree->node.data;
        }
    }
    return NULL;
}

static char* server_window_fetch_title(wsland_window *window) {
    return window->wayland->title;
}

static bool alt_keybinding(wsland_server *server, xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_F1:
        {
            if (wl_list_length(&server->windows) < 2) {
                break;
            }
            wsland_window *next_window = wl_container_of(server->windows.prev, next_window, server_link);
            server_window_focus(next_window);
        }
        break;
    default:
        return false;
    }
    return true;
}

static bool alt_shift_keybinding(wsland_server *server, xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Q:
        {
            struct wlr_surface *ws = server->seat->keyboard_state.focused_surface;
            if (ws) {
                struct wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_try_from_wlr_surface(ws);
                if (toplevel) {
                    wlr_xdg_toplevel_send_close(toplevel);
                }
            }
        }
        break;
    default:
        return false;
    }
    return true;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    wsland_keyboard *keyboard = wl_container_of(listener, keyboard, events.key);
    wsland_server *server = keyboard->server;

    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server->seat;

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->keyboard.xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(&keyboard->keyboard);
    if (modifiers & WLR_MODIFIER_ALT && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled = alt_keybinding(server, syms[i]);
        }
    }
    if (modifiers & WLR_MODIFIER_ALT && modifiers & WLR_MODIFIER_SHIFT && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled = alt_shift_keybinding(server, syms[i]);
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(seat, &keyboard->keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
    wsland_keyboard *keyboard = wl_container_of(listener, keyboard, events.destroy);
    wl_list_remove(&keyboard->events.modifiers.link);
    wl_list_remove(&keyboard->events.destroy.link);
    wl_list_remove(&keyboard->events.key.link);
    wl_list_remove(&keyboard->server_link);
}

static void server_new_keyboard(wsland_server *server, struct wlr_input_device *device) {
    wsland_keyboard *keyboard = device->data;
    if (!keyboard) {
        return;
    }
    keyboard->server = server;

    keyboard->events.key.notify = keyboard_handle_key;
    wl_signal_add(&keyboard->keyboard.events.key, &keyboard->events.key);
    keyboard->events.modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&keyboard->keyboard.events.modifiers, &keyboard->events.modifiers);
    keyboard->events.destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->events.destroy);
    wl_list_insert(&server->keyboards, &keyboard->server_link);

    wlr_seat_set_keyboard(server->seat, &keyboard->keyboard);
}

static void server_new_pointer(wsland_server *server, struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        server_new_pointer(server, device);
        break;
    default:
        break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.map);
    wl_list_insert(&window->server->windows, &window->server_link);

    server_window_focus(window);

    {
        wsland_output *output = wsland_output_fetch(window);
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
        reset_cursor_mode(window->server);
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

static void begin_interactive(wsland_window *window, enum wsland_cursor_mode mode, uint32_t edges) {
    wsland_server *server = window->server;
    server->grab.window = window;
    server->move.mode = mode;

    if (mode == WSLAND_CURSOR_MOVE) {
        server->grab.x = server->cursor->x - window->tree->node.x;
        server->grab.y = server->cursor->y - window->tree->node.y;
    }
    else {
        struct wlr_box *geo_box = &window->wayland->base->current.geometry;

        double border_x = window->tree->node.x + geo_box->x + (edges & WLR_EDGE_RIGHT ? geo_box->width : 0);
        double border_y = window->tree->node.y + geo_box->y + (edges & WLR_EDGE_BOTTOM ? geo_box->height : 0);
        server->grab.x = server->cursor->x - border_x;
        server->grab.y = server->cursor->y - border_y;

        server->grab.geobox = *geo_box;
        server->grab.geobox.x += window->tree->node.x;
        server->grab.geobox.y += window->tree->node.y;
        server->move.resize_edges = edges;
    }
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_move);
    struct wlr_xdg_toplevel_move_event *event = data;

    begin_interactive(window, WSLAND_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;

    begin_interactive(window, WSLAND_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_maximize);

    if (window->wayland->base->surface->mapped) {
        wsland_output *output = wsland_output_fetch(window);

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
        wsland_output *output = wsland_output_fetch(window);

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

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.new_xdg_toplevel);

    wsland_window *window = calloc(1, sizeof(*window));
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

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.new_xdg_popup);

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

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    // todo
}

static wsland_window* desktop_toplevel_at(
    wsland_server *server, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy
) {
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }

    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return NULL;
    }

    *surface = scene_surface->surface;
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }

    if (tree) {
        wsland_window *window = tree->node.data;
        if (window->type == WAYLAND) {
            return window;
        }
    }
    return NULL;
}

static void process_cursor_move(wsland_server *server) {
    wsland_window *window = server->grab.window;

    if (window->wayland->current.maximized || window->wayland->current.fullscreen) {
        return;
    }

    wlr_scene_node_set_position(
        &window->tree->node,
        server->cursor->x - server->grab.x,
        server->cursor->y - server->grab.y
    );

    wl_signal_emit(&window->server->events.wsland_window_motion, window);
}

static void process_cursor_resize(wsland_server *server) {
    wsland_window *window = server->grab.window;

    if (window->wayland->current.maximized || window->wayland->current.fullscreen) {
        return;
    }

    double border_x = server->cursor->x - server->grab.x;
    double border_y = server->cursor->y - server->grab.y;
    int new_left = server->grab.geobox.x;
    int new_right = server->grab.geobox.x + server->grab.geobox.width;
    int new_top = server->grab.geobox.y;
    int new_bottom = server->grab.geobox.y + server->grab.geobox.height;

    if (server->move.resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    }
    else if (server->move.resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server->move.resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    }
    else if (server->move.resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    struct wlr_box *geo_box = &window->wayland->base->current.geometry;
    wlr_scene_node_set_position(&window->tree->node, new_left - geo_box->x, new_top - geo_box->y);

    int new_width = new_right - new_left;
    int new_height = new_bottom - new_top;
    {
        int min_w = 1 - geo_box->x;
        int min_h = 1 - geo_box->y;
        if (new_width < min_w) {
            new_width = min_w;
        }
        if (new_height < min_h) {
            new_height = min_h;
        }

        wsland_output *output = wsland_output_fetch(window);
        if (output) {
            if (new_width > output->monitor.width) {
                new_width = output->monitor.width;
            }
            if (new_height
                > output->monitor.height) {
                new_height = output->monitor.height;
                }
        }
    }
    wlr_xdg_toplevel_set_size(window->wayland, new_width, new_height);
}

static void process_cursor_motion(wsland_server *server, uint32_t time) {
    if (server->move.mode == WSLAND_CURSOR_MOVE) {
        process_cursor_move(server);
        return;
    }
    if (server->move.mode == WSLAND_CURSOR_RESIZE) {
        process_cursor_resize(server);
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    wsland_window *window = desktop_toplevel_at(
        server, server->cursor->x, server->cursor->y, &surface, &sx, &sy
    );
    if (!window) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    }
    else {
        wlr_seat_pointer_clear_focus(seat);
    }
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;

    wlr_cursor_warp_closest(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.cursor_button);

    bool handled = false;
    struct wlr_pointer_button_event *event = data;
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        reset_cursor_mode(server);
    }
    else {
        double sx, sy;
        struct wlr_surface *surface = NULL;
        wsland_window *window = desktop_toplevel_at(
            server, server->cursor->x, server->cursor->y, &surface, &sx, &sy
        );
        server_window_focus(window);

        if (window && (event->button == BTN_LEFT || event->button == BTN_RIGHT)) {
            uint32_t modifiers = wlr_keyboard_get_modifiers(server->seat->keyboard_state.keyboard);
            if (modifiers & WLR_MODIFIER_ALT) {
                if (event->button == BTN_LEFT) {
                    server->cache_cursor.restore = true;
                    server->cache_cursor.s_hotspot_x = server->cache_cursor.hotspot_x;
                    server->cache_cursor.s_hotspot_y = server->cache_cursor.hotspot_y;
                    wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "move");
                    begin_interactive(window, WSLAND_CURSOR_MOVE, 0);
                    handled = true;
                } else if (event->button == BTN_RIGHT) {
                    enum wlr_edges edges = WLR_EDGE_NONE;
                    if (server->cursor->x < window->current.x + window->current.width / 2) {
                        edges |= WLR_EDGE_LEFT;
                    } else {
                        edges |= WLR_EDGE_RIGHT;
                    }
                    if (server->cursor->y < window->current.y + window->current.height / 2) {
                        edges |= WLR_EDGE_TOP;
                    } else {
                        edges |= WLR_EDGE_BOTTOM;
                    }

                    server->cache_cursor.restore = true;
                    server->cache_cursor.s_hotspot_x = server->cache_cursor.hotspot_x;
                    server->cache_cursor.s_hotspot_y = server->cache_cursor.hotspot_y;
                    wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, wlr_xcursor_get_resize_name(edges));
                    begin_interactive(window, WSLAND_CURSOR_RESIZE, edges);
                    handled = true;
                }
            }
        }
    }

    if (!handled) {
        wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.cursor_axis);
    struct wlr_pointer_axis_event *event = data;

    wlr_seat_pointer_notify_axis(
        server->seat, event->time_msec, event->orientation, event->delta,
        event->delta_discrete, event->source, event->relative_direction
    );
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.cursor_frame);

    wlr_seat_pointer_notify_frame(server->seat);
    wl_signal_emit(&server->events.wsland_cursor_frame, server);
}

static void wsland_cursor_destroy(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.wsland_cursor_destroy);

    server->cache_cursor.surface = NULL;
    wl_list_remove(&server->events.wsland_cursor_destroy.link);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;

    struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);

        if (event->surface) {
            if (server->cache_cursor.surface) {
                server->cache_cursor.surface = NULL;
                wl_list_remove(&server->events.wsland_cursor_destroy.link);
            }

            server->cache_cursor.surface = event->surface;
            server->cache_cursor.s_hotspot_x = event->hotspot_x;
            server->cache_cursor.s_hotspot_y = event->hotspot_y;
            server->events.wsland_cursor_destroy.notify = wsland_cursor_destroy;
            wl_signal_add(&event->surface->events.destroy, &server->events.wsland_cursor_destroy);
        }
    }
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.request_set_selection);
}

wsland_wayland_handle wsland_server_handle_impl = {
    .server_new_output = server_new_output,
    .server_new_input = server_new_input,
    .server_new_xdg_toplevel = server_new_xdg_toplevel,
    .server_new_xdg_popup = server_new_xdg_popup,
    .server_cursor_motion = server_cursor_motion,
    .server_cursor_motion_absolute = server_cursor_motion_absolute,
    .server_cursor_button = server_cursor_button,
    .server_cursor_axis = server_cursor_axis,
    .server_cursor_frame = server_cursor_frame,
    .seat_request_cursor = seat_request_cursor,
    .seat_request_set_selection = seat_request_set_selection,

    .server_window_focus = server_window_focus,
    .server_window_activate = server_window_activate,
    .server_window_fetch_parent = server_window_fetch_parent,
    .server_window_fetch_title = server_window_fetch_title,
};

wsland_wayland_handle* wsland_wayland_handle_init(wsland_server *server) {
    return &wsland_server_handle_impl;
}
