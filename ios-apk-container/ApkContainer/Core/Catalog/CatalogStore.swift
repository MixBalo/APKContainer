//
//  CatalogStore.swift
//  ApkContainer
//
//  Status: IMPLEMENTED.
//
//  Single source of truth for where the APKLive catalog JSON and per-app
//  sandboxes live on disk. Layout:
//
//    <Application Support>/APKLive/
//        catalog.json                ← array of AppRecord
//        <packageId>/                ← per-app sandbox (see SandboxManager)
//            files/ cache/ code_cache/ lib/ databases/ shared_prefs/
//            info.json                ← per-app AppRecord snapshot
//            icon.png
//
//  Honesty contract: real FileManager usage; no stubs.
//

import Foundation

/// Filesystem locations for the APKLive catalog and sandbox root.
public enum CatalogStore {

    /// `<Application Support>/APKLive/`. Created on first access.
    public static let rootURL: URL = {
        let fm = FileManager.default
        let appSupport = try! fm.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        )
        let root = appSupport.appendingPathComponent("APKLive", isDirectory: true)
        // `try!` is acceptable here: if Application Support is unwritable the
        // app cannot function at all. This is a one-time bootstrap, not a
        // runtime code path.
        if !fm.fileExists(atPath: root.path) {
            try! fm.createDirectory(at: root, withIntermediateDirectories: true)
            // Protect user data at rest on disk (best-effort; ignored on
            // pre-iOS 9 / non-APFS volumes).
            try? (root as NSURL).setResourceValue(
                URLFileProtection.completeUntilFirstUserAuthentication,
                forKey: .fileProtectionKey
            )
        }
        return root
    }()

    /// `<root>/catalog.json`. File may not exist yet on first launch; callers
    /// must handle a missing file (AppCatalog.load treats it as empty).
    public static let catalogURL: URL = rootURL.appendingPathComponent("catalog.json")

    /// Per-app sandbox root: `<root>/<packageId>/`.
    public static func sandboxRoot(forPackage id: String) -> URL {
        rootURL.appendingPathComponent(id, isDirectory: true)
    }
}
