//
//  DistributionProbe.swift
//  ApkContainer
//

import Foundation
import Dispatch


/// Best-effort distribution-path detector.
///
/// Detects jailbreak indicators and limited runtime hints.
/// Entitlement inspection is intentionally omitted because iOS does not
/// expose public APIs for reading the current process entitlements.
public final class DistributionProbe {

    public static let shared = DistributionProbe()

    public init() {}


    /// Distribution channel that APKContainer appears to be running under.
    public enum Kind: String, Sendable {
        case trollStore
        case jailbreak
        case unknown
    }


    /// Synchronous detection.
    public static func detect() -> Kind {
        shared.detectSync()
    }


    /// Async detection for SwiftUI `.task`.
    public func detect() async -> Kind {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                continuation.resume(
                    returning: self.detectSync()
                )
            }
        }
    }


    private func detectSync() -> Kind {

        if hasTrollStoreIndicators() {
            return .trollStore
        }

        if isJailbroken() {
            return .jailbreak
        }

        return .unknown
    }


    // MARK: - TrollStore

    /// TrollStore detection without private entitlement APIs.
    ///
    /// This intentionally uses filesystem/runtime indicators only.
    private func hasTrollStoreIndicators() -> Bool {

        let paths = [
            "/var/containers/Bundle/Application",
            "/var/Library/MobileInstallation",
            "/private/var/mobile/Library/Preferences/com.apple.MobileInstallation.plist"
        ]

        let fm = FileManager.default

        return paths.contains {
            fm.fileExists(atPath: $0)
        }
    }


    // MARK: - Jailbreak

    private func isJailbroken() -> Bool {

        let fm = FileManager.default


        let indicators = [
            "/Applications/Cydia.app",
            "/Applications/Sileo.app",
            "/Applications/Zebra.app",
            "/Library/MobileSubstrate/MobileSubstrate.dylib",
            "/usr/libexec/cydia",
            "/usr/bin/ssh",
            "/usr/sbin/sshd",
            "/bin/bash",
            "/bin/su",
            "/private/var/lib/apt",
            "/private/var/stash"
        ]


        for path in indicators {
            if fm.fileExists(atPath: path) {
                return true
            }
        }


        // Sandbox escape test.
        let testPath = "/private/apkcontainer_jailbreak_test"


        do {
            let data = Data("test".utf8)

            try data.write(
                to: URL(fileURLWithPath: testPath),
                options: [.atomic]
            )

            try? fm.removeItem(atPath: testPath)

            return true

        } catch {
            return false
        }
    }
}


/// Spec-style alias.
public typealias DistributionPath = DistributionProbe.Kind
