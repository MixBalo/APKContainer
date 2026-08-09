# APKLive — LiveContainer-style APK runner for iOS

> **Status: ARCHITECTURE + PROJECT SKELETON. Not a bootable product.**
> Read [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) before anything else.

APKLive is an iOS app that installs and runs Android `.apk` files inside a
LiveContainer-style container UI: a grid of installed "apps" with icons,
tap-to-launch, per-app sandboxed storage, and a per-app detail/settings screen.
The benchmark for "it actually works" is **Among Us** (Unity/IL2CPP, native
arm64, OpenGL ES, multitouch, audio, networking).

## ⚠️ Environment note (read this first)

This deliverable was produced in a **web/Next.js Linux sandbox that has no
Xcode, no macOS, and no iOS toolchain**. The Swift/C/C++ sources here are
written to be correct and build-ready on a Mac with **Xcode 15+**, but they
have **not been compiled or run** in this environment, and they have not been
tested on a device. Open the project in Xcode on a Mac to build. There is
nothing to preview in a browser — this is a native iOS app, not a website.

## What's in this repo

```
ios-apk-container/
├─ README.md                         (this file)
├─ docs/
│  ├─ ARCHITECTURE.md                the hard decisions, before any code
│  ├─ CAPABILITY_MATRIX.md           implemented vs stubbed vs unsupported
│  ├─ BUILD_AND_RUN.md               device + provisioning requirements
│  └─ LIMITATIONS.md                 blunt honesty
├─ project.yml                       XcodeGen project spec (run `xcodegen generate`)
├─ ApkContainer/                     Swift app
│  ├─ App/                           @main, ContentView (TabView)
│  ├─ UI/                            LiveContainer-style SwiftUI views
│  ├─ Core/
│  │  ├─ Catalog/                    AppRecord, AppCatalog, CatalogStore
│  │  ├─ Installer/                  ApkInstaller, ApkParser, BinaryManifestParser,
│  │  │                              ZipReader, DexInspector, NativeLibExtractor,
│  │  │                              IconExtractor, ResourcesArscReader
│  │  ├─ Sandbox/                    SandboxManager, PathLayout
│  │  └─ Runtime/                    RuntimeEngine, ActivityLifecycle, GraphicsSurface,
│  │                                 InputBridge, AudioBridge, DistributionProbe
│  └─ Bridging/include/ApkContainer.h   C ABI the Swift façades call
└─ Native/                           C/C++ runtime
   ├─ Loader/  elf_loader.c, bionic_shim.c
   ├─ ART/     art_runtime.cpp       (interpreter-only ART wrapper; embeds AOSP — STUB)
   ├─ JNI/     jni_bridge.cpp
   ├─ Graphics/ graphics_bridge.cpp  (ANGLE-over-Metal — integration STUB)
   ├─ Audio/   opensl_bridge.c
   ├─ Input/   input_bridge.c
   ├─ Lifecycle/ lifecycle_bridge.c
   └─ include/ *.h
```

## The one-paragraph summary

Running a real Android game inside an iOS app is **not** "installing an app" —
it is reimplementing enough of Android's OS/runtime layer that the APK believes
it is on Android. We pick **TrollStore on iOS 14.0–16.6.1** (non-jailbroken,
CoreTrust-bug) as the install path because only it grants the entitlements
needed for JIT-capable memory and unsigned-code execution; we run
**interpreter-only ART** (JIT/AOT disabled) for the Dalvik bytecode; we load the
APK's native `.so` files with a **custom in-process ELF loader** + **Bionic libc
shim**; and we render via **ANGLE (GLES 2/3 over Metal)** into a `CAMetalLayer`.
**Among Us is the integration target, not a verified result** — components 1–5
must each pass on a trivial test APK before Among Us is even attempted.

## Quick links

- Why your device/install path matters: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §1
- What actually works today: [`docs/CAPABILITY_MATRIX.md`](docs/CAPABILITY_MATRIX.md)
- How to build & run: [`docs/BUILD_AND_RUN.md`](docs/BUILD_AND_RUN.md)
- What's fake / what's honest: [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md)

## License

Research/educational. Not affiliated with Google, Apple, Innersloth (Among Us),
or the ANGLE/LiveContainer/TrollStore projects. All trademarks belong to their
owners.
