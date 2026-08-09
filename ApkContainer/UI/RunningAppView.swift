//
//  RunningAppView.swift
//  ApkContainer
//
//  Status: REAL surface host + framebuffer readback.
//    - ContainerMTKView owns a CAMetalLayer and forwards touches to InputBridge.
//    - On .task, calls RuntimeEngine.launch(packageId:, record:) which calls
//      apkcontainer_runtime_configure + apkcontainer_runtime_launch.
//    - A CADisplayLink reads the software-GLES framebuffer (returned by
//      apkcontainer_get_framebuffer) and uploads it to an MTLTexture, which is
//      set as the CAMetalLayer's contents. This is how the software renderer's
//      output actually reaches the screen.
//    - HUD shows FPS (measured from the display link), force-quit, and back.
//
//  Honest caveats:
//    - If the native runtime fails to launch (e.g. TrollStore not present,
//      ART init failed, .so load failed), the alert shows the error and the
//      surface stays black. The per-run log file (Settings > Log) has detail.
//    - FPS is the upload-frame rate, not the app's render rate; if the app
//      never calls eglSwapBuffers, FPS will be 0 and the surface will be black.
//

import SwiftUI
import UIKit
import QuartzCore
import Metal

struct RunningAppView: View {
    let packageId: String
    let record: AppRecord?

    @Environment(\.dismiss) private var dismiss
    @State private var launchError: String? = nil
    @State private var hudDimmed = false
    @State private var fps: Int = 0
    @StateObject private var surface = SurfaceState()

    var body: some View {
        ZStack(alignment: .topTrailing) {
            ContainerSurfaceView(surface: surface)
                .ignoresSafeArea()
                .background(Color.black)

            RunningAppHUD(
                packageId: packageId,
                fps: fps,
                dimmed: hudDimmed,
                onForceQuit: {
                    Task {
                        try? await RuntimeEngine.shared.forceQuit(packageId: packageId)
                        dismiss()
                    }
                },
                onBack: { dismiss() }
            )
            .padding(12)
            .onTapGesture {
                withAnimation(.easeInOut(duration: 0.2)) {
                    hudDimmed.toggle()
                }
            }
        }
        .task {
            do {
                try await RuntimeEngine.shared.launch(packageId: packageId, record: record)
                surface.startDisplayLink { newFPS in
                    fps = newFPS
                }
            } catch {
                launchError = error.localizedDescription
            }
        }
        .onDisappear {
            surface.stopDisplayLink()
            Task { try? await RuntimeEngine.shared.suspend(packageId: packageId) }
        }
        .alert(
            "Launch Failed",
            isPresented: Binding(
                get: { launchError != nil },
                set: { if !$0 { launchError = nil; dismiss() } }
            )
        ) {
            Button("Back", role: .cancel) { dismiss() }
        } message: {
            Text(launchError ?? "")
        }
    }
}

/// Holds the Metal device + the container view's CAMetalLayer so the display
/// link callback can upload framebuffers.
final class SurfaceState: ObservableObject {
    let device: MTLDevice
    let commandQueue: MTLCommandQueue
    private var displayLink: CADisplayLink?
    private var layer: CAMetalLayer?
    private var texture: MTLTexture?
    private var lastFPSUpdate: CFTimeInterval = 0
    private var frameCount: Int = 0

    init() {
        guard let d = MTLCreateSystemDefaultDevice(),
              let q = d.makeCommandQueue() else {
            // Should never happen on a real iOS device; Simulator is not supported.
            fatalError("Metal not available")
        }
        self.device = d
        self.commandQueue = q
    }

