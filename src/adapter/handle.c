// ReSharper disable All
#include <drm/drm_fourcc.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_scene.h>

#include "wsland/adapter.h"
#include "wsland/utils/log.h"

#define RAIL_WINDOW_FULLSCREEN_STYLE (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_GROUP | WS_TABSTOP)
#define RAIL_WINDOW_NORMAL_STYLE (RAIL_WINDOW_FULLSCREEN_STYLE | WS_THICKFRAME | WS_CAPTION)

static uint32_t wsland_window_id = 0;
static uint32_t wsland_surface_id = 0;

typedef struct wsland_frame_context {
    int frame_id;
    bool need_end_frame;

    wsland_peer *peer;
    wsland_output *output;
    wsland_toplevel *toplevel;
    struct wlr_render_pass *pass;
    struct wlr_buffer *buffer;
} wsland_frame_context;

typedef struct wsland_render_data {
    struct wlr_scene_node *node;
    int sx, sy;
} wsland_render_data;

static void wlr_output_transform_coords(enum wl_output_transform tr, int *x, int *y) {
    if (tr & WL_OUTPUT_TRANSFORM_90) {
        int tmp = *x;
        *x = *y;
        *y = tmp;
    }
}

static void scene_node_get_size(struct wlr_scene_node *node, int *width, int *height) {
    *width = 0;
    *height = 0;

    switch (node->type) {
    case WLR_SCENE_NODE_TREE:
        return;
    case WLR_SCENE_NODE_RECT: ;
        struct wlr_scene_rect *scene_rect = wlr_scene_rect_from_node(node);
        *width = scene_rect->width;
        *height = scene_rect->height;
        break;
    case WLR_SCENE_NODE_BUFFER: ;
        struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
        if (scene_buffer->dst_width > 0 && scene_buffer->dst_height > 0) {
            *width = scene_buffer->dst_width;
            *height = scene_buffer->dst_height;
        }
        else {
            *width = scene_buffer->buffer_width;
            *height = scene_buffer->buffer_height;
            wlr_output_transform_coords(scene_buffer->transform, width, height);
        }
        break;
    }
}

static void scene_node_bounds(struct wlr_scene_node *node, int x, int y, pixman_region32_t *visible, struct wl_array *nodes) {
    if (!node->enabled) {
        return;
    }

    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
        struct wlr_scene_node *child;
        wl_list_for_each(child, &scene_tree->children, link) {
            scene_node_bounds(child, x + child->x, y + child->y, visible, nodes);
        }
        return;
    }

    int width, height;
    scene_node_get_size(node, &width, &height);
    pixman_region32_union_rect(visible, visible, x, y, width, height);

    {

        wsland_render_data *slot = wl_array_add(nodes, sizeof(*slot));
        if (!slot) { return; }
        *slot = (wsland_render_data){ .node = node, .sx = x, .sy = y };
    }
}

static void scene_buffer_set_texture(struct wlr_scene_buffer *scene_buffer, struct wlr_texture *texture);

static void scene_buffer_handle_renderer_destroy(struct wl_listener *listener, void *data) {
    struct wlr_scene_buffer *scene_buffer = wl_container_of(listener, scene_buffer, renderer_destroy);
    scene_buffer_set_texture(scene_buffer, NULL);
}

static void scene_buffer_set_texture(struct wlr_scene_buffer *scene_buffer, struct wlr_texture *texture) {
    wl_list_remove(&scene_buffer->renderer_destroy.link);
    wlr_texture_destroy(scene_buffer->texture);
    scene_buffer->texture = texture;

    if (texture != NULL) {
        scene_buffer->renderer_destroy.notify = scene_buffer_handle_renderer_destroy;
        wl_signal_add(&texture->renderer->events.destroy, &scene_buffer->renderer_destroy);
    } else {
        wl_list_init(&scene_buffer->renderer_destroy.link);
    }
}

static struct wlr_texture *scene_buffer_get_texture(struct wlr_scene_buffer *scene_buffer, struct wlr_renderer *renderer) {
    if (scene_buffer->buffer == NULL || scene_buffer->texture != NULL) {
        return scene_buffer->texture;
    }

