//
//  SandboxManager.swift
//  ApkContainer
//
//  Status: IMPLEMENTED.
//
//  Owns the per-app on-disk sandbox layout (`<Application Support>/APKLive/<packageId>/`).
//  Mirrors the Android `/data/data/<pkg>/` structure so the native Bionic shim
//  (Task 4) can serve an APK's own file APIs by simple path rewriting.
//
//  Deviation from spec:
//    - `storageUsed(for:)` and `clearData(for:)` use external label `for:`
//      (spec said `forPackage:`). The Task 3-a UI calls them with `for:`.
//    - `clearData(for:)` is non-throwing. The UI calls it from a sync button
//      action without `try`; errors are logged via `os_log`/`NSLog` and the
//      method silently returns. `createSandbox(forPackage:)` and
//      `removeSandbox(forPackage:)` keep the spec's `forPackage:` label and
//      throwing behavior because they are internal-only (used by ApkInstaller
//      and AppCatalog.uninstall, both of which can handle errors).
//    - Added `clearAllCaches()` (not in spec) because the Task 3-a Settings
//      view calls `SandboxManager.shared.clearAllCaches()`.
//
//  Honesty contract: real FileManager usage; no stubs.
//

import Foundation

/// Errors raised by `SandboxManager`.
public enum SandboxError: LocalizedError {
    case invalidPackageId(String)
    case io(underlying: Error)

    public var errorDescription: String? {
        switch self {
        case .invalidPackageId(let id):
            return "Invalid package id: \(id)"
        case .io(let e):
            return "Sandbox I/O error: \(e.localizedDescription)"
        }
    }
}

/// Manages per-app sandbox directories.
public final class SandboxManager {

    public static let shared = SandboxManager()

    private let fileManager = FileManager.default

    public init() {}

    /// Creates the sandbox directory tree for `id` if it doesn't already exist
    /// and returns the paths. Idempotent — calling twice for the same id is
    /// safe and does not wipe existing data.
    public func createSandbox(forPackage id: String) throws -> SandboxPaths {
        try validate(packageId: id)

        let root = CatalogStore.sandboxRoot(forPackage: id)
        let files = root.appendingPathComponent("files", isDirectory: true)
        let cache = root.appendingPathComponent("cache", isDirectory: true)
        let codeCache = root.appendingPathComponent("code_cache", isDirectory: true)
        let lib = root.appendingPathComponent("lib", isDirectory: true)
        let databases = root.appendingPathComponent("databases", isDirectory: true)
        let sharedPrefs = root.appendingPathComponent("shared_prefs", isDirectory: true)

        for url in [root, files, cache, codeCache, lib, databases, sharedPrefs] {
            try fileManager.createDirectory(at: url, withIntermediateDirectories: true)
        }
        // Best-effort data protection at rest.
        for url in [files, databases, sharedPrefs] {
            try? (url as NSURL).setResourceValue(
                URLFileProtection.completeUntilFirstUserAuthentication,
                forKey: .fileProtectionKey
            )
        }

        return SandboxPaths(
            root: root,
            files: files,
            cache: cache,
            codeCache: codeCache,
            lib: lib,
            databases: databases,
            sharedPrefs: sharedPrefs
        )
    }

    /// Recursive directory size in bytes. Returns 0 if the sandbox does not
    /// exist. Used by the catalog UI to show storage usage per app.
    public func storageUsed(for id: String) -> Int64 {
        guard isValidPackageId(id) else { return 0 }
        let root = CatalogStore.sandboxRoot(forPackage: id)
        guard fileManager.fileExists(atPath: root.path) else { return 0 }
        return directorySize(at: root)
    }

    /// Wipes the per-app data subdirectories (`files`, `cache`, `code_cache`,
    /// `databases`, `shared_prefs`) but keeps `lib` and the install record
    /// (`info.json`, `icon.png`). Equivalent to Android's "Clear data" button.
    ///
    /// Non-throwing per UI contract; errors are logged.
    public func clearData(for id: String) {
        guard isValidPackageId(id) else {
            NSLog("[SandboxManager] clearData rejected invalid package id: \(id)")
            return
        }
        let root = CatalogStore.sandboxRoot(forPackage: id)
        let dataSubdirs = ["files", "cache", "code_cache", "databases", "shared_prefs"]
        for name in dataSubdirs {
            let url = root.appendingPathComponent(name, isDirectory: true)
            if fileManager.fileExists(atPath: url.path) {
                do {
                    try fileManager.removeItem(at: url)
                } catch {
                    NSLog("[SandboxManager] clearData failed to remove \(name): \(error.localizedDescription)")
                    // Continue with the next subdir.
                }
            }
            // Recreate empty so subsequent file API calls don't have to.
            try? fileManager.createDirectory(at: url, withIntermediateDirectories: true)
        }
    }

    /// Removes the entire sandbox directory tree for `id`. Used by uninstall.
    public func removeSandbox(forPackage id: String) throws {
        try validate(packageId: id)
        let root = CatalogStore.sandboxRoot(forPackage: id)
        if fileManager.fileExists(atPath: root.path) {
            do {
                try fileManager.removeItem(at: root)
            } catch {
                throw SandboxError.io(underlying: error)
            }
        }
    }

    /// Clears the `cache/` and `code_cache/` subdirectories of every installed
    /// app's sandbox. Used by the Settings → "Clear All Caches" button. No-op
    /// for apps that have no sandbox yet.
    public func clearAllCaches() {
        let root = CatalogStore.rootURL
        guard fileManager.fileExists(atPath: root.path) else { return }
        guard
            let enumerator = fileManager.enumerator(
                at: root,
                includingPropertiesForKeys: [.isDirectoryKey],
                options: [.skipsHiddenFiles, .skipsSubdirectoryDescendants]
            )
        else { return }
        for case let url as URL in enumerator {
            // Each direct child of root is a per-app sandbox.
            let cacheDir = url.appendingPathComponent("cache", isDirectory: true)
            let codeCacheDir = url.appendingPathComponent("code_cache", isDirectory: true)
            for dir in [cacheDir, codeCacheDir] {
                if fileManager.fileExists(atPath: dir.path) {
                    try? fileManager.removeItem(at: dir)
                    try? fileManager.createDirectory(at: dir, withIntermediateDirectories: true)
                }
            }
        }
    }

    // MARK: - Private

    /// Returns true if `id` is a syntactically-valid Android package name.
    private func isValidPackageId(_ id: String) -> Bool {
        guard !id.isEmpty, id.count <= 255 else { return false }
        if id.contains("/") || id.contains("\\") || id.contains("..") { return false }
        let allowed = CharacterSet(charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._")
        return id.unicodeScalars.allSatisfy { allowed.contains($0) }
    }

    /// Validates a package id. Throws on invalid input.
    private func validate(packageId id: String) throws {
        guard isValidPackageId(id) else {
            throw SandboxError.invalidPackageId(id)
        }
    }

    /// Recursively sums file sizes under `url`. Follows symlinks but does not
    /// double-count directories (URLResourceKey.fileSize only applies to
    /// regular files).
    private func directorySize(at url: URL) -> Int64 {
        guard
            let enumerator = fileManager.enumerator(
                at: url,
                includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey],
                options: [.skipsHiddenFiles]
            )
        else { return 0 }

        var total: Int64 = 0
        for case let fileURL as URL in enumerator {
            let resources = try? fileURL.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
            if resources?.isRegularFile == true {
                total += Int64(resources?.fileSize ?? 0)
            }
        }
        return total
    }
}
