#ifndef WAYLAND_WSEGL_SYNC_H
#define WAYLAND_WSEGL_SYNC_H

#include <wayland-client.h>

/* Set the queue before sending the constructor request.  This avoids a reply
 * racing with wl_proxy_set_queue(), without requiring newer proxy wrappers. */
static struct wl_proxy *wayland_create_proxy(struct wl_proxy *parent,
                                            const struct wl_interface *interface,
                                            struct wl_event_queue *queue)
{
    struct wl_proxy *proxy = wl_proxy_create(parent, interface);
    if (proxy)
        wl_proxy_set_queue(proxy, queue);
    return proxy;
}

static void roundtrip_callback(void *data, struct wl_callback *callback, uint32_t serial)
{
    int *done = data;
    *done = 1;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener roundtrip_listener = {
    roundtrip_callback
};

static int wayland_roundtrip(struct wl_display *display, struct wl_event_queue *queue)
{
    struct wl_callback *callback;
    int done = 0, ret = 0;

    callback = (struct wl_callback *)wayland_create_proxy(
        (struct wl_proxy *)display, &wl_callback_interface, queue);
    if (!callback)
        return -1;
    if (wl_callback_add_listener(callback, &roundtrip_listener, &done) < 0) {
        wl_callback_destroy(callback);
        return -1;
    }
    wl_proxy_marshal((struct wl_proxy *)display, WL_DISPLAY_SYNC, callback);
    while (ret >= 0 && !done)
        ret = wl_display_dispatch_queue(display, queue);

    if (!done)
        wl_callback_destroy(callback);
    return ret;
}

#endif
