# Architecture — APKLive (iOS APK runner)

> This document records the decisions that gate feasibility. It was written
> **before** the code, because the prompt demanded it and because almost every
> downstream choice depends on three upstream decisions: how the app is signed,
> how Dalvik bytecode runs, and how native `.so` code executes.

---

## 0. The problem, stated honestly

LiveContainer works for **iOS apps** because the payload is already a native
iOS binary — same OS, same executable format, same calling conventions. You
re-sign and re-sandbox; the kernel runs it natively.

An **APK** is a different OS's application. It contains:
- **Dalvik/ART bytecode** (`classes*.dex`) targeting the Android framework
  (Activity, SurfaceFlinger, Binder, ActivityManager, PackageManager).
- **Native ARM64 code** (`lib/arm64-v8a/*.so`) compiled against **Bionic** libc
  and the Android NDK ABI, expecting EGL/OpenGL ES, JNI, OpenSL ES, ashmem,
  binder, and Android's TLS/thread layout.

To "run an APK on iOS" you are **reimplementing enough of Android's OS/runtime
layer that the APK believes it is on Android.** There are two distinct
sub-problems:

1. **Managed code** — Dalvik/ART bytecode. Needs an ART/Dalvik interpreter (or
   AOT translation) running inside your process.
2. **Native code** — `.so` libraries. iOS devices are arm64, so for
   `arm64-v8a` APKs you need **no CPU emulation**. You *do* need: a Bionic libc
   shim, JNI bridging back into your ART layer, EGL/GLES reimplemented to route
   to Metal, and faked/removed Android syscalls (`ashmem`, `binder`, ...).

**`armeabi-v7a` (32-bit) APKs are out of scope** — iOS dropped 32-bit process
support after iOS 11. Only `arm64-v8a` native libraries can run in principle.

---

## 1. Distribution & code-signing strategy (the gating decision)

### The iOS constraints that must be solved first

- **W^X (writable XOR executable memory).** iOS enforces W^X for third-party
  apps. You cannot have a page that is both writable and executable unless your
  process holds an entitlement that lifts it. ART's **JIT** and any
  **runtime code generation** need RWX pages.
- **Code signing of the APK's `.so` files.** Every executable page on iOS must
  belong to a code-signed binary. The `.so` files pulled from an APK are
  **unsigned** ARM64 object code. To execute them you must either (a) re-sign
  them at install time with a profile that permits it, or (b) load them via a
  custom in-process loader that maps them and executes through a signed
  trampoline — which still needs an entitlement allowing unsigned executable
  memory.
- **App sandbox.** Each APK needs a writable data dir mimicking
  `/data/data/<package>/`. This is easy on top of iOS's own sandbox (one
  subdirectory per package); it is the *least* hard problem.

### The realistic install paths and what each permits

| Path | JIT (RWX for ART) | Execute unsigned `.so` | Verdict |
|------|-------------------|------------------------|---------|
| App Store | never | never | **Impossible** for this project |
| Free sideload (7-day) | no | no | Impossible |
| Paid dev ($99/yr) sideload | no (no `dynamic-codesigning`) | no | Impossible for native code; only interpreter-only pure-Java apps with no native libs |
| Enterprise cert | no | no | Same as paid dev on modern iOS |
| **TrollStore (iOS 14.0–16.6.1, CoreTrust bug)** | yes via `com.apple.security.cs.allow-jit` + `get-task-allow` | yes via `com.apple.security.cs.allow-unsigned-executable-memory` | **Primary target** |
| **Jailbreak (palera1n / Dopamine)** | yes (W^X lifted per-process via `cs_disable`/MAP_JIT) | yes | **Secondary target** |

### Decision

**Primary install target: TrollStore on iOS 14.0–16.6.1 (CoreTrust-bug devices).**
**Secondary: jailbroken devices.**
**App Store / free sideload / paid-dev sideload on non-jailbroken: NOT SUPPORTED.**

