# Capability Matrix — APKLive (Phase 2)

> Legend: ✅ **Implemented** (real, wired) · 🟡 **Partial** (real but limited) ·
> 🟥 **Stubbed** (logs + returns default, not real) · ❌ **Unsupported** (will
> fail, by design) · ⏳ **Planned** (not yet in repo)

Phase 2 upgrade: the DEX interpreter, software GLES 2.0, ELF relocations, JNI
bridge, and per-run log file are now REAL. The honest milestone reached: **a
trivial test APK that draws a textured triangle via GLES 2.0 should render
pixels to the screen.** Real games like Among Us need more (full framework,
IL2CPP, GLES 3, audio, networking) — see the matrix below.

---

## 1. APK install / parsing

| Capability | Status | Among Us relies on | Notes |
|---|---|---|---|
| ZIP read (store + DEFLATE) | ✅ | yes | From-scratch Swift `ZipReader` |
| Binary AndroidManifest.xml (AXML) parse | ✅ | yes | `BinaryManifestParser` |
| Launcher Activity detection (MAIN/LAUNCHER intent-filter) | ✅ | yes | `ApkParser.detectLauncherActivity` — heuristic; falls back to first `<activity android:name>` |
| `resources.arsc` global string pool read | ✅ | yes | `ResourcesArscReader` |
| `resources.arsc` entry→value reference resolution | 🟥 | yes (label/icon) | STUB; heuristics used for label/icon |
| `classes.dex` extraction to sandbox | ✅ | yes | `DexExtractor` writes `<sandbox>/dex/classes.dex` (+ classes2.dex …) |
| `classes.dex` header inspection | ✅ | yes | `DexInspector` |
| `lib/arm64-v8a/*.so` extraction | ✅ | yes | `NativeLibExtractor` |
| `armeabi-v7a` (32-bit) native libs | ❌ | — | Install throws `unsupportedAbi` |
| Adaptive icon XML resolution | 🟥 | partial | Foreground PNG only |
| Per-app sandbox dir layout | ✅ | yes | `SandboxManager` |
| Catalog persistence | ✅ | yes | JSON in Application Support |
| `.apk` UTI declaration | ⏳ | — | Needs `UTExportedTypeDeclarations` in Info.plist |

---

## 2. DEX / ART execution layer

| Capability | Status | Among Us relies on | Notes |
|---|---|---|---|
| DEX file loader (header + string/type/proto/field/method/class tables) | ✅ | yes | `dex_loader.cpp` — mmap + MUTF-8 + class_data walk |
| DEX bytecode interpreter (~130 opcodes) | ✅ | yes | `dex_interp.cpp` — const, move, arithmetic, branches, return, new-instance, iget/iput/sget/sput, invoke-{static,virtual,direct,interface}, aget/aput, throw, try/catch |
| Object model (dex_obj_t, dex_cls_t, vtable, field layout) | ✅ | yes | Bump allocator + free-list; **no GC** (logs WARN at 64 MiB) |
| Method dispatch (virtual override / direct / static / interface) | ✅ | yes | |
| Exception handling (try/catch, throw, propagation) | ✅ | yes | Catch-all via Throwable/Exception matching |
| Framework method stubs (~130 methods across ~30 classes) | ✅ | yes | java.lang.{Object,String,StringBuilder,Math,Integer,Thread,Throwable}, java.io.{PrintStream,File}, android.util.Log, android.app.Activity (lifecycle + utilities), android.view.{View,Window}, android.content.Context, android.os.{Bundle,Build}, android.opengl.GLSurfaceView (Java side) |
| Activity lifecycle dispatch (onCreate/onStart/onResume/onPause/onStop/onDestroy) | ✅ | yes | `art_runtime_dispatch_activity` |
| `Build.VERSION.SDK_INT` = 33 | ✅ | yes | Hardcoded in framework stubs |
| `Context.getPackageName()` / `getFilesDir()` | ✅ | yes | Backed by per-VM config |
| `android.opengl.GLSurfaceView` Java wrapper | ✅ (no-op) | yes | Renderer/RenderMode/onResume/onPause log + no-op; native GL calls go through JNI to swgl |
| Multi-threading | ❌ | partial | Single-threaded; `monitor-enter/exit` are no-ops. Apps spawning real threads will misbehave. |
| GC | ❌ | yes | None — heap grows unbounded. Long-running apps OOM. |
| Reflection / ClassLoader / custom class loaders | ❌ | no | Not implemented |
| `invoke-polymorphic` / `invoke-custom` / MethodHandle | ❌ | no | Unknown-opcode path |
| Full Android framework JAR | ❌ | — | Scoped subset only; deep-API apps fail |

