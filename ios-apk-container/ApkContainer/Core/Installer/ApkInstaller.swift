//
//  ApkInstaller.swift
//  ApkContainer
//
//  Status: IMPLEMENTED orchestration; relies on ApkParser + extractors which
//  are real (ZIP + AXML) except for the resources.arsc-driven parts which are
//  PARTIAL (see ResourcesArscReader / IconExtractor headers).
//
//  Orchestrates a single APK install:
//    1. Copy the picked APK into a temp working dir (stable URL — file
//       providers / iCloud URLs are not always re-openable later).
//    2. Parse AndroidManifest.xml via ApkParser.
//    3. Create the per-app sandbox via SandboxManager.
//    4. Extract arm64-v8a native libs into `<sandbox>/lib/`.
//    5. Extract the launcher icon into `<sandbox>/icon.png`.
//    6. Inspect classes.dex header for the class count.
//    7. Write `<sandbox>/info.json` (the AppRecord).
//    8. Return the AppRecord. The caller (AppCatalog.install) registers it
//       in catalog.json.
//
//  Deviation from spec: the spec lists "create sandbox" as step 5, after the
//  extractors. We create the sandbox first (step 3) so the extractors can
//  write directly into it via their `into:` parameter — this avoids an extra
//  temp-dir + move step. The net effect is identical to the spec's intent.
//
//  Honesty contract:
//    - Real orchestration: ZIP, AXML, native lib extraction, icon extraction
//      (heuristic), sandbox creation, DEX header inspection.
//    - STUB: resources.arsc label/icon resolution — we fall back to a literal
//      manifest label or "[unknown label]". See ResourcesArscReader.
//

import Foundation

/// Orchestrates a single APK install. Stateless aside from the sandbox root
/// discovered via `CatalogStore`; safe to instantiate per-operation or share.
public final class ApkInstaller {

    public init() {}

    /// Installs the APK at `apkURL` and returns the resulting `AppRecord`.
    /// The caller (typically `AppCatalog.install`) is responsible for
    /// persisting the record to `catalog.json`.
    public func install(apkURL: URL) async throws -> AppRecord {
        // 1. Copy APK into a temp working dir.
        let tempDir = makeTempWorkingDir()
        defer { try? FileManager.default.removeItem(at: tempDir) }

        let workingAPK = tempDir.appendingPathComponent("source.apk")
        do {
            try FileManager.default.copyItem(at: apkURL, to: workingAPK)
        } catch {
            // `copyItem` fails across security-scoped resources sometimes; fall
            // back to a byte copy via Data.
            do {
                let data = try Data(contentsOf: apkURL, options: [.alwaysMapped])
                try data.write(to: workingAPK, options: [.atomic])
            } catch {
                throw ApkInstallerError.io(underlying: error)
            }
        }

        // 2. Parse manifest.
        let manifest: ApkManifest
        do {
            manifest = try ApkParser.parse(apkURL: workingAPK)
        } catch let p as ApkParseError {
            throw ApkInstallerError.parser(p)
        }

        // 3. Create sandbox. (Reordered from spec; see header.)
        let sandboxPaths: SandboxPaths
        do {
            sandboxPaths = try SandboxManager.shared.createSandbox(forPackage: manifest.packageName)
        } catch {
            throw ApkInstallerError.sandboxSetupFailed(error.localizedDescription)
        }

        // 4. Extract native libs. May throw .unsupportedAbi (the honest
        //    "32-bit only" case).
        let nativeLibs: [String]
        do {
            nativeLibs = try NativeLibExtractor.extract(
                apkURL: workingAPK,
                into: sandboxPaths.lib
            )
        } catch let e as ApkInstallerError {
            // Roll back the half-created sandbox on failure.
            try? SandboxManager.shared.removeSandbox(forPackage: manifest.packageName)
            throw e
        } catch {
            try? SandboxManager.shared.removeSandbox(forPackage: manifest.packageName)
            throw ApkInstallerError.io(underlying: error)
        }

        // 5. Extract icon. Non-fatal if missing.
        let iconURL: URL?
        do {
            iconURL = try IconExtractor.extract(
                apkURL: workingAPK,
                manifest: manifest,
                into: sandboxPaths.root
            )
        } catch {
            // An icon failure shouldn't block install.
            NSLog("[ApkInstaller] Icon extraction failed: \(error.localizedDescription)")
            iconURL = nil
        }

        // 6. DEX class count. Non-fatal if missing.
        let dexClassCount = inspectDexClassCount(apkURL: workingAPK)

        // 6b. Extract classes.dex (and classes2.dex, etc.) into <sandbox>/dex/.
        //     The native ART loader needs a pre-extracted path; it does not
        //     read APKs directly. Non-fatal: a missing DEX means a pure-native
        //     app (rare), which we still install but cannot run.
        let dexDir = sandboxPaths.root.appendingPathComponent("dex", isDirectory: true)
        let dexPaths: [String]
        do {
            dexPaths = try DexExtractor.extract(apkURL: workingAPK, into: dexDir)
        } catch {
            NSLog("[ApkInstaller] DEX extraction failed: \(error.localizedDescription)")
            dexPaths = []
        }
        let classesDexPath: String? = dexPaths.first

        // 7. Resolve label. Try resources.arsc first (STUB — always nil for
        //    now); fall back to a literal label from the manifest; finally
        //    "[unknown label]".
        let label = resolveLabel(manifest: manifest, apkURL: workingAPK)

        // 8. Build the AppRecord.
        //    Normalize empty/zero version fields to nil so the UI can hide
        //    the version line cleanly when the manifest didn't declare one.
        let versionName: String? = manifest.versionName.isEmpty ? nil : manifest.versionName
        let versionCode: Int? = manifest.versionCode != 0 ? manifest.versionCode : nil
        let record = AppRecord(
            id: manifest.packageName,
            name: label,
            versionName: versionName,
            versionCode: versionCode,
            minSdk: manifest.minSdk,
            targetSdk: manifest.targetSdk,
            iconPath: iconURL?.path,
            nativeLibs: nativeLibs,
            dexClassCount: dexClassCount,
            permissions: manifest.permissions,
            installDate: Date(),
            sandboxPath: sandboxPaths.root.path,
            launcherActivity: manifest.launcherActivity,
            classesDexPath: classesDexPath
        )

        // 9. Write per-app info.json into the sandbox.
        try writeInfoJSON(record: record, into: sandboxPaths.root)

        return record
    }

