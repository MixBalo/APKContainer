//
//  ApkParser.swift
//  ApkContainer
//
//  Status: IMPLEMENTED (ZIP + binary AndroidManifest.xml are real;
//  resources.arsc is delegated to ResourcesArscReader which is PARTIAL).
//
//  Top-level entry point for "tell me what this APK is". Opens the APK as a
//  ZIP via `ZipReader`, locates `AndroidManifest.xml`, parses the binary AXML
//  via `BinaryManifestParser`, and reads the icon/label from `resources.arsc`
//  via `ResourcesArscReader` (which is currently only able to read the global
//  string pool — see its header).
//
//  Honesty contract:
//    - ZIP parsing: real.
//    - AXML parsing: real (delegated to BinaryManifestParser).
//    - resources.arsc: PARTIAL — label/icon reference resolution is stubbed in
//      ResourcesArscReader; we fall back to "[unknown label]" with a comment
//      when no literal value is available.
//

import Foundation

/// Parsed-but-not-yet-installed APK metadata.
///
/// This is the installer's view of the APK; the persistent `AppRecord` is
/// produced later by `ApkInstaller` after sandbox setup.
public struct ApkManifest {
    /// `package="..."` from `<manifest>`.
    public let packageName: String
    /// `android:versionName` from `<manifest>`.
    public let versionName: String
    /// `android:versionCode` from `<manifest>`.
    public let versionCode: Int
    /// `<uses-sdk android:minSdkVersion>` (defaults to 1 if absent).
    public let minSdk: Int
    /// `<uses-sdk android:targetSdkVersion>` (defaults to minSdk if absent).
    public let targetSdk: Int
    /// `<uses-permission android:name="...">` entries.
    public let permissions: [String]
    /// String-form of `android:label`. May be a literal string or `@string/...`
    /// reference; the latter needs resources.arsc resolution. Empty if unset.
    public let labelRes: String
    /// String-form of `android:icon` (e.g. `@mipmap/ic_launcher` or
    /// `res/mipmap-xxhdpi-v4/ic_launcher.png`). Empty if unset.
    public let iconRes: String
    /// `<application android:name="...">` (entry-point Application subclass).
    public let applicationName: String
    /// Launcher Activity class (dotted binary name, e.g. `com.example.MainActivity`).
    /// Heuristic: the first <activity> with a MAIN/LAUNCHER intent-filter, or
    /// failing that, the first <activity android:name="...">. Empty if none.
    public let launcherActivity: String

    public init(
        packageName: String,
        versionName: String,
        versionCode: Int,
        minSdk: Int,
        targetSdk: Int,
        permissions: [String],
        labelRes: String,
        iconRes: String,
        applicationName: String,
        launcherActivity: String = ""
    ) {
        self.packageName = packageName
        self.versionName = versionName
        self.versionCode = versionCode
        self.minSdk = minSdk
        self.targetSdk = targetSdk
        self.permissions = permissions
        self.labelRes = labelRes
        self.iconRes = iconRes
        self.applicationName = applicationName
        self.launcherActivity = launcherActivity
    }
}

/// Errors raised by APK parsing.
public enum ApkParseError: LocalizedError {
    case zip(ZipError)
    case missingManifest
    case malformedManifest(String)
    case missingPackageName
    case ioError(underlying: Error)

    public var errorDescription: String? {
        switch self {
        case .zip(let z):                  return "ZIP error: \(z.localizedDescription)"
        case .missingManifest:             return "APK has no AndroidManifest.xml entry."
        case .malformedManifest(let why):  return "Malformed AndroidManifest.xml: \(why)"
        case .missingPackageName:          return "Manifest has no package attribute."
        case .ioError(let e):              return "I/O error: \(e.localizedDescription)"
        }
    }
}

/// Top-level APK parser. Stateless; safe to call from any thread.
public enum ApkParser {

