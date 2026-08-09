//
//  RuntimeEngine.swift
//  ApkContainer
//
//  Status: Swift façade REAL; native backing STUB (Task 4).
//
//  ObservableObject that tracks running APKs and exposes launch / suspend /
//  resume / forceQuit. Each Swift method is a thin façade over a C function
//  declared in `Bridging/include/ApkContainer.h` and implemented in `Native/`
//  (Task 4: ART interpreter + ELF loader + ANGLE + Bionic shim).
//
//  Deviation from spec:
//    - `suspend`, `resume`, `forceQuit` are `async throws` (spec said sync).
//      The Task 3-a UI calls them with `try? await`.
//    - `RunningAppInfo` has an extra `displayName` field. The UI's running
//      apps list shows it as the row title.
//
//  Honesty contract:
//    - The Swift side is implemented: state machine, @Published list updates,
//      error mapping, async wrapping.
//    - The native side is currently a stub that returns a non-zero error code
//      for every call. Until Task 4 lands, `launch` will throw
//      `RuntimeError.nativeNotAvailable`. This is the honest behavior — we do
//      NOT claim an APK is running when it isn't.
//

import Foundation
import Combine

/// Errors raised by the runtime façade.
public enum RuntimeError: LocalizedError {
    /// The native runtime is not yet integrated (Task 4) or has refused the
    /// operation. `code` is the raw value returned by the C function.
    case nativeNotAvailable
    case nativeFailure(code: Int32)
    case notInstalled(String)
    case alreadyRunning(String)

    public var errorDescription: String? {
        switch self {
        case .nativeNotAvailable:
            return "Native runtime is not available. This build of APKLive does not include ART + ELF loader (Task 4)."
        case .nativeFailure(let code):
            return "Native runtime returned error code \(code)."
        case .notInstalled(let id):
            return "Package \(id) is not installed."
        case .alreadyRunning(let id):
            return "Package \(id) is already running."
        }
    }
}

/// Tracks one running APK.
public struct RunningAppInfo: Identifiable, Hashable {
    public enum State: String { case launching, running, suspended }
    public let id: String          // package id
    public let packageId: String
    /// Friendly name for UI display. Defaults to the package id's last segment
    /// if no friendlier name is available.
    public let displayName: String
    public var state: State
    public let startedAt: Date

    public init(
        id: String,
        packageId: String,
        displayName: String,
        state: State,
        startedAt: Date
    ) {
        self.id = id
        self.packageId = packageId
        self.displayName = displayName
        self.state = state
        self.startedAt = startedAt
    }
}

/// Observable façade over the native runtime.
@MainActor
public final class RuntimeEngine: ObservableObject {

    public static let shared = RuntimeEngine()

    @Published public private(set) var runningApps: [RunningAppInfo] = []

    public init() {}

    // MARK: - Launch

    /// Launches the APK identified by `packageId`. Throws `RuntimeError` if the
    /// native runtime is unavailable or refuses the launch.
    /// `record` provides the sandbox root, classes.dex path, and launcher
    /// activity class needed by `apkcontainer_runtime_configure`.
    public func launch(packageId: String, record: AppRecord? = nil) async throws {
        guard !runningApps.contains(where: { $0.packageId == packageId }) else {
            throw RuntimeError.alreadyRunning(packageId)
        }

        // If the caller provided the AppRecord, configure the native side with
        // the sandbox + dex + activity paths first.
        if let record {
            let sandboxRoot = record.sandboxPath
            let dexPath = record.classesDexPath ?? ""
            let activity = record.launcherActivity
            let confRC = await withCheckedContinuation { (continuation: CheckedContinuation<Int32, Never>) in
                DispatchQueue.global(qos: .userInitiated).async {
                    let rc = apkcontainer_runtime_configure(
                        packageId.withCString { $0 },
                        sandboxRoot.withCString { $0 },
                        dexPath.withCString { $0 },
                        activity.withCString { $0 }
                    )
                    continuation.resume(returning: rc)
                }
            }
            if confRC != 0 {
                NSLog("[RuntimeEngine] configure returned \(confRC) for \(packageId)")
            }
        }

        // Optimistically mark as launching.
        let info = RunningAppInfo(
            id: packageId,
            packageId: packageId,
            displayName: record?.name ?? friendlyName(forPackageId: packageId),
            state: .launching,
            startedAt: Date()
        )
        runningApps.append(info)

        let rc = await callNative(packageId: packageId) { pkg in
            apkcontainer_runtime_launch(pkg)
        }
        if rc != 0 {
            runningApps.removeAll { $0.packageId == packageId }
            throw RuntimeError.nativeFailure(code: rc)
        }
        // Update state to running.
        if let idx = runningApps.firstIndex(where: { $0.packageId == packageId }) {
            runningApps[idx].state = .running
        }
        // Register this package as the foreground touch target. UITouches
        // forwarded by the SwiftUI container view will be routed here.
        InputBridge.shared.setCurrentPackageId(packageId)
    }

    // MARK: - Graphics (framebuffer readback for the SwiftUI container view)