    struct wlr_client_buffer *client_buffer = wlr_client_buffer_get(scene_buffer->buffer);
    if (client_buffer != NULL) {
        return client_buffer->texture;
    }

    struct wlr_texture *texture = wlr_texture_from_buffer(renderer, scene_buffer->buffer);
    if (texture != NULL && scene_buffer->own_buffer) {
        scene_buffer->own_buffer = false;
        wlr_buffer_unlock(scene_buffer->buffer);
    }
    scene_buffer_set_texture(scene_buffer, texture);
    return texture;
}

static void wsland_cursor_frame(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_cursor_frame);
    wsland_server *server = data;

    if (!adapter->freerdp->peer || !server->cursor_output || !server->cursor_output->cache_cursor.dirty) {
        return;
    }

    {
        if (server->cursor_output->cache_cursor.buffer) {
            struct wlr_texture *texture = wlr_texture_from_buffer(
                server->renderer, server->cursor_output->cache_cursor.buffer
            );

            if (texture) {
                wsland_cursor_data cursor = {0};
                struct wlr_buffer *buffer = server->allocator->impl->create_buffer(
                    server->allocator, texture->width, texture->height,
                    &(const struct wlr_drm_format) { .format = DRM_FORMAT_ARGB8888}
                );

                wlr_buffer_lock(buffer);
                struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server->renderer, buffer, NULL);
                wlr_render_pass_add_texture(pass, &(const struct wlr_render_texture_options) {
                    .texture = texture, .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                    .transform = WL_OUTPUT_TRANSFORM_FLIPPED_180,
                });

                void *ptr;
                size_t stride;
                uint32_t format;
                if (wlr_render_pass_submit(pass) && wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &ptr, &format, &stride)) {
                    wlr_buffer_end_data_ptr_access(buffer);

                    cursor.data = ptr;
                    cursor.format = &format;
                    cursor.stride = &stride;
                    cursor.dirty = true;
                }

                cursor.width = server->cursor_output->cache_cursor.buffer->width;
                cursor.height = server->cursor_output->cache_cursor.buffer->height;
                cursor.hotspot_x = server->cursor_output->cache_cursor.hotspot_x;
                cursor.hotspot_y = server->cursor_output->cache_cursor.hotspot_y;

                {
                    if (!cursor.dirty) {
                        return;
                    }

                    int cursor_bpp = 4;
                    rdpUpdate *update = adapter->freerdp->peer->peer->update;

                    POINTER_LARGE_UPDATE pointerUpdate = {0};
                    pointerUpdate.xorBpp = cursor_bpp * 8;
                    pointerUpdate.cacheIndex = 0;
                    pointerUpdate.hotSpotX = cursor.hotspot_x;
                    pointerUpdate.hotSpotY = cursor.hotspot_y;
                    pointerUpdate.width = cursor.width,
                        pointerUpdate.height = cursor.height,
                        pointerUpdate.lengthXorMask = cursor_bpp * cursor.width * cursor.height;
                    pointerUpdate.xorMaskData = cursor.data;
                    pointerUpdate.lengthAndMask = 0;
                    pointerUpdate.andMaskData = NULL;

                    update->BeginPaint(update->context);
                    update->pointer->PointerLarge(update->context, &pointerUpdate);
                    update->EndPaint(update->context);
                }
                server->cursor_output->cache_cursor.dirty = false;

                wlr_texture_destroy(texture);
                wlr_buffer_unlock(buffer);
            }
        }
    }
}

static void scene_surface_commit(struct wl_listener *listener, void *data) {
    wsland_scene_buffer *buffer = wl_container_of(listener, buffer, events.scene_surface_commit);
    struct wlr_surface *surface = data;

    if (!buffer->buffer->buffer) {
        return;
    }

    int sx, sy;
    wlr_scene_node_coords(&buffer->buffer->node, &sx, &sy);

    pixman_region32_t damage;
    pixman_region32_init(&damage);
    wlr_surface_get_effective_damage(surface, &damage);
    pixman_region32_translate(&damage, sx - buffer->toplevel->window_data->visible.x, sy - buffer->toplevel->window_data->visible.y);
    pixman_region32_union(&buffer->toplevel->window_data->damage, &buffer->toplevel->window_data->damage, &damage);
    pixman_region32_fini(&damage);
}