    func attach(layer: CAMetalLayer) {
        self.layer = layer
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.framebufferOnly = true
        layer.contentsScale = UIScreen.main.scale
    }
    @MainActor
    func resize(width: Int, height: Int) {
        guard width > 0 && height > 0 else { return }
        let scale = UIScreen.main.scale
        let wPx = Int(CGFloat(width) * scale)
        let hPx = Int(CGFloat(height) * scale)
        layer?.drawableSize = CGSize(width: width, height: height)
        RuntimeEngine.shared.resizeGraphics(width: wPx, height: hPx)
        // Recreate the texture.
        let desc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm,
            width: wPx, height: hPx,
            mipmapped: false
        )
        desc.usage = [.shaderWrite, .shaderRead]
        texture = device.makeTexture(descriptor: desc)
    }

    func startDisplayLink(onFPS: @escaping (Int) -> Void) {
        stopDisplayLink()
        let link = CADisplayLink(target: self, selector: #selector(tick(_:)))
        link.preferredFramesPerSecond = 60
        link.add(to: .main, forMode: .common)
        displayLink = link
        self.onFPS = onFPS
    }

    func stopDisplayLink() {
        displayLink?.invalidate()
        displayLink = nil
    }

    private var onFPS: ((Int) -> Void)?
    
    @MainActor
    @objc private func tick(_ link: CADisplayLink) {
        guard let layer, layer.drawableSize.width > 0 else { return }
        // Pull the latest framebuffer from the software GLES and blit to the texture.
        guard let fb = RuntimeEngine.shared.readFramebuffer() else { return }
        // Lazily (re)create texture if dims changed.
        if texture == nil ||
           texture!.width != fb.width ||
           texture!.height != fb.height {
            let desc = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm,
                width: fb.width, height: fb.height,
                mipmapped: false
            )
            desc.usage = [.shaderWrite, .shaderRead]
            texture = device.makeTexture(descriptor: desc)
            layer.drawableSize = CGSize(width: fb.width, height: fb.height)
        }
        guard let tex = texture else { return }

        // Upload bytes into the texture.
        let region = MTLRegionMake2D(0, 0, fb.width, fb.height)
        let bytesPerRow = fb.width * 4
        fb.ptr.withMemoryRebound(to: UInt8.self, capacity: fb.width * fb.height * 4) { bytes in
            tex.replace(
                region: region,
                mipmapLevel: 0,
                withBytes: bytes,
                bytesPerRow: bytesPerRow
            )
        }

        // Present via a Metal drawable.
        guard let drawable = layer.nextDrawable() else { return }
        if let cmd = commandQueue.makeCommandBuffer(),
           let blit = cmd.makeBlitCommandEncoder() {
            blit.copy(from: tex, sourceSlice: 0, sourceLevel: 0,
                      sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                      sourceSize: MTLSize(width: fb.width, height: fb.height, depth: 1),
                      to: drawable.texture, destinationSlice: 0, destinationLevel: 0,
                      destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
            blit.endEncoding()
            cmd.present(drawable)
            cmd.commit()
        }

        // FPS: count frames per second.
        frameCount += 1
        let now = link.timestamp
        if now - lastFPSUpdate >= 1.0 {
            onFPS?(frameCount)
            frameCount = 0
            lastFPSUpdate = now
        }
    }
}

// MARK: - HUD

private struct RunningAppHUD: View {
    let packageId: String
    let fps: Int
    let dimmed: Bool
    let onForceQuit: () -> Void
    let onBack: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(packageId)
                    .font(.caption2.weight(.semibold))
                    .lineLimit(1)
                Text("\(fps) FPS")
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
            Button(action: onBack) {
                Image(systemName: "chevron.left")
                    .font(.caption.weight(.semibold))
                    .frame(width: 28, height: 28)
            }
            .buttonStyle(.borderless)
            Button(role: .destructive, action: onForceQuit) {
                Image(systemName: "xmark")
                    .font(.caption.weight(.semibold))
                    .frame(width: 28, height: 28)
            }
            .buttonStyle(.borderless)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.ultraThinMaterial, in: Capsule())
        .opacity(dimmed ? 0.15 : 1.0)
    }
}

// MARK: - Surface (UIViewRepresentable)

private struct ContainerSurfaceView: UIViewRepresentable {
    let surface: SurfaceState

    func makeUIView(context: Context) -> ContainerMTKView {
        let v = ContainerMTKView()
        v.backgroundColor = .black
        v.surface = surface
        return v
    }

    func updateUIView(_ uiView: ContainerMTKView, context: Context) {
        // No per-frame SwiftUI updates; the display link drives the layer.
    }
}

// MARK: - Container host view

final class ContainerMTKView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    weak var surface: SurfaceState?

    override init(frame: CGRect) {
        super.init(frame: frame)
        configure()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        configure()
    }

    private func configure() {
        isMultipleTouchEnabled = true
        isExclusiveTouch = false
        contentScaleFactor = UIScreen.main.scale
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        guard let metal = layer as? CAMetalLayer, let surface else { return }
        surface.attach(layer: metal)
        let w = Int(bounds.width * contentScaleFactor)
        let h = Int(bounds.height * contentScaleFactor)
        if w > 0 && h > 0 { surface.resize(width: w, height: h) }
        // Hand the layer pointer to native so swgl knows where to blit.
        let ptr = Unmanaged.passUnretained(metal).toOpaque()
        RuntimeEngine.shared.attachGraphics(layer: ptr)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        guard let metal = layer as? CAMetalLayer, let surface else { return }
        metal.frame = bounds
        let w = Int(bounds.width * contentScaleFactor)
        let h = Int(bounds.height * contentScaleFactor)
        if w > 0 && h > 0 { surface.resize(width: w, height: h) }
    }

    // Forward every touch phase to InputBridge.
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesBegan(touches, with: event)
        InputBridge.shared.enqueueTouch(phase: .began, touches: touches, in: bounds)
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesMoved(touches, with: event)
        InputBridge.shared.enqueueTouch(phase: .moved, touches: touches, in: bounds)
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesEnded(touches, with: event)
        InputBridge.shared.enqueueTouch(phase: .ended, touches: touches, in: bounds)
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesCancelled(touches, with: event)
        InputBridge.shared.enqueueTouch(phase: .cancelled, touches: touches, in: bounds)
    }
}
