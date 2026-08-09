//
//  InputBridge.swift
//  ApkContainer
//
//  Status: façade REAL; native backing STUB (Task 4).
//
//  Swift façade that converts UITouch events (collected by the SwiftUI
//  container view, Task 3-a) into Android MotionEvents and forwards them to
//  the native runtime. The native side (Task 4) injects them into the running
//  app's InputDispatcher via Bionic shims.
//
//  Two `enqueueTouch` overloads:
//    1. `enqueueTouch(_:forPackage:)` — spec form: one explicit MotionEvent
//       for one explicit package. Used for programmatic injection / tests.
//    2. `enqueueTouch(phase:touches:in:)` — UITouch-set convenience used by
//       the SwiftUI container view (Task 3-a). Forwards to the package
//       currently registered as the foreground target via `currentPackageId`
//       (set by `RuntimeEngine.launch` after a successful launch).
//
//  Honesty contract:
//    - The Swift `MotionEvent` type and both `enqueueTouch` overloads' wiring
//      are real.
//    - The native `apkcontainer_input_enqueue_touch` is currently a stub.
//    - Coordinate scaling to the Android surface size happens on the native
//      side (Task 4); the Swift side passes iOS-point coordinates.
//

import Foundation
import UIKit

/// Touch input bridge (singleton). Forward UITouch-derived events to native.
public final class InputBridge {

    public static let shared = InputBridge()

    public init() {}

    /// UITouch phase, mirroring the cases UIKit reports.
    public enum TouchPhase: Sendable {
        case began
        case moved
        case ended
        case cancelled
    }

    /// One touch event for one pointer. Mirrors a slice of Android's
    /// `MotionEvent` for a single pointer.
    public struct MotionEvent: Sendable, Hashable {
        /// Android action codes (subset):
        ///   0 = ACTION_DOWN
        ///   1 = ACTION_UP
        ///   2 = ACTION_MOVE
        ///   3 = ACTION_CANCEL
        ///   5 = ACTION_POINTER_DOWN
        ///   6 = ACTION_POINTER_UP
        public typealias Action = Int

        public let pointerId: Int32
        public let x: Float
        public let y: Float
        public let pressure: Float
        public let action: Action

        public init(pointerId: Int32, x: Float, y: Float, pressure: Float, action: Action) {
            self.pointerId = pointerId
            self.x = x
            self.y = y
            self.pressure = pressure
            self.action = action
        }
    }

    // MARK: - Foreground package target

    private let targetLock = NSLock()
    private var _currentPackageId: String?

    /// The package id that UITouch-derived events are currently forwarded to.
    /// Set by `RuntimeEngine.launch` after a successful launch; cleared on
    /// force-quit. Thread-safe (lock-protected) so it can be read from UIKit's
    /// touch callbacks (main thread) and written from RuntimeEngine (also main
    /// thread, but the lock makes the contract explicit).
    public var currentPackageId: String? {
        targetLock.lock()
        defer { targetLock.unlock() }
        return _currentPackageId
    }

    /// Sets the foreground package target. Called by `RuntimeEngine`.
    public func setCurrentPackageId(_ id: String?) {
        targetLock.lock()
        defer { targetLock.unlock() }
        _currentPackageId = id
    }

    // MARK: - Spec form: explicit MotionEvent for an explicit package

    /// Forwards a single touch event to the running app identified by
    /// `packageId`. Returns the native return code (0 = ok, non-zero = error).
    @discardableResult
    public func enqueueTouch(_ event: MotionEvent, forPackage id: String) -> Int32 {
        // STUB-backed: the native function exists in the bridging header but
        // its implementation (Task 4) is what actually injects the event into
        // the running app's InputDispatcher.
        return id.withCString { ptr in
            apkcontainer_input_enqueue_touch(
                ptr,
                event.pointerId,
                event.x,
                event.y,
                event.pressure,
                Int32(event.action)
            )
        }
    }

    // MARK: - UITouch convenience overload (used by the SwiftUI container view)

    /// Converts a set of UITouches (from UIKit touch callbacks) into native
    /// `apkcontainer_input_enqueue_touch` calls. The package id is taken from
    /// `currentPackageId` (set by `RuntimeEngine.launch`). If no package is
    /// registered, the events are dropped.
    public func enqueueTouch(phase: TouchPhase, touches: Set<UITouch>, in bounds: CGRect) {
        guard let packageId = currentPackageId else {
            // No foreground target — drop the events.
            return
        }
        _ = bounds // Reserved for future coordinate scaling; native side scales today.

        for touch in touches {
            let pointerId = self.pointerId(for: touch)
            let location = touch.location(in: nil) // window coordinate space
            let pressure = self.pressure(for: touch)
            let action = self.androidAction(for: phase, pointerId: pointerId)

            let event = MotionEvent(
                pointerId: pointerId,
                x: Float(location.x),
                y: Float(location.y),
                pressure: pressure,
                action: action
            )
            _ = enqueueTouch(event, forPackage: packageId)

            // Track active pointers for ACTION_DOWN vs ACTION_POINTER_DOWN.
            if phase == .began {
                activePointerIds.insert(pointerId)
            } else if phase == .ended || phase == .cancelled {
                activePointerIds.remove(pointerId)
            }
        }
    }

    // MARK: - Private: UITouch → Android pointer / action mapping

    /// Maps UITouch identity to a stable Android pointer ID (0..N).
    private var pointerIdMap: [ObjectIdentifier: Int32] = [:]
    private var nextPointerId: Int32 = 0
    private var activePointerIds: Set<Int32> = []

    private func pointerId(for touch: UITouch) -> Int32 {
        let key = ObjectIdentifier(touch)
        if let id = pointerIdMap[key] { return id }
        let id = nextPointerId
        nextPointerId &+= 1
        pointerIdMap[key] = id
        return id
    }

    private func pressure(for touch: UITouch) -> Float {
        guard touch.force > 0, touch.maximumPossibleForce > 0 else { return 1.0 }
        return Float(min(1.0, touch.force / touch.maximumPossibleForce))
    }

    /// Maps a UIKit touch phase + pointer state to an Android MotionEvent
    /// action code.
    private func androidAction(for phase: TouchPhase, pointerId: Int32) -> MotionEvent.Action {
        switch phase {
        case .began:
            // First touch → ACTION_DOWN (0); subsequent → ACTION_POINTER_DOWN (5).
            return activePointerIds.isEmpty ? 0 : 5
        case .moved:
            return 2 // ACTION_MOVE
        case .ended:
            // Last touch → ACTION_UP (1); otherwise → ACTION_POINTER_UP (6).
            return activePointerIds.count <= 1 ? 1 : 6
        case .cancelled:
            return 3 // ACTION_CANCEL
        }
    }
}
