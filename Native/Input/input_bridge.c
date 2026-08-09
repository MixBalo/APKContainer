/*
 * input_bridge.c — UITouch → Android MotionEvent
 *
 * Status: façade real; routing into ART Activity.onTouchEvent is STUB until
 *         ART is wired. See docs/ARCHITECTURE.md §5.
 */
#include "input_bridge.h"

#include <stdio.h>
#include <string.h>
#include "log_file.h"


static int s_surf_w = 0, s_surf_h = 0;
static char s_foreground_pkg[256] = {0};

void input_bridge_set_surface_size(int w, int h) {
    s_surf_w = w; s_surf_h = h;
    LOGI("input", "surface size %dx%d px", w, h);
}

void input_bridge_set_foreground(const char *package_id) {
    if (!package_id) { s_foreground_pkg[0] = '\0'; return; }
    strncpy(s_foreground_pkg, package_id, sizeof(s_foreground_pkg) - 1);
}

int input_bridge_enqueue(const char *package_id,
                         int pointer_id, float x, float y,
                         float pressure, int action) {
    /* STUB: real impl builds an Android MotionEvent and dispatches it into
     * the foreground Activity's onTouchEvent via art_runtime_dispatch. */
    (void)package_id; (void)pointer_id; (void)x; (void)y; (void)pressure; (void)action;
    if (s_foreground_pkg[0] == '\0') return -1;
    LOGD("input", "touch STUB pkg=%s pid=%d (%.1f,%.1f) act=%d",
                 package_id ? package_id : "?", pointer_id, x, y, action);
    return 0;
}
