/*
 * lifecycle_bridge.c — SwiftUI scene phase → Android Activity lifecycle
 *
 * Status: façade real; dispatch into ART is STUB until ART is wired.
 *        See docs/ARCHITECTURE.md §5.
 */
#include "lifecycle_bridge.h"
#include "art_runtime.h"

#include <stdio.h>
#include <string.h>
#include <os_log.h>

static os_log_t s_log = OS_LOG_DEFAULT;

int lifecycle_bridge_dispatch(const char *package_id, int event) {
    if (!package_id) return -1;
    const char *name = "?";
    switch (event) {
        case LIFECYCLE_ON_CREATE:  name = "onCreate";  break;
        case LIFECYCLE_ON_START:   name = "onStart";   break;
        case LIFECYCLE_ON_RESUME:  name = "onResume";  break;
        case LIFECYCLE_ON_PAUSE:   name = "onPause";   break;
        case LIFECYCLE_ON_STOP:    name = "onStop";    break;
        case LIFECYCLE_ON_DESTROY: name = "onDestroy"; break;
    }
    os_log_info(s_log, "lifecycle %{public}s -> %{public}s", package_id, name);

    /* STUB: real impl resolves the launcher Activity class from the manifest
     * and calls art_runtime_dispatch_activity(vm, activity_class, event).
     * Requires ART to be running. */
    art_vm_t *vm = art_runtime_get_javavm_handle();
    if (!vm) {
        os_log_error(s_log,
            "lifecycle dispatch skipped — ART VM handle is NULL (ART not embedded)");
        return -1;
    }
    /* TODO: art_runtime_dispatch_activity(vm, activity_class, event); */
    (void)vm;
    return -1;
}
