//
//  DistributionProbe.swift
//  ApkContainer
//

import Foundation
import Security
import Dispatch

/// Best-effort distribution-path detector.
///
/// Detects whether the application appears to have the entitlements
/// normally associated with JIT / unsigned executable memory, or whether
/// common jailbreak indicators are present.
///
/// This is heuristic detection only. The UI should never prevent the user
/// from attempting to launch based solely on this result.
public final class DistributionProbe {

    public static let shared = DistributionProbe()

    public init() {}

    /// Distribution channel that APKLive appears to be running under.
    public enum Kind: String, Sendable {
        case trollStore
        case jailbreak
        case unknown
    }

    /// Synchronous detection.
    public static func detect() -> Kind {
        shared.detectSync()
    }

    /// Async detection for SwiftUI `.task` / async contexts.
    public func detect() async -> Kind {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                let result = self.detectSync()
                continuation.resume(returning: result)
            }
        }
    }

    private func detectSync() -> Kind {
        // 1. Check for the entitlements normally required by the runtime.
        if hasTrollStoreEntitlements() {
            return .trollStore
        }

        // 2. Check for common jailbreak indicators.
        if isJailbroken() {
            return .jailbreak
        }

        // 3. Nothing detected.
        return .unknown
    }

    // MARK: - Entitlements

    private func hasTrollStoreEntitlements() -> Bool {
        let required: Set<String> = [
            "com.apple.security.cs.allow-jit",
            "com.apple.security.cs.allow-unsigned-executable-memory"
        ]

        guard let entitlements = currentEntitlements() else {
            return false
        }

        let present = Set(entitlements.keys)
        return required.isSubset(of: present)
    }

    private func currentEntitlements() -> [String: Any]? {
        var code: SecCode?

        let status = SecCodeCopySelf(
            kSecCSDefaultFlags,
            &code
        )

        guard status == errSecSuccess, let code else {
            return nil
        }

        var info: CFDictionary?

        let infoStatus = SecCodeCopySigningInformation(
            code,
            kSecCSSigningInformation,
            &info
        )

        guard infoStatus == errSecSuccess,
              let info
        else {
            return nil
        }

        let dictionary = info as NSDictionary

        guard let entitlements =
                dictionary[kSecCodeInfoEntitlementsDict as String]
                as? [String: Any]
        else {
            return nil
        }

        return entitlements
    }

    // MARK: - Jailbreak

    private func isJailbroken() -> Bool {
        let fileManager = FileManager.default

        let indicators = [
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
            if fileManager.fileExists(atPath: path) {
                return true
            }
        }

        // Best-effort filesystem escape test.
        let testPath = "/private/apkcontainer_jailbreak_test"

        do {
            let data = Data("test".utf8)
            try data.write(to: URL(fileURLWithPath: testPath))

            try? fileManager.removeItem(atPath: testPath)

            return true
        } catch {
            return false
        }
    }
}

/// Spec-style alias.
public typealias DistributionPath = DistributionProbe.Kind