---

## 3. Native loader & Bionic libc shim

| Capability | Status | Among Us relies on | Notes |
|---|---|---|---|
| ELF64 `aarch64` `ET_DYN` header validation | ✅ | yes | `elf_loader.c` |
| `PT_LOAD` mapping + BSS zero-fill | ✅ | yes | |
| **AARCH64 relocations** (RELATIVE, ABS64, GLOB_DAT, JUMP_SLOT, TLS_*, COPY, IRELATIVE) | ✅ | yes | Real apply loop |
| **DT_GNU_HASH + DT_HASH symbol lookup** | ✅ | yes | Both, GNU preferred |
| `DT_NEEDED` resolution (sibling modules + shim libs + host dlsym) | ✅ | yes | `apkcontainer_elf_register_loaded` for sibling `.so`s |
| `DT_INIT` / `DT_INIT_ARRAY` execution | ✅ | yes | |
| `JNI_OnLoad` invocation with our `JavaVM*` | ✅ | yes | |
| `mprotect PROT_EXEC` on code segments (TrollStore) | ✅ | yes | |
| `mprotect PROT_EXEC` on code segments (normal sideload) | ❌ | — | Returns EPERM → load fails. By design. |
| Lazy PLT binding | 🟡 | yes | Eager binding (no trampoline needed) |
| TLS slot layout (`__get_tls`, `TLS_SLOT_*`) | 🟡 | yes | Bionic-compatible TLS allocated per thread |
| `pthread_*` (create/join/mutex/cond/rwlock/key) | ✅ | yes | Wraps Darwin pthread |
| `libc` string/mem/stdlib | ✅ | yes | Forwarded to Darwin libsystem |
| `malloc`/`free`/`calloc`/`realloc` | ✅ | yes | Darwin |
| `ashmem_create_region` / `ashmem_*` | ✅ | yes | Emulated via `mmap MAP_ANON` + fd registry |
| `binder` ioctls | 🟥 | no | `ioctl(BINDER_*)` → `ENOSYS` |
| `__system_property_get` | ✅ | yes | Returns hardcoded `ro.build.version.sdk=33` etc. (ABI-correct 2-arg signature) |
| `android/log.h` `__android_log_print` | ✅ | yes | → log_file + os_log |
| `prctl` / `getrandom` / `getauxval` | 🟡 | partial | Subset mapped; rest STUB |
| `ifunc` resolvers, Bionic-internal `__bionic_*` | ❌ | no | Unsupported |

---

## 4. JNI bridge

