//
//  RunningAppView.swift
//  ApkContainer
//
//  Status: REAL surface host + framebuffer readback.
//    - ContainerMTKView owns a CAMetalLayer and forwards touches to InputBridge.
//    - On .task, calls RuntimeEngine.launch(packageId:, record:).
//    - A CADisplayLink reads the software-GLES framebuffer and uploads it
//      to an MTLTexture, which is presented through the CAMetalLayer.
//    - HUD shows FPS, force-quit, and back.
//

import SwiftUI
import UIKit
import QuartzCore
import Metal

struct RunningAppView: View {

    let packageId: String
    let record: AppRecord?

    @Environment(\.dismiss) private var dismiss

    @State private var launchError: String?
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
                        try? await RuntimeEngine.shared.forceQuit(
                            packageId: packageId
                        )
                        dismiss()
                    }
                },
                onBack: {
                    dismiss()
                }
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
                try await RuntimeEngine.shared.launch(
                    packageId: packageId,
                    record: record
                )

                surface.startDisplayLink { newFPS in
                    fps = newFPS
                }
            } catch {
                launchError = error.localizedDescription
            }
        }
        .onDisappear {
            surface.stopDisplayLink()

            Task {
                try? await RuntimeEngine.shared.suspend(
                    packageId: packageId
                )
            }
        }
        .alert(
            "Launch Failed",
            isPresented: Binding(
                get: {
                    launchError != nil
                },
                set: {
                    if !$0 {
                        launchError = nil
                        dismiss()
                    }
                }
            )
        ) {
            Button("Back", role: .cancel) {
                dismiss()
            }
        } message: {
            Text(launchError ?? "")
        }
    }
}

// MARK: - Surface State

/// Holds the Metal device and the container view's CAMetalLayer.
///
/// The display link callback pulls the latest software-renderer framebuffer
/// from RuntimeEngine and uploads it into a Metal texture.
final class SurfaceState: ObservableObject {

    let device: MTLDevice
    let commandQueue: MTLCommandQueue

    private var displayLink: CADisplayLink?
    private weak var layer: CAMetalLayer?
    private var texture: MTLTexture?

    private var lastFPSUpdate: CFTimeInterval = 0
    private var frameCount: Int = 0

    private var onFPS: ((Int) -> Void)?

    init() {
        guard
            let device = MTLCreateSystemDefaultDevice(),
            let queue = device.makeCommandQueue()
        else {
            fatalError("Metal not available")
        }

        self.device = device
        self.commandQueue = queue
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
        guard width > 0, height > 0 else {
            return
        }

        let scale = UIScreen.main.scale

        let widthPixels = max(
            1,
            Int(CGFloat(width) * scale)
        )

        let heightPixels = max(
            1,
            Int(CGFloat(height) * scale)
        )

        layer?.drawableSize = CGSize(
            width: widthPixels,
            height: heightPixels
        )

        RuntimeEngine.shared.resizeGraphics(
            width: widthPixels,
            height: heightPixels
        )

        recreateTexture(
            width: widthPixels,
            height: heightPixels
        )
    }

    private func recreateTexture(width: Int, height: Int) {
        guard width > 0, height > 0 else {
            texture = nil
            return
        }

        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm,
            width: width,
            height: height,
            mipmapped: false
        )

        descriptor.usage = [
            .shaderRead,
            .shaderWrite
        ]

