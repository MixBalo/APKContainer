/*
 * input_bridge.h — UITouch → Android MotionEvent
 *
 * Status: façade real; routing into ART Activity onTouchEvent is STUB until
 *         ART is wired. See docs/ARCHITECTURE.md §5.
 */
#ifndef APKCONTAINER_INPUT_BRIDGE_H
#define APKCONTAINER_INPUT_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Android MotionEvent action codes (subset). */
enum {
    INPUT_ACTION_DOWN         = 0,
    INPUT_ACTION_UP           = 1,
    INPUT_ACTION_MOVE         = 2,
    INPUT_ACTION_CANCEL       = 3,
    INPUT_ACTION_POINTER_DOWN = 5,
    INPUT_ACTION_POINTER_UP   = 6
};

/* Surface size in pixels (set by graphics_bridge when EGL surface is created),
 * used to convert UIKit points → Android surface pixels. */
void input_bridge_set_surface_size(int width_px, int height_px);

/* Enqueue one pointer event for the foreground package. Coordinates in
 * surface pixels (caller converts from UIKit points). */
int  input_bridge_enqueue(const char *package_id,
                          int pointer_id, float x, float y,
                          float pressure, int action);

#ifdef __cplusplus
}
#endif
#endif
