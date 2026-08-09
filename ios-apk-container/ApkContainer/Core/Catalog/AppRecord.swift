//
//  AppRecord.swift
//  ApkContainer
//
//  Status: IMPLEMENTED.
//
//  Plain Codable record describing one installed APK. This file is data-only;
//  no parsing or runtime behavior lives here. All fields are populated by the
//  installer pipeline (ApkInstaller + ApkParser + extractors) and persisted to
//  catalog.json by AppCatalog.
//
//  Deviation from spec: `versionName` and `versionCode` are Optional here so
//  the UI (Task 3-a) can hide the version line cleanly when an APK ships
//  without them. The spec listed them as non-optional; the deviation is
//  documented here for the orchestrator.
//
//  Honesty contract: every field is documented; nothing here is stubbed.
//

import Foundation

/// A single installed APK record.
///
/// `id` is the Android package name (e.g. `com.innersloth.spacemafia`). It is
/// unique across the catalog and also used as the on-disk sandbox directory
/// name (`<root>/<id>/`). `packageId` is a computed alias for `id` so the UI
/// can refer to it by the more descriptive name.
public struct AppRecord: Codable, Identifiable, Hashable {

    /// Android package name. Acts as the primary key.
    public let id: String

    /// Convenience alias for `id` (the Android package name). Read-only.
    public var packageId: String { id }

    /// Human-readable label resolved at install time. May be `"[unknown label]"`
    /// if neither resources.arsc resolution nor a literal string was available.
    public var name: String

    /// `android:versionName` from the manifest (string, e.g. `"2024.10.15"`).
    /// `nil` if the manifest did not declare `android:versionName`.
    public var versionName: String?

    /// `android:versionCode` from the manifest (integer). `nil` if the manifest
    /// did not declare `android:versionCode`.
    public var versionCode: Int?

    /// `<uses-sdk android:minSdkVersion>`.
    public var minSdk: Int

    /// `<uses-sdk android:targetSdkVersion>`.
    public var targetSdk: Int

    /// Absolute path to the extracted launcher icon PNG inside the sandbox
    /// (`<sandbox>/icon.png`). `nil` if no icon could be extracted.
    public var iconPath: String?

    /// Filenames of native libraries under `lib/arm64-v8a/` (e.g.
    /// `["libunity.so", "libmain.so"]`). The files themselves live at
    /// `<sandbox>/lib/<filename>`.
    public var nativeLibs: [String]

    /// Number of class definitions in the primary `classes.dex`, read from the
    /// DEX header at offset 0x60. `-1` if unknown / unreadable.
    public var dexClassCount: Int

    /// Android permission strings declared via `<uses-permission>` (e.g.
    /// `"android.permission.INTERNET"`).
    public var permissions: [String]

    /// Timestamp the APK was first installed into APKLive.
    public var installDate: Date

    /// Absolute path to the per-app sandbox root (`<root>/<id>`).
    public var sandboxPath: String

    /// Launcher activity class (dotted binary name, e.g. `com.example.MainActivity`).
    /// Captured at install time by walking the manifest for the first <activity>
    /// with a MAIN/LAUNCHER intent-filter (heuristic). Empty string if none.
    public var launcherActivity: String

    /// Absolute path to the extracted primary classes.dex inside the sandbox
    /// (`<sandbox>/dex/classes.dex`). `nil` if the APK had no classes.dex
    /// (very rare; pure-native apps).
    public var classesDexPath: String?

    enum CodingKeys: String, CodingKey {
        case id, name, versionName, versionCode, minSdk, targetSdk
        case iconPath, nativeLibs, dexClassCount, permissions, installDate
        case sandboxPath, launcherActivity, classesDexPath
    }

    public init(
        id: String,
        name: String,
        versionName: String?,
        versionCode: Int?,
        minSdk: Int,
        targetSdk: Int,
        iconPath: String?,
        nativeLibs: [String],
        dexClassCount: Int,
        permissions: [String],
        installDate: Date,
        sandboxPath: String,
        launcherActivity: String = "",
        classesDexPath: String? = nil
    ) {
        self.id = id
        self.name = name
        self.versionName = versionName
        self.versionCode = versionCode
        self.minSdk = minSdk
        self.targetSdk = targetSdk
        self.iconPath = iconPath
        self.nativeLibs = nativeLibs
        self.dexClassCount = dexClassCount
        self.permissions = permissions
        self.installDate = installDate
        self.sandboxPath = sandboxPath
        self.launcherActivity = launcherActivity
        self.classesDexPath = classesDexPath
    }
}
