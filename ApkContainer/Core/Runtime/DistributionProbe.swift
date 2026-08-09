//
//  DistributionProbe.swift
//  ApkContainer
//

import Foundation
import Security
import Dispatch

/// Best-effort distribution-path detector.
///
/// Detects whether the application appears to have entitlements
/// normally associated with JIT / unsigned executable memory, or whether
/// common jailbreak indicators are present.
///
/// This is heuristic detection only. The UI should never prevent the user
/// from attempting to launch based solely on this result.
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

    /// Async detection for SwiftUI `.task` / async contexts.
    public func detect() async -> Kind {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                continuation.resume(returning: self.detectSync())
            }
        }
    }

    private func detectSync() -> Kind {

        if hasTrollStoreEntitlements() {
            return .trollStore
        }

        if isJailbroken() {
            return .jailbreak
        }

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


    /// Reads the current application's signing entitlements.
    ///
    /// Uses public Security.framework APIs supported on iOS.
    private func currentEntitlements() -> [String: Any]? {

        var staticCode: SecCode?

        let status = SecCodeCopySelf(
            SecCSFlags(),
            &staticCode
        )

        guard status == errSecSuccess,
              let code = staticCode else {
            return nil
        }


        var information: CFDictionary?

        let infoStatus = SecCodeCopySigningInformation(
            code,
            SecCSFlags(rawValue: kSecCSSigningInformation),
            &information
        )

        guard infoStatus == errSecSuccess,
              let dictionary = information as? [String: Any] else {
            return nil
        }


        return dictionary["entitlements"] as? [String: Any]
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


        // Sandbox escape test.
        let testPath = "/private/apkcontainer_jailbreak_test"


        do {
            let data = Data("test".utf8)

            try data.write(
                to: URL(fileURLWithPath: testPath),
                options: [.atomic]
            )

            try? fileManager.removeItem(atPath: testPath)

            return true

        } catch {
            return false
        }
    }
}


/// Spec-style alias.
public typealias DistributionPath = DistributionProbe.Kind
