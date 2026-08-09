//
//  PathLayout.swift
//  ApkContainer
//
//  Status: IMPLEMENTED.
//
//  Plain value type capturing the per-app sandbox directory layout. Mirrors
//  the Android `/data/data/<pkg>/` layout so that an APK's own file APIs
//  (which expect that structure) can be served by simple path rewriting in
//  the native Bionic shim (Task 4).
//
//  Honesty contract: data-only; no stubs.
//

import Foundation

/// Per-app sandbox directory layout.
public struct SandboxPaths {
    /// `<root>/<packageId>/` — the sandbox root.
    public let root: URL
    /// `<root>/files/` — app private files (`Context.getFilesDir()`).
    public let files: URL
    /// `<root>/cache/` — app cache (`Context.getCacheDir()`).
    public let cache: URL
    /// `<root>/code_cache/` — code cache (`Context.getCodeCacheDir()`).
    public let codeCache: URL
    /// `<root>/lib/` — extracted `.so` files (`Context.getApplicationInfo().nativeLibraryDir`).
    public let lib: URL
    /// `<root>/databases/` — SQLite databases.
    public let databases: URL
    /// `<root>/shared_prefs/` — `SharedPreferences` XML files.
    public let sharedPrefs: URL

    public init(
        root: URL,
        files: URL,
        cache: URL,
        codeCache: URL,
        lib: URL,
        databases: URL,
        sharedPrefs: URL
    ) {
        self.root = root
        self.files = files
        self.cache = cache
        self.codeCache = codeCache
        self.lib = lib
        self.databases = databases
        self.sharedPrefs = sharedPrefs
    }
}
