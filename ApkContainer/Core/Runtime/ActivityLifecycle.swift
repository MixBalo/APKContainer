//
//  ActivityLifecycle.swift
//  ApkContainer
//
//  Status: façade REAL; native backing STUB (Task 4).
//
//  Swift façade over the native `apkcontainer_lifecycle_dispatch` function.
//  The SwiftUI container view (Task 3-a) drives these events as it appears,
//  disappears, and goes to the background.
//
//  Honesty contract: the Swift enum + dispatch logic is real; the native
//  function currently no-ops (Task 4 will wire it to ART's
//  `Activity.onXxx` callbacks).
//

import Foundation

/// The six standard Android Activity lifecycle events, in the order Android
/// dispatches them. Raw `Int` values are passed to the native side as the
/// `event` argument of `apkcontainer_lifecycle_dispatch`.
public enum ActivityLifecycleEvent: Int, Sendable {
    case onCreate  = 0
    case onStart   = 1
    case onResume  = 2
    case onPause   = 3
    case onStop    = 4
    case onDestroy = 5
}

/// Stateless façade over the native lifecycle dispatcher.
public enum ActivityLifecycle {

    /// Dispatches `event` to the running Activity of `packageId`. No-op if the
    /// native runtime is not yet wired (Task 4).
    @discardableResult
    public static func dispatch(
        _ event: ActivityLifecycleEvent,
        forPackage packageId: String
    ) -> Int32 {
        // STUB-backed: native function exists in the bridging header but its
        // implementation (Task 4) is what actually calls into ART to invoke
        // the Activity lifecycle method. Until then this is a no-op that
        // returns 0.
        return packageId.withCString { ptr in
            apkcontainer_lifecycle_dispatch(ptr, Int32(event.rawValue))
        }
    }
}