| Capability | Status | Among Us relies on | Notes |
|---|---|---|---|
| `JavaVM*` vtable (`JNIInvokeInterface`: `AttachCurrentThread`, `DetachCurrentThread`, `GetEnv`, `DestroyJavaVM`) | ✅ | yes | Full vtable in standard layout |
| `JNIEnv*` vtable (`JNINativeInterface_`, ~230 entries) | ✅ | yes | Standard layout; common entries real, rest log + default |
| `FindClass` | ✅ | yes | Resolves against DEX interpreter |
| `GetMethodID` / `GetStaticMethodID` | ✅ | yes | Intern'd descriptors; lazy resolution at invoke |
| `GetFieldID` / `GetStaticFieldID` | ✅ | yes | |
| `RegisterNatives` / `UnregisterNatives` | ✅ | yes | Critical for IL2CPP |
| `NewStringUTF` / `GetStringUTFChars` / `ReleaseStringUTFChars` | ✅ | yes | |
| `NewIntArray` / `GetIntArrayElements` / `ReleaseIntArrayElements` / `SetIntArrayRegion` | ✅ | yes | Backed by `jni_array_hdr` |
| `NewByteArray` / `GetByteArrayElements` / `SetByteArrayRegion` | ✅ | yes | |
| `NewFloatArray` / `GetFloatArrayElements` | ✅ | yes | |
| `NewObject` / `NewObjectV` | ✅ | yes | |
| `CallVoidMethod` / `CallObjectMethod` / `CallIntMethod` / `CallBooleanMethod` (+V variants) | ✅ | yes | Variadic arg unpacking by shorty |
| `CallStaticVoidMethod` / `CallStaticObjectMethod` / `CallStaticIntMethod` (+V variants) | ✅ | yes | |
| `GetIntField` / `SetIntField` / `GetObjectField` / `GetBooleanField` | ✅ | yes | |
| `GetStaticIntField` / `SetStaticIntField` / `GetStaticObjectField` | ✅ | yes | |
| `NewGlobalRef` / `DeleteGlobalRef` / `DeleteLocalRef` / `NewLocalRef` / `IsSameObject` | ✅ | yes | Identity-based (no refcounting in v1) |
| `GetJavaVM` | ✅ | yes | |
| `Throw` / `ThrowNew` / `ExceptionOccurred` / `ExceptionClear` / `ExceptionCheck` | ✅ | yes | Thread-local flag |
| `NewDirectByteBuffer` / `GetDirectBufferAddress` / `GetDirectBufferCapacity` | ✅ | yes | Critical for IL2CPP mesh data |
| `GetVersion` (returns JNI 1.6) | ✅ | yes | |
| `IsInstanceOf` | 🟡 | yes | Permissive (returns TRUE) |
| `GetObjectClass` | 🟥 | yes | STUB — caller should use FindClass |
| `CallByte/Char/Short/Long/Float/Double*Method` | 🟥 | partial | STUB — return 0 |
| `CallNonvirtual*Method` | 🟥 | no | Treated as virtual (partial) |
| `DefineClass` | ❌ | no | Not implemented |
| `ToReflectedMethod` / `ToReflectedField` | ❌ | no | Not implemented |
| `GetStringChars` / `ReleaseStringChars` (UTF-16) | 🟥 | no | STUB |
| `NewObjectArray` / `GetObjectArrayElement` / `SetObjectArrayElement` | 🟥 | partial | STUB |

---

## 5. Graphics bridge (software GLES 2.0 → Metal)