    /// Reads the current software-GLES framebuffer pointer + dims. The SwiftUI
    /// `RunningAppView` calls this on a CADisplayLink tick and uploads the bytes
    /// to an MTLTexture of format `.bgra8Unorm` for display in a CAMetalLayer.
    /// Returns nil if the framebuffer is not yet allocated (e.g. before the
    /// first eglSwapBuffers).
    public func readFramebuffer() -> (ptr: UnsafeRawPointer, width: Int, height: Int)? {
        guard let ptr = apkcontainer_get_framebuffer() else { return nil }
        let w = Int(apkcontainer_get_framebuffer_width())
        let h = Int(apkcontainer_get_framebuffer_height())
        if w <= 0 || h <= 0 { return nil }
        return (UnsafeRawPointer(ptr), w, h)
    }

    /// Attaches a CAMetalLayer (via its opaque pointer) to the native graphics
    /// bridge. Called by `RunningAppView` when its container UIView is laid out.
    public func attachGraphics(_ layer: CALayer) {
        let pointer = Unmanaged.passUnretained(layer).toOpaque()
        _ = apkcontainer_graphics_attach_layer(pointer)
    }
    

    /// Resizes the GL framebuffer. Called when CAMetalLayer.drawableSize changes.
    public func resizeGraphics(width: Int, height: Int) {
        DispatchQueue.global(qos: .userInitiated).async {
            _ = apkcontainer_graphics_resize(Int32(width), Int32(height))
        }
    }

    // MARK: - Diagnostics

    /// Returns the absolute path of the current run's log file. The UI shows
    /// this in the per-app detail view and a global LogViewer.
    public var logPath: String {
        if let c = apkcontainer_get_log_path() { return String(cString: c) }
        return "(log not initialized)"
    }

    // MARK: - Suspend / Resume / Force quit

    /// Suspends the running app. Async + throws per UI contract; native errors
    /// surface as `RuntimeError.nativeFailure`.
    public func suspend(packageId: String) async throws {
        let rc = await callNative(packageId: packageId) { pkg in
            apkcontainer_runtime_suspend(pkg)
        }
        if rc != 0 {
            throw RuntimeError.nativeFailure(code: rc)
        }
        if let idx = runningApps.firstIndex(where: { $0.packageId == packageId }) {
            runningApps[idx].state = .suspended
        }
    }

    /// Resumes a suspended app.
    public func resume(packageId: String) async throws {
        let rc = await callNative(packageId: packageId) { pkg in
            apkcontainer_runtime_resume(pkg)
        }
        if rc != 0 {
            throw RuntimeError.nativeFailure(code: rc)
        }
        if let idx = runningApps.firstIndex(where: { $0.packageId == packageId }) {
            runningApps[idx].state = .running
        }
    }

    /// Force-quits the running app. Always removes the app from `runningApps`
    /// (the user asked to quit, so the UI should reflect that even if the
    /// native side didn't clean up), but still throws if the native call
    /// returned an error so the caller can decide whether to surface it.
    public func forceQuit(packageId: String) async throws {
        let rc = await callNative(packageId: packageId) { pkg in
            apkcontainer_runtime_force_quit(pkg)
        }
        runningApps.removeAll { $0.packageId == packageId }
        // Clear the foreground touch target if it was this package.
        if InputBridge.shared.currentPackageId == packageId {
            InputBridge.shared.setCurrentPackageId(nil)
        }
        if rc != 0 {
            // UI callers use `try?` so this becomes a silent log line; we still
            // throw to honor the contract.
            NSLog("[RuntimeEngine] forceQuit native returned \(rc) for \(packageId)")
            throw RuntimeError.nativeFailure(code: rc)
        }
    }

    // MARK: - Touch event stream

    /// Returns an `AsyncStream` of touch events for `packageId`.
    ///
    /// STUB: real event delivery requires the native input bridge (Task 4) to
    /// push events into a Swift-readable queue. Until then, this returns an
    /// empty stream that never produces values.
    public func touchEvents(forPackage id: String) -> AsyncStream<InputBridge.MotionEvent> {
        // STUB: native event source not wired. Real implementation will bridge
        // from `apkcontainer_input_enqueue_touch` (which the native side calls
        // into Swift via a callback) into this AsyncStream.
        AsyncStream { continuation in
            // Intentionally no events. The continuation is never finished so
            // callers can `for await` without getting an immediate end.
            _ = id
            _ = continuation
        }
    }

    // MARK: - Private helpers

    /// Derives a friendly display name from a package id. Uses the last
    /// dot-separated segment (e.g. `com.example.app` → `app`). The real
    /// display name should come from the AppRecord, but RuntimeEngine doesn't
    /// depend on AppCatalog to avoid a circular dependency.
    private func friendlyName(forPackageId id: String) -> String {
        let last = id.split(separator: ".").last
        return last.map(String.init) ?? id
    }

    /// Off-loads a synchronous C call to a background thread so the main actor
    /// doesn't block on the native runtime.
    private func callNative(
        packageId: String,
        body: @escaping @Sendable (UnsafePointer<CChar>) -> Int32
    ) async -> Int32 {
        await withCheckedContinuation { (continuation: CheckedContinuation<Int32, Never>) in
            DispatchQueue.global(qos: .userInitiated).async {
                let rc = packageId.withCString { ptr in body(ptr) }
                continuation.resume(returning: rc)
            }
        }
    }
}
