# Android APK Runner for iOS

A LiveContainer-style iOS app that installs and runs Android `.apk` packages on-device — pick an APK, it appears in a home-screen-style grid, tap to launch.

**This is not an emulator in the traditional sense and not a simple "installer."** An APK targets a different OS's runtime (Dalvik/ART bytecode, Bionic libc, the Android native graphics/IPC stack). Since target devices are arm64, there's no CPU emulation needed for 64-bit native code — but nearly everything else about Android's OS layer has to be reimplemented well enough that the app believes it's running on Android.

## Status

> [!IMPORTANT]
> Early / architecture stage. See the [capability matrix](docs/CAPABILITY_MATRIX.md) below for what actually works today.

## Why this is hard (read before filing issues)

Two separate problems, both required:

1. **Managed code** — most app logic ships as Dalvik/ART bytecode. Needs either an ART/Dalvik interpreter (or JIT) embedded in the app, or AOT translation of DEX to native code.
2. **Native code** — game engines (Unity/IL2CPP, Cocos2d, etc.) ship `.so` libraries in `lib/arm64-v8a/`, compiled against Bionic libc and the Android NDK ABI. Running them requires a Bionic libc shim, a custom ELF loader, JNI bridging back to the managed-code layer, and an EGL/OpenGL ES shim that routes into Metal.

`armeabi-v7a`-only (32-bit) APKs are **out of scope** — iOS has not supported 32-on-64 execution since iOS 11.

## Platform constraints that shape everything else

- **JIT is blocked.** iOS enforces W^X for third-party apps; the `dynamic-codesigning` entitlement isn't granted outside jailbreak/enterprise contexts. This project must pick one of:
  - Jailbroken device (entitlement restriction lifted) — full JIT/interpreter, best compatibility
  - Interpreter-only ART (no JIT) — works on non-jailbroken devices, significantly slower
  - AOT-compile DEX to a native binary at install time — still requires a codesigning path that allows re-signing generated code (enterprise/dev cert)
- **Unsigned native code.** `.so` files pulled from an APK are unsigned ARM64 object code; iOS requires every executable page to belong to a codesigned binary. Requires either a custom in-process loader that maps them as data and executes via a pre-signed trampoline (jailbreak) or re-signing at install time under a provisioning profile that permits it.
- **Per-app sandbox.** Each installed APK gets its own writable directory mimicking `/data/data/<package>/`, built on iOS's own sandboxed filesystem.

**Target install method: [TBD — pick one and document why: jailbroken + tweak injection / TrollStore-style / dev-signed sideload].** This decision determines what "fully functional" can mean for non-jailbroken users and should be finalized and stated explicitly before further implementation.

## Architecture

Build order:

1. **APK parser/installer** — unzip, parse binary `AndroidManifest.xml` and `classes.dex`, extract `lib/arm64-v8a/*.so`, extract icon/resources, register in a local catalog (SQLite/plist), create sandboxed data dir.
2. **DEX/ART execution layer** — the core of the project. Either adapt an existing open-source Dalvik/ART interpreter, or write a DEX bytecode interpreter scoped to the instruction subset real apps use. Framework surface (Activity lifecycle, common `android.*` classes) is implemented explicitly and incrementally — unimplemented calls are stubbed with logged no-ops, never silently ignored.
3. **Bionic libc / native loader shim** — custom ELF loader for `.so`s, symbol resolution against the libc shim, JNI environment (`JNIEnv*`, `JavaVM*`) so native code can call back into layer 2.
4. **Graphics bridge** — EGL/OpenGL ES shim whose `eglSwapBuffers` etc. render into a `CAMetalLayer`, via GL→Metal translation (e.g. adapting ANGLE) or draw-call interception. This is the most CPU/GPU-costly piece and the one that determines whether a game renders at all vs. black-screens.
5. **Input/audio/lifecycle bridging** — touch → `MotionEvent` equivalents, audio buffers → `AVAudioEngine`, Activity lifecycle driven by the container UI.
6. **Container UI** — SwiftUI: grid of installed apps, `.apk` import via document picker, tap-to-launch full-screen container view, per-app detail/uninstall, running-apps list if backgrounding is supported.

## Benchmark target

**Among Us** (Unity/IL2CPP) is the integration-test target — it exercises native code execution, real-framerate OpenGL ES rendering, multitouch, sockets, and audio all at once. It is the *last* thing to try, not the first: get a minimal, mostly-Java, non-native test APK running end-to-end through every layer first.

## Requirements

- Device: modern iPhone/iPad, arm64
- [Sidestore](https://sidestore.io/) to install app ([LiveContainer](https://github.com/LiveContainer/LiveContainer) will be tested later on)
- Sufficient storage to install APK's

## Non-goals

- 32-bit (`armeabi-v7a`-only) APKs
- Full Android API surface parity
- App Store distribution (given the JIT/codesigning constraints above, this is unlikely to be shippable there — state this plainly rather than after the fact)

## Contributing

By contributing to this project via Pull Requests or issue submissions, you explicitly agree to the terms of the [APKContainer CLA](CLA.md).
