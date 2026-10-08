/*
 * Wayland layer-shell overlay, keyboard input, grid rendering,
 * and virtual pointer.
 *
 * Connects to the compositor, creates a fullscreen transparent
 * overlay on the Overlay layer with exclusive keyboard grab,
 * draws the grid with cairo into wl_shm buffers, and controls
 * the mouse via wlr-virtual-pointer.
 *
 * Based on wl-kbptr's approach.
 */

#include "log.h"
#include "memory-util.h"
#include "string-util.h"
#include "waynav.h"

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

static void send_frame(struct overlay *ov);
static void buf_release(void *data, struct wl_buffer *wl_buf);

enum buf_state {
    BUF_UNINIT = 0,
    BUF_READY = 1,
    BUF_BUSY = 2,
};

struct shm_buffer {
    struct buffer_pool *pool;
    enum buf_state state;
    struct wl_buffer *wl_buf;
    cairo_surface_t *cairo_surface;
    cairo_t *cr;
    void *data;
    size_t data_size;
    uint32_t width;
    uint32_t height;
};

struct buffer_pool {
    struct overlay *overlay;
    struct shm_buffer bufs[2];
};

struct output {
    struct overlay *overlay;
    uint32_t global_name;
    struct wl_output *wl_output;
    struct zxdg_output_v1 *xdg_output;
    int32_t width;
    int32_t height;
    int32_t x;
    int32_t y;
    int32_t scale;
    int32_t mode_width;
    int32_t mode_height;
    int32_t transform;
    bool has_logical_size;
    int32_t pending_scale;
    int32_t pending_mode_width;
    int32_t pending_mode_height;
    int32_t pending_transform;
    int32_t pending_logical_width;
    int32_t pending_logical_height;
    uint32_t pending_changes;
    char *name;
    struct output *next;
};

/* A rectangle in the compositor's logical layout space. */
struct layout_rect {
    int x, y, w, h;
};

static int create_shm_file(size_t size) {
    char name[] = "/tmp/waynav-shm-XXXXXX";
    int fd = mkostemp(name, O_CLOEXEC);
    if (fd < 0)
        return -1;
    unlink(name);
    int err;
    while ((err = ftruncate(fd, (off_t)size)) && errno == EINTR)
        ;
    if (err) {
        close(fd);
        return -1;
    }
    return fd;
}

static void buf_destroy(struct shm_buffer *b) {
    if (b->state == BUF_UNINIT)
        return;
    if (b->cr)
        cairo_destroy(b->cr);
    if (b->cairo_surface)
        cairo_surface_destroy(b->cairo_surface);
    if (b->wl_buf)
        wl_buffer_destroy(b->wl_buf);
    if (b->data)
        munmap(b->data, b->data_size);
    ZERO_OBJECT(*b);
}

static const struct wl_buffer_listener buf_listener = {
    .release = buf_release,
};

static struct shm_buffer *buf_get(struct wl_shm *shm, struct buffer_pool *pool,
                                  uint32_t w, uint32_t h) {
    struct shm_buffer *b = NULL;
    for (int i = 0; i < 2; i++) {
        if (pool->bufs[i].state != BUF_BUSY) {
            b = &pool->bufs[i];
            break;
        }
    }
    if (!b)
        return NULL;

    if (b->width != w || b->height != h)
        buf_destroy(b);

    if (b->state == BUF_UNINIT) {
        uint32_t stride = (uint32_t)cairo_format_stride_for_width(
            CAIRO_FORMAT_ARGB32, (int)w);
        size_t sz = (size_t)h * stride;
        int fd = create_shm_file(sz);
        if (fd < 0)
            return NULL;

        void *data = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return NULL;
        }

        struct wl_shm_pool *pool_wl = wl_shm_create_pool(shm, fd, (int32_t)sz);
        b->wl_buf =
            wl_shm_pool_create_buffer(pool_wl, 0, (int32_t)w, (int32_t)h,
                                      (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
        wl_buffer_add_listener(b->wl_buf, &buf_listener, b);
        wl_shm_pool_destroy(pool_wl);
        close(fd);

        b->pool = pool;
        b->data = data;
        b->data_size = sz;
        b->width = w;
        b->height = h;
        b->state = BUF_READY;
        b->cairo_surface = cairo_image_surface_create_for_data(
            b->data, CAIRO_FORMAT_ARGB32, (int)w, (int)h, (int)stride);
        b->cr = cairo_create(b->cairo_surface);
    }
    return b;
}

struct overlay {
    /* Wayland globals */
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    uint32_t seat_global_name;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_virtual_pointer_manager_v1 *vptr_mgr;
    struct zxdg_output_manager_v1 *xdg_out_mgr;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *frac_scale_mgr;

    /* Objects */
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_callback *frame_cb;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *frac_scale;
    struct wl_region *input_region;
    struct zwlr_virtual_pointer_v1 *vptr;

    /* Input */
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    int cursor_x;
    int cursor_y;
    bool cursor_position_known;
    bool keyboard_released;

    /* Keyboard / xkb */
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    /* Outputs */
    struct output *outputs;
    struct output *selected_output;
    int32_t frac_scale_v; /* scale*120, 0 if unavailable */

    /* Surface */
    uint32_t surf_width;
    uint32_t surf_height;
    bool surf_width_chosen;
    bool surf_height_chosen;
    bool configured;

    /* Rendering */
    struct buffer_pool pool;
    bool redraw_pending;

    /* Key repeat */
    int repeat_fd;        /* timerfd */
    int32_t repeat_rate;  /* keys per second */
    int32_t repeat_delay; /* ms before first repeat */
    uint32_t repeat_key;  /* evdev code of held key, 0=none */
    uint32_t *pressed_keys;
    size_t pressed_key_count;
    size_t pressed_key_capacity;

    /* Idle timeout */
    int idle_fd;   /* timerfd, -1 when there is no timeout */
    int idle_secs; /* how long a gap in keyboard input may be */
    bool idled;    /* the loop ended because that gap was reached */

    /* State pointers (set during overlay_run) */
    struct config *cfg;
    struct region_state *rs;
    bool running;
    bool stop_requested;

    /* Key events deferred by reentrant Wayland dispatch. */
    bool dispatching_key;
    struct deferred_key {
        uint32_t key;
        const struct binding *binding;
    } *deferred_keys;
    size_t deferred_key_count;
    size_t deferred_key_capacity;
};

static void buf_release(void *data, struct wl_buffer *wl_buf) {
    (void)wl_buf;
    struct shm_buffer *buffer = data;
    buffer->state = BUF_READY;
    if (buffer->pool->overlay->redraw_pending)
        send_frame(buffer->pool->overlay);
}

static void render_grid(struct overlay *ov, cairo_t *cr,
                        struct region_state *rs);
static uint32_t xkb_mods_to_config(struct overlay *ov);
static void disarm_repeat(struct overlay *ov);
static void arm_repeat(struct overlay *ov, uint32_t key);
static uint32_t latest_repeatable_key(struct overlay *ov);

static void noop() {
}

static uint32_t negotiated_version(uint32_t advertised_version,
                                   uint32_t supported_version) {
    if (advertised_version < supported_version)
        return advertised_version;
    return supported_version;
}

static const char *output_name_or_unknown(const struct output *output) {
    if (!output || !output->name)
        return "<unknown>";
    return output->name;
}

static void set_output_name(struct output *output, const char *name) {
    char *copy = strdup(name);
    if (!copy)
        return;

    free(output->name);
    output->name = copy;
}

enum output_change {
    OUTPUT_CHANGE_SCALE = 1 << 0,
    OUTPUT_CHANGE_MODE = 1 << 1,
    OUTPUT_CHANGE_TRANSFORM = 1 << 2,
    OUTPUT_CHANGE_LOGICAL_SIZE = 1 << 3,
};

static void output_update_size(struct output *output) {
    int old_width = output->width;
    int old_height = output->height;
    if (!output->has_logical_size && output->mode_width > 0 &&
        output->mode_height > 0 && output->scale > 0) {
        int32_t mode_width = output->mode_width;
        int32_t mode_height = output->mode_height;
        if (output->transform == WL_OUTPUT_TRANSFORM_90 ||
            output->transform == WL_OUTPUT_TRANSFORM_270 ||
            output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
            output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270) {
            mode_width = output->mode_height;
            mode_height = output->mode_width;
        }
        output->width = mode_width / output->scale;
        output->height = mode_height / output->scale;
    }
    if (output->width == old_width && output->height == old_height &&
        !output->has_logical_size)
        return;

    struct overlay *ov = output->overlay;
    if (ov->selected_output == output || !ov->selected_output) {
        if (ov->surf_width_chosen || !ov->configured)
            ov->surf_width = (uint32_t)output->width;
        if (ov->surf_height_chosen || !ov->configured)
            ov->surf_height = (uint32_t)output->height;
    }
    if (ov->selected_output == output)
        send_frame(ov);
}

static void output_apply_changes(struct output *output, uint32_t changes) {
    changes &= output->pending_changes;
    if (!changes)
        return;

    if (changes & OUTPUT_CHANGE_SCALE)
        output->scale = output->pending_scale;
    if (changes & OUTPUT_CHANGE_MODE) {
        output->mode_width = output->pending_mode_width;
        output->mode_height = output->pending_mode_height;
    }
    if (changes & OUTPUT_CHANGE_TRANSFORM)
        output->transform = output->pending_transform;
    if (changes & OUTPUT_CHANGE_LOGICAL_SIZE) {
        output->width = output->pending_logical_width;
        output->height = output->pending_logical_height;
        output->has_logical_size = true;
    }
    output->pending_changes &= ~changes;
    output_update_size(output);
}

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x,
                            int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform) {
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    struct output *output = data;
    output->pending_transform = transform;
    output->pending_changes |= OUTPUT_CHANGE_TRANSFORM;
    if (wl_output_get_version(wl_output) < WL_OUTPUT_DONE_SINCE_VERSION)
        output_apply_changes(output, OUTPUT_CHANGE_TRANSFORM);
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
    (void)refresh;
    if (!(flags & WL_OUTPUT_MODE_CURRENT))
        return;
    struct output *output = data;
    output->pending_mode_width = width;
    output->pending_mode_height = height;
    output->pending_changes |= OUTPUT_CHANGE_MODE;
    if (wl_output_get_version(wl_output) < WL_OUTPUT_DONE_SINCE_VERSION)
        output_apply_changes(output, OUTPUT_CHANGE_MODE);
}

