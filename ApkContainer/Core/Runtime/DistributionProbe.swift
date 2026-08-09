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
                let result = self.detectSync()
                continuation.resume(returning: result)
            }
        }
    }

    private func detectSync() -> Kind {
        // 1. Check for entitlements normally associated with the runtime.
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

    /// Reads the current process's task entitlements.
    ///
    /// `SecCode`, `SecCodeCopySelf`, and related APIs are macOS code-signing
    /// APIs and are not available to an iOS target. `SecTask` is the
    /// appropriate Security.framework API for querying the current task.
    private func currentEntitlements() -> [String: Any]? {
        guard let task = SecTaskCreateFromSelf(nil) else {
            return nil
        }

        let keys = [
            "com.apple.security.cs.allow-jit",
            "com.apple.security.cs.allow-unsigned-executable-memory"
        ]

        var result: [String: Any] = [:]

        for key in keys {
            if let value = SecTaskCopyValue(
                task,
                key as CFString
            ) {
                result[key] = value
            }
        }

        return result.isEmpty ? nil : result
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
        //
        // On a normal sandboxed iOS application this should fail.
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
