# Limitations — the blunt version (Phase 3)

> The prompt explicitly said: *"if something 'unsupported' find a work around that will let app still run."* Phase 3 did exactly that. Where a feature was missing, we added either a real implementation or a degraded-but-non-crashing fallback.

---

## TL;DR (Phase 3)

Phase 3 replaced most "unsupported" labels with workarounds:
- **GLES 3** → compat shim advertises "OpenGL ES 3.0" + stubs all GLES 3 entry points as no-op-success or GLES 2 fallback. Apps that gate on GLES 3 version boot instead of refusing.
- **Audio** → real OpenSL ES → AVAudioEngine PCM routing (Obj-C++ .mm).
- **java.net.*** → real BSD socket wrappers (Socket/ServerSocket/DatagramSocket/InetAddress).
- **Reflection** → real Class.forName/getMethod/invoke/getField/set.
- **android.media.*** → AudioTrack routes to OpenSL; MediaPlayer/SoundPool degrade to no-op-success.
- **JNI typed variants** → all Call*Method/Get*Field/Set*Field/Array types now real.
- **Thread.start()** → runs synchronously on current thread (single-threaded workaround).
- **Static stub arg bug** → fixed (Math/Log/Integer were reading wrong arg index).

**Remaining hard limits** (no honest workaround exists):
- **GC** — none. Heap grows unbounded. Workaround: log WARN at 64 MiB. Long-running apps OOM.
- **Real multi-threading** — Thread.start() runs inline. Apps depending on concurrency may deadlock.
- **Vulkan** — no Vulkan-on-iOS story. Advertise as unavailable; apps should fall back to GLES.
- **armeabi-v7a (32-bit)** — iOS dropped 32-bit after iOS 11. No workaround. Install rejects.
- **x86/x86_64** — not arm64. No workaround. Install rejects.
- **iOS 17+ non-jailbroken** — no TrollStore. No workaround. App reports "Unsupported."
- **App Store / free sideload / paid-dev sideload** — no entitlement for unsigned exec. No workaround.

---

## What is REAL now (Phase 3 additions)

- **GLES 3 compat shim** (~80 entry points): VAO (mapped to OES), glBufferSubData/glMapBufferRange/glUnmapBuffer (real), glReadPixels (real, Y-flipped), FBOs (degraded — default FB only), transform feedback / queries / UBO / SSBO / compute (no-op success), integer uniforms (stored as int), instancing (degraded — 1 instance), glTexStorage2D (mapped to glTexImage2D), sync objects, multi-draw, samplers, glInvalidateFramebuffer, debug output (all no-op success). glGetString(GL_VERSION) returns "OpenGL ES 3.0".
- **Real audio**: OpenSL ES → AVAudioEngine. CreateAudioPlayer(bufferQueue) wires into AVAudioPlayerNode + ring buffer + AVAudioConverter for format conversion. slCreateEngine shim provided so native .so code using standard SLES API works.
- **java.net.*** (real BSD sockets): Socket (connect/read/write/close), ServerSocket (bind/listen/accept), DatagramSocket (send/receive), InetAddress (getaddrinfo). InputStream/OutputStream wrappers backed by recv/send.
- **Reflection** (real via dex_invoke): Class.forName, getMethod/getDeclaredMethod, Method.invoke (unbox args by shorty, invoke, box return), getField/getDeclaredField, Field.get/set, Class.newInstance, Class.getMethods (walks method table).
- **Boxing**: Integer/Long/Float/Double/Boolean valueOf + *Value (real, correct JNI shorties).
- **android.media.AudioTrack**: real — routes to opensl_bridge_player_enqueue/play/stop.
- **android.media.MediaPlayer**: degraded — prepareAsync fires onPrepared synchronously; start/pause/stop/release no-op+log.
- **android.content.SharedPreferences**: real — JSON-backed in <sandbox>/shared_prefs/.
- **JNI typed variants**: CallByte/Char/Short/Long/Float/Double*Method, Get/Set*Field (all types), Get/Set*StaticField (all types), CallStaticBoolean/Long*Method, New*Array/Get*ArrayElements/Release*ArrayElements/Get/Set*ArrayRegion (all types), NewObjectArray/GetObjectArrayElement/SetObjectArrayElement, GetObjectClass. All real against dex_interp.
- **Thread.start()**: calls run() synchronously (single-threaded workaround).
- **Static stub arg fix**: Math.max/min/abs, Integer.parseInt/toString/valueOf, Log.i/d/w/e/v now use args[0] for first static param (was args[1] — latent bug producing garbage results).

## What is still degraded (but doesn't crash)

- **GLES 3 rendering** — swgl renders GLES 2.0; GLES 3-only calls (transform feedback, compute, FBO-to-texture, instancing) are no-op-success. Visual output may be wrong (missing effects, missing geometry) but the app doesn't crash. Real GLES 3 needs ANGLE.
- **FBOs** — glBindFramebuffer/glFramebufferTexture2D accept non-zero IDs but render to the default framebuffer. Apps rendering to texture FBOs render to the screen instead.
- **Multi-threading** — Thread.start() runs the Runnable synchronously on the current thread. Apps that depend on concurrency (e.g. networking on a background thread) will block the main thread. Apps using wait/notify may deadlock.
- **MediaPlayer** — no media container decoding. setDataSource/prepare/start are no-op-success. Apps playing video/audio files get silence.
- **GC** — none. Heap grows unbounded. Long-running apps will eventually OOM at ~64 MiB.
- **resources.arsc reference resolution** — @string/@drawable refs not resolved; labels/icons use heuristics.
- **Reflection** — setAccessible is no-op (we don't enforce access checks). getMethods returns the method table but doesn't filter by visibility properly.
- **Compressed textures** (ETC2, ASTC, PVRTC) — glCompressedTexImage2D is no-op+log; texture will be blank.
- **3D textures** — not supported (no-op+log).

## What will never work (honest hard limits)

- **Vulkan** — no Vulkan-on-iOS. Apps requiring Vulkan fail.
- **armeabi-v7a (32-bit)** — iOS dropped 32-bit. Install rejects with clear error.
- **x86 / x86_64** — not arm64. Install rejects.
- **iOS 17+ non-jailbroken** — no TrollStore. App reports "Unsupported distribution."
- **App Store / free sideload / paid-dev sideload** — no entitlement for unsigned exec. Every launch returns nativeNotAvailable.
- **Google Play Services / Firebase / WebView** — not implemented and won't be (massive scope).

## What we will not claim

- That Among Us runs. It still needs full IL2CPP + GLES 3 + audio + networking + many framework classes. The GLES 3 compat shim lets it *boot past the version check* but rendering will be degraded (GLES 2 path only without ANGLE).
- That the software GLES 3 renderer is fast. It isn't — it's a per-pixel software rasterizer with no-op stubs for advanced features.
- That multi-threading works. It doesn't — Thread.start runs inline.
- That GC works. It doesn't — long-running apps OOM.
- That the code is bug-free or builds first-try. It has not been compiled in this sandbox (no Xcode).

## How to diagnose issues

Open **Settings → View Log** (or AppDetailView → View Last Run Log). Every native module logs timestamped, tagged, leveled lines. Look for:
- `[WARN]` lines for degraded paths (GLES 3 stubs, Thread.start synchronous, MediaPlayer no-op)
- `[ERROR]` lines for failures (unresolved symbols, class not found, method not found)
- `BIONIC_STUB` for missing Bionic symbols
- `JNI_STUB` for missing JNI entries (few remain)
- `STUB` for framework stubs that are no-op-success