static void scene_surface_destroy(struct wl_listener *listener, void *data) {
    wsland_scene_buffer *buffer = wl_container_of(listener, buffer, events.scene_surface_destroy);

    wl_list_remove(&buffer->events.scene_surface_commit.link);
    wl_list_remove(&buffer->events.scene_surface_destroy.link);
    free(buffer);
}

static void each_buffer_create(struct wlr_scene_buffer *scene_buffer, int sx, int sy, void *user_data) {
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);

    wsland_scene_buffer *buffer = calloc(1, sizeof(*buffer));
    buffer->toplevel = user_data;
    buffer->buffer = scene_buffer;

    buffer->events.scene_surface_commit.notify = scene_surface_commit;
    wl_signal_add(&scene_surface->surface->events.commit, &buffer->events.scene_surface_commit);

    buffer->events.scene_surface_destroy.notify = scene_surface_destroy;
    wl_signal_add(&scene_surface->surface->events.destroy, &buffer->events.scene_surface_destroy);
}

static void output_frame_configure(wsland_frame_context *ctx) {
    if (!ctx->toplevel->window_data) {
        wsland_window_data *window_data = calloc(1, sizeof(*window_data));
        if (!window_data) {
            wsland_log(ADAPTER, ERROR, "calloc failed for wsland_window_data");
            return;
        }

        ctx->toplevel->window_data = window_data;
        ctx->toplevel->window_data->window_id = ++wsland_window_id;
        ctx->toplevel->window_data->need_create = true;

        ctx->toplevel->window_data->scene = wlr_scene_create();
        ctx->toplevel->window_data->scene_tree = wlr_scene_xdg_surface_create(
            &ctx->toplevel->window_data->scene->tree, ctx->toplevel->toplevel->base
        );
        wlr_scene_node_for_each_buffer(&ctx->toplevel->window_data->scene_tree->node, each_buffer_create, ctx->toplevel);
    }

    if (ctx->toplevel->toplevel->title) {
        if (!ctx->toplevel->window_data->title || strcmp(ctx->toplevel->toplevel->title, ctx->toplevel->window_data->title) != 0) {
            ctx->toplevel->window_data->title = strdup(ctx->toplevel->toplevel->title);
            ctx->toplevel->window_data->update_title = true;
            ctx->toplevel->window_data->need_update = true;
        }
    }

    {
        struct wl_array nodes;
        wl_array_init(&nodes);
        pixman_region32_t visible;
        pixman_region32_init(&visible);
        scene_node_bounds(&ctx->toplevel->window_data->scene_tree->node, 0, 0, &visible, &nodes);

        struct wlr_box new_visible = (struct wlr_box){
            visible.extents.x1, visible.extents.y1,
            visible.extents.x2 - visible.extents.x1,
            visible.extents.y2 - visible.extents.y1
        };

        if (!wlr_box_equal(&new_visible, &ctx->toplevel->window_data->visible)) {
            ctx->toplevel->window_data->visible = new_visible;
            {
                wlr_swapchain_destroy(ctx->toplevel->window_data->chain);
                ctx->toplevel->window_data->chain = wlr_swapchain_create(
                    ctx->toplevel->server->allocator,
                    new_visible.width, new_visible.height,
                    &(const struct wlr_drm_format) { .format = DRM_FORMAT_ARGB8888 }
                );

                pixman_region32_init_rect(
                    &ctx->toplevel->window_data->damage, 0, 0,
                    new_visible.width, new_visible.height
                );
            }
            ctx->toplevel->window_data->update_visible = true;
            ctx->toplevel->window_data->need_update = true;
        }

        if (pixman_region32_not_empty(&ctx->toplevel->window_data->damage)) {
            struct wlr_buffer *buffer = wlr_swapchain_acquire(ctx->toplevel->window_data->chain);
            if (!buffer) {
                wl_array_release(&nodes);
                return;
            }

            ctx->pass = wlr_renderer_begin_buffer_pass(ctx->toplevel->server->renderer, buffer, NULL);
            if (!ctx->pass) {
                wlr_buffer_unlock(buffer);
                wl_array_release(&nodes);
                return;
            }

            wlr_render_pass_add_rect(ctx->pass, &(const struct wlr_render_rect_options) {
                .box = { 0, 0, ctx->toplevel->window_data->visible.width, ctx->toplevel->window_data->visible.height },
                .color = { 0,0,0,0 }, .blend_mode = WLR_RENDER_BLEND_MODE_NONE
            });

            wsland_render_data *data;
            wl_array_for_each(data, &nodes) {
                struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(data->node);
                struct wlr_texture *texture = wlr_texture_from_buffer(ctx->toplevel->server->renderer, scene_buffer->buffer);
                if (!texture) { return; }

                pixman_region32_t region;
                pixman_region32_init_rect(&region, data->sx, data->sy, texture->width, texture->height);
                pixman_region32_translate(&region, -ctx->toplevel->window_data->visible.x, -ctx->toplevel->window_data->visible.y);
                struct wlr_box dst_box = { region.extents.x1, region.extents.y1, region.extents.x2 -region.extents.x1, region.extents.y2 -region.extents.y1  };

                wlr_render_pass_add_texture(ctx->pass, &(const struct wlr_render_texture_options) {
                    .texture = texture, .src_box = scene_buffer->src_box, .alpha = &scene_buffer->opacity,
                    .filter_mode = scene_buffer->filter_mode, .dst_box = dst_box,
                });
                wlr_buffer_unlock(scene_buffer->buffer);
                pixman_region32_fini(&region);
            }
            wl_array_release(&nodes);

            if (!wlr_render_pass_submit(ctx->pass)) {
                wlr_buffer_unlock(buffer);
                return;
            }

            wlr_buffer_unlock(ctx->buffer);
            ctx->buffer = wlr_buffer_lock(buffer);
            wlr_buffer_unlock(buffer);
            
            ctx->toplevel->window_data->need_damage = true;
        }
        pixman_region32_fini(&visible);
    }

    {
        int pos_x, pos_y;
        wlr_scene_node_coords(&ctx->toplevel->tree->node, &pos_x, &pos_y);
        pos_x += ctx->toplevel->window_data->visible.x;
        pos_y += ctx->toplevel->window_data->visible.y;

        if (pos_x != ctx->toplevel->window_data->pos_x || pos_y != ctx->toplevel->window_data->pos_y) {
            ctx->toplevel->window_data->pos_x = pos_x;
            ctx->toplevel->window_data->pos_y = pos_y;

            ctx->toplevel->window_data->update_position = true;
            ctx->toplevel->window_data->need_update = true;
        }
    }
}