| Capability | Status | Among Us relies on | Notes |
|---|---|---|---|
| **Software GLES 2.0 rasterizer** (`swgl.cpp`) | ✅ | yes | Real: triangles, textures, shaders, blend, depth, scissor |
| EGL subset (GetDisplay, Initialize, ChooseConfig, CreateContext, MakeCurrent, CreateWindowSurface, SwapBuffers, Destroy*, Terminate) | ✅ | yes | |
| `glClear` / `glClearColor` / `glViewport` / `glScissor` | ✅ | yes | |
| `glEnable` / `glDisable` / `glBlendFunc` / `glDepthFunc` / `glDepthMask` / `glColorMask` | ✅ | yes | |
| Shader compile (`glCreateShader`, `glShaderSource`, `glCompileShader`, `glGetShaderiv`, `glGetShaderInfoLog`) | ✅ | yes | |
| Program link (`glCreateProgram`, `glAttachShader`, `glLinkProgram`, `glGetProgramiv`, `glGetProgramInfoLog`, `glUseProgram`) | ✅ | yes | |
| `glGetAttribLocation` / `glGetUniformLocation` | ✅ | yes | |
| `glUniform*` (1i/1f/2f/3f/4f/Matrix4fv/1fv/2fv/3fv/4fv/1iv) | ✅ | yes | |
| `glGenBuffers` / `glBindBuffer` / `glBufferData` / `glDeleteBuffers` | ✅ | yes | |
| `glGenTextures` / `glBindTexture` / `glTexImage2D` / `glTexSubImage2D` / `glTexParameteri` / `glActiveTexture` / `glDeleteTextures` | ✅ | yes | RGBA/RGB/RED/LUMINANCE/etc. + UBYTE/USHORT_5_6_5/FLOAT |
| `glEnableVertexAttribArray` / `glDisableVertexAttribArray` / `glVertexAttribPointer` / `glVertexAttrib4f` | ✅ | yes | |
| `glDrawArrays` (TRIANGLES/STRIP/FAN) | ✅ | yes | |
| `glDrawElements` (UBYTE/USHORT/UINT) | ✅ | yes | |
| **GLSL ES 1.0 interpreter** (`glsl.cpp`) | ✅ | yes | Real lexer + parser + AST + interpreter |
| GLSL types: void/float/vec2-4/mat2-4/int/ivec2-4/bool/sampler2D | ✅ | yes | |
| GLSL qualifiers: attribute/uniform/varying/const | ✅ | yes | |
| GLSL built-ins: gl_Position, gl_FragColor, gl_FragCoord, gl_PointCoord, gl_PointSize | ✅ | yes | |
| GLSL operators: + - * /, unary -, !, comparison, logical, assignment, swizzles (read+write), subscript | ✅ | yes | |
| GLSL built-in functions: mix, clamp, min, max, abs, sqrt, length, normalize, dot, cross, reflect, texture2D, pow, sin, cos, radians, degrees | ✅ | yes | |
| GLSL constructors: vec2-4, ivec2-4, mat2-4, casts | ✅ | yes | |
| GLSL control flow: if/else | ✅ | yes | |
| GLSL control flow: for/while | ❌ | no | Not implemented (logged + rejected) |
| GLSL user-defined functions, structs, dynamic indexing | ❌ | no | Not implemented |
| **Framebuffer → CAMetalLayer upload** (Swift `SurfaceState`) | ✅ | yes | CADisplayLink reads `swgl_get_framebuffer` → MTLTexture replace → CAMetalLayer drawable present |
| GL_POINTS / GL_LINES / GL_LINE_STRIP / GL_LINE_LOOP | 🟥 | no | Stubbed (no output) |
| FBOs (`glGenFramebuffers`) | ❌ | no | Not implemented |
| Cubemaps | ❌ | no | Not implemented |
| Mipmaps | 🟡 | no | Only level 0 sampled; mipmapped min_filter falls back to level 0 |
| Multi-sample / stencil | ❌ | no | Not implemented |
| `glBlendEquation` / `glBlendFuncSeparate` | ❌ | no | Always FUNC_ADD |
| GLES 3.0 core (instancing, transform feedback, etc.) | ❌ | yes (Among Us targets GLES 3) | **Among Us will fail** on GLES 3 paths; needs ANGLE for real GLES 3 |
| Vulkan | ❌ | no | Unsupported |
| SurfaceFlinger | ❌ | no | Single-surface direct-to-layer |

---

## 6. Input

| Capability | Status | Among Us relies on | Notes |
|---|---|---|---|
| Single-touch | ✅ | yes | |
| Multi-touch (up to ~10 pointers) | ✅ | yes | Stable pointer IDs per `UITouch` |
| Point→pixel coordinate conversion | ✅ | yes | Scaled to swgl surface size |
| `MotionEvent` action codes (DOWN/UP/MOVE/POINTER_DOWN/POINTER_UP/CANCEL) | ✅ | yes | |
| Hardware keyboard → `KeyEvent` | 🟡 | no | Common keys only |
| IME / text input | 🟥 | no | STUB |
| GameController / MFi controller | ⏳ | no | Planned |
| Accelerometer / gyroscope → `SensorEvent` | 🟥 | no | STUB |

---

## 7. Audio

| Capability | Status | Among Us relies on | Notes |
|---|---|---|---|
| OpenSL ES engine shell | 🟥 | yes | Skeleton only; PCM routing not wired |
| AAudio | ❌ | no | Returns `AAUDIO_ERROR_UNIMPLEMENTED` |
| `android.media.AudioTrack` / `MediaPlayer` (Java) | 🟥 | yes | STUB |
| MIDI | ❌ | no | |

---

## 8. Lifecycle & process model