    // MARK: - Helpers

    private func makeTempWorkingDir() -> URL {
        let base = FileManager.default.temporaryDirectory
            .appendingPathComponent("apklive-install", isDirectory: true)
        try? FileManager.default.createDirectory(
            at: base, withIntermediateDirectories: true
        )
        return base.appendingPathComponent(UUID().uuidString, isDirectory: true)
    }

    /// Extracts `classes.dex` from the APK to a temp file and inspects the
    /// header. Returns -1 on any failure (non-fatal).
    private func inspectDexClassCount(apkURL: URL) -> Int {
        guard let zip = try? ZipReader(apkURL: apkURL),
              let entry = zip.entry(named: "classes.dex") else {
            return -1
        }
        do {
            let dexData = try zip.readData(for: entry)
            return DexInspector.classCount(dexData: dexData)
        } catch {
            return -1
        }
    }

    /// Resolves the app label. Real resolution requires resources.arsc
    /// (currently STUB). Falls back to the manifest's literal label or
    /// "[unknown label]".
    private func resolveLabel(manifest: ApkManifest, apkURL: URL) -> String {
        // 1) If the manifest has a literal string label, use it.
        if !manifest.labelRes.isEmpty && !manifest.labelRes.hasPrefix("@") {
            return manifest.labelRes
        }
        // 2) Try resources.arsc. (Currently always returns nil.)
        if let arscData = readArsc(apkURL: apkURL),
           let resolved = ResourcesArscReader.extractLabel(arscData: arscData, manifest: manifest) {
            return resolved
        }
        // 3) Fallback. Honest: we don't know the label.
        return "[unknown label]"
    }

    /// Reads `resources.arsc` from the APK into memory. Returns nil on any
    /// failure (APKs without resources.arsc exist but are rare).
    private func readArsc(apkURL: URL) -> Data? {
        guard let zip = try? ZipReader(apkURL: apkURL),
              let entry = zip.entry(named: "resources.arsc") else {
            return nil
        }
        return try? zip.readData(for: entry)
    }

    /// Writes the AppRecord as `info.json` into the sandbox root.
    private func writeInfoJSON(record: AppRecord, into sandboxRoot: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(record)
        let url = sandboxRoot.appendingPathComponent("info.json")
        try data.write(to: url, options: [.atomic])
    }
}