        texture = device.makeTexture(
            descriptor: descriptor
        )
    }

    func startDisplayLink(onFPS: @escaping (Int) -> Void) {
        stopDisplayLink()

        frameCount = 0
        lastFPSUpdate = 0
        self.onFPS = onFPS

        let link = CADisplayLink(
            target: self,
            selector: #selector(tick(_:))
        )

        link.preferredFramesPerSecond = 60
        link.add(
            to: .main,
            forMode: .common
        )

        displayLink = link
    }

    func stopDisplayLink() {
        displayLink?.invalidate()
        displayLink = nil
        onFPS = nil
    }

    @MainActor
    @objc private func tick(_ link: CADisplayLink) {
        guard let layer else {
            return
        }

        guard layer.drawableSize.width > 0,
              layer.drawableSize.height > 0
        else {
            return
        }

        // Pull the latest framebuffer from the software renderer.
        guard let framebuffer = RuntimeEngine.shared.readFramebuffer()
        else {
            return
        }

        let width = framebuffer.width
        let height = framebuffer.height

        guard width > 0, height > 0 else {
            return
        }

        // Recreate the texture when the framebuffer dimensions change.
        if texture == nil ||
            texture?.width != width ||
            texture?.height != height {

            recreateTexture(
                width: width,
                height: height
            )

            layer.drawableSize = CGSize(
                width: width,
                height: height
            )
        }

        guard let texture else {
            return
        }

        let region = MTLRegionMake2D(
            0,
            0,
            width,
            height
        )

        let bytesPerRow = width * 4
        let byteCount = height * bytesPerRow

        framebuffer.ptr.withMemoryRebound(
            to: UInt8.self,
            capacity: byteCount
        ) { bytes in

            texture.replace(
                region: region,
                mipmapLevel: 0,
                withBytes: bytes,
                bytesPerRow: bytesPerRow
            )
        }

        // Obtain a drawable from the CAMetalLayer.
        guard let drawable = layer.nextDrawable()
        else {
            return
        }

        guard let commandBuffer = commandQueue.makeCommandBuffer(),
              let blit = commandBuffer.makeBlitCommandEncoder()
        else {
            return
        }

        blit.copy(
            from: texture,
            sourceSlice: 0,
            sourceLevel: 0,
            sourceOrigin: MTLOrigin(
                x: 0,
                y: 0,
                z: 0
            ),
            sourceSize: MTLSize(
                width: width,
                height: height,
                depth: 1
            ),
            to: drawable.texture,
            destinationSlice: 0,
            destinationLevel: 0,
            destinationOrigin: MTLOrigin(
                x: 0,
                y: 0,
                z: 0
            )
        )

        blit.endEncoding()

        commandBuffer.present(drawable)
        commandBuffer.commit()

        // FPS: number of framebuffer uploads per second.
        frameCount += 1

        let now = link.timestamp

        if lastFPSUpdate == 0 {
            lastFPSUpdate = now
        }

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

            VStack(
                alignment: .leading,
                spacing: 2
            ) {
                Text(packageId)
                    .font(
                        .caption2
                        .weight(.semibold)
                    )
                    .lineLimit(1)

                Text("\(fps) FPS")
                    .font(
                        .caption2
                        .monospacedDigit()
                    )
                    .foregroundStyle(.secondary)
            }

            Button(action: onBack) {
                Image(systemName: "chevron.left")
                    .font(
                        .caption
                        .weight(.semibold)
                    )
                    .frame(
                        width: 28,
                        height: 28
                    )
            }
            .buttonStyle(.borderless)

            Button(
                role: .destructive,
                action: onForceQuit
            ) {
                Image(systemName: "xmark")
                    .font(
                        .caption
                        .weight(.semibold)
                    )
                    .frame(
                        width: 28,
                        height: 28
                    )
            }
            .buttonStyle(.borderless)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(
            .ultraThinMaterial,
            in: Capsule()
        )
        .opacity(
            dimmed ? 0.15 : 1.0
        )
    }
}

// MARK: - Surface UIViewRepresentable

private struct ContainerSurfaceView: UIViewRepresentable {

    let surface: SurfaceState

    func makeUIView(
        context: Context
    ) -> ContainerMTKView {

        let view = ContainerMTKView()

        view.backgroundColor = .black
        view.surface = surface

        return view
    }

    func updateUIView(
        _ uiView: ContainerMTKView,
        context: Context
    ) {
        // No per-frame SwiftUI updates.
        // CADisplayLink drives the surface.
    }
}

// MARK: - Container Host View

final class ContainerMTKView: UIView {

    override class var layerClass: AnyClass {
        CAMetalLayer.self
    }

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

        guard
            let metalLayer = layer as? CAMetalLayer,
            let surface
        else {
            return
        }

        surface.attach(layer: metalLayer)

        let width = Int(
            bounds.width * contentScaleFactor
        )

        let height = Int(
            bounds.height * contentScaleFactor
        )

        if width > 0, height > 0 {
            surface.resize(
                width: width,
                height: height
            )
        }

        // RuntimeEngine.attachGraphics expects a CALayer.
        //
        // Do NOT pass:
        // Unmanaged.passUnretained(metalLayer).toOpaque()
        //
        // That produces UnsafeMutableRawPointer and causes the compiler
        // error seen in the GitHub Actions build.
        RuntimeEngine.shared.attachGraphics(metalLayer)
    }

    override func layoutSubviews() {
        super.layoutSubviews()

        guard
            let metalLayer = layer as? CAMetalLayer,
            let surface
        else {
            return
        }

        metalLayer.frame = bounds

        let width = Int(
            bounds.width * contentScaleFactor
        )

        let height = Int(
            bounds.height * contentScaleFactor
        )

        if width > 0, height > 0 {
            surface.resize(
                width: width,
                height: height
            )
        }
    }

    // MARK: - Touch forwarding

    override func touchesBegan(
        _ touches: Set<UITouch>,
        with event: UIEvent?
    ) {
        super.touchesBegan(
            touches,
            with: event
        )

        InputBridge.shared.enqueueTouch(
            phase: .began,
            touches: touches,
            in: bounds
        )
    }

    override func touchesMoved(
        _ touches: Set<UITouch>,
        with event: UIEvent?
    ) {
        super.touchesMoved(
            touches,
            with: event
        )

        InputBridge.shared.enqueueTouch(
            phase: .moved,
            touches: touches,
            in: bounds
        )
    }

    override func touchesEnded(
        _ touches: Set<UITouch>,
        with event: UIEvent?
    ) {
        super.touchesEnded(
            touches,
            with: event
        )

        InputBridge.shared.enqueueTouch(
            phase: .ended,
            touches: touches,
            in: bounds
        )
    }

    override func touchesCancelled(
        _ touches: Set<UITouch>,
        with event: UIEvent?
    ) {
        super.touchesCancelled(
            touches,
            with: event
        )

        InputBridge.shared.enqueueTouch(
            phase: .cancelled,
            touches: touches,
            in: bounds
        )
    }
}