| Capability | Status | Among Us relies on | Notes |
|---|---|---|---|
| `onCreate` → `onStart` → `onResume` on launch | ✅ | yes | Driven from SwiftUI via `art_runtime_dispatch_activity` |
| `onPause` → `onStop` on background | ✅ | yes | Scene phase |
| `onDestroy` on force-quit / uninstall | ✅ | yes | Unloads `.so`, frees ART |
| `onSaveInstanceState` round-trip | 🟥 | no | No-op |
| Multi-instance (same APK launched twice) | ❌ | no | One instance per package |
| Background services / `Service` component | ❌ | no | Unsupported |
| `BroadcastReceiver` | ❌ | no | Unsupported |
| Multiple Activities in one APK | 🟡 | no | Only the launcher Activity is launched |

---

## 9. Distribution / platform

| Capability | Status | Notes |
|---|---|---|
| TrollStore install (iOS 14.0–16.6.1) | ✅ target | Primary supported path |
| Jailbreak (palera1n / Dopamine) | ✅ target | Secondary |
| App Store | ❌ | Entitlements unavailable |
| Free sideload | ❌ | Entitlements unavailable |
| Paid dev sideload (non-jailbroken) | ❌ | Native `.so` execution impossible |
| iOS 17+ non-jailbroken | ❌ | TrollStore not available |
| iPhone (arm64) | ✅ | Required |
| iPad (arm64) | 🟡 | Works but UI tuned for iPhone |
| Apple Silicon Mac (Catalyst) | ❌ | Out of scope |

---

## 10. Diagnostics

| Capability | Status | Notes |
|---|---|---|
| Per-run log file (`<AppSupport>/APKLive/logs/apklive-<epochms>.log`) | ✅ | `log_file.c` — timestamped, tagged, leveled; mirrors to os_log |
| Log rotation (keep last 5) | ✅ | `log_rotate` at init |
| LogViewer screen (SwiftUI, auto-follow) | ✅ | `LogViewer.swift` — Settings → View Log, AppDetailView → View Last Run Log |
| Surface launch errors with log path | ✅ | `RuntimeError.nativeFailure(code:)` shown in alert |
| os_log subsystem `com.you.apklive` | ✅ | `log stream --predicate 'subsystem == "com.you.apklive"'` |

---

## 11. Among Us specifically — integration-test target

| Requirement | Component | Status | Expected |
|---|---|---|---|
| Parse APK + extract `lib/arm64-v8a/{libunity,libil2cpp,libmain}.so` | §1 | ✅ | OK |
| Run `libil2cpp.so` native code (arm64) | §3 | ✅ loader / ✅ JNI | Native code loads; `JNI_OnLoad` runs; `RegisterNatives` records |
| IL2CPP `RegisterNatives` ↔ our `JavaVM*` | §4 | ✅ | Wired |
| GLES 3 rendering → Metal | §5 | ❌ (GLES 2 only) | **Among Us targets GLES 3; swgl is GLES 2. Needs ANGLE for GLES 3.** |
| Multitouch | §6 | ✅ | OK |
| OpenSL ES audio | §7 | 🟥 | Not wired — no audio |
| UDP networking (lobby/multiplayer) | §2 java.net | 🟥 | Java-net wrapper STUB; native BSD sockets would work but Java wrapper isn't |
| `Activity` lifecycle | §8 | ✅ | OK |

**Verdict (honest):** Among Us has **not** been booted and is not claimed to
work. The blocker for Among Us specifically is **GLES 3** — swgl implements
GLES 2.0, but Among Us (via Unity) targets GLES 3. To run Among Us, you need
to build + link ANGLE per `BUILD_AND_RUN.md §2` (which provides GLES 3 over
Metal). With ANGLE linked, the graphics_bridge falls back to ANGLE symbols for
any GLES 3 call swgl doesn't implement. The DEX interpreter, ELF loader, JNI
bridge, and lifecycle are all real and should handle Among Us's Java side.

**Realistic first milestone (reached by this Phase 2 work):** a trivial test
APK with a single Activity that calls GLES 2.0 to draw a textured triangle
should render pixels to the screen via the software rasterizer + Metal
framebuffer upload. This has not been verified on a device (no Xcode in this
sandbox) but every component in the path is a real implementation.
