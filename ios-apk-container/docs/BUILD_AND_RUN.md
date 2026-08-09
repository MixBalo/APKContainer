# Build & Run Guide — APKLive

> This is an **iOS native app**. It cannot be built or run in a browser, on
> Linux, or on Windows. You need a **Mac** with **Xcode 15+** and a **real
> arm64 iPhone**. The Simulator will not work (it's x86_64/arm64-simulator and
> cannot run the Android arm64 `.so` files).

---

## 0. Will this even run for you? (check before investing time)

You need **one** of:

- **TrollStore-compatible iPhone**, iOS **14.0 through 16.6.1** (CoreTrust-bug
  range). Check your iOS version and device at <https://trollstore.app>.
- **Jailbroken iPhone** (palera1n on A11/iOS 15–17 checkra1n; Dopamine on
  A12–A15/iOS 15–16.6.1). JIT/unsigned-exec enabled per-process.

If you have only a stock non-jailbroken iPhone (any iOS), or iOS 17+, or you
intend to distribute via the App Store — **stop. The app will install and open
but every launch will return `RuntimeError.nativeNotAvailable` and no APK will
run.** This is by design (see `docs/ARCHITECTURE.md` §1).

---

## 1. Prerequisites (on your Mac)

- macOS 14 Sonoma or newer
- Xcode 15 or newer (with iOS 16 SDK)
- [XcodeGen](https://github.com/yonaskolb/XcodeGen): `brew install xcodegen`
- Apple Developer account (free works for personal sideload, but see §0 — you
  still need TrollStore/jailbreak to actually run APKs)
- A real arm64 iPhone connected over USB (trust the computer)
- The APK(s) you want to test, transferred to the iPhone's Files app

## 2. Build ANGLE for iOS (required for graphics)

ANGLE is **not vendored** in this repo (it's large and licensed separately).
Build it once:

```bash
# Clone ANGLE
git clone https://chromium.googlesource.com/angle/angle ~/angle
cd ~/angle

# Use the iOS + Metal build recipe (see ANGLE's dev setup docs).
# Install depot_tools per ANGLE docs, then:
python scripts/bootstrap.py
gclient sync

# Build static libraries for device, arm64, Metal backend
gn gen out/ios-arm64 --args='
  target_os="ios"
  target_cpu="arm64"
  is_component_build=false
  is_debug=false
  angle_enable_metal=true
  angle_enable_vulkan=false
  angle_enable_gl=false
  angle_has_frame_capture=false
'
ninja -C out/ios-arm64 libEGL libGLESv2

# The outputs are:
#   out/ios-arm64/libEGL.a
#   out/ios-arm64/libGLESv2.a
# plus the public headers in include/EGL, include/GLES2, include/GLES3
```

Then drop the `.a` files into `Native/ANGLE/lib/` and the headers into
`Native/ANGLE/include/` (create the dirs). The `project.yml` references them.
**If you skip this step, the graphics bridge is unwired and any GL call from an
APK will crash with an unresolved symbol.**

## 3. Generate the Xcode project

```bash
cd /path/to/ios-apk-container
xcodegen generate      # reads project.yml, writes APKContainer.xcodeproj
open APKContainer.xcodeproj
```

## 4. Configure signing & entitlements

In Xcode → target `APKContainer` → **Signing & Capabilities**:

- For a **TrollStore** build: select your team, but the entitlements that
  matter are enforced at install time by TrollStore, not by Xcode signing.
  Set the bundle id to something unique, e.g. `com.you.apklive`.
- Open `ApkContainer/Info.plist` and ensure these are present (they're also in
  `APKContainer.entitlements`):
  ```xml
  <key>com.apple.security.cs.allow-jit</key><true/>
  <key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
  <key>com.apple.security.cs.disable-library-validation</key><true/>
  <key>get-task-allow</key><true/>
  <key>com.apple.developer.kernel.increased-memory-limit</key><true/>
  ```
- Add `UTExportedTypeDeclarations` for `.apk` (conforms to `public.data`) so
  the document picker filters properly.

## 5. Build the .ipa

```bash
xcodebuild -scheme APKContainer -configuration Release \
  -archivePath build/APKContainer.xcarchive archive \
  CODE_SIGNING_ALLOWED=YES

# Export an unsigned IPA (TrollStore will sign it):
mkdir -p build/Payload
cp -R build/APKContainer.xcarchive/Products/Applications/APKContainer.app build/Payload/
cd build && zip -r APKContainer.ipa Payload && cd ..
```

## 6. Install via TrollStore

1. Open **TrollStore** on the iPhone.
2. Tap the `+` → browse to `APKContainer.ipa` → install.
3. Open **Settings → General → VPN & Device Management** and trust the profile
   if prompted (TrollStore usually makes this unnecessary).
4. Launch **APKLive**. Open the **Settings** tab; it should show
   `Distribution: TrollStore`. If it shows `Unsupported`, stop — your device
   can't run APKs and launches will fail honestly.

## 7. (Alternative) Install on jailbreak

- Build the IPA as above.
- Sign with your dev cert normally (no special entitlements handling needed —
  jailbreak lifts W^X globally for your process).
- Sideload via your preferred jailbreak tool (e.g. Sileo, Zebra, or directly).
- In APKLive Settings, you should see `Distribution: Jailbreak`.

## 8. Install an APK and try to run it

1. In APKLive, **Apps** tab → tap `+` → pick a `.apk` from Files.
2. The installer unzips, parses the manifest, extracts native libs + icon, and
   registers the app. The grid shows its icon + name.
3. Tap the app → full-screen container view appears.
4. **What you should see, today:** the container view opens, the HUD shows
   `FPS: 0`, and `RuntimeEngine.launch` returns `nativeNotAvailable` because
   the ART runtime (§2 of the capability matrix) is not wired in this repo.
   **This is the honest current state.** See `docs/LIMITATIONS.md`.
5. **What you will see once ART + ANGLE are integrated:** the app's main
   Activity renders into the Metal layer; touch works; audio plays.

## 9. Recommended first test APKs (in order of difficulty)

Do **not** start with Among Us. Start with:

1. A pure-Java "Hello World" APK (no native libs, single Activity, a `TextView`).
   Validates: install, manifest parse, ART subset, basic lifecycle.
2. A 2D canvas APK drawing a rotating square with `Canvas.drawLine` in `onDraw`.
   Validates: surface, GLES 2.0 via ANGLE, basic render loop.
3. A native-activity APK that calls `glClear` from C++ via `egl*`.
   Validates: ELF loader, Bionic shim, JNI, ANGLE swap.
4. A minimal Unity APK (an empty scene, no assets).
   Validates: IL2CPP load, `libunity.so` + `libil2cpp.so`, Unity player boot.
5. **Among Us.** Integration target. Expect failures; bisect with logs.

## 10. Diagnostics

- **Per-app logs:** `APKLive → App Details → Logs` surfaces the ART + native
  log ring buffer (Bionic stub hits, JNI STUB hits, GL compile errors).
- **os_log:** everything is also tagged `com.you.apklive`; stream with
  `log stream --predicate 'subsystem == "com.you.apklive"'` from a Mac.
- **First-run checklist if a launch fails:**
  1. Settings tab shows `Distribution: TrollStore` (or `Jailbreak`).
  2. The APK's detail view lists `lib/arm64-v8a/*.so` (not `armeabi-v7a`).
  3. ANGLE `.a` files are linked (check the binary for `eglInitialize`).
  4. ART is present (once wired) — check for `art_runtime_init` in logs.

## 11. Known build-time gotchas

- Xcode will warn that the entitlements (`allow-jit`, `allow-unsigned-executable-
  memory`) are not allowed for a normal provisioning profile. **Ignore the
  warning**; TrollStore enforces them at install, not at Xcode signing.
- `disable-library-validation` is required or the app process will be killed
  on first `dlopen` of an APK `.so`.
- If you build for the Simulator, the native loader will reject all `.so`
  files (simulator is not `EM_AARCH64`). This is correct behavior.
- ANGLE's Metal backend needs the `Metal` framework linked; `project.yml`
  already links it.
