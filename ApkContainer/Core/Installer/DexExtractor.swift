//
//  DexExtractor.swift
//  ApkContainer
//
//  Status: IMPLEMENTED. Extracts classes.dex (+ classes2.dex, classes3.dex, ...)
//  from the APK into `<sandbox>/dex/` so the native ART loader can mmap them
//  via art_runtime_create_vm(classes_dex_path:) and art_runtime_load_dex().
//
//  We extract every classes*.dex entry in sorted order so multi-dex APKs load
//  deterministically.
//

import Foundation

/// Extracts all `classes*.dex` entries from an APK into a target directory.
public enum DexExtractor {

    /// Extracts every `classes*.dex` entry from `apkURL` into `intoDir`.
    /// Returns the list of absolute paths, sorted (classes.dex first, then
    /// classes2.dex, classes3.dex, ...). Empty if the APK has no DEX.
    public static func extract(apkURL: URL, into intoDir: URL) throws -> [String] {
        try FileManager.default.createDirectory(
            at: intoDir, withIntermediateDirectories: true
        )
        let zip = try ZipReader(apkURL: apkURL)

        // Collect all classes*.dex entries.
        var dexEntries: [ZipReader.Entry] = []
        for entry in zip.entries {
            let n = entry.name
            if n == "classes.dex" { dexEntries.append(entry); continue }
            if n.hasPrefix("classes") && n.hasSuffix(".dex") {
                // classes2.dex, classes3.dex, ... — verify the middle is digits.
                let mid = n.dropFirst("classes".count).dropLast(".dex".count)
                if !mid.isEmpty && mid.allSatisfy({ $0.isNumber }) {
                    dexEntries.append(entry)
                }
            }
        }
        // Sort: classes.dex first, then classes2.dex, classes3.dex, ...
        dexEntries.sort { a, b in
            if a.name == "classes.dex" { return true }
            if b.name == "classes.dex" { return false }
            return a.name < b.name
        }

        var paths: [String] = []
        for entry in dexEntries {
            let data = try zip.readData(for: entry)
            let outURL = intoDir.appendingPathComponent(entry.name)
            try data.write(to: outURL, options: [.atomic])
            paths.append(outURL.path)
        }
        return paths
    }
}