Why TrollStore over jailbreak as primary: TrollStore is non-jailbroken and
survives reboots, so the install is persistent and the user doesn't need a
jailbreak. It exploits a permanent CoreTrust validation bug to install an IPA
with **arbitrary entitlements** that the OS accepts as valid, including
`get-task-allow`, `com.apple.security.cs.allow-jit`, and
`com.apple.security.cs.allow-unsigned-executable-memory`. With those three, we
can (a) mmap pages as RWX for ART's interpreter/JIT, and (b) load and execute
the APK's unsigned `.so` files via our in-process ELF loader. This is exactly
the mechanism SideStore/UTM/AltJIT use.

**Caveat we will not hide:** on TrollStore, JIT memory sometimes needs an
`altjit`-style `pthread_create`/`mmap MAP_JIT` dance and a one-time `cs_ops`
call; on some iOS versions an on-device "JIT enable" via debugger attach is
required. We treat the JIT-enabling sequence as part of `RuntimeEngine` startup
and fail loudly if it can't be enabled. Because we run **interpreter-only ART**
(§2), we actually don't need JIT for correctness — we need RWX only for the
native `.so` loader. JIT is a future performance knob, not a requirement.

### Entitlements the app declares (in the TrollStore IPA)

```xml
<key>get-task-allow</key><true/>
<key>com.apple.security.cs.allow-jit</key><true/>
<key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
<key>com.apple.security.cs.disable-library-validation</key><true/>
<key>com.apple.developer.kernel.increased-memory-limit</key><true/>
```

`disable-library-validation` lets us `dlopen` our own and the APK's `.so`
without each one being re-signed against the same team. `increased-memory-limit`
gives headroom for ANGLE + the loaded `.so`s + ART heap.

---

## 2. ART execution approach (managed code)

### Options considered

1. **Full ART with JIT** — needs RWX at runtime; fastest. Works on TrollStore/
   jailbreak. Large porting effort (ART is ~1M lines of C++).
2. **Interpreter-only ART (JIT + AOT backends disabled)** — no RWX needed for
   ART itself; the interpreter is pre-compiled into our binary. ~5–20x slower
   than JIT. Still a large port, but the interpreter alone is a tractable subset.
3. **AOT-compile DEX to a native binary at install time** — needs to sign the
   generated binary; only works with a cert that supports re-signing generated
   code, which on iOS means TrollStore/jailbreak anyway, *and* you've now got
   the JIT problem at code-gen time. Strictly worse than (2) for our target.
4. **Dalvik interpreter (legacy)** — smaller than ART, but deprecated, doesn't
   match modern DEX (ART-specific opcodes), and modern APKs assume ART semantics.

### Decision

**Interpreter-only ART, JIT/AOT backends disabled.** Embed AOSP's ART interpreter
(`art/runtime/interpreter/`) as C++ source, rip out the JIT compiler
(`art/runtime/jit/`) and the `dex2oat` AOT backend, keep the interpreter, the
dex loader, the verifier (gutted to permissive), the GC, and the JNI entry
points. Run `kInterpreterImpl`/`Switch` interpreters only.

### Why this is acceptable for Among Us

Among Us is **Unity/IL2CPP**. Unity's C# game code is AOT-compiled by IL2CPP
into **native ARM64** machine code inside `libil2cpp.so`. The Dalvik/ART side
holds mostly: the Unity player's Java glue, `Activity`/`SurfaceView` lifecycle,
`AndroidManifest`-driven permissions, and JNI calls from native back into Java
for things like `AndroidJavaObject`. The **hot path is native code**, not
bytecode. So ART interpreter perf (~5–20x slower than JIT) does not dominate
frame time — the dominant costs are the native `.so` execution (native speed)
and the GLES→Metal translation (§4).

### What this does NOT get us

