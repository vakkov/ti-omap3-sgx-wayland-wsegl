/* Exercise the actual WSEGL entry points with deterministic Wayland event
 * ordering and fake PVR allocations.  No GPU or compositor is required. */
#include <stdarg.h>
#include <stdint.h>
#include "legacy-wayland.h"
#include "../libwayland-egl/wayland-egl.c"
#include "../wsegl/waylandwsegl.c"

/* Keep test checks active when testing production code with NDEBUG. */
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); \
} } while (0)

enum event_type { GLOBAL, DONE, RELEASE };
struct event { enum event_type type; struct wl_proxy *proxy; bool pending; };
struct wl_event_queue {
    struct event events[128];
    int count;
};
struct wl_proxy {
    const struct wl_interface *interface;
    struct wl_event_queue *queue;
    void (**listener)(void);
    void *data;
};

static struct wl_event_queue *queues[16];
static int queue_count, proxy_count, memory_count;
static int dispatch_count, commit_count, flush_count, detach_count;
static int export_count, blit_count, update_count;
static bool fail_dispatch, fail_sync, fail_export, fail_blit, fail_flush;
static struct wl_proxy *sync_proxy;
static struct wl_egl_window *freeing_window;

static void enqueue(struct wl_proxy *proxy, enum event_type type, bool pending)
{
    CHECK(proxy->queue);
    struct wl_event_queue *queue = proxy->queue;
    CHECK(queue->count < 128);
    queue->events[queue->count++] = (struct event){ type, proxy, pending };
}

struct wl_proxy *wl_proxy_create(struct wl_proxy *parent, const struct wl_interface *interface)
{
    struct wl_proxy *proxy = calloc(1, sizeof *proxy);
    CHECK(proxy);
    proxy->interface = interface;
    proxy->queue = parent ? parent->queue : NULL;
    ++proxy_count;
    return proxy;
}

void wl_proxy_destroy(struct wl_proxy *proxy)
{
    for (int q = 0; q < queue_count; ++q) {
        for (int i = 0; i < queues[q]->count; ++i) {
            if (queues[q]->events[i].proxy == proxy)
                queues[q]->events[i].proxy = NULL;
        }
    }
    if (sync_proxy == proxy)
        sync_proxy = NULL;
    --proxy_count;
    free(proxy);
}

void wl_proxy_set_queue(struct wl_proxy *proxy, struct wl_event_queue *queue)
{
    proxy->queue = queue;
}

uint32_t wl_proxy_get_version(struct wl_proxy *proxy) { return 1; }

int wl_proxy_add_listener(struct wl_proxy *proxy, void (**listener)(void), void *data)
{
    CHECK(!proxy->listener);
    proxy->listener = listener;
    proxy->data = data;
    return 0;
}

void wl_proxy_marshal(struct wl_proxy *proxy, uint32_t opcode, ...)
{
    va_list args;
    va_start(args, opcode);
    struct wl_proxy *child = va_arg(args, struct wl_proxy *);
    va_end(args);
    /* No request can expose a callback before its queue and listener exist. */
    CHECK(child && child->queue && child->listener);
    if (proxy->interface == &wl_display_interface) {
        if (opcode == WL_DISPLAY_GET_REGISTRY)
            enqueue(child, GLOBAL, false);
        else {
            CHECK(opcode == WL_DISPLAY_SYNC);
            sync_proxy = child;
            enqueue(child, DONE, false);
        }
    } else {
        CHECK(proxy->interface == &wl_surface_interface && opcode == WL_SURFACE_FRAME);
    }
}

struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
                                        const struct wl_interface *interface,
                                        uint32_t version, uint32_t flags, ...)
{
    if (interface)
        return wl_proxy_create(proxy, interface);
    if (flags & WL_MARSHAL_FLAG_DESTROY) {
        wl_proxy_destroy(proxy);
        return NULL;
    }
    if (proxy->interface == &wl_surface_interface) {
        if (opcode == WL_SURFACE_COMMIT)
            ++commit_count;
        if (opcode == WL_SURFACE_ATTACH) {
            va_list args;
            va_start(args, flags);
            if (!va_arg(args, struct wl_buffer *))
                ++detach_count;
            va_end(args);
        }
    }
    return NULL;
}

