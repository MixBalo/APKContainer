//
//  GraphicsSurface.swift
//  ApkContainer
//
//  Status: PARTIAL.
//    - Real: bridges a `CAMetalLayer` from the SwiftUI container view to the
//      native graphics module via `apkcontainer_graphics_attach_layer`.
//    - STUB: `swapBuffers()` is a no-op because the real frame presentation
//      happens inside ANGLE via `eglSwapBuffers` (Native/Graphics, Task 4).
//      The class is the wiring point, not the renderer.
//
//  Honesty contract: see header. We do NOT claim to render anything from
//  Swift; rendering is ANGLE's job.
//

import Foundation
import QuartzCore

/// Owns the connection between a SwiftUI-hosted `CAMetalLayer` and the native
/// ANGLE-over-Metal graphics bridge.
public final class GraphicsSurface {

    public init() {}

    /// Hands the layer pointer to the native graphics module so ANGLE can
    /// render into it. Must be called on the main thread.
    @discardableResult
    public func attach(layer: CAMetalLayer) -> Int32 {
        // Pass the layer as an opaque pointer. We use `passUnretained` because
        // the layer is owned by the SwiftUI hosting view; the native side
        // must NOT release it.
        let opaque = Unmanaged.passUnretained(layer).toOpaque()
        return apkcontainer_graphics_attach_layer(opaque)
    }

    /// Presents the current backbuffer to the layer.
    ///
    /// STUB: in the real architecture, ANGLE's `eglSwapBuffers` does this
    /// directly against the CAMetalLayer (because ANGLE is the EGL
    /// implementation). This Swift method exists only as a future hook for
    /// cases where the host wants to force a swap (e.g. snapshotting); it is
    /// currently a no-op.
    public func swapBuffers() {
        // STUB: real swap happens inside ANGLE (Native/Graphics, Task 4).
    }
}