    public static func parse(apkURL: URL) throws -> ApkManifest {
        let zip: ZipReader
        do {
            zip = try ZipReader(apkURL: apkURL)
        } catch let z as ZipError {
            throw ApkParseError.zip(z)
        } catch {
            throw ApkParseError.ioError(underlying: error)
        }

        guard let manifestEntry = zip.entry(named: "AndroidManifest.xml") else {
            throw ApkParseError.missingManifest
        }
        let manifestData: Data
        do {
            manifestData = try zip.readData(for: manifestEntry)
        } catch let z as ZipError {
            throw ApkParseError.zip(z)
        } catch {
            throw ApkParseError.ioError(underlying: error)
        }

        // Parse the binary AXML tree.
        let tree: [String: Any]
        do {
            tree = try BinaryManifestParser.parse(manifestData)
        } catch let e as BinaryManifestError {
            throw ApkParseError.malformedManifest(e.localizedDescription)
        }

        // Pull out the bits we care about.
        guard let manifestNode = tree["manifest"] as? [String: Any] else {
            throw ApkParseError.malformedManifest("no <manifest> root element")
        }
        let attrs = (manifestNode["_attrs"] as? [String: String]) ?? [:]
        guard let packageName = attrs["package"], !packageName.isEmpty else {
            throw ApkParseError.missingPackageName
        }

        // versionName may be a literal or @string/... — we keep the raw form
        // here; ApkInstaller resolves @string refs via ResourcesArscReader.
        let versionName = attrs["versionName"] ?? ""
        let versionCode: Int
        if let vc = attrs["versionCode"], let n = Int(vc) {
            versionCode = n
        } else {
            versionCode = 0
        }

        // <uses-sdk>
        var minSdk = 1
        var targetSdk: Int
        if let usesSdkNode = manifestNode["uses-sdk"] as? [String: Any],
           let sdkAttrs = usesSdkNode["_attrs"] as? [String: String] {
            if let m = sdkAttrs["minSdkVersion"], let n = Int(m) { minSdk = n }
            targetSdk = (sdkAttrs["targetSdkVersion"].flatMap { Int($0) }) ?? minSdk
        } else {
            targetSdk = minSdk
        }

        // <uses-permission android:name="...">
        var permissions: [String] = []
        // AXML tree stores repeated children as an array keyed by tag name.
        let permChildren = manifestNode["uses-permission"]
        if let perm = permChildren as? [String: Any] {
            if let a = perm["_attrs"] as? [String: String], let name = a["name"] {
                permissions.append(name)
            }
        } else if let permArray = permChildren as? [[String: Any]] {
            for node in permArray {
                if let a = node["_attrs"] as? [String: String], let name = a["name"] {
                    permissions.append(name)
                }
            }
        }

        // <application android:label android:icon android:name>
        var labelRes = ""
        var iconRes = ""
        var applicationName = ""
        var launcherActivity = ""
        if let appNode = manifestNode["application"] as? [String: Any],
           let appAttrs = appNode["_attrs"] as? [String: String] {
            labelRes = appAttrs["label"] ?? ""
            iconRes = appAttrs["icon"] ?? ""
            applicationName = appAttrs["name"] ?? ""
            launcherActivity = detectLauncherActivity(in: appNode, packageName: packageName)
        }

        return ApkManifest(
            packageName: packageName,
            versionName: versionName,
            versionCode: versionCode,
            minSdk: minSdk,
            targetSdk: targetSdk,
            permissions: permissions,
            labelRes: labelRes,
            iconRes: iconRes,
            applicationName: applicationName,
            launcherActivity: launcherActivity
        )
    }

    /// Heuristic: find the first <activity> with a MAIN/LAUNCHER intent-filter.
    /// Falls back to the first <activity android:name>. Returns the dotted
    /// binary name. Handles the case where android:name starts with '.' (relative
    /// to the package name).
    private static func detectLauncherActivity(in appNode: [String: Any], packageName: String) -> String {
        // Collect all <activity> children. They may be a single dict or an array.
        var activities: [[String: Any]] = []
        if let one = appNode["activity"] as? [String: Any] {
            activities.append(one)
        } else if let arr = appNode["activity"] as? [[String: Any]] {
            activities.append(contentsOf: arr)
        }

        // First pass: MAIN + LAUNCHER intent-filter.
        for act in activities {
            if hasLauncherIntentFilter(act) {
                if let name = (act["_attrs"] as? [String: String])?["name"] {
                    return resolveActivityName(name, packageName: packageName)
                }
            }
        }
        // Fallback: first <activity android:name>.
        for act in activities {
            if let name = (act["_attrs"] as? [String: String])?["name"], !name.isEmpty {
                return resolveActivityName(name, packageName: packageName)
            }
        }
        return ""
    }

    private static func hasLauncherIntentFilter(_ activity: [String: Any]) -> Bool {
        var hasMain = false
        var hasLauncher = false
        // intent-filter may be single or array.
        var filters: [[String: Any]] = []
        if let one = activity["intent-filter"] as? [String: Any] { filters.append(one) }
        else if let arr = activity["intent-filter"] as? [[String: Any]] { filters.append(contentsOf: arr) }
        for f in filters {
            // <action android:name="android.intent.action.MAIN">
            if let one = f["action"] as? [String: Any],
               let a = one["_attrs"] as? [String: String], a["name"] == "android.intent.action.MAIN" {
                hasMain = true
            } else if let arr = f["action"] as? [[String: Any]] {
                for n in arr {
                    if let a = n["_attrs"] as? [String: String], a["name"] == "android.intent.action.MAIN" {
                        hasMain = true
                    }
                }
            }
            // <category android:name="android.intent.category.LAUNCHER">
            if let one = f["category"] as? [String: Any],
               let a = one["_attrs"] as? [String: String], a["name"] == "android.intent.category.LAUNCHER" {
                hasLauncher = true
            } else if let arr = f["category"] as? [[String: Any]] {
                for n in arr {
                    if let a = n["_attrs"] as? [String: String], a["name"] == "android.intent.category.LAUNCHER" {
                        hasLauncher = true
                    }
                }
            }
        }
        return hasMain && hasLauncher
    }

    /// Resolves a relative activity name (e.g. `.MainActivity`) against the
    /// package name. Absolute names pass through unchanged.
    private static func resolveActivityName(_ name: String, packageName: String) -> String {
        if name.hasPrefix(".") { return packageName + name }
        if name.contains(".") { return name }
        return packageName + "." + name
    }
}