struct wl_event_queue *wl_display_create_queue(struct wl_display *display)
{
    CHECK(queue_count < 16);
    struct wl_event_queue *queue = calloc(1, sizeof *queue);
    CHECK(queue);
    queues[queue_count++] = queue;
    return queue;
}

void wl_event_queue_destroy(struct wl_event_queue *queue)
{
    for (int i = 0; i < queue->count; ++i)
        CHECK(!queue->events[i].proxy);
    for (int i = 0; i < queue_count; ++i) {
        if (queues[i] == queue) {
            queues[i] = queues[--queue_count];
            free(queue);
            return;
        }
    }
    CHECK(false);
}

static int dispatch(struct wl_event_queue *queue, bool pending_only)
{
    CHECK(queue);
    if (fail_dispatch)
        return -1;
    for (int i = 0; i < queue->count; ++i) {
        struct event event = queue->events[i];
        if (!event.proxy || (pending_only && !event.pending))
            continue;
        if (event.proxy == sync_proxy && fail_sync)
            return -1;
        memmove(&queue->events[i], &queue->events[i + 1],
                (--queue->count - i) * sizeof event);
        if (event.type == GLOBAL) {
            const struct wl_registry_listener *listener = (void *)event.proxy->listener;
            listener->global(event.proxy->data, (void *)event.proxy, 1, "sgx_wlegl", 1);
        } else if (event.type == DONE) {
            const struct wl_callback_listener *listener = (void *)event.proxy->listener;
            listener->done(event.proxy->data, (void *)event.proxy, 0);
        } else {
            const struct wl_buffer_listener *listener = (void *)event.proxy->listener;
            listener->release(event.proxy->data, (void *)event.proxy);
        }
        return 1;
    }
    /* An unexpected blocking wait fails the test instead of hanging it. */
    CHECK(pending_only);
    return 0;
}

int wl_display_dispatch_queue(struct wl_display *display, struct wl_event_queue *queue)
{
    ++dispatch_count;
    return dispatch(queue, false);
}

int wl_display_dispatch_queue_pending(struct wl_display *display, struct wl_event_queue *queue)
{
    int total = 0, result;
    while ((result = dispatch(queue, true)) > 0)
        total += result;
    return result < 0 ? -1 : total;
}

int wl_display_flush(struct wl_display *display)
{
    ++flush_count;
    if (fail_flush) { errno = EPIPE; return -1; }
    return 0;
}