static void output_done(void *data, struct wl_output *wl_output) {
    (void)wl_output;
    output_apply_changes(data, OUTPUT_CHANGE_SCALE | OUTPUT_CHANGE_MODE |
                                   OUTPUT_CHANGE_TRANSFORM);
}

static void output_scale(void *data, struct wl_output *wl_output,
                         int32_t factor) {
    (void)wl_output;
    struct output *output = data;
    output->pending_scale = factor;
    output->pending_changes |= OUTPUT_CHANGE_SCALE;
}

static void output_name(void *data, struct wl_output *wl_output,
                        const char *name) {
    (void)wl_output;
    set_output_name(data, name);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = noop,
};

static void xdg_output_logical_position(void *data,
                                        struct zxdg_output_v1 *xdg_output,
                                        int32_t x, int32_t y) {
    (void)xdg_output;
    struct output *output = data;
    output->x = x;
    output->y = y;
}

static void xdg_output_logical_size(void *data,
                                    struct zxdg_output_v1 *xdg_output,
                                    int32_t width, int32_t height) {
    (void)xdg_output;
    struct output *output = data;
    output->pending_logical_width = width;
    output->pending_logical_height = height;
    output->pending_changes |= OUTPUT_CHANGE_LOGICAL_SIZE;
    if (zxdg_output_v1_get_version(output->xdg_output) <
        ZXDG_OUTPUT_V1_DONE_SINCE_VERSION)
        output_apply_changes(output, OUTPUT_CHANGE_LOGICAL_SIZE);
}

static void xdg_output_done(void *data, struct zxdg_output_v1 *xdg_output) {
    (void)xdg_output;
    output_apply_changes(data, OUTPUT_CHANGE_LOGICAL_SIZE);
}

static void xdg_output_name(void *data, struct zxdg_output_v1 *xdg_output,
                            const char *name) {
    (void)xdg_output;
    set_output_name(data, name);
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_logical_position,
    .logical_size = xdg_output_logical_size,
    .done = xdg_output_done,
    .name = xdg_output_name,
    .description = noop,
};

static void create_xdg_output(struct overlay *ov, struct output *output) {
    if (!ov->xdg_out_mgr || output->xdg_output)
        return;

    output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
        ov->xdg_out_mgr, output->wl_output);
    zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener,
                                output);
}

static void create_xdg_outputs(struct overlay *ov) {
    for (struct output *output = ov->outputs; output; output = output->next)
        create_xdg_output(ov, output);
}

static void output_destroy(struct output *output) {
    if (output->xdg_output)
        zxdg_output_v1_destroy(output->xdg_output);
    if (wl_output_get_version(output->wl_output) >=
        WL_OUTPUT_RELEASE_SINCE_VERSION)
        wl_output_release(output->wl_output);
    else
        wl_output_destroy(output->wl_output);
    free(output->name);
    free(output);
}

