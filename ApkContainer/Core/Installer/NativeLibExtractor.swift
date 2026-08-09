//
//  NativeLibExtractor.swift
//  ApkContainer
//
//  Status: IMPLEMENTED.
//
//  Extracts the arm64-v8a `.so` files from the APK into the per-app sandbox
//  `lib/` directory. Throws `ApkInstallerError.unsupportedAbi` if the APK only
//  ships 32-bit `armeabi-v7a` libs — this is the one explicitly unsupported
//  case in the project (iOS dropped 32-bit after iOS 11; see worklog decision
//  #3). Pure-Java/Kotlin APKs with no native libs return an empty list.
//
//  Honesty contract: real extraction via `ZipReader`; no stubs.
//

import Foundation

/// Errors raised by the installer pipeline.
public enum ApkInstallerError: LocalizedError {
    case unsupportedAbi(String)
    case parser(ApkParseError)
    case io(underlying: Error)
    case sandboxSetupFailed(String)
    case missingManifest

    public var errorDescription: String? {
        switch self {
        case .unsupportedAbi(let why):
            return "Unsupported ABI: \(why)"
        case .parser(let p):
            return p.localizedDescription
        case .io(let e):
            return "Installer I/O error: \(e.localizedDescription)"
        case .sandboxSetupFailed(let why):
            return "Sandbox setup failed: \(why)"
        case .missingManifest:
            return "APK is missing AndroidManifest.xml."
        }
    }
}

/// Extracts native libraries from an APK into a target directory.
public enum NativeLibExtractor {

    /// Extracts `lib/arm64-v8a/*.so` from `apkURL` into `sandboxLibsDir`,
    /// preserving filenames. Returns the list of `.so` filenames extracted
    /// (empty if the APK has no arm64-v8a libs but is otherwise valid, e.g.
    /// a pure-Java/Kotlin app).
    ///
    /// Throws `ApkInstallerError.unsupportedAbi` if the APK ships
    /// `lib/armeabi-v7a/*.so` but no `lib/arm64-v8a/*.so` — that is the
    /// explicitly unsupported 32-bit case.
    public static func extract(apkURL: URL, into sandboxLibsDir: URL) throws -> [String] {
        let zip = try ZipReader(apkURL: apkURL)

        let arm64Entries = zip.entries.filter { entry in
            entry.name.hasPrefix("lib/arm64-v8a/") && entry.name.hasSuffix(".so")
        }

        if arm64Entries.isEmpty {
            let v7aEntries = zip.entries.filter { entry in
                entry.name.hasPrefix("lib/armeabi-v7a/") && entry.name.hasSuffix(".so")
            }
            if !v7aEntries.isEmpty {
                // Honest unsupported case. See worklog decision #3.
                throw ApkInstallerError.unsupportedAbi(
                    "armeabi-v7a: 32-bit native libs are not supported on iOS (dropped after iOS 11)"
                )
            }
            // No native libs at all — fine.
            return []
        }

        try FileManager.default.createDirectory(
            at: sandboxLibsDir,
            withIntermediateDirectories: true
        )

        var extracted: [String] = []
        extracted.reserveCapacity(arm64Entries.count)
        for entry in arm64Entries {
            let data = try zip.readData(for: entry)
            let filename = (entry.name as NSString).lastPathComponent
            let dest = sandboxLibsDir.appendingPathComponent(filename)
            try data.write(to: dest, options: [.atomic])
            extracted.append(filename)
        }
        return extracted
    }
}