int PVR2DEnumerateDevices(PVR2DDEVICEINFO *devices)
{
    if (!devices) return 1;
    devices[0].ulDevID = 1;
    return PVR2D_OK;
}
PVR2DERROR PVR2DCreateDeviceContext(unsigned long id, PVR2DCONTEXTHANDLE *context, unsigned long flags)
{ *context = (void *)1; return PVR2D_OK; }
PVR2DERROR PVR2DDestroyDeviceContext(PVR2DCONTEXTHANDLE context) { return PVR2D_OK; }
PVR2DERROR PVR2DMemAlloc(PVR2DCONTEXTHANDLE context, unsigned long size, unsigned long alignment,
                        unsigned long flags, PVR2DMEMINFO **memory)
{
    *memory = calloc(1, sizeof **memory);
    CHECK(*memory);
    ++memory_count;
    return PVR2D_OK;
}
PVR2DERROR PVR2DMemFree(PVR2DCONTEXTHANDLE context, PVR2DMEMINFO *memory)
{
    if (freeing_window && !fail_dispatch) {
        for (int i = 0; i < WAYLANDWSEGL_MAX_BACK_BUFFERS; ++i)
            CHECK(!freeing_window->buffer_busy[i]);
    }
    --memory_count;
    free(memory);
    return PVR2D_OK;
}
PVR2DERROR PVR2DMemExport(PVR2DCONTEXTHANDLE context, unsigned long flags, PVR2DMEMINFO *memory, void **handle)
{
    *handle = (void *)(uintptr_t)++export_count;
    return fail_export ? PVR2DERROR_MEMORY_UNAVAILABLE : PVR2D_OK;
}
PVR2DERROR PVR2DMemMap(PVR2DCONTEXTHANDLE context, unsigned long flags, void *handle, PVR2DMEMINFO **memory)
{ return PVR2DMemAlloc(context, 0, 0, 0, memory); }
PVR2DERROR PVR2DGetDeviceInfo(PVR2DCONTEXTHANDLE context, PVR2DDISPLAYINFO *info)
{ memset(info, 0, sizeof *info); return PVR2D_OK; }
PVR2DERROR PVR2DGetFrameBuffer(PVR2DCONTEXTHANDLE context, int heap, PVR2DMEMINFO **memory)
{ static PVR2DMEMINFO front; *memory = &front; return PVR2D_OK; }
PVR2DERROR PVR2DBlt(PVR2DCONTEXTHANDLE context, PVR2DBLTINFO *info)
{ ++blit_count; return fail_blit ? PVR2DERROR_INVALID_PARAMETER : PVR2D_OK; }
PVR2DERROR PVR2DQueryBlitsComplete(PVR2DCONTEXTHANDLE context, const PVR2DMEMINFO *memory, unsigned int wait)
{ return PVR2D_OK; }
PVR2DERROR PVR2DPresentFlip(PVR2DCONTEXTHANDLE context, PVR2DFLIPCHAINHANDLE chain, PVR2DMEMINFO *memory, long id)
{ CHECK(false); return PVR2D_OK; }
PVR2DERROR PVR2DCreateFlipChain(PVR2DCONTEXTHANDLE context, unsigned long flags, unsigned long count,
    unsigned long width, unsigned long height, PVR2DFORMAT format, long *stride,
    unsigned long *id, PVR2DFLIPCHAINHANDLE *chain)
{ CHECK(false); return PVR2D_OK; }
PVR2DERROR PVR2DGetFlipChainBuffers(PVR2DCONTEXTHANDLE context, PVR2DFLIPCHAINHANDLE chain,
                                  unsigned long *count, PVR2DMEMINFO **memory)
{ CHECK(false); return PVR2D_OK; }

int ioctl(int fd, unsigned long request, ...)
{
    CHECK(request == OMAPFB_UPDATE_WINDOW);
    ++update_count;
    return 0;
}

static struct wl_egl_display *create_display(struct wl_display **native)
{
    *native = (void *)wl_proxy_create(NULL, &wl_display_interface);
    WSEGLDisplayHandle display = NULL;
    const WSEGLCaps *caps;
    WSEGLConfig *configs;
    CHECK(wseglInitializeDisplay(*native, &display, &caps, &configs) == WSEGL_SUCCESS);
    CHECK(display && !sync_proxy);
    return display;
}

static struct wl_egl_window *create_window(struct wl_egl_display *display)
{
    struct wl_surface *surface = (void *)wl_proxy_create((void *)display->display, &wl_surface_interface);
    struct wl_egl_window *window = wl_egl_window_create(surface, 64, 32);
    WSEGLDrawableHandle drawable;
    WSEGLRotationAngle rotation;
    CHECK(wseglCreateWindowDrawable(display, &display->wseglDisplayConfigs[0],
                                    &drawable, window, &rotation) == WSEGL_SUCCESS);
    CHECK(drawable == window && window->queue && window->swap_interval == 1);
    return window;
}

static void frame_done(struct wl_egl_window *window)
{ CHECK(window->frame_callback); enqueue((void *)window->frame_callback, DONE, false); }
static void release_buffer(struct wl_egl_window *window, int index)
{ enqueue((void *)window->drmbuffers[index], RELEASE, false); }

static void destroy_window(struct wl_egl_window *window)
{
    for (int i = 0; i < WAYLANDWSEGL_MAX_BACK_BUFFERS; ++i) {
        if (window->buffer_busy[i])
            release_buffer(window, i);
    }
    freeing_window = window;
    CHECK(wseglDeleteDrawable(window) == (fail_dispatch ? WSEGL_BAD_NATIVE_WINDOW : WSEGL_SUCCESS));
    freeing_window = NULL;
    CHECK(!window->frame_callback && !window->queue);
    wl_surface_destroy(window->surface);
    wl_egl_window_destroy(window);
}

