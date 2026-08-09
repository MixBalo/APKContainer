//
//  IconExtractor.swift
//  ApkContainer
//
//  Status: PARTIAL.
//    - Real: heuristic-based icon path candidate enumeration + first-found PNG
//      extraction into the sandbox.
//    - STUB: resources.arsc-driven icon resolution (the proper way to find
//      the launcher icon) is not implemented because ResourcesArscReader is
//      partial. Falls back to common paths.
//
//  Note on signature: the spec lists `extract(apkURL:manifest:)`. We add an
//  `into targetDir` parameter because the function must know where to write
//  `icon.png`. The deviation is documented for the orchestrator.
//
//  Honesty contract: every STUB is marked.
//

import Foundation

/// Extracts the launcher icon PNG from an APK into a target directory.
public enum IconExtractor {

    /// Tries to extract the launcher icon. Looks for `res/<drawable-*>/<name>.png`
    /// entries referenced by the manifest's icon string (only literal paths are
    /// usable; `@mipmap/...` references need resources.arsc resolution which is
    /// STUB). Falls back to common launcher-icon paths. Writes the first match
    /// to `<targetDir>/icon.png` and returns its URL. Returns nil if no icon
    /// entry can be found.
    public static func extract(
        apkURL: URL,
        manifest: ApkManifest,
        into targetDir: URL
    ) throws -> URL? {
        let zip = try ZipReader(apkURL: apkURL)
        let candidates = candidatePaths(for: manifest)

        try FileManager.default.createDirectory(
            at: targetDir,
            withIntermediateDirectories: true
        )

        for path in candidates {
            if let entry = zip.entry(named: path) {
                let data = try zip.readData(for: entry)
                // Sanity check: PNG magic header.
                if data.count < 8 { continue }
                if !(data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
                    continue
                }
                let iconURL = targetDir.appendingPathComponent("icon.png")
                try data.write(to: iconURL, options: [.atomic])
                return iconURL
            }
        }
        return nil
    }

    /// Build a list of candidate icon paths to try, in priority order.
    private static func candidatePaths(for manifest: ApkManifest) -> [String] {
        var paths: [String] = []

        // 1) Manifest-provided literal path (if `android:icon` is a path, not a
        //    `@mipmap/...` reference).
        if !manifest.iconRes.isEmpty, !manifest.iconRes.hasPrefix("@") {
            paths.append(manifest.iconRes)
        }

        // 2) STUB-driven resources.arsc resolution: would give us the actual
        //    entry path here. Skipped — see ResourcesArscReader header.
        // if let resolved = ResourcesArscReader.extractBestIconPath(...) { ... }

        // 3) Density-bucket heuristic. We try xxhdpi first (matches most modern
        //    phones), then xhdpi, hdpi, mdpi, and finally no-density.
        let baseNames = ["ic_launcher", "icon", manifest.packageName]
        let densityDirs = [
            "mipmap-xxxhdpi-v4",
            "mipmap-xxhdpi-v4",
            "mipmap-xhdpi-v4",
            "mipmap-hdpi-v4",
            "mipmap-mdpi-v4",
            "drawable-xxxhdpi-v4",
            "drawable-xxhdpi-v4",
            "drawable-xhdpi-v4",
            "drawable-hdpi-v4",
            "drawable-mdpi-v4",
            "mipmap-anydpi-v26", // adaptive icon (vector; PNG fallback handled below)
            "drawable",
            "mipmap"
        ]
        for dir in densityDirs {
            for base in baseNames {
                paths.append("res/\(dir)/\(base).png")
                paths.append("res/\(dir)/\(base).webp")
            }
        }

        // 4) APK-root fallback (rare but seen in older builds).
        paths.append("ic_launcher.png")

        return paths
    }
}