- We do **not** support arbitrary Java framework classes. We implement a
  scoped subset of `android.app.*`, `android.view.*`, `android.os.*`,
  `android.content.*`, `android.util.*`, `java.lang.*`, `java.util.*`,
  `java.io.*`, `java.net.*` — the classes a typical game touches. Everything
  else is a **logged no-op stub** that returns default values, surfaced in the
  capability matrix.
- We do **not** ship the full Android framework JAR. We ship a minimal
  "framework stubs" DEX that provides the class skeletons our interpreter
  resolves against. Apps that call deep framework APIs will fail loudly.

---

## 3. Native loader & Bionic libc shim (the `.so` execution layer)

### What the `.so` files assume

A `lib/arm64-v8a/libfoo.so` is a standard **ELF shared object** for `aarch64`,
built against:
- **Bionic libc** (Android's libc). Symbol names overlap glibc/Darwin libsystem
  but behavior diverges (`__libc_init`, `pthread_atfork`, TLS layout, `android_*`
  helpers, `ashmem`, `tls` slots).
- The **Android NDK ABI**: `JNI_OnLoad`, `JavaVM*`, `JNIEnv*`, `ANativeActivity`,
  `EGL/egl.h`, `GLES2/gl2.h`, `SLES/OpenSLES.h`, `android/log.h`,
  `android/sensor.h`, `android/looper.h`, `android/hwbuffer.h`.
- Syscalls expected from the Android kernel: `ashmem`, `binder` ioctls,
  `/proc/self/...` layout, `prctl(PR_SET_*`, etc.

### Component: custom in-process ELF loader (`Native/Loader/elf_loader.c`)

- `apkcontainer_elf_load(const char *path, elf_module_t *out)`:
  1. `mmap` the file read-only.
  2. Validate ELF header (`EM_AARCH64`, `ET_DYN`, e_machine).
  3. For each `PT_LOAD` program header, `mmap` a region with `PROT_READ|PROT_WRITE`
     (we promote to `PROT_EXEC` after relocations, see signing note below),
     copy file contents, zero-fill `memsz - filesz` (BSS).
  4. Walk `.dynamic`: collect `DT_NEEDED`, `DT_STRTAB`, `DT_SYMTAB`,
     `DT_RELA`/`DT_RELASZ`/`DT_RELAENT`, `DT_PLTREL`/`DT_JMPREL`/`DT_PLTRELSZ`,
     `DT_GNU_HASH`/`DT_HASH`, `DT_INIT`/`DT_FINI`/`DT_INIT_ARRAY`/`DT_FINI_ARRAY`.
  5. Apply `R_AARCH64_RELATIVE`, `R_AARCH64_GLOB_DAT`, `R_AARCH64_JUMP_SLOT`,
     `R_AARCH64_ABS64` relocations. Resolve undefined symbols against the
     **Bionic shim** and our exported `egl*`/`gl*`/`sl*`/`JNI_*` tables.
  6. Set up **Bionic-compatible TLS**: allocate TLS blocks, run `DT_INIT_ARRAY`
     (this is where `__libc_init` and `pthread_key` allocation happen).
  7. Call `JNI_OnLoad(JavaVM*, void*)` if present, capturing the `JavaVM*` we
     provide from the ART layer.
- **Sign note (TrollStore):** because we hold `allow-unsigned-executable-memory`,
  we can `mprotect(..., PROT_READ|PROT_EXEC)` the code segments without the
  pages being part of a signed Mach-O. This is the *entire reason* TrollStore
  is required. On a normal sideload this `mprotect` returns EPERM and loading
  fails — we surface that as a clear error.
- **`DT_NEEDED` resolution order:** the loader resolves internal deps
  (`libc.so`, `libdl.so`, `liblog.so`, `libm.so`, `libandroid.so`,
  `libEGL.so`, `libGLESv2.so`, `libOpenSLES.so`) **to our own shim symbols**,
  not to any on-disk file. External deps (e.g. `libunity.so` needing
  `libil2cpp.so`) are resolved by loading those siblings first.

### Component: Bionic libc shim (`Native/Loader/bionic_shim.c`)

- `libc`/`string.h`/`stdlib.h`/`unistd.h`: forward to Darwin's libsystem where
  semantics match (`memcpy`, `strlen`, `malloc`/`free` via our own arena or
  Darwin's, `open`/`read`/`write`/`close` mapped to `open` with the right flags).
- `pthread_*`: wrap Darwin's `pthread` but emulate Bionic's TLS slot layout
  (`__get_tls()` returns a pointer to a per-thread array of `TLS_SLOT_*`).
  `pthread_key_create`/`getspecific`/`setspecific` mapped 1:1.
- `__libc_init`, `__libc_preinit`, `__system_property_get`: minimal; property
  get returns hardcoded values (`ro.build.version.sdk=33`, etc.) so apps that
  read `Build.VERSION` see a plausible Android.
- `android/log.h` `__android_log_print`: bridge to `os_log` / `NSLog`.
- `ashmem`: emulated with `mmap MAP_ANON` + a per-fd registry so `ashmem_get_size_region`
  etc. work. No cross-process sharing (we're single-process anyway).
- `binder`: stubbed. `ioctl(BINDER_*)` returns `ENOSYS`; `ServiceManager`
  lookups return `NULL`. Most apps don't call binder directly — they go through
  Java framework services. Framework services that *need* binder (e.g. real
  `ActivityManager`, `PackageManager`) are **not implemented**; we provide our
  own in-process fakes for the subset apps touch.
- `prctl`, `getrandom`, `__cxa_*`: mapped where possible.
- **Honest scope:** this is a shim, not a reimplementation. Apps using exotic
  Bionic internals (`__bionic_verify_tm_ptr`, `getauxval`, `ifunc` resolvers)
  may break. We log every unimplemented symbol the first time it's requested.

### Component: JNI bridge (`Native/JNI/jni_bridge.cpp`)

- Implements `JavaVM` (`JNIInvokeInterface`) and `JNIEnv` (`JNINativeInterface`)
  vtables. The `JNIEnv*` handed to `JNI_OnLoad` is ours; native methods the
  `.so` registers via `RegisterNatives` get stored in a per-class table that
  the ART interpreter consults when a method is `native`.
- Implemented core: `FindClass`, `GetMethodID`/`GetStaticMethodID`,
  `GetFieldID`/`GetStaticFieldID`, `NewStringUTF`/`GetStringUTFChars`,
  `NewIntArray`/`GetIntArrayElements`, `CallVoidMethod`/`CallObjectMethod`/
  `CallStaticIntMethod` (and the typed variants), `RegisterNatives`,
  `UnregisterNatives`, `GetJavaVM`, `DefineClass` (STUB), `Throw`/`ExceptionOccurred`.
- Many JNI functions are **STUB** (return 0/NULL + log). The matrix lists them.

---

## 4. Graphics bridge (EGL/GLES → Metal)

### The decision

**Use ANGLE (Google's "Almost Native Graphics Layer Engine") built for iOS with
the Metal renderer backend** as our EGL/GLES2/GLES3 implementation. The loaded
`.so`'s `egl*`, `gl*`, and `gl2ext*` symbol references resolve to ANGLE's
exports. ANGLE renders into a `CAMetalLayer` that we hand it via
`eglCreateWindowSurface` with a pointer to the layer.

### Why ANGLE and not a hand-rolled translator

- Writing a GLES2/3 → Metal translator from scratch is multiple engineer-years
  and you will get shader translation (GLSL ES → MSL) wrong on real game
  shaders. ANGLE already does GLSL ES → MSL via its Metal backend, and ANGLE's
  Metal backend is shipped, tested, and used in Chrome on iOS.
- ANGLE exposes the EGL 1.5 + GLES 3.x symbol surface that Android NDK apps
  link against. We just need to point the loader's symbol resolution at ANGLE.

### Integration shape

1. Build ANGLE as a static `.a` (or a dynamically-loaded `.dylib` we bundle)
   with `angle_enable_metal=1`, `angle_enable_vulkan=0`, target `iphoneos arm64`.
2. Our `graphics_bridge.cpp` exports `eglGetDisplay`/`eglInitialize`/
   `eglCreateWindowSurface`/`eglCreateContext`/`eglMakeCurrent`/`eglSwapBuffers`
   and the full `gl*` table as trampolines into ANGLE. (Alternative: configure
   the ELF loader to resolve `libEGL.so`/`libGLESv2.so` directly to ANGLE's
   exported symbols — preferred, no trampoline overhead.)
3. The Swift `ContainerMTKView` creates a `CAMetalLayer` and hands it to native
   via `apkcontainer_graphics_attach_layer(layer)`, which constructs the
   `EGLNativeWindowType` ANGLE expects (a `CALayer*`).
4. `eglSwapBuffers` → ANGLE presents the next `CAMetalDrawable`.

### What this gives us / doesn't

- **GLES 2.0:** fully supported by ANGLE/Metal.
- **GLES 3.0:** mostly supported (a few extension gaps).
- **GLES 3.1/3.2:** partial (compute shaders are hit-or-miss on Metal).
- **Vulkan:** **unsupported** (no real Vulkan-on-iOS story; out of scope).
- **SurfaceFlinger:** **not implemented.** We render a single surface
  directly to the layer. Apps that assume a compositor hierarchy (multiple
  Surfaces, `SurfaceView` over `TextureView`, system bars) get a flattened
  single-surface approximation.

### Honest perf note

ANGLE-over-Metal adds CPU overhead per draw call vs. native Metal. For
draw-call-heavy games this is real. Among Us is moderately draw-call-heavy but
not pathological; we expect sub-native but playable framerates on A12+ devices,
**unverified**.

---

## 5. Input, audio, and lifecycle bridging

### Input (`Native/Input/input_bridge.c`)

- `UITouch` → Android `MotionEvent`. Multi-touch via `Set<UITouch>`; each touch
  gets a stable pointer id assigned on `began` and released on `ended/cancelled`.
- Coordinates are converted from UIKit points to the Android surface's pixel
  size (the EGL surface size reported by ANGLE), because Android `MotionEvent`
  is in surface pixels, not points.
- Action codes: `ACTION_DOWN`, `ACTION_UP`, `ACTION_MOVE`, `ACTION_POINTER_DOWN`,
  `ACTION_POINTER_UP`, `ACTION_CANCEL`.
- Delivered to the foreground app via its `AInputQueue`/`ANativeActivity` callbacks
  if the app uses `native_app_glue`, or via `onTouchEvent` dispatch into the
  Activity's `DecorView` if the app is Java-driven.
- **Keyboard:** partial — hardware keyboard `UIKeyInput` → Android `KeyEvent`
  for the common keys; IME/text input STUB.

### Audio (`Native/Audio/opensl_bridge.c`)

- Implement a minimal **OpenSL ES** engine (`SLEngineItf`, `SLObjectItf`,
  `SLPlayItf`, `SLBufferQueueItf`, `SLRecordItf`) on top of `AVAudioEngine`:
  - `CreateOutputMix` → an `AVAudioMixerNode` on the default output.
  - `CreateAudioPlayer` with `bufferQueue` data source → an `AVAudioPlayerNode`
    fed from a ring buffer; the SLES `Enqueue` callback pulls PCM frames.
  - Sample rate / channel format conversion via `AVAudioConverter`.
- **AAudio** (the newer NDK audio API): **stubbed** (returns `AAUDIO_ERROR_UNIMPLEMENTED`).
  Many Unity versions still default to OpenSL ES, so this is OK for Among Us;
  if a target app forces AAudio it will have no audio.
- `libamidi.so`, `libaaudio.so`: stubs.

### Lifecycle (`Native/Lifecycle/lifecycle_bridge.c`)

- Driven by the SwiftUI `RunningAppView`: `onAppear` → `onCreate`+`onStart`+`onResume`;
  `onDisappear` (tab switch) → `onPause`+`onStop`; app backgrounded (scene phase
  `background`) → `onStop`; force-quit → `onDestroy` + unload `.so`s + free ART.
- We do **not** fully implement Android's full lifecycle state machine; we
  implement the common path games use. Apps that rely on `onSaveInstanceState`
  round-tripping get a no-op.

---

## 6. Module map (how the pieces connect)

```
SwiftUI UI (ApkContainer/UI)
   │  calls
   ▼
Swift façades (ApkContainer/Core/Runtime)
   │  C ABI  (ApkContainer/Bridging/include/ApkContainer.h)
   ▼
Native runtime (Native/)
   ├─ RuntimeEngine glue        ── orchestrates per-app launch
   ├─ ART interpreter (C++)     ── runs DEX (interpreter-only)
   │     ▲  JNI upcalls
   │     │
   ├─ JNI bridge (C++)          ── JNIEnv*/JavaVM* vtables
   │     ▲  native calls back into ART
   │     │
   ├─ ELF loader (C)            ── loads lib/arm64-v8a/*.so
   │     └─ Bionic shim (C)     ── libc/pthread/TLS/ashmem
   │
   ├─ Graphics bridge (C++)     ── EGL/GLES symbols → ANGLE → Metal → CAMetalLayer
   ├─ Audio bridge (C)          ── OpenSL ES → AVAudioEngine
   ├─ Input bridge (C)          ── UITouch → MotionEvent
   └─ Lifecycle bridge (C)      ── SwiftUI scene phase → Activity lifecycle
```

### Per-app launch sequence (the golden path)

1. User taps an app in `AppLibraryView` → `RunningAppView` appears.
2. `RuntimeEngine.launch(packageId)`:
   a. Resolve `AppRecord` from `AppCatalog`; find sandbox paths.
   b. **Enable JIT/unsigned memory** (TrollStore entitlement check; fail loud if absent).
   c. Spin up (or reuse) a per-package ART instance; load `classes.dex` from the APK.
   d. Run the ELF loader over each `.so` in dependency order (`libmain.so` first,
      then `libunity.so`/`libil2cpp.so` as `DT_NEEDED`).
   e. Each `.so`'s `JNI_OnLoad` runs against our `JavaVM*`; `RegisterNatives`
      wires native methods.
   f. Attach the `CAMetalLayer` from the SwiftUI container to ANGLE via
      `apkcontainer_graphics_attach_layer`.
   g. Start audio engine.
   h. Dispatch `Activity.onCreate` → `onStart` → `onResume` into ART.
3. The app's main thread (an ART thread) starts rendering; `eglSwapBuffers`
   presents to the `CAMetalLayer`; touches flow in via `InputBridge`.

---

## 7. What can go wrong, and what we'll say about it

- **TrollStore unavailable / iOS version out of range** → app launches but the
  library list is empty and the Settings tab shows `Distribution: Unsupported`.
  Every launch returns `RuntimeError.nativeNotAvailable`. **Honest, not fake.**
- **APK ships only `armeabi-v7a`** → install throws
  `ApkInstallerError.unsupportedAbi` with an explanatory message. **No silent
  fallback.**
- **APK's native code calls a Bionic symbol we didn't shim** → first call logs
  `BIONIC_STUB: <symbol>` and returns a default; if the app depends on it, the
  app crashes with a stack trace we surface in the per-app detail view.
- **APK needs a Java framework class we didn't implement** → ART throws
  `ClassNotFoundException`; we catch and surface as "unsupported app" with the
  class name.
- **ANGLE can't translate a shader** → GL compile error logged; surface goes
  black or partial. We surface the GLSL compile error in the HUD's debug overlay.

See `docs/CAPABILITY_MATRIX.md` for the full implemented/stubbed/unsupported
breakdown and `docs/LIMITATIONS.md` for the blunt version.
