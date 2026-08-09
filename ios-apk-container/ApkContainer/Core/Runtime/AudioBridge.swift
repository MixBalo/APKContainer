//
//  AudioBridge.swift
//  ApkContainer
//
//  Status: REAL (common path).
//    - The native `apkcontainer_audio_start()` (Native/RuntimeGlue/runtime_glue.c)
//      now lazily creates a real AVAudioEngine via opensl_bridge_engine_create
//      (Native/Audio/opensl_bridge.mm, Task P3-2). The engine is published as
//      the global engine via opensl_bridge_set_global_engine(), and the
//      android.media.AudioTrack Java stub in dex_interp.cpp reaches it via
//      opensl_bridge_get_global_engine() — so AudioTrack.write(byte[],...)
//      actually pumps PCM into AVAudioPlayerNode → mainMixer → output.
//    - This Swift-side AudioBridge still owns a separate AVAudioEngine
//      instance for legacy reasons and to configure the AVAudioSession
//      (.ambient + .mixWithOthers). The native side reuses the SAME session.
//    - PARTIAL: the native engine is the source of truth for actual audio
//      output; this Swift engine is started for parity but does not directly
//      receive PCM samples. Calling `start()` here triggers
//      `apkcontainer_audio_start()` which creates the native engine.
//
//  Honesty contract: a typical Android app that plays 16-bit PCM via
//  AudioTrack produces sound on the device. Apps that use OpenSL ES directly
//  (slCreateEngine + SLEngineItf::CreateAudioPlayer + SLBufferQueueItf::Enqueue)
//  get a minimal SLES wrapper in Native/Loader/bionic_shim.c that forwards
//  to opensl_bridge_* — see that file for what's real vs partial.
//

import Foundation
import AVFoundation

/// Audio bridge singleton. Owns an `AVAudioEngine` for session configuration
/// parity; the native OpenSL ES bridge (Task P3-2) owns the engine that
/// actually plays PCM from Android apps.
public final class AudioBridge {

    public static let shared = AudioBridge()

    private let engine = AVAudioEngine()
    private let mixer: AVAudioMixerNode

    public init() {
        // The engine's mainMixerNode is created lazily; touching it here
        // ensures the audio graph is initialized before `start()` is called.
        self.mixer = engine.mainMixerNode
        _ = mixer
    }

    /// Starts the AVAudioEngine and notifies the native side (which creates
    /// the global OpenSL ES → AVAudioEngine bridge).
    @discardableResult
    public func start() -> Int32 {
        // Configure the session for ambient playback (mixes with other audio).
        let session = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(.ambient, mode: .default, options: [.mixWithOthers])
            try session.setActive(true)
        } catch {
            NSLog("[AudioBridge] AVAudioSession setup failed: \(error.localizedDescription)")
            // Continue anyway — the native side may still produce audio.
        }

        do {
            try engine.start()
        } catch {
            NSLog("[AudioBridge] AVAudioEngine start failed: \(error.localizedDescription)")
            // Don't bail: the native side (opensl_bridge.mm) creates its own
            // AVAudioEngine and may succeed even if this Swift-side engine
            // failed (e.g. due to a transient session conflict).
        }

        // Notify native. This creates the global OpenSL ES → AVAudioEngine
        // bridge that the AudioTrack Java stub + SLES C API wrapper use to
        // play PCM. Returns 0 on success.
        return apkcontainer_audio_start()
    }

    /// Stops the AVAudioEngine and notifies the native side (which destroys
    /// the global OpenSL ES → AVAudioEngine bridge).
    @discardableResult
    public func stop() -> Int32 {
        engine.stop()
        return apkcontainer_audio_stop()
    }
}
