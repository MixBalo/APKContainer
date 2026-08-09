//
//  AppCatalog.swift
//  ApkContainer
//
//  Status: IMPLEMENTED.
//
//  ObservableObject that owns the in-memory list of installed apps and the
//  on-disk catalog.json. It is the UI-facing entry point for install / uninstall
//  / clearData; the actual APK heavy lifting is delegated to `ApkInstaller` and
//  `SandboxManager`.
//
//  Deviation from spec:
//    - `load()` is `async` (spec didn't specify). The Task 3-a UI awaits it.
//    - `uninstall(id:)` and `clearData(id:)` are non-throwing. The Task 3-a UI
//      calls them from synchronous button actions without `try`. Errors are
//      captured into `lastError` so the UI can surface them if desired.
//
//  Honesty contract:
//    - load(), install(), uninstall(), clearData() are real.
//    - install() delegates to ApkInstaller, which itself delegates to real
//      parsers and extractors (see Installer/).
//

import Foundation
import Combine

/// Errors that can be raised by `AppCatalog` operations.
public enum CatalogError: LocalizedError {
    case notInstalled(String)
    case ioError(underlying: Error)

    public var errorDescription: String? {
        switch self {
        case .notInstalled(let id):
            return "No installed app with package id \(id)."
        case .ioError(let underlying):
            return "Catalog I/O error: \(underlying.localizedDescription)"
        }
    }
}

/// ObservableObject holding the list of installed APKs and persisting it to
/// `CatalogStore.catalogURL`.
@MainActor
public final class AppCatalog: ObservableObject {

    @Published public private(set) var installedApps: [AppRecord] = []

    /// Last error raised by a non-throwing operation (`uninstall`, `clearData`).
    /// The UI may observe this to surface error alerts. Reset to nil by the UI
    /// after display.
    @Published public var lastError: Error?

    private let installer = ApkInstaller()
    private let fileManager = FileManager.default

    public init() {}

    // MARK: - Load

    /// Loads `catalog.json` from disk into `installedApps`. If the file does
    /// not exist (first launch), the catalog is left empty.
    ///
    /// Async to allow the UI to `await catalog.load()`; the body is synchronous
    /// I/O so this returns quickly. May be called from any actor.
    public func load() async {
        let url = CatalogStore.catalogURL
        guard fileManager.fileExists(atPath: url.path) else {
            self.installedApps = []
            return
        }
        do {
            let data = try Data(contentsOf: url)
            // Tolerate an empty file (size 0) as an empty list rather than
            // throwing a decode error.
            if data.isEmpty {
                self.installedApps = []
                return
            }
            let apps = try JSONDecoder.iso8601.decode([AppRecord].self, from: data)
            // Drop any records whose sandbox dir has been removed out-of-band.
            self.installedApps = apps.filter {
                fileManager.fileExists(atPath: $0.sandboxPath)
            }
        } catch {
            // A corrupt catalog must not crash the app; reset to empty and
            // surface the error in logs. The user can reinstall.
            NSLog("[AppCatalog] Failed to load catalog: \(error.localizedDescription)")
            self.lastError = error
            self.installedApps = []
        }
    }

    // MARK: - Install

    /// Installs the APK at `apkURL`, appends the resulting record to the
    /// in-memory list, and persists the catalog.
    @discardableResult
    public func install(apkURL: URL) async throws -> AppRecord {
        let record = try await installer.install(apkURL: apkURL)
        // Replace any existing record with the same package id (reinstall).
        self.installedApps.removeAll { $0.id == record.id }
        self.installedApps.append(record)
        try persist()
        return record
    }

    // MARK: - Uninstall

    /// Removes the sandbox dir and the catalog entry. Does not touch the
    /// original `.apk` file the user picked.
    ///
    /// Non-throwing per UI contract. Errors are captured in `lastError`.
    public func uninstall(id: String) {
        guard let record = installedApps.first(where: { $0.id == id }) else {
            lastError = CatalogError.notInstalled(id)
            return
        }
        do {
            try SandboxManager.shared.removeSandbox(forPackage: record.id)
        } catch {
            lastError = error
            // Continue: still remove the catalog entry so the UI is consistent.
        }
        installedApps.removeAll { $0.id == id }
        do {
            try persist()
        } catch {
            lastError = error
        }
    }

    // MARK: - Clear data

    /// Wipes the per-app data subdirectories (`files`, `cache`, `databases`,
    /// `shared_prefs`, `code_cache`) but keeps `lib` and the install record.
    ///
    /// Non-throwing per UI contract. Errors are captured in `lastError`.
    public func clearData(id: String) {
        guard installedApps.first(where: { $0.id == id }) != nil else {
            lastError = CatalogError.notInstalled(id)
            return
        }
        do {
            try SandboxManager.shared.clearData(for: id)
        } catch {
            lastError = error
        }
    }

    // MARK: - Persistence

    private func persist() throws {
        do {
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            encoder.dateEncodingStrategy = .iso8601
            let data = try encoder.encode(installedApps)
            try data.write(to: CatalogStore.catalogURL, options: [.atomic])
        } catch {
            throw CatalogError.ioError(underlying: error)
        }
    }
}

private extension JSONDecoder {
    /// Shared decoder configured to match the catalog.json date format
    /// (ISO 8601) used by `AppCatalog.persist`.
    static let iso8601: JSONDecoder = {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .iso8601
        return d
    }()
}