static void bind_output(struct overlay *ov, struct wl_registry *registry,
                        uint32_t name, uint32_t version) {
    struct output *output = calloc(1, sizeof(*output));
    if (!output) {
        log_err("failed to allocate wl_output");
        return;
    }

    output->overlay = ov;
    output->global_name = name;
    output->scale = 1;
    output->wl_output = wl_registry_bind(registry, name, &wl_output_interface,
                                         negotiated_version(version, 4));
    wl_output_add_listener(output->wl_output, &output_listener, output);
    output->next = ov->outputs;
    ov->outputs = output;
    create_xdg_output(ov, output);
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    struct overlay *ov = data;

    if (streq(interface, wl_compositor_interface.name)) {
        ov->compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             negotiated_version(version, 4));
    } else if (streq(interface, wl_shm_interface.name)) {
        ov->shm = wl_registry_bind(registry, name, &wl_shm_interface,
                                   negotiated_version(version, 1));
    } else if (streq(interface, wl_seat_interface.name)) {
        if (!ov->seat) {
            ov->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                        negotiated_version(version, 7));
            ov->seat_global_name = name;
        }
    } else if (streq(interface, wl_output_interface.name)) {
        bind_output(ov, registry, name, version);
    } else if (streq(interface, zwlr_layer_shell_v1_interface.name)) {
        uint32_t layer_shell_version = version;
        if (layer_shell_version > ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION)
            layer_shell_version = ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION;
        ov->layer_shell =
            wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
                             layer_shell_version);
    } else if (streq(interface,
                     zwlr_virtual_pointer_manager_v1_interface.name)) {
        ov->vptr_mgr = wl_registry_bind(
            registry, name, &zwlr_virtual_pointer_manager_v1_interface,
            negotiated_version(version, 2));
    } else if (streq(interface, zxdg_output_manager_v1_interface.name)) {
        ov->xdg_out_mgr =
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface,
                             negotiated_version(version, 2));
        create_xdg_outputs(ov);
    } else if (streq(interface, wp_viewporter_interface.name)) {
        ov->viewporter =
            wl_registry_bind(registry, name, &wp_viewporter_interface,
                             negotiated_version(version, 1));
    } else if (streq(interface,
                     wp_fractional_scale_manager_v1_interface.name)) {
        ov->frac_scale_mgr = wl_registry_bind(
            registry, name, &wp_fractional_scale_manager_v1_interface,
            negotiated_version(version, 1));
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
    (void)registry;
    struct overlay *ov = data;
    if (ov->seat && name == ov->seat_global_name) {
        log_warn("seat removed; stopping overlay");
        overlay_stop(ov);
        return;
    }

    struct output **link = &ov->outputs;

    while (*link) {
        struct output *output = *link;
        if (output->global_name == name) {
            *link = output->next;
            if (ov->selected_output == output)
                ov->selected_output = NULL;
            output_destroy(output);
            return;
        }
        link = &output->next;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static struct output *find_output(const struct overlay *ov,
                                  const struct wl_output *wl_output) {
    for (struct output *output = ov->outputs; output; output = output->next) {
        if (output->wl_output == wl_output)
            return output;
    }
    return NULL;
}

static void surface_enter(void *data, struct wl_surface *surface,
                          struct wl_output *wl_output) {
    (void)surface;
    struct overlay *ov = data;
    struct output *output = find_output(ov, wl_output);
    if (!output) {
        log_warn("surface entered an unknown output");
        return;
    }

    bool output_changed = ov->selected_output != output;
    ov->selected_output = output;
    ov->cursor_position_known = false;
    if (output_changed && ov->surf_width_chosen)
        ov->surf_width = (uint32_t)output->width;
    if (output_changed && ov->surf_height_chosen)
        ov->surf_height = (uint32_t)output->height;
    if (output_changed)
        send_frame(ov);
    log_debug("surface entered output %s: %dx%d+%d+%d scale=%d",
              output_name_or_unknown(output), output->width, output->height,
              output->x, output->y, output->scale);
}

static void surface_leave(void *data, struct wl_surface *surface,
                          struct wl_output *wl_output) {
    (void)surface;
    struct overlay *ov = data;
    if (ov->selected_output && ov->selected_output->wl_output == wl_output)
        ov->selected_output = NULL;
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t w, uint32_t h) {
    struct overlay *ov = data;
    bool width_chosen = w == 0;
    bool height_chosen = h == 0;
    if (w == 0 && ov->selected_output)
        w = (uint32_t)ov->selected_output->width;
    if (h == 0 && ov->selected_output)
        h = (uint32_t)ov->selected_output->height;
    if (w == 0)
        w = ov->surf_width;
    if (h == 0)
        h = ov->surf_height;
    if (w == 0 && ov->outputs)
        w = (uint32_t)ov->outputs->width;
    if (h == 0 && ov->outputs)
        h = (uint32_t)ov->outputs->height;

    zwlr_layer_surface_v1_ack_configure(ls, serial);
    ov->surf_width_chosen = width_chosen;
    ov->surf_height_chosen = height_chosen;
    if (w == 0 || h == 0) {
        log_debug("layer configure deferred");
        return;
    }
    if (w > INT_MAX)
        w = INT_MAX;
    if (h > INT_MAX)
        h = INT_MAX;

    ov->surf_width = w;
    ov->surf_height = h;
    ov->configured = true;
    log_debug("layer configure: %ux%u", w, h);
    if (ov->rs)
        send_frame(ov);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    (void)ls;
    struct overlay *ov = data;
    if (ov->rs)
        overlay_stop_drag(ov, ov->rs);
    overlay_stop(ov);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void frac_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                           uint32_t scale) {
    (void)fs;
    struct overlay *ov = data;
    if (ov->frac_scale_v != (int32_t)scale) {
        ov->frac_scale_v = (int32_t)scale;
        send_frame(ov);
    }
    log_debug("fractional scale: %u/120 = %.2f", scale, scale / 120.0);
}

static const struct wp_fractional_scale_v1_listener frac_listener = {
    .preferred_scale = frac_preferred,
};

static void kbd_keymap(void *data, struct wl_keyboard *kbd, uint32_t fmt,
                       int fd, uint32_t size) {
    struct overlay *ov = data;
    (void)kbd;

    if (ov->xkb_state) {
        xkb_state_unref(ov->xkb_state);
        ov->xkb_state = NULL;
    }
    if (ov->xkb_keymap) {
        xkb_keymap_unref(ov->xkb_keymap);
        ov->xkb_keymap = NULL;
    }
    disarm_repeat(ov);

    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        log_warn("keyboard: unsupported keymap format %d", fmt);
        close(fd);
        return;
    }

    void *buf = mmap(NULL, size - 1, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buf != MAP_FAILED) {
        ov->xkb_keymap = xkb_keymap_new_from_buffer(
            ov->xkb_ctx, buf, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(buf, size - 1);
    }
    close(fd);
    if (!ov->xkb_keymap) {
        log_warn("keyboard: failed to load keymap");
        return;
    }
    ov->xkb_state = xkb_state_new(ov->xkb_keymap);
    if (!ov->xkb_state) {
        log_warn("keyboard: failed to create keymap state");
        xkb_keymap_unref(ov->xkb_keymap);
        ov->xkb_keymap = NULL;
        return;
    }
    uint32_t key = latest_repeatable_key(ov);
    if (key != 0)
        arm_repeat(ov, key);
    log_debug("keyboard keymap loaded");
    if (ov->cfg)
        config_resolve_keycodes(ov->cfg, ov->xkb_keymap);
}

static void disarm_repeat(struct overlay *ov) {
    if (ov->repeat_fd < 0)
        return;
    struct itimerspec its = {0};
    timerfd_settime(ov->repeat_fd, 0, &its, NULL);
    ov->repeat_key = 0;
}

static void arm_repeat(struct overlay *ov, uint32_t key) {
    if (ov->repeat_fd < 0 || ov->repeat_rate <= 0)
        return;

    ov->repeat_key = key;

    long delay_ns = (long)ov->repeat_delay * 1000000L;
    long rate_ns = 1000000000L / ov->repeat_rate;

    struct itimerspec its = {
        .it_value = {delay_ns / 1000000000L, delay_ns % 1000000000L},
        .it_interval = {rate_ns / 1000000000L, rate_ns % 1000000000L},
    };
    if (delay_ns == 0)
        its.it_value.tv_nsec = 1;
    if (timerfd_settime(ov->repeat_fd, 0, &its, NULL) < 0) {
        log_warn("key: failed to arm repeat: %s", strerror(errno));
        ov->repeat_key = 0;
    }
}

static const struct binding *find_key_binding(const struct overlay *ov,
                                              xkb_keycode_t keycode,
                                              uint32_t mods) {
    log_debug("key: keycode=%u mods=0x%x", keycode, mods);
    return config_find_binding(ov->cfg, keycode, mods);
}

static void execute_binding(struct overlay *ov, uint32_t key,
                            const struct binding *binding) {
    if (!binding)
        return;
    log_debug("key: execute keycode=%u", key + 8);
    execute_commands(ov, ov->rs, binding->commands, binding->num_commands);
}

static void defer_key(struct overlay *ov, uint32_t key,
                      const struct binding *binding) {
    if (ov->deferred_key_count == ov->deferred_key_capacity) {
        size_t capacity =
            ov->deferred_key_capacity == 0 ? 8 : ov->deferred_key_capacity * 2;
        struct deferred_key *keys =
            reallocarray(ov->deferred_keys, capacity, sizeof(*keys));
        if (!keys) {
            log_warn("key: failed to defer key %u", key + 8);
            return;
        }
        ov->deferred_keys = keys;
        ov->deferred_key_capacity = capacity;
    }
    ov->deferred_keys[ov->deferred_key_count++] =
        (struct deferred_key){.key = key, .binding = binding};
}

static bool pressed_key_contains(const struct overlay *ov, uint32_t key) {
    for (size_t i = 0; i < ov->pressed_key_count; i++) {
        if (ov->pressed_keys[i] == key)
            return true;
    }
    return false;
}

static void pressed_key_add(struct overlay *ov, uint32_t key) {
    if (pressed_key_contains(ov, key))
        return;
    if (ov->pressed_key_count == ov->pressed_key_capacity) {
        size_t capacity =
            ov->pressed_key_capacity == 0 ? 8 : ov->pressed_key_capacity * 2;
        uint32_t *keys =
            reallocarray(ov->pressed_keys, capacity, sizeof(*keys));
        if (!keys) {
            log_warn("key: failed to track pressed key %u", key + 8);
            return;
        }
        ov->pressed_keys = keys;
        ov->pressed_key_capacity = capacity;
    }
    ov->pressed_keys[ov->pressed_key_count++] = key;
}

static void pressed_key_remove(struct overlay *ov, uint32_t key) {
    for (size_t i = 0; i < ov->pressed_key_count; i++) {
        if (ov->pressed_keys[i] != key)
            continue;
        memmove(&ov->pressed_keys[i], &ov->pressed_keys[i + 1],
                (ov->pressed_key_count - i - 1) * sizeof(*ov->pressed_keys));
        ov->pressed_key_count--;
        return;
    }
}

static uint32_t latest_repeatable_key(struct overlay *ov) {
    if (!ov->xkb_keymap)
        return 0;
    for (size_t i = ov->pressed_key_count; i > 0; i--) {
        uint32_t key = ov->pressed_keys[i - 1];
        if (xkb_keymap_key_repeats(ov->xkb_keymap, key + 8))
            return key;
    }
    return 0;
}

static void finish_key_press(struct overlay *ov, uint32_t key) {
    if (!ov->running) {
        disarm_repeat(ov);
        return;
    }

    if (!ov->xkb_keymap || !xkb_keymap_key_repeats(ov->xkb_keymap, key + 8))
        return;
    if (pressed_key_contains(ov, key))
        arm_repeat(ov, key);
    else if (ov->repeat_key == key)
        disarm_repeat(ov);
}

static void process_deferred_keys(struct overlay *ov) {
    while (ov->running && ov->deferred_key_count > 0) {
        struct deferred_key deferred = ov->deferred_keys[0];
        memmove(&ov->deferred_keys[0], &ov->deferred_keys[1],
                (ov->deferred_key_count - 1) * sizeof(ov->deferred_keys[0]));
        ov->deferred_key_count--;

        ov->dispatching_key = true;
        execute_binding(ov, deferred.key, deferred.binding);
        ov->dispatching_key = false;
        finish_key_press(ov, deferred.key);
    }
}

static void run_key_binding(struct overlay *ov, uint32_t key,
                            const struct binding *binding, bool update_repeat) {
    ov->dispatching_key = true;
    execute_binding(ov, key, binding);
    ov->dispatching_key = false;
    if (update_repeat)
        finish_key_press(ov, key);
    process_deferred_keys(ov);
}

/* Restart the idle countdown.
 *
 * Every keyboard event the compositor delivers rearms it, so the timeout
 * measures the gap between keypresses rather than the age of the overlay: a
 * navigation that takes minutes is never cut short, and one that is getting no
 * input still ends. Key repeat deliberately does not rearm it -- those
 * expirations are this process talking to itself, and a key held down when the
 * keyboard stopped reporting would otherwise hold the grab forever. */
static void arm_idle(struct overlay *ov) {
    if (ov->idle_fd < 0)
        return;

    struct itimerspec its = {.it_value = {ov->idle_secs, 0}};
    timerfd_settime(ov->idle_fd, 0, &its, NULL);
}

static void handle_key_dispatch(struct overlay *ov, uint32_t key) {
    if (!ov->running || !ov->xkb_state || !ov->cfg)
        return;

    uint32_t mods = xkb_mods_to_config(ov);
    const struct binding *binding = find_key_binding(ov, key + 8, mods);
    if (ov->dispatching_key) {
        defer_key(ov, key, binding);
        return;
    }
    run_key_binding(ov, key, binding, true);
}

static void kbd_key(void *data, struct wl_keyboard *kbd, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state) {
    (void)kbd;
    (void)serial;
    (void)time;
    struct overlay *ov = data;

    arm_idle(ov);

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        pressed_key_remove(ov, key);
        if (key == ov->repeat_key) {
            disarm_repeat(ov);
            uint32_t next_key = latest_repeatable_key(ov);
            if (next_key != 0)
                arm_repeat(ov, next_key);
        }
        return;
    }

    pressed_key_add(ov, key);
    handle_key_dispatch(ov, key);
}

static void kbd_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial,
                          uint32_t dep, uint32_t lat, uint32_t locked,
                          uint32_t group) {
    (void)kbd;
    (void)serial;
    struct overlay *ov = data;
    if (ov->xkb_state)
        xkb_state_update_mask(ov->xkb_state, dep, lat, locked, 0, 0, group);
}

