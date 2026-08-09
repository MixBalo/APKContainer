//
//  DistributionProbe.swift
//  ApkContainer
//
//  iOS-safe best-effort distribution detector.
//
//  Important:
//  Apple's SecCode / Code Signing Services APIs are macOS APIs and
//  therefore are intentionally not used here. The iOS target cannot
//  compile SecCodeCopySelf / SecCodeCopySigningInformation.
//
//  Detection is therefore heuristic:
//    - jailbreak indicators are checked where possible;
//    - TrollStore cannot be reliably identified from a normal iOS API;
//    - unknown is returned when the environment cannot be positively
//      identified.
//
//  The UI should treat this as advisory only and should not prevent the
//  user from attempting to launch an APK.
//

import Foundation

/// Best-effort distribution-path detector.
public final class DistributionProbe {

```
public static let shared = DistributionProbe()

public init() {}

/// Distribution channel that APKLive is currently running under.
public enum Kind: String, Sendable {
    case trollStore
    case jailbreak
    case unknown
}

/// Synchronous detection.
///
/// Kept for callers that need a result immediately.
public static func detect() -> Kind {
    return Self.shared.detectSync()
}

/// Async detection.
///
/// The actual checks are quick, but this API is kept async so existing
/// SwiftUI `.task` callers can continue to use:
///
///     await DistributionProbe.shared.detect()
///
public func detect() async -> Kind {
    return await withCheckedContinuation { continuation in
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Self.shared.detectSync()
            continuation.resume(returning: result)
        }
    }
}

// MARK: - Detection

private func detectSync() -> Kind {
    // iOS does not expose the macOS SecCode APIs that were previously
    // used to inspect entitlements.
    //
    // Therefore we cannot reliably distinguish TrollStore from another
    // installation solely by inspecting the current code signature.
    //
    // A positive jailbreak signal is still useful.
    if isJailbroken() {
        return .jailbreak
    }

    return .unknown
}

// MARK: - Jailbreak

/// Best-effort jailbreak detection.
///
/// Modern rootless jailbreaks can hide some traditional paths, so a
/// negative result does not prove that the device is not jailbroken.
private func isJailbroken() -> Bool {
    let fm = FileManager.default

    let indicators: [String] = [
        "/bin/bash",
        "/usr/sbin/sshd",
        "/Applications/Cydia.app",
        "/Library/MobileSubstrate/MobileSubstrate.dylib",
        "/bin/su",
        "/usr/bin/ssh",
        "/private/var/lib/apt",
        "/usr/sbin/inject"
    ]

    for path in indicators {
        if fm.fileExists(atPath: path) {
            return true
        }
    }

    // A normal sandboxed iOS application should not be able to create
    // arbitrary files outside its sandbox.
    //
    // This is only a heuristic. Failure is expected on normal iOS.
    let jailbreakTestPath = "/private/apklive_jailbreak_test"

    do {
        let data = Data("test".utf8)
        try data.write(
            to: URL(fileURLWithPath: jailbreakTestPath),
            options: .atomic
        )

        try? fm.removeItem(atPath: jailbreakTestPath)
        return true
    } catch {
        return false
    }
}
```

}

/// Spec-style alias.
///
/// Existing code can continue to use:
///
///     DistributionPath
///
/// without changing its type.
public typealias DistributionPath = DistributionProbe.Kind