static void destroy_display(struct wl_egl_display *display, struct wl_display *native)
{
    CHECK(wseglCloseDisplay(display) == WSEGL_SUCCESS);
    wl_proxy_destroy((void *)native);
    CHECK(!memory_count && !proxy_count && !queue_count);
}

static void test_roundtrip(void)
{
    struct wl_display *native;
    int before = dispatch_count;
    struct wl_egl_display *display = create_display(&native);
    /* A positive registry-event count must not terminate the sync wait. */
    CHECK(dispatch_count == before + 2);
    fail_sync = true;
    CHECK(wayland_roundtrip(display->display, display->queue) == -1);
    CHECK(!sync_proxy);
    fail_sync = false;
    destroy_display(display, native);
}

static void test_independent_windows(void)
{
    struct wl_display *native;
    struct wl_egl_display *display = create_display(&native);
    struct wl_egl_window *a = create_window(display), *b = create_window(display);
    CHECK(a->queue != b->queue);
    CHECK(wseglSwapDrawable(a, 0) == WSEGL_SUCCESS);
    struct wl_callback *hidden = a->frame_callback;
    int before = dispatch_count;
    CHECK(wseglSwapDrawable(b, 0) == WSEGL_SUCCESS);
    CHECK(dispatch_count == before && a->frame_callback == hidden);
    frame_done(b);
    CHECK(wseglSwapDrawable(b, 0) == WSEGL_SUCCESS);
    CHECK(a->frame_callback == hidden);
    CHECK(b->attached_width == 64 && b->attached_height == 32);
    destroy_window(a);
    destroy_window(b);
    destroy_display(display, native);
}

static void test_release_before_render(void)
{
    struct wl_display *native;
    struct wl_egl_display *display = create_display(&native);
    struct wl_egl_window *window = create_window(display);
    PVR2DMEMINFO *source, *render;
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_SUCCESS);
    CHECK(wseglGetBuffers(window, &source, &render) == 1);
    CHECK(render == window->backBuffers[1]);
    frame_done(window);
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_SUCCESS);
    CHECK(window->buffer_busy[0] && window->buffer_busy[1]);
    frame_done(window);
    release_buffer(window, 0);
    int before = dispatch_count;
    CHECK(wseglGetBuffers(window, &source, &render) == 1);
    CHECK(dispatch_count == before + 2); /* frame.done alone did not suffice */
    CHECK(render == window->backBuffers[0] && source == window->backBuffers[1]);
    CHECK(!window->buffer_busy[0] && window->buffer_busy[1]);
    destroy_window(window);
    destroy_display(display, native);
}

static void test_interval_zero_and_teardown(void)
{
    struct wl_display *native;
    struct wl_egl_display *display = create_display(&native);
    struct wl_egl_window *window = create_window(display);
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_SUCCESS);
    frame_done(window); /* destruction must discard this queued callback */
    CHECK(wseglSwapControlInterval(window, 0) == WSEGL_SUCCESS);
    CHECK(!window->frame_callback);
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_SUCCESS);
    CHECK(!window->frame_callback && window->buffer_busy[0]);
    release_buffer(window, 0);
    PVR2DMEMINFO *source, *render;
    CHECK(wseglGetBuffers(window, &source, &render) == 1);
    int before = detach_count;
    destroy_window(window);
    CHECK(detach_count == before + 1);
    destroy_display(display, native);
}

static void test_disconnect(void)
{
    struct wl_display *native;
    struct wl_egl_display *display = create_display(&native);
    struct wl_egl_window *window = create_window(display);
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_SUCCESS);
    fail_dispatch = true;
    WSEGLDrawableParams source, render;
    CHECK(wseglGetDrawableParameters(window, &source, &render) == WSEGL_BAD_NATIVE_WINDOW);
    int before = commit_count;
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_BAD_NATIVE_WINDOW);
    CHECK(commit_count == before);
    destroy_window(window);
    fail_dispatch = false;
    destroy_display(display, native);
}