static void wsland_adapter_output_frame(wsland_adapter *adapter, wsland_output *output) {
    {
        if (!adapter->freerdp->peer || !(adapter->freerdp->peer->flags & WSLAND_PEER_OUTPUT_ENABLED)) {
            return;
        }
        if (!(adapter->freerdp->peer->is_acknowledged_suspended || adapter->freerdp->peer->current_frame_id - adapter->freerdp->peer->acknowledged_frame_id < 2)) {
            return;
        }
    }

    RdpgfxServerContext *gfx_ctx = adapter->freerdp->peer->ctx_server_rdpgfx;
    wsland_frame_context ctx = {.output = output, .peer = adapter->freerdp->peer};

    wsland_toplevel *toplevel;
    wl_list_for_each(toplevel, &output->server->toplevels, server_link) {
        ctx.toplevel = toplevel;
        output_frame_configure(&ctx);

        if (toplevel->window_data->need_create || toplevel->window_data->need_update) {
            WINDOW_ORDER_INFO window_order_info = {0};
            WINDOW_STATE_ORDER window_state_order = {0};

            window_order_info.windowId = toplevel->window_data->window_id;
            window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

            if (toplevel->window_data->need_create) {
                window_order_info.fieldFlags |= WINDOW_ORDER_STATE_NEW;

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
                window_state_order.style = RAIL_WINDOW_NORMAL_STYLE;
                window_state_order.extendedStyle = WS_EX_LAYERED;

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;
                window_state_order.ownerWindowId = toplevel->window_data->parent_id;

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET;
                window_state_order.clientOffsetX = 0;
                window_state_order.clientOffsetY = 0;

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_CLIENT_DELTA;
                window_state_order.windowClientDeltaX = 0;
                window_state_order.windowClientDeltaY = 0;

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VIS_OFFSET;
                window_state_order.visibleOffsetX = 0;
                window_state_order.visibleOffsetY = 0;

                {
                    RailServerContext *rail_ctx = ctx.peer->ctx_server_rail;

                    RAIL_MINMAXINFO_ORDER minmax_order = {0};
                    minmax_order.windowId = toplevel->window_data->window_id;
                    minmax_order.maxPosX = 0;
                    minmax_order.maxPosY = 0;
                    minmax_order.maxWidth = output->monitor.width;
                    minmax_order.maxHeight = output->monitor.height;
                    minmax_order.minTrackWidth = 0;
                    minmax_order.minTrackHeight = 0;
                    minmax_order.maxTrackWidth = output->monitor.width;
                    minmax_order.maxTrackHeight = output->monitor.height;
                    rail_ctx->ServerMinMaxInfo(rail_ctx, &minmax_order);
                }
            }

            if (toplevel->window_data->update_visible) {
                bool has_content = !wlr_box_empty(&toplevel->window_data->visible);
                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
                window_state_order.showState = has_content ? WINDOW_SHOW : WINDOW_HIDE;
                window_state_order.TaskbarButton = has_content ? 0 : 1;

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
                window_state_order.clientAreaWidth = toplevel->window_data->visible.width;
                window_state_order.clientAreaHeight = toplevel->window_data->visible.height;

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
                window_state_order.windowWidth = toplevel->window_data->visible.width;
                window_state_order.windowHeight = toplevel->window_data->visible.height;

                RECTANGLE_16 window_rect = {
                    .left = 0,
                    .top = 0,
                    .right = toplevel->window_data->visible.width,
                    .bottom = toplevel->window_data->visible.height
                };

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
                window_state_order.numWindowRects = 1;
                window_state_order.windowRects = &window_rect;

                RECTANGLE_16 window_vis = {
                    .left = 0,
                    .top = 0,
                    .right = toplevel->window_data->visible.width,
                    .bottom = toplevel->window_data->visible.height
                };

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
                window_state_order.numVisibilityRects = 1;
                window_state_order.visibilityRects = &window_vis;
            }

            if (toplevel->window_data->update_position) {
                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
                window_state_order.windowOffsetX = toplevel->window_data->pos_x;
                window_state_order.windowOffsetY = toplevel->window_data->pos_y;
            }

            RAIL_UNICODE_STRING rail_window_title = {0, NULL};
            if (toplevel->window_data->update_title) {
                if (utf8_string_to_rail_string(toplevel->toplevel->title, &rail_window_title)) {
                    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_TITLE;
                    window_state_order.titleInfo = rail_window_title;
                }
            }

            struct rdp_update *update = adapter->freerdp->peer->peer->update;
            update->BeginPaint(update->context);
            if (toplevel->window_data->need_create) {
                update->window->WindowCreate(update->context, &window_order_info, &window_state_order);
            }
            else if (toplevel->window_data->need_update) {
                update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
            }
            update->EndPaint(update->context);

            if (toplevel->window_data->update_title) {
                free(rail_window_title.string);
            }

            if (toplevel->window_data->update_visible) {
                uint32_t prev_surface_id = toplevel->window_data->surface_id;

                RDPGFX_CREATE_SURFACE_PDU create_surface = {0};
                create_surface.surfaceId = (uint16_t)++wsland_surface_id;
                create_surface.width = toplevel->window_data->visible.width;
                create_surface.height = toplevel->window_data->visible.height;
                create_surface.pixelFormat = GFX_PIXEL_FORMAT_ARGB_8888;
                if (gfx_ctx->CreateSurface(gfx_ctx, &create_surface) == 0) {
                    toplevel->window_data->surface_id = create_surface.surfaceId;
                }

                RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU map_surface_to_window = {0};
                map_surface_to_window.windowId = toplevel->window_data->window_id;
                map_surface_to_window.surfaceId = toplevel->window_data->surface_id;
                map_surface_to_window.mappedWidth = toplevel->window_data->visible.width;
                map_surface_to_window.mappedHeight = toplevel->window_data->visible.height;
                map_surface_to_window.targetWidth = toplevel->window_data->visible.width;
                map_surface_to_window.targetHeight = toplevel->window_data->visible.height;
                if (gfx_ctx->MapSurfaceToScaledWindow(gfx_ctx, &map_surface_to_window)) {
                    toplevel->window_data->scale_w = 100;
                    toplevel->window_data->scale_h = 100;
                }

                if (prev_surface_id) {
                    RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
                    deleteSurface.surfaceId = (uint16_t)prev_surface_id;
                    gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
                }
            }

            toplevel->window_data->need_create = false;
            toplevel->window_data->need_update = false;
            toplevel->window_data->need_show = false;
            toplevel->window_data->need_hide = false;
            toplevel->window_data->update_title = false;
            toplevel->window_data->update_visible = false;
            toplevel->window_data->update_position = false;
        }

        if (toplevel->window_data->need_damage) {
            {
                struct wlr_texture *texture = wlr_texture_from_buffer(
                    ctx.toplevel->server->renderer, ctx.buffer
                );

                int buffer_bpp = 4; /* Bytes Per Pixel. */
                int width = texture->width;
                int height = texture->height;

                int damage_stride = width * 4;
                int damage_size = damage_stride * height;
                uint8_t *ptr = malloc(damage_size);

                if (wlr_texture_read_pixels(
                    texture, &(const struct wlr_texture_read_pixels_options){
                        .data = ptr, .format = DRM_FORMAT_ARGB8888, .stride = damage_stride,
                    }
                )) {
                    if (!ctx.need_end_frame) {
                        ctx.frame_id = ++ctx.peer->current_frame_id;

                        RDPGFX_START_FRAME_PDU start_frame = {0};
                        start_frame.frameId = ctx.frame_id;
                        gfx_ctx->StartFrame(gfx_ctx, &start_frame);
                        ctx.need_end_frame = true;
                    }

                    BYTE *data = ptr;
                    bool has_alpha = !wlr_buffer_is_opaque(ctx.buffer);
                    int alpha_size;
                    BYTE *alpha;
                    {
                        int alpha_codec_header_size = 4;
                        if (has_alpha) {
                            alpha_size = alpha_codec_header_size + width * height;
                        }
                        else {
                            /* 8 = max of ALPHA_RLE_SEGMENT for single alpha value. */
                            alpha_size = alpha_codec_header_size + 8;
                        }
                        alpha = malloc(alpha_size);

                        /* generate alpha only bitmap */
                        /* set up alpha codec header */
                        alpha[0] = 'L'; /* signature */
                        alpha[1] = 'A'; /* signature */
                        alpha[2] = has_alpha ? 0 : 1; /* compression: RDP spec indicate this is non-zero value for compressed, but it must be 1.*/
                        alpha[3] = 0; /* compression */

                        if (has_alpha) {
                            BYTE *alpha_bits = &data[0];

                            for (int i = 0; i < height; i++, alpha_bits += width * buffer_bpp) {
                                BYTE *src_alpha_pixel = alpha_bits + 3; /* 3 = xxxA. */
                                BYTE *dst_alpha_pixel = &alpha[alpha_codec_header_size + i * width];

                                for (int j = 0; j < width; j++, src_alpha_pixel += buffer_bpp, dst_alpha_pixel++) {
                                    *dst_alpha_pixel = *src_alpha_pixel;
                                }
                            }
                        }
                        else {
                            int bitmap_size = width * height;

                            alpha[alpha_codec_header_size] = 0xFF; /* alpha value (opaque) */
                            if (bitmap_size < 0xFF) {
                                alpha[alpha_codec_header_size + 1] = (BYTE)bitmap_size;
                                alpha_size = alpha_codec_header_size + 2; /* alpha value + size in byte. */
                            }
                            else if (bitmap_size < 0xFFFF) {
                                alpha[alpha_codec_header_size + 1] = 0xFF;
                                *(short*)&alpha[alpha_codec_header_size + 2] = (short)bitmap_size;
                                alpha_size = alpha_codec_header_size + 4; /* alpha value + 1 + size in short. */
                            }
                            else {
                                alpha[alpha_codec_header_size + 1] = 0xFF;
                                *(short*)&alpha[alpha_codec_header_size + 2] = 0xFFFF;
                                *(int*)&alpha[alpha_codec_header_size + 4] = bitmap_size;
                                alpha_size = alpha_codec_header_size + 8; /* alpha value + 1 + 2 + size in int. */
                            }
                        }
                    }

                    RDPGFX_SURFACE_COMMAND surface_command = {0};
                    surface_command.surfaceId = ctx.toplevel->window_data->surface_id;
                    surface_command.format = PIXEL_FORMAT_BGRA32;
                    surface_command.left = 0;
                    surface_command.top = 0;
                    surface_command.right = width;
                    surface_command.bottom = height;
                    surface_command.width = width;
                    surface_command.height = height;
                    surface_command.contextId = 0;
                    surface_command.extra = NULL;

                    surface_command.codecId = RDPGFX_CODECID_ALPHA;
                    surface_command.length = alpha_size;
                    surface_command.data = &alpha[0];
                    gfx_ctx->SurfaceCommand(gfx_ctx, &surface_command);
                    free(alpha);

                    surface_command.codecId = RDPGFX_CODECID_UNCOMPRESSED;
                    surface_command.length = damage_size;
                    surface_command.data = &data[0];
                    gfx_ctx->SurfaceCommand(gfx_ctx, &surface_command);
                    wlr_buffer_unlock(ctx.buffer);
                    free(ptr);
                }
            }
            wlr_buffer_unlock(ctx.buffer);
            pixman_region32_clear(&ctx.toplevel->window_data->damage);
            toplevel->window_data->need_damage = false;
        }
    }

    if (ctx.need_end_frame) {
        RDPGFX_END_FRAME_PDU endFrame = {0};
        endFrame.frameId = ctx.peer->current_frame_id;
        gfx_ctx->EndFrame(gfx_ctx, &endFrame);
        ctx.need_end_frame = false;
    }
}