static void kbd_repeat_info(void *data, struct wl_keyboard *kbd, int32_t rate,
                            int32_t delay) {
    (void)kbd;
    struct overlay *ov = data;
    ov->repeat_rate = rate;
    ov->repeat_delay = delay;
    if (rate <= 0) {
        disarm_repeat(ov);
    } else if (ov->repeat_key != 0) {
        arm_repeat(ov, ov->repeat_key);
    } else {
        uint32_t key = latest_repeatable_key(ov);
        if (key != 0)
            arm_repeat(ov, key);
    }
    log_debug("repeat info: rate=%d delay=%d", rate, delay);
}

static void kbd_leave(void *data, struct wl_keyboard *kbd, uint32_t serial,
                      struct wl_surface *surface) {
    (void)kbd;
    (void)serial;
    (void)surface;
    struct overlay *ov = data;
    ov->pressed_key_count = 0;
    disarm_repeat(ov);
}

static const struct wl_keyboard_listener kbd_listener = {
    .keymap = kbd_keymap,
    .enter = noop,
    .leave = kbd_leave,
    .key = kbd_key,
    .modifiers = kbd_modifiers,
    .repeat_info = kbd_repeat_info,
};

static void save_cursor_position(struct overlay *ov, wl_fixed_t surface_x,
                                 wl_fixed_t surface_y) {
    ov->cursor_x = wl_fixed_to_int(surface_x);
    ov->cursor_y = wl_fixed_to_int(surface_y);
    ov->cursor_position_known = true;
    log_debug("pointer at %d,%d", ov->cursor_x, ov->cursor_y);
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t surface_x, wl_fixed_t surface_y) {
    (void)pointer;
    (void)serial;
    struct overlay *ov = data;
    if (surface == ov->surface)
        save_cursor_position(ov, surface_x, surface_y);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t surface_x,
                           wl_fixed_t surface_y) {
    (void)pointer;
    (void)time;
    save_cursor_position(data, surface_x, surface_y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
    (void)pointer;
    (void)serial;
    struct overlay *ov = data;
    if (surface == ov->surface)
        ov->cursor_position_known = false;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = noop,
    .axis = noop,
    .frame = noop,
    .axis_source = noop,
    .axis_stop = noop,
    .axis_discrete = noop,
};

static void release_keyboard(struct overlay *ov) {
    if (!ov->keyboard)
        return;
    if (wl_keyboard_get_version(ov->keyboard) >=
        WL_KEYBOARD_RELEASE_SINCE_VERSION)
        wl_keyboard_release(ov->keyboard);
    else
        wl_keyboard_destroy(ov->keyboard);
    ov->keyboard = NULL;
}

static void release_pointer(struct overlay *ov) {
    if (!ov->pointer)
        return;
    if (wl_pointer_get_version(ov->pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
        wl_pointer_release(ov->pointer);
    else
        wl_pointer_destroy(ov->pointer);
    ov->pointer = NULL;
    ov->cursor_position_known = false;
}

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps) {
    (void)s;
    struct overlay *ov = data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !ov->keyboard) {
        ov->keyboard = wl_seat_get_keyboard(ov->seat);
        wl_keyboard_add_listener(ov->keyboard, &kbd_listener, ov);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && ov->keyboard) {
        ov->pressed_key_count = 0;
        disarm_repeat(ov);
        release_keyboard(ov);
        /* No binding can release the grab once the seat loses its keyboard. */
        log_warn("the seat lost its keyboard; exiting");
        overlay_stop(ov);
    }

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ov->pointer) {
        ov->pointer = wl_seat_get_pointer(ov->seat);
        wl_pointer_add_listener(ov->pointer, &pointer_listener, ov);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && ov->pointer) {
        release_pointer(ov);
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name = noop,
};

static uint32_t xkb_mods_to_config(struct overlay *ov) {
    uint32_t out = 0;
    if (!ov->xkb_state)
        return 0;

    if (xkb_state_mod_name_is_active(ov->xkb_state, XKB_MOD_NAME_SHIFT,
                                     XKB_STATE_MODS_DEPRESSED))
        out |= MOD_SHIFT;
    if (xkb_state_mod_name_is_active(ov->xkb_state, XKB_MOD_NAME_CTRL,
                                     XKB_STATE_MODS_DEPRESSED))
        out |= MOD_CTRL;
    if (xkb_state_mod_name_is_active(ov->xkb_state, XKB_MOD_NAME_ALT,
                                     XKB_STATE_MODS_DEPRESSED))
        out |= MOD_ALT;
    if (xkb_state_mod_name_is_active(ov->xkb_state, XKB_MOD_NAME_LOGO,
                                     XKB_STATE_MODS_DEPRESSED))
        out |= MOD_SUPER;

    return out;
}

static int32_t get_scale_120(struct overlay *ov) {
    if (ov->frac_scale_v > 0)
        return ov->frac_scale_v;
    if (ov->selected_output && ov->selected_output->scale > 0)
        return ov->selected_output->scale * 120;
    return 120;
}

static void clear_buffer(struct shm_buffer *buffer) {
    cairo_identity_matrix(buffer->cr);
    cairo_set_operator(buffer->cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(buffer->cr, 0, 0, 0, 0);
    cairo_paint(buffer->cr);
}

static void commit_buffer(struct overlay *ov, struct shm_buffer *buffer) {
    buffer->state = BUF_BUSY;
    wl_surface_set_buffer_scale(ov->surface, 1);
    wl_surface_attach(ov->surface, buffer->wl_buf, 0, 0);
    if (ov->viewport)
        wp_viewport_set_destination(ov->viewport, (int32_t)ov->surf_width,
                                    (int32_t)ov->surf_height);
    wl_surface_damage(ov->surface, 0, 0, (int32_t)ov->surf_width,
                      (int32_t)ov->surf_height);
    wl_surface_commit(ov->surface);
}

static bool map_transparent_surface(struct overlay *ov) {
    if (!ov->configured || ov->surf_width == 0 || ov->surf_height == 0)
        return false;

    struct shm_buffer *buffer =
        buf_get(ov->shm, &ov->pool, ov->surf_width, ov->surf_height);
    if (!buffer)
        return false;

    clear_buffer(buffer);
    commit_buffer(ov, buffer);
    return true;
}

static void send_frame(struct overlay *ov) {
    if (!ov->configured || !ov->rs)
        return;
    if (ov->dispatching_key) {
        ov->redraw_pending = true;
        return;
    }

    int32_t scale_120 = get_scale_120(ov);
    uint32_t buffer_width = ov->surf_width * (uint32_t)scale_120 / 120;
    uint32_t buffer_height = ov->surf_height * (uint32_t)scale_120 / 120;

    struct shm_buffer *buffer =
        buf_get(ov->shm, &ov->pool, buffer_width, buffer_height);
    if (!buffer) {
        ov->redraw_pending = true;
        return;
    }

    ov->redraw_pending = false;
    clear_buffer(buffer);
    cairo_scale(buffer->cr, scale_120 / 120.0, scale_120 / 120.0);
    render_grid(ov, buffer->cr, ov->rs);
    commit_buffer(ov, buffer);
}

/* Set the cairo source to a packed 0xRRGGBBAA color, scaling each
 * channel to cairo's 0..1 range. */
static void set_source_color(cairo_t *cr, uint32_t packed) {
    cairo_set_source_rgba(
        cr, ((packed >> 24) & 0xff) / 255.0, ((packed >> 16) & 0xff) / 255.0,
        ((packed >> 8) & 0xff) / 255.0, (packed & 0xff) / 255.0);
}

static void render_grid(struct overlay *ov, cairo_t *cr,
                        struct region_state *rs) {
    int w = rs->current.w;
    int h = rs->current.h;
    if (w > (int)ov->surf_width)
        w = (int)ov->surf_width;
    if (h > (int)ov->surf_height)
        h = (int)ov->surf_height;

    int x = rs->current.x;
    int y = rs->current.y;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x > (int)ov->surf_width - w)
        x = (int)ov->surf_width - w;
    if (y > (int)ov->surf_height - h)
        y = (int)ov->surf_height - h;

    int cols = rs->current.grid_cols;
    int rows = rs->current.grid_rows;

    if (w <= 0 || h <= 0)
        return;

    double line_width = ov->cfg->line_width;
    int line_width_max = w < h ? w : h;
    if (line_width > line_width_max)
        line_width = line_width_max;
    double line_inset = line_width / 2.0;

    set_source_color(cr, ov->cfg->region_bg);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);

    cairo_set_line_width(cr, line_width);
    set_source_color(cr, ov->cfg->grid_color);
    cairo_rectangle(cr, x + line_inset, y + line_inset, w - line_width,
                    h - line_width);
    cairo_stroke(cr);

    for (int c = 1; c < cols; c++) {
        double lx = x + (double)w * c / cols;
        cairo_move_to(cr, lx, y);
        cairo_line_to(cr, lx, y + h);
    }

    for (int r = 1; r < rows; r++) {
        double ly = y + (double)h * r / rows;
        cairo_move_to(cr, x, ly);
        cairo_line_to(cr, x + w, ly);
    }
    cairo_stroke(cr);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)time;
    struct overlay *ov = data;
    wl_callback_destroy(cb);
    ov->frame_cb = NULL;
    send_frame(ov);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void request_frame(struct overlay *ov) {
    if (ov->frame_cb)
        return;
    ov->frame_cb = wl_surface_frame(ov->surface);
    wl_callback_add_listener(ov->frame_cb, &frame_listener, ov);
    wl_surface_commit(ov->surface);
}