static void test_export_failure(void)
{
    struct wl_display *native;
    struct wl_egl_display *display = create_display(&native);
    struct wl_surface *surface = (void *)wl_proxy_create((void *)native, &wl_surface_interface);
    struct wl_egl_window *window = wl_egl_window_create(surface, 64, 32);
    WSEGLDrawableHandle drawable;
    WSEGLRotationAngle rotation;
    fail_export = true;
    CHECK(wseglCreateWindowDrawable(display, &display->wseglDisplayConfigs[0],
                                    &drawable, window, &rotation) == WSEGL_OUT_OF_MEMORY);
    CHECK(!memory_count && !window->queue && !window->backBuffersValid);
    fail_export = false;
    wl_egl_window_destroy(window);
    wl_surface_destroy(surface);
    destroy_display(display, native);
}

static void test_resize(void)
{
    struct wl_display *native;
    struct wl_egl_display *display = create_display(&native);
    struct wl_egl_window *window = create_window(display);
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_SUCCESS);
    frame_done(window);
    wl_egl_window_resize(window, 96, 48, 0, 0);
    WSEGLDrawableParams source, render;
    CHECK(wseglGetDrawableParameters(window, &source, &render) == WSEGL_BAD_DRAWABLE);
    release_buffer(window, 0);
    freeing_window = window;
    CHECK(wseglDeleteDrawable(window) == WSEGL_SUCCESS);
    freeing_window = NULL;
    CHECK(!window->queue && !window->frame_callback && !memory_count);
    WSEGLDrawableHandle drawable;
    WSEGLRotationAngle rotation;
    CHECK(wseglCreateWindowDrawable(display, &display->wseglDisplayConfigs[0],
                                    &drawable, window, &rotation) == WSEGL_SUCCESS);
    CHECK(wseglGetDrawableParameters(window, &source, &render) == WSEGL_SUCCESS);
    CHECK(render.ui32Width == 96 && render.ui32Height == 48);
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_SUCCESS);
    destroy_window(window);
    destroy_display(display, native);
}

static void test_pending_release_and_flush_error(void)
{
    struct wl_display *native;
    struct wl_egl_display *display = create_display(&native);
    struct wl_egl_window *window = create_window(display);
    CHECK(wseglSwapControlInterval(window, 0) == WSEGL_SUCCESS);
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_SUCCESS);
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_SUCCESS);
    enqueue((void *)window->drmbuffers[0], RELEASE, true);
    int before = dispatch_count;
    PVR2DMEMINFO *source, *render;
    CHECK(wseglGetBuffers(window, &source, &render) == 1);
    CHECK(dispatch_count == before && !window->buffer_busy[0]);
    fail_flush = true;
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_BAD_NATIVE_WINDOW);
    fail_flush = false;
    destroy_window(window);
    destroy_display(display, native);
}

static void test_framebuffer(void)
{
    struct wl_egl_display *display = wl_egl_display_create(NULL);
    struct wl_egl_window *window = wl_egl_window_create(NULL, 64, 32);
    window->display = display;
    window->format = WSEGL_PIXELFORMAT_8888;
    window->strideBytes = 256;
    display->fd = 123;
    CHECK(allocateBackBuffers(display, window) == WSEGL_SUCCESS);
    CHECK(PVR2DGetFrameBuffer(NULL, 0, &window->frontBufferPVRMEM) == PVR2D_OK);
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_SUCCESS);
    CHECK(blit_count == 1 && update_count == 1);
    fail_blit = true;
    CHECK(wseglSwapDrawable(window, 0) == WSEGL_BAD_NATIVE_WINDOW);
    CHECK(window->currentBackBuffer == 1 && update_count == 1);
    fail_blit = false;
    CHECK(wseglDeleteDrawable(window) == WSEGL_SUCCESS);
    wl_egl_window_destroy(window);
    display->fd = -1;
    wl_egl_display_destroy(display);
}

int main(void)
{
    test_roundtrip();
    test_independent_windows();
    test_release_before_render();
    test_interval_zero_and_teardown();
    test_disconnect();
    test_export_failure();
    test_resize();
    test_pending_release_and_flush_error();
    test_framebuffer();
    CHECK(!memory_count && !proxy_count && !queue_count);
    CHECK(export_count > 0 && flush_count > 0);
    puts("render synchronization tests passed");
    return 0;
}
