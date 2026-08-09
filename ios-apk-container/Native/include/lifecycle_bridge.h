/*
 * lifecycle_bridge.h — SwiftUI scene phase → Android Activity lifecycle
 *
 * Status: façade real; dispatch into ART is STUB until ART is wired.
 *        See docs/ARCHITECTURE.md §5.
 */
#ifndef APKCONTAINER_LIFECYCLE_BRIDGE_H
#define APKCONTAINER_LIFECYCLE_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LIFECYCLE_ON_CREATE  = 0,
    LIFECYCLE_ON_START   = 1,
    LIFECYCLE_ON_RESUME  = 2,
    LIFECYCLE_ON_PAUSE   = 3,
    LIFECYCLE_ON_STOP    = 4,
    LIFECYCLE_ON_DESTROY = 5
};

int lifecycle_bridge_dispatch(const char *package_id, int event);

#ifdef __cplusplus
}
#endif
#endif