static bool required_globals_available(const struct overlay *ov) {
    if (!ov->compositor) {
        log_err("missing wl_compositor");
        return false;
    }
    if (!ov->shm) {
        log_err("missing wl_shm");
        return false;
    }
    if (!ov->layer_shell) {
        log_err("missing zwlr_layer_shell_v1");
        return false;
    }
    if (!ov->vptr_mgr) {
        log_err("missing zwlr_virtual_pointer_manager_v1");
        return false;
    }
    if (!ov->viewporter) {
        log_err("missing wp_viewporter");
        return false;
    }
    if (!ov->seat) {
        log_err("missing wl_seat");
        return false;
    }
    if (!ov->outputs) {
        log_err("missing wl_output");
        return false;
    }
    if (!ov->xdg_out_mgr) {
        /* Without logical geometry there is no layout to place the overlay
         * in, so warps can only assume its output is the whole of it. */
        log_warn("missing zxdg_output_manager_v1: warps assume one output");
    }
    return true;
}

static void log_outputs(const struct overlay *ov) {
    for (const struct output *output = ov->outputs; output;
         output = output->next) {
        log_debug("output %s: %dx%d+%d+%d scale=%d",
                  output_name_or_unknown(output), output->width, output->height,
                  output->x, output->y, output->scale);
    }
}

static bool create_overlay_surface(struct overlay *ov) {
    ov->surface = wl_compositor_create_surface(ov->compositor);
    wl_surface_add_listener(ov->surface, &surface_listener, ov);

    ov->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        ov->layer_shell, ov->surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "waynav");
    zwlr_layer_surface_v1_add_listener(ov->layer_surface, &layer_listener, ov);
    zwlr_layer_surface_v1_set_exclusive_zone(ov->layer_surface, -1);
    zwlr_layer_surface_v1_set_anchor(ov->layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
    zwlr_layer_surface_v1_set_keyboard_interactivity(ov->layer_surface, true);

    if (ov->frac_scale_mgr) {
        ov->frac_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            ov->frac_scale_mgr, ov->surface);
        wp_fractional_scale_v1_add_listener(ov->frac_scale, &frac_listener, ov);
    }
    if (ov->viewporter)
        ov->viewport = wp_viewporter_get_viewport(ov->viewporter, ov->surface);

    /* Keep pointer input on the windows beneath the overlay. */
    ov->input_region = wl_compositor_create_region(ov->compositor);
    wl_surface_set_input_region(ov->surface, ov->input_region);
    wl_surface_commit(ov->surface);

    wl_display_roundtrip(ov->display);
    if (!map_transparent_surface(ov)) {
        log_err("failed to map overlay surface");
        return false;
    }

    /* Wait for wl_surface.enter to identify the compositor-selected output. */
    wl_display_roundtrip(ov->display);
    if (!ov->selected_output) {
        log_err("compositor did not select an output for the overlay");
        return false;
    }
    return true;
}

struct overlay *overlay_create(void) {
    struct overlay *ov = calloc(1, sizeof(*ov));
    if (!ov)
        return NULL;

    ov->pool.overlay = ov;
    ov->repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    ov->idle_fd = -1;
    ov->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ov->xkb_ctx)
        goto fail;

    ov->display = wl_display_connect(NULL);
    if (!ov->display) {
        log_err("failed to connect to Wayland compositor");
        goto fail;
    }

    ov->registry = wl_display_get_registry(ov->display);
    wl_registry_add_listener(ov->registry, &registry_listener, ov);
    wl_display_roundtrip(ov->display);

    if (!required_globals_available(ov))
        goto fail;

    wl_seat_add_listener(ov->seat, &seat_listener, ov);
    wl_display_roundtrip(ov->display);
    log_outputs(ov);

    if (!create_overlay_surface(ov))
        goto fail;

    /* Leave the output hint unset: river ignores it, while sway honors it.
     * An unmapped pointer accepts layout coordinates on both compositors. */
    ov->vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
        ov->vptr_mgr, ov->seat);

    log_info("overlay created: %ux%u on %s", ov->surf_width, ov->surf_height,
             output_name_or_unknown(ov->selected_output));
    return ov;

fail:
    overlay_destroy(ov);
    return NULL;
}

static void destroy_layer_shell(struct zwlr_layer_shell_v1 *layer_shell) {
    if (!layer_shell)
        return;

    if (zwlr_layer_shell_v1_get_version(layer_shell) >=
        ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION) {
        zwlr_layer_shell_v1_destroy(layer_shell);
    } else {
        wl_proxy_destroy((struct wl_proxy *)layer_shell);
    }
}

static void destroy_input_state(struct overlay *ov) {
    if (ov->rs)
        overlay_stop_drag(ov, ov->rs);
    if (ov->repeat_fd >= 0)
        close(ov->repeat_fd);
    if (ov->idle_fd >= 0)
        close(ov->idle_fd);

    if (ov->vptr)
        zwlr_virtual_pointer_v1_destroy(ov->vptr);

    release_keyboard(ov);
    release_pointer(ov);
    free(ov->pressed_keys);
    free(ov->deferred_keys);
    if (ov->xkb_state)
        xkb_state_unref(ov->xkb_state);
    if (ov->xkb_keymap)
        xkb_keymap_unref(ov->xkb_keymap);
    if (ov->xkb_ctx)
        xkb_context_unref(ov->xkb_ctx);
}

static void destroy_surface_state(struct overlay *ov) {
    if (ov->frame_cb)
        wl_callback_destroy(ov->frame_cb);
    if (ov->viewport)
        wp_viewport_destroy(ov->viewport);
    if (ov->frac_scale)
        wp_fractional_scale_v1_destroy(ov->frac_scale);

    buf_destroy(&ov->pool.bufs[0]);
    buf_destroy(&ov->pool.bufs[1]);

    if (ov->layer_surface)
        zwlr_layer_surface_v1_destroy(ov->layer_surface);
    if (ov->surface)
        wl_surface_destroy(ov->surface);
    if (ov->input_region)
        wl_region_destroy(ov->input_region);
}