static void wsland_output_frame(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_output_frame);
    wsland_output *output = data;

    wsland_adapter_output_frame(adapter, output);
}

static void server_destroy_wsland_toplevel(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_toplevel_destroy);
    wsland_toplevel *toplevel = data;

    if (!adapter->freerdp->peer) {
        goto destroy_window_data;
        return;
    }

    WINDOW_ORDER_INFO window_order_info = {0};
    window_order_info.windowId = toplevel->window_data->window_id;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_DELETED;

    struct rdp_update *update = adapter->freerdp->peer->peer->update;
    update->BeginPaint(update->context);
    update->window->WindowDelete(update->context, &window_order_info);
    update->EndPaint(update->context);

    if (toplevel->window_data->surface_id) {
        RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
        deleteSurface.surfaceId = (uint16_t)toplevel->window_data->surface_id;

        RdpgfxServerContext *gfx_ctx = adapter->freerdp->peer->ctx_server_rdpgfx;
        gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
        toplevel->window_data->surface_id = 0;
    }
    toplevel->window_data->window_id = 0;

    goto destroy_window_data;
    return;

destroy_window_data:
    if (toplevel->window_data) {
        wlr_swapchain_destroy(toplevel->window_data->chain);
        wlr_scene_node_destroy(&toplevel->window_data->scene->tree.node);
        free(toplevel->window_data);
    }
}

wsland_adapter_handle wsland_adapter_handle_impl = {
    .wsland_cursor_frame = wsland_cursor_frame,
    .wsland_output_frame = wsland_output_frame,

    .server_destroy_wsland_toplevel = server_destroy_wsland_toplevel,
};

wsland_adapter_handle *wsland_adapter_handle_init(wsland_adapter *adapter) {
    return &wsland_adapter_handle_impl;
}

struct wl_event_loop *wsland_adapter_fetch_event_loop(wsland_adapter *adapter) {
    return adapter->server->event_loop;
}