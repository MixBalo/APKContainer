//
//  AppDetailView.swift
//  ApkContainer
//
//  Implemented: real SwiftUI detail Form showing icon, name, package id,
//  version, install date, storage used (from SandboxManager), native libs,
//  DEX class count, permissions, target/min SDK. Action buttons: Open
//  (launch), Force Quit (if running), Clear Data, Uninstall (with
//  confirmation alert). All buttons call into AppCatalog / SandboxManager
//  / RuntimeEngine.
//  Stubbed: nothing in this file's UI; only the underlying Core singletons
//  (Task 3-b) decide whether the actions actually do anything end-to-end.
//  Unsupported: nothing in this file.
//

import SwiftUI

struct AppDetailView: View {
    @EnvironmentObject private var catalog: AppCatalog
    @Environment(\.dismiss) private var dismiss

    let app: AppRecord

    @State private var storageUsed: Int64? = nil
    @State private var confirmingUninstall = false
    @State private var confirmingClearData = false
    @State private var actionError: String? = nil
    @State private var isLaunching = false

    var body: some View {
        Form {
            headerSection
            metadataSection
            nativeLibsSection
            dexSection
            permissionsSection
            actionsSection
            diagnosticsSection
        }
        .navigationTitle(app.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button("Done") { dismiss() }
            }
        }
        .task {
            storageUsed = SandboxManager.shared.storageUsed(for: app.packageId)
        }
        .alert("Uninstall \(app.name)?", isPresented: $confirmingUninstall) {
            Button("Uninstall", role: .destructive) {
                catalog.uninstall(id: app.packageId)
                dismiss()
            }
            Button("Cancel", role: .cancel) { }
        } message: {
            Text("This removes the app, its data, and native libraries from the container. The original .apk file on disk is not deleted.")
        }
        .alert("Clear Data?", isPresented: $confirmingClearData) {
            Button("Clear", role: .destructive) {
                SandboxManager.shared.clearData(for: app.packageId)
                storageUsed = SandboxManager.shared.storageUsed(for: app.packageId)
            }
            Button("Cancel", role: .cancel) { }
        }
        .alert(
            "Action Failed",
            isPresented: Binding(
                get: { actionError != nil },
                set: { if !$0 { actionError = nil } }
            )
        ) {
            Button("OK", role: .cancel) { }
        } message: {
            Text(actionError ?? "")
        }
    }

    private var headerSection: some View {
        Section {
            HStack(spacing: 16) {
                AppIconView(path: app.iconPath)
                    .frame(width: 64, height: 64)
                VStack(alignment: .leading, spacing: 4) {
                    Text(app.name).font(.headline)
                    Text(app.packageId).font(.subheadline).foregroundStyle(.secondary)
                    if let v = app.versionName {
                        Text("v\(v) (\(app.versionCode ?? 0))")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .padding(.vertical, 4)
        }
    }

    private var metadataSection: some View {
        Section("Metadata") {
            LabeledContent("Install Date") {
                Text(app.installDate, style: .date)
            }
            LabeledContent("Storage Used") {
                if let bytes = storageUsed {
                    Text(ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file))
                } else {
                    ProgressView()
                }
            }
            LabeledContent("Target SDK", value: "\(app.targetSdk)")
            LabeledContent("Min SDK", value: "\(app.minSdk)")
        }
    }

    private var nativeLibsSection: some View {
        Section("Native Libraries") {
            if app.nativeLibs.isEmpty {
                Text("None").foregroundStyle(.secondary)
            } else {
                ForEach(app.nativeLibs, id: \.self) { lib in
                    Label(lib, systemImage: "shippingbox")
                        .font(.subheadline)
                }
            }
        }
    }

    private var dexSection: some View {
        Section("Dalvik (DEX)") {
            LabeledContent("Class Count", value: "\(app.dexClassCount)")
        }
    }

    private var permissionsSection: some View {
        Section("Permissions Requested") {
            if app.permissions.isEmpty {
                Text("None declared").foregroundStyle(.secondary)
            } else {
                ForEach(app.permissions, id: \.self) { perm in
                    Text(perm)
                        .font(.subheadline.monospaced())
                }
            }
        }
    }

    private var actionsSection: some View {
        Section {
            Button {
                launch()
            } label: {
                if isLaunching {
                    HStack {
                        ProgressView()
                        Text("Launching…")
                    }
                } else {
                    Label("Open", systemImage: "play.fill")
                }
            }
            .disabled(isLaunching)

            if isCurrentlyRunning {
                Button(role: .destructive) {
                    forceQuit()
                } label: {
                    Label("Force Quit", systemImage: "stop.fill")
                }
            }

            Button(role: .destructive) {
                confirmingClearData = true
            } label: {
                Label("Clear Data", systemImage: "eraser")
            }

            Button(role: .destructive) {
                confirmingUninstall = true
            } label: {
                Label("Uninstall", systemImage: "trash")
            }
        }
    }

    private var isCurrentlyRunning: Bool {
        RuntimeEngine.shared.runningApps.contains { $0.packageId == app.packageId }
    }

    private var diagnosticsSection: some View {
        Section("Diagnostics") {
            NavigationLink {
                LogViewer(path: nil)
            } label: {
                Label("View Last Run Log", systemImage: "doc.text.magnifyingglass")
            }
            if app.launcherActivity.isEmpty {
                Text("No launcher activity detected — this APK cannot be launched.")
                    .font(.caption)
                    .foregroundStyle(.orange)
            } else {
                LabeledContent("Launcher Activity", value: app.launcherActivity)
            }
        }
    }

    private func launch() {
        isLaunching = true
        Task {
            do {
                try await RuntimeEngine.shared.launch(packageId: app.packageId)
                dismiss()
            } catch {
                actionError = error.localizedDescription
            }
            isLaunching = false
        }
    }

    private func forceQuit() {
        Task { try? await RuntimeEngine.shared.forceQuit(packageId: app.packageId) }
    }
}