static void release_seat(struct overlay *ov) {
    if (!ov->seat)
        return;
    if (wl_seat_get_version(ov->seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
        wl_seat_release(ov->seat);
    else
        wl_seat_destroy(ov->seat);
}

static void destroy_globals(struct overlay *ov) {
    if (ov->frac_scale_mgr)
        wp_fractional_scale_manager_v1_destroy(ov->frac_scale_mgr);
    if (ov->viewporter)
        wp_viewporter_destroy(ov->viewporter);
    if (ov->vptr_mgr)
        zwlr_virtual_pointer_manager_v1_destroy(ov->vptr_mgr);
    while (ov->outputs) {
        struct output *output = ov->outputs;
        ov->outputs = output->next;
        output_destroy(output);
    }
    if (ov->xdg_out_mgr)
        zxdg_output_manager_v1_destroy(ov->xdg_out_mgr);
    destroy_layer_shell(ov->layer_shell);
    release_seat(ov);
    if (ov->shm)
        wl_shm_destroy(ov->shm);
    if (ov->compositor)
        wl_compositor_destroy(ov->compositor);
    if (ov->registry)
        wl_registry_destroy(ov->registry);
    if (ov->display) {
        wl_display_roundtrip(ov->display);
        wl_display_disconnect(ov->display);
    }
}

void overlay_destroy(struct overlay *ov) {
    if (!ov)
        return;

    destroy_input_state(ov);
    destroy_surface_state(ov);
    destroy_globals(ov);
    free(ov);
}

void overlay_redraw(struct overlay *ov, struct region_state *rs) {
    if (!ov)
        return;
    ov->rs = rs;
    request_frame(ov);
}

int overlay_get_width(const struct overlay *ov) {
    if (!ov)
        return 0;
    if (ov->surf_width > 0)
        return (int)ov->surf_width;
    if (ov->selected_output)
        return ov->selected_output->width;
    return 0;
}

int overlay_get_height(const struct overlay *ov) {
    if (!ov)
        return 0;
    if (ov->surf_height > 0)
        return (int)ov->surf_height;
    if (ov->selected_output)
        return ov->selected_output->height;
    return 0;
}

/* Dispatch until wl_pointer.enter lands, or the budget runs out. Returns <0
 * on a display error, matching wl_display_roundtrip. */
static int pump_until_cursor_known(struct overlay *ov, int timeout_ms) {
    struct pollfd pfd = {.fd = wl_display_get_fd(ov->display),
                         .events = POLLIN};
    int waited = 0;

    for (;;) {
        if (wl_display_roundtrip(ov->display) < 0)
            return -1;
        if (ov->cursor_position_known || waited >= timeout_ms)
            return 0;

        int step = 5;
        if (poll(&pfd, 1, step) < 0 && errno != EINTR)
            return -1;
        waited += step;
    }
}

bool overlay_get_cursor_position(struct overlay *ov, int *x, int *y) {
    if (!ov || !x || !y)
        return false;

    if (ov->pointer && ov->surface) {
        /* Wayland has no global pointer-position query. Give the overlay
         * pointer focus briefly so wl_pointer.enter supplies the current
         * position; the empty input region makes cached positions stale. */
        ov->cursor_position_known = false;
        wl_surface_set_input_region(ov->surface, NULL);
        wl_surface_commit(ov->surface);
        int capture_result = wl_display_roundtrip(ov->display);

        /* Widening the region is not by itself an event the compositor
         * recomputes pointer focus on -- that happens when the pointer
         * moves -- so enter goes unsent and the position stays unknown. A
         * zero-length relative motion is a move that moves nothing. */
        if (capture_result >= 0 && !ov->cursor_position_known && ov->vptr) {
            zwlr_virtual_pointer_v1_motion(ov->vptr, 0, wl_fixed_from_int(0),
                                           wl_fixed_from_int(0));
            zwlr_virtual_pointer_v1_frame(ov->vptr);
            capture_result = pump_until_cursor_known(ov, 100);
        }

        bool captured = ov->cursor_position_known;
        int captured_x = ov->cursor_x;
        int captured_y = ov->cursor_y;

        wl_surface_set_input_region(ov->surface, ov->input_region);
        wl_surface_commit(ov->surface);
        int restore_result = wl_display_roundtrip(ov->display);

        if (capture_result < 0 || restore_result < 0)
            return false;
        if (captured) {
            *x = captured_x;
            *y = captured_y;
            return true;
        }
    }

    if (!ov->cursor_position_known)
        return false;

    *x = ov->cursor_x;
    *y = ov->cursor_y;
    return true;
}

void overlay_stop(struct overlay *ov) {
    if (!ov)
        return;
    ov->running = false;
    ov->stop_requested = true;
}

static int display_error(struct overlay *ov, const char *action) {
    int error = wl_display_get_error(ov->display);
    if (error)
        log_err("wayland display error during %s: %s", action, strerror(error));
    else
        log_err("wayland %s failed: %s", action, strerror(errno));
    return -1;
}

static int flush_display(struct overlay *ov) {
    while (wl_display_flush(ov->display) < 0) {
        if (errno != EAGAIN)
            return display_error(ov, "flush");

        struct pollfd fd = {
            .fd = wl_display_get_fd(ov->display),
            .events = POLLOUT,
        };
        int poll_result;
        do {
            poll_result = poll(&fd, 1, -1);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0)
            return display_error(ov, "poll");
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return display_error(ov, "poll");
        }
    }
    return 0;
}

static int prepare_display_read(struct overlay *ov) {
    while (wl_display_prepare_read(ov->display) != 0) {
        if (wl_display_dispatch_pending(ov->display) < 0)
            return display_error(ov, "dispatch");
        if (!ov->running)
            return 1;
    }
    if (flush_display(ov) != 0) {
        wl_display_cancel_read(ov->display);
        return -1;
    }
    if (!ov->running) {
        wl_display_cancel_read(ov->display);
        return 1;
    }
    return 0;
}

static int read_display_events(struct overlay *ov, short revents) {
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        wl_display_cancel_read(ov->display);
        errno = EIO;
        return display_error(ov, "poll");
    }
    if (revents & POLLIN) {
        if (wl_display_read_events(ov->display) < 0)
            return display_error(ov, "read");
    } else {
        wl_display_cancel_read(ov->display);
    }
    if (wl_display_dispatch_pending(ov->display) < 0)
        return display_error(ov, "dispatch");
    return 0;
}

static void dispatch_repeat(struct overlay *ov) {
    uint64_t expirations;
    if (read(ov->repeat_fd, &expirations, sizeof(expirations)) <= 0 ||
        ov->repeat_key == 0 || !ov->xkb_state || !ov->cfg)
        return;

    uint32_t mods = xkb_mods_to_config(ov);
    const struct binding *binding =
        find_key_binding(ov, ov->repeat_key + 8, mods);
    run_key_binding(ov, ov->repeat_key, binding, false);
}

void overlay_release_keyboard(struct overlay *ov) {
    if (!ov || !ov->layer_surface || ov->keyboard_released)
        return;

    log_debug("releasing the keyboard grab");

    zwlr_layer_surface_v1_set_keyboard_interactivity(ov->layer_surface, false);
    wl_surface_commit(ov->surface);
    /* The roundtrip is the point: it is what makes the focus change, and the
     * selection offer that follows it, reach the client before whatever this
     * was called ahead of. */
    wl_display_roundtrip(ov->display);
    ov->keyboard_released = true;
}

void overlay_set_idle_timeout(struct overlay *ov, int secs) {
    if (!ov || secs <= 0)
        return;

    if (ov->idle_fd < 0) {
        ov->idle_fd =
            timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (ov->idle_fd < 0) {
            log_warn("no idle timeout: timerfd_create failed: %s",
                     strerror(errno));
            return;
        }
    }

    ov->idle_secs = secs;
}

bool overlay_idled(const struct overlay *ov) {
    return ov && ov->idled;
}

static void dispatch_idle(struct overlay *ov) {
    uint64_t expirations;
    if (read(ov->idle_fd, &expirations, sizeof(expirations)) <= 0)
        return;

    log_warn("no keyboard input for %ds; exiting", ov->idle_secs);
    overlay_stop_drag(ov, ov->rs);
    ov->idled = true;
    overlay_stop(ov);
}

