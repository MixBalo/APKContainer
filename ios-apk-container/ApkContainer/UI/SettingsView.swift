//
//  SettingsView.swift
//  ApkContainer
//
//  Implemented: real SwiftUI Form with four sections — Distribution,
//  Runtime, About, Diagnostics. Distribution path is probed via
//  DistributionProbe (declared in Core/Runtime/DistributionProbe.swift).
//  Re-scan Catalog calls catalog.load(); Clear All Caches calls
//  SandboxManager.shared.clearAllCaches(). Version is read from the main
//  bundle's Info dictionary.
//  Stubbed:
//    - Audio output device picker — no AVAudioEngine route enumeration yet.
//    - Capability Matrix link — bundled doc not yet shipped in the app;
//      pushes a static placeholder view.
//    - GitHub link — placeholder URL.
//  Unsupported:
//    - Interpreter-only ART toggle is forced ON and disabled (architecture
//      decision #2: JIT/AOT are unsupported on iOS without W^X-violating
//      entitlements we do not carry).
//

import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var catalog: AppCatalog

    @State private var distribution: DistributionProbe.Kind = .unknown
    @State private var angleDebugLayers = false
    @State private var audioDevice: String = "Default"
    @State private var rescanProgress = false

    private let audioDevices = ["Default", "Built-In Speaker", "Bluetooth"]

    var body: some View {
        Form {
            distributionSection
            runtimeSection
            aboutSection
            diagnosticsSection
        }
        .navigationTitle("Settings")
        .task {
            distribution = await DistributionProbe.shared.detect()
        }
    }

    private var distributionSection: some View {
        Section {
            HStack {
                Image(systemName: distribution.icon)
                    .foregroundStyle(distribution.color)
                VStack(alignment: .leading) {
                    Text("Install Path")
                    Text(distribution.title)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Text(distribution.badge)
                    .font(.caption.weight(.semibold))
                    .padding(.horizontal, 8)
                    .padding(.vertical, 2)
                    .background(distribution.color.opacity(0.15), in: Capsule())
            }
        } header: {
            Text("Distribution")
        } footer: {
            Text("Only TrollStore (iOS 14–16.6.1) and jailbroken devices can grant the JIT + unsigned-memory entitlements this container needs.")
        }
    }

    private var runtimeSection: some View {
        Section("Runtime") {
            Toggle(isOn: .constant(true)) {
                Label("Interpreter-only ART", systemImage: "tortoise")
            }
            .disabled(true)
            .foregroundStyle(.secondary)

            Toggle("ANGLE Debug Layers", isOn: $angleDebugLayers)

            Picker(selection: $audioDevice) {
                ForEach(audioDevices, id: \.self) { Text($0).tag($0) }
            } label: {
                Label("Audio Output", systemImage: "speaker.wave.2")
            }
            // STUB: AVAudioEngine route enumeration not wired; this picker is
            // a placeholder until the OpenSL→AVAudioEngine bridge reports
            // available output devices.
        }
    }

    private var aboutSection: some View {
        Section("About") {
            LabeledContent("Version", value: appVersionString)

            NavigationLink {
                CapabilityMatrixStubView()
            } label: {
                Label("Capability Matrix", systemImage: "checklist")
            }
            // STUB: docs/CAPABILITY_MATRIX.md is committed at the repo root
            // but not bundled into the app yet; the pushed view shows a
            // placeholder message.

            NavigationLink {
                LogViewer(path: nil)
            } label: {
                Label("View Log", systemImage: "doc.text")
            }

            Link(destination: URL(string: "https://example.invalid/apklive-github")!) {
                Label("GitHub", systemImage: "network")
            }
            // STUB: GitHub URL placeholder.
        }
    }

    private var diagnosticsSection: some View {
        Section("Diagnostics") {
            Button {
                Task {
                    rescanProgress = true
                    await catalog.load()
                    rescanProgress = false
                }
            } label: {
                HStack {
                    Label("Re-scan Catalog", systemImage: "arrow.triangle.2.circlepath")
                    if rescanProgress { ProgressView() }
                }
            }
            .disabled(rescanProgress)

            Button(role: .destructive) {
                SandboxManager.shared.clearAllCaches()
            } label: {
                Label("Clear All Caches", systemImage: "trash")
            }
        }
    }

    private var appVersionString: String {
        let v = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0.0"
        let b = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "0"
        return "\(v) (\(b))"
    }
}

// MARK: - Capability matrix placeholder

private struct CapabilityMatrixStubView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Label("Capability Matrix", systemImage: "checklist")
                    .font(.title2.bold())
                Text("docs/CAPABILITY_MATRIX.md is not bundled in the app yet.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                Text("See the project repository for the authoritative matrix.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                // STUB: replace with rendered Markdown from a bundled copy of
                // docs/CAPABILITY_MATRIX.md once it is added to the app target.
            }
            .padding()
        }
        .navigationTitle("Capability Matrix")
        .navigationBarTitleDisplayMode(.inline)
    }
}

// MARK: - DistributionProbe.Kind presentation

private extension DistributionProbe.Kind {
    var title: String {
        switch self {
        case .trollStore: return "TrollStore (CoreTrust-bug)"
        case .jailbreak:  return "Jailbroken (palera1n / Dopamine)"
        case .unknown:    return "Unsupported / Not detected"
        }
    }
    var badge: String {
        switch self {
        case .trollStore: return "OK"
        case .jailbreak:  return "OK"
        case .unknown:    return "UNSUPPORTED"
        }
    }
    var icon: String {
        switch self {
        case .trollStore: return "checkmark.seal.fill"
        case .jailbreak:  return "checkmark.shield.fill"
        case .unknown:    return "exclamationmark.triangle.fill"
        }
    }
    var color: Color {
        switch self {
        case .trollStore: return .green
        case .jailbreak:  return .green
        case .unknown:    return .orange
        }
    }
}
