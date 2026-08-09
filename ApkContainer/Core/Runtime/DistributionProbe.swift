//
//  DistributionProbe.swift
//  ApkContainer
//
//  Status: IMPLEMENTED (heuristic — not 100% reliable, but good enough to
//  gate the UI's "you need TrollStore / jailbreak" warning).
//
//  Detects whether APKLive is running under:
//    - TrollStore (CoreTrust-bug installs on iOS 14–16.6.1 with the JIT
//      entitlements needed for ART interpreter + unsigned .so loading), or
//    - jailbreak (palera1n / Dopamine etc., which can also grant those
//      entitlements), or
//    - unknown (everything else — App Store / free sideload / paid-dev
//      sideload on a non-jailbroken device, which cannot grant the
//      entitlements ART + ELF loader require).
//
//  Deviation from spec:
//    - The spec described `DistributionPath` as a free enum with four cases
//      (`trollstore`, `jailbreak`, `unsupported`, `unknown`) and
//      `DistributionProbe` as an enum with a `static detect()`. To match the
//      Task 3-a UI, this file ships:
//        * `DistributionProbe` as a `final class` with `static let shared` and
//          both a static `detect()` and an instance `detect() async`.
//        * A nested `Kind` enum with three cases (`trollStore`, `jailbreak`,
//          `unknown`). The spec's separate `unsupported` case is folded into
//          `unknown` because the UI displays both as "Unsupported / Not
//          detected".
//        * `DistributionPath` as a typealias for `DistributionProbe.Kind` so
//          spec-style code (`DistributionPath`) keeps compiling.
//
//  Honesty contract: every detection method is best-effort. False negatives
//  (a real TrollStore install misdetected as `.unknown`) are possible.
//  The UI must always let the user override this and attempt a launch anyway;
//  the launch itself will fail loudly if the entitlements are truly missing.
//

import Foundation
import Security

/// Best-effort distribution-path detector.
public final class DistributionProbe {

    public static let shared = DistributionProbe()

    public init() {}

    /// Distribution channel that APKLive is currently running under.
    public enum Kind: String, Sendable {
        case trollStore
        case jailbreak
        case unknown
    }

    /// Synchronous detection. Use from non-async contexts.
    @available(*, renamed: "detect()")
    public static func detect() -> Kind {
        return shared.detectSync()
    }

    /// Async detection. Use from `await` contexts (e.g. SwiftUI `.task`).
    /// The actual detection is synchronous and fast; this is `async` only to
    /// match the Task 3-a UI's `await DistributionProbe.shared.detect()` call.
    public func detect() async -> Kind {
        // The detection involves `SecCodeCopySigningInformation` which can
        // block briefly; hop off the main actor for safety.
        return await withCheckedContinuation { (continuation: CheckedContinuation<Kind, Never>) in
            DispatchQueue.global(qos: .userInitiated).async {
                let result = shared.detectSync()
                continuation.resume(returning: result)
            }
        }
    }

    private func detectSync() -> Kind {
        // 1) TrollStore: look for the JIT/unsigned-memory entitlements.
        if hasTrollStoreEntitlements() {
            return .trollStore
        }
        // 2) Jailbreak: look for telltale files. False positives are possible
        //    on rootless jailbreaks that hide these paths, but a positive is a
        //    strong signal.
        if isJailbroken() {
            return .jailbreak
        }
        // 3) Default: unknown. The UI should warn the user that APK launch
        //    will likely fail without TrollStore or jailbreak.
        return .unknown
    }

    // MARK: - TrollStore

    /// Returns true if the running app's entitlements include the JIT and
    /// unsigned-executable-memory grants that TrollStore installs provide.
    /// Uses `SecCodeCopySigningInformation` to read the entitlements
    /// dictionary of the current code signature.
    ///
    /// Best-effort: on a non-jailbroken, non-TrollStore install this returns
    /// false (which is correct — those entitlements are unavailable).
    private func hasTrollStoreEntitlements() -> Bool {
        // The two entitlements ART + ELF loader require:
        //   com.apple.security.cs.allow-jit
        //   com.apple.security.cs.allow-unsigned-executable-memory
        // (TrollStore installs both as part of its CoreTrust-bug bypass.)
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

    /// Reads the current process's entitlements dictionary via the Security
    /// framework. Returns nil on any error.
    private func currentEntitlements() -> [String: Any]? {
        var code: SecCode?
        let attrs: [String: Any] = [kSecCSSigningRequired as String: true]
        let status = SecCodeCopySelf(attrs, &code)
        guard status == errSecSuccess, let code = code else {
            return nil
        }
        var info: CFDictionary?
        let infoAttrs: [String: Any] = [
            kSecCSSigningRequired as String: true,
            kSecCSEntitlements as String: true
        ]
        let infoStatus = SecCodeCopySigningInformation(code, infoAttrs, &info)
        guard infoStatus == errSecSuccess, let info = info else {
            return nil
        }
        // The entitlements dictionary is under the kSecCodeInfoEntitlementsDict
        // key in the signing info.
        let infoNS = info as NSDictionary
        guard let ents = infoNS[kSecCodeInfoEntitlementsDict as String] as? [String: Any] else {
            return nil
        }
        return ents
    }

    // MARK: - Jailbreak

    /// Returns true if any of the classic jailbreak-indicator files exist.
    /// Modern rootless jailbreaks may hide these, so a false negative is
    /// possible — but a positive is a strong signal.
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
            if fm.fileExists(atPath: path) { return true }
        }
        // Try to write outside the sandbox (a jailbreak grants this). Best-effort.
        let jailbreakTest = "/private/jailbreak_test"
        do {
            try "test".data(using: .utf8)?.write(to: URL(fileURLWithPath: jailbreakTest))
            try? fm.removeItem(atPath: jailbreakTest)
            return true
        } catch {
            // Normal sandboxed behavior — expected.
        }
        return false
    }
}

/// Spec-style alias. `DistributionPath` is the same type as
/// `DistributionProbe.Kind`.
public typealias DistributionPath = DistributionProbe.Kind