int overlay_run(struct overlay *ov, struct config *cfg,
                struct region_state *rs) {
    if (!ov)
        return -1;
    ov->cfg = cfg;
    ov->rs = rs;
    if (ov->stop_requested)
        return 0;
    ov->running = true;

    if (ov->xkb_keymap)
        config_resolve_keycodes(cfg, ov->xkb_keymap);

    send_frame(ov);

    int wl_fd = wl_display_get_fd(ov->display);

    /* The Wayland connection, plus whichever timers were created: neither
     * timerfd is guaranteed, so the indices are assigned rather than fixed. */
    struct pollfd fds[3];
    int nfds = 0;
    int repeat_index = -1;
    int idle_index = -1;

    fds[nfds].fd = wl_fd;
    fds[nfds].events = POLLIN;
    nfds++;

    if (ov->repeat_fd >= 0) {
        repeat_index = nfds;
        fds[nfds].fd = ov->repeat_fd;
        fds[nfds].events = POLLIN;
        nfds++;
    }

    if (ov->idle_fd >= 0) {
        idle_index = nfds;
        fds[nfds].fd = ov->idle_fd;
        fds[nfds].events = POLLIN;
        nfds++;
        arm_idle(ov);
    }

    while (ov->running) {
        int prepare_result = prepare_display_read(ov);
        if (prepare_result < 0)
            return -1;
        if (prepare_result > 0)
            break;

        if (poll(fds, (nfds_t)nfds, -1) < 0) {
            wl_display_cancel_read(ov->display);
            if (errno == EINTR)
                continue;
            return display_error(ov, "poll");
        }

        if (read_display_events(ov, fds[0].revents) != 0)
            return -1;
        if (ov->running && repeat_index >= 0 &&
            (fds[repeat_index].revents & POLLIN))
            dispatch_repeat(ov);

        if (ov->running && idle_index >= 0 &&
            (fds[idle_index].revents & POLLIN))
            dispatch_idle(ov);
    }

    return 0;
}

/* The union of every output's logical geometry. */
static void layout_box(const struct overlay *ov, struct layout_rect *box) {
    bool found = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    for (const struct output *o = ov->outputs; o; o = o->next) {
        /* Geometry arrives on xdg-output; without that protocol, or before
         * its events land, an output contributes nothing to the union. */
        if (o->width <= 0 || o->height <= 0)
            continue;
        if (!found) {
            x0 = o->x;
            y0 = o->y;
            x1 = o->x + o->width;
            y1 = o->y + o->height;
            found = true;
            continue;
        }
        if (o->x < x0)
            x0 = o->x;
        if (o->y < y0)
            y0 = o->y;
        if (o->x + o->width > x1)
            x1 = o->x + o->width;
        if (o->y + o->height > y1)
            y1 = o->y + o->height;
    }

    if (!found) {
        /* No logical geometry to place the overlay in: treat its own surface
         * as the whole layout, which is what a single-output setup is. */
        box->x = 0;
        box->y = 0;
        box->w = overlay_get_width(ov);
        box->h = overlay_get_height(ov);
        return;
    }

    box->x = x0;
    box->y = y0;
    box->w = x1 - x0;
    box->h = y1 - y0;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

void vptr_warp(struct overlay *ov, int x, int y) {
    if (!ov || !ov->vptr)
        return;

    int clamped_x = clamp_int(x, 0, overlay_get_width(ov));
    int clamped_y = clamp_int(y, 0, overlay_get_height(ov));
    x = clamped_x;
    y = clamped_y;

    /* motion_absolute is expressed in the coordinate space the compositor
     * maps the virtual pointer to, and that is the whole output layout: a
     * suggested output is only a hint, and river ignores it outright. So the
     * overlay-local point is lifted into layout space and the extent is the
     * layout's. Sending output-local coordinates instead stretches every
     * warp across the layout -- on two side-by-side outputs it doubles x --
     * and makes the overlay on a secondary output warp onto the primary. */
    struct layout_rect lb;
    layout_box(ov, &lb);
    if (lb.w <= 0 || lb.h <= 0) {
        log_warn("vptr warp: no layout geometry to warp within");
        return;
    }

    int lx = x;
    int ly = y;
    if (ov->selected_output && ov->selected_output->width > 0 &&
        ov->selected_output->height > 0) {
        lx += ov->selected_output->x;
        ly += ov->selected_output->y;
    }
    lx = clamp_int(lx - lb.x, 0, lb.w);
    ly = clamp_int(ly - lb.y, 0, lb.h);

    log_debug("vptr warp: %d,%d on %s -> %d,%d in %dx%d layout", x, y,
              output_name_or_unknown(ov->selected_output), lx, ly, lb.w, lb.h);

    zwlr_virtual_pointer_v1_motion_absolute(ov->vptr, 0, (uint32_t)lx,
                                            (uint32_t)ly, (uint32_t)lb.w,
                                            (uint32_t)lb.h);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    ov->cursor_x = clamped_x;
    ov->cursor_y = clamped_y;
    ov->cursor_position_known = true;
    wl_display_flush(ov->display);
}

/* Map keynav button numbers to Linux input event codes. */
static uint32_t keynav_btn(int button) {
    switch (button) {
    case 1:
        return BTN_LEFT;
    case 2:
        return BTN_MIDDLE;
    case 3:
        return BTN_RIGHT;
    default:
        return 0;
    }
}

void vptr_click(struct overlay *ov, int button) {
    if (!ov || !ov->vptr)
        return;

    if (button == 4 || button == 5) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
            log_warn("scroll: clock_gettime failed: %s", strerror(errno));
            return;
        }
        uint32_t time_msec = (uint32_t)((uint64_t)now.tv_sec * 1000 +
                                        (uint64_t)now.tv_nsec / 1000000);
        int32_t steps = button == 5 ? 1 : -1;
        zwlr_virtual_pointer_v1_axis_discrete(
            ov->vptr, time_msec, WL_POINTER_AXIS_VERTICAL_SCROLL,
            wl_fixed_from_int(15 * steps), steps);
        zwlr_virtual_pointer_v1_axis_source(ov->vptr,
                                            WL_POINTER_AXIS_SOURCE_WHEEL);
        zwlr_virtual_pointer_v1_frame(ov->vptr);
        wl_display_flush(ov->display);
        return;
    }

    uint32_t btn = keynav_btn(button);
    if (!btn)
        return;

    zwlr_virtual_pointer_v1_button(ov->vptr, 0, btn,
                                   WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(ov->vptr);

    zwlr_virtual_pointer_v1_button(ov->vptr, 0, btn,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    wl_display_flush(ov->display);
}

void vptr_button_down(struct overlay *ov, int button) {
    if (!ov || !ov->vptr)
        return;
    uint32_t btn = keynav_btn(button);
    if (!btn)
        return;

    zwlr_virtual_pointer_v1_button(ov->vptr, 0, btn,
                                   WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    wl_display_flush(ov->display);
}

void vptr_button_up(struct overlay *ov, int button) {
    if (!ov || !ov->vptr)
        return;
    uint32_t btn = keynav_btn(button);
    if (!btn)
        return;

    zwlr_virtual_pointer_v1_button(ov->vptr, 0, btn,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    wl_display_flush(ov->display);
}

void overlay_stop_drag(struct overlay *ov, struct region_state *rs) {
    if (!rs || !rs->dragging)
        return;

    log_debug("drag end button=%d", rs->drag_button);
    vptr_button_up(ov, rs->drag_button);
    rs->dragging = false;
    rs->drag_button = 0;
}
