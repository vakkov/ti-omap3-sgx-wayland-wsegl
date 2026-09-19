/* Host-only definition removed from modern Wayland server headers.  Production
 * still builds against the target's legacy server API. */
#include <wayland-server.h>
struct wl_buffer {
    struct wl_resource resource;
    int width;
    int height;
};
