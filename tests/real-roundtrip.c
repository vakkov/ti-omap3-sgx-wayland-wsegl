/* Check the legacy-compatible proxy construction against real libwayland,
 * including a concurrent reader of the application's default event queue. */
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <wayland-server.h>
#include "../wsegl/wayland-sync.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); \
} } while (0)

struct test_server {
    struct wl_display *display;
    struct wl_listener destroyed;
    bool done;
};

static void client_destroyed(struct wl_listener *listener, void *data)
{
    struct test_server *server = wl_container_of(listener, server, destroyed);
    server->done = true;
}

static void *run_server(void *data)
{
    struct test_server *server = data;
    while (!server->done) {
        CHECK(wl_event_loop_dispatch(wl_display_get_event_loop(server->display), -1) >= 0);
        wl_display_flush_clients(server->display);
    }
    return NULL;
}

static void *run_default_queue(void *data)
{
    for (int i = 0; i < 200; ++i)
        CHECK(wl_display_roundtrip(data) >= 0);
    return NULL;
}

static void unused_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{ CHECK(false); }

static void global(void *data, struct wl_registry *registry, uint32_t name,
                    const char *interface, uint32_t version)
{
    CHECK(strcmp(interface, "wl_compositor") == 0);
    ++*(int *)data;
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener listener = { global, global_remove };

int main(void)
{
    int sockets[2], globals = 0;
    pthread_t server_thread, reader_thread;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct test_server server = { .display = wl_display_create() };
    CHECK(server.display);
    CHECK(wl_global_create(server.display, &wl_compositor_interface, 1, NULL, unused_bind));
    struct wl_client *client = wl_client_create(server.display, sockets[0]);
    CHECK(client);
    server.destroyed.notify = client_destroyed;
    wl_client_add_destroy_listener(client, &server.destroyed);
    CHECK(pthread_create(&server_thread, NULL, run_server, &server) == 0);

    struct {
        struct wl_display *display;
        struct wl_event_queue *queue;
        struct wl_registry *registry;
    } display = { .display = wl_display_connect_to_fd(sockets[1]) };
    CHECK(display.display);
    display.queue = wl_display_create_queue(display.display);
    CHECK(display.queue);
    CHECK(pthread_create(&reader_thread, NULL, run_default_queue, display.display) == 0);
    display.registry = (void *)wayland_create_proxy((void *)display.display,
                                                    &wl_registry_interface, display.queue);
    CHECK(display.registry);
    CHECK(wl_registry_add_listener(display.registry, &listener, &globals) == 0);
    wl_proxy_marshal((void *)display.display, WL_DISPLAY_GET_REGISTRY, display.registry);
    for (int i = 0; i < 200; ++i)
        CHECK(wayland_roundtrip(display.display, display.queue) >= 0);
    CHECK(globals == 1);
    CHECK(pthread_join(reader_thread, NULL) == 0);
    wl_registry_destroy(display.registry);
    wl_event_queue_destroy(display.queue);
    wl_display_disconnect(display.display);
    CHECK(pthread_join(server_thread, NULL) == 0);
    wl_display_destroy(server.display);
    puts("real Wayland concurrent roundtrip test passed");
    return 0;
}
