//
//  InstallApkView.swift
//  ApkContainer
//
//  Implemented: real SwiftUI sheet wrapping .fileImporter for .apk files,
//  async/await call into AppCatalog.install(apkURL:), with three states
//  (idle / installing / done), error alert, and cancel. Security-scoped
//  resource access is started/stopped around the install call so URLs
//  picked from external providers (Files, iCloud Drive) resolve.
//  Stubbed: nothing in this file.
//  Unsupported: nothing in this file.
//
//  Note on UTType: UTType("com.android.apk") is NOT a system-registered
//  UTI on iOS, so the picker would show no files if we only allowed that
//  type. We attempt to look it up, and fall back to .data (all files) so
//  the picker is usable. For proper .apk-only filtering, export a UTI via
//  UTExportedTypeDeclarations in Info.plist (see docs/BUILD_AND_RUN.md).
//

import SwiftUI
import UniformTypeIdentifiers

struct InstallApkView: View {
    @EnvironmentObject private var catalog: AppCatalog
    @Environment(\.dismiss) private var dismiss

    @State private var presentingPicker = false
    @State private var phase: InstallPhase = .idle
    @State private var installError: String? = nil

    private enum InstallPhase {
        case idle
        case installing
        case done
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 24) {
                switch phase {
                case .idle:       idleContent
                case .installing: installingContent
                case .done:       doneContent
                }
            }
            .padding(24)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .navigationTitle("Install APK")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Cancel") { dismiss() }
                        .disabled(phase == .installing)
                }
            }
            .fileImporter(
                isPresented: $presentingPicker,
                allowedContentTypes: apkContentTypes,
                allowsMultipleSelection: false
            ) { result in
                handlePickerResult(result)
            }
            .alert(
                "Install Failed",
                isPresented: Binding(
                    get: { installError != nil },
                    set: { if !$0 { installError = nil } }
                )
            ) {
                Button("OK", role: .cancel) { }
            } message: {
                Text(installError ?? "")
            }
        }
    }

    // STUB-adjacent: .apk is not a system-known UTI. Falling back to .data
    // shows all files in the picker. Exporting a UTI in Info.plist is
    // required for .apk-only filtering.
    private var apkContentTypes: [UTType] {
        if let apk = UTType("com.android.apk"), apk != .data {
            return [apk]
        }
        return [.data]
    }

    private var idleContent: some View {
        VStack(spacing: 16) {
            Image(systemName: "square.and.arrow.down")
                .font(.system(size: 56))
                .foregroundStyle(.tint)
            Text("Choose an .apk file to install into the container.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button {
                presentingPicker = true
            } label: {
                Label("Choose File", systemImage: "folder")
                    .padding(.horizontal, 20)
                    .padding(.vertical, 10)
            }
            .buttonStyle(.borderedProminent)
        }
    }

    private var installingContent: some View {
        VStack(spacing: 16) {
            ProgressView()
                .scaleEffect(1.4)
            Text("Installing…")
                .font(.headline)
            Text("Parsing manifest, extracting DEX and native libraries.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
    }

    private var doneContent: some View {
        VStack(spacing: 16) {
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 56))
                .foregroundStyle(.green)
            Text("Installed")
                .font(.headline)
            Button("Done") { dismiss() }
                .buttonStyle(.borderedProminent)
        }
    }

    private func handlePickerResult(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            phase = .installing
            Task {
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                do {
                    try await catalog.install(apkURL: url)
                    phase = .done
                } catch {
                    installError = error.localizedDescription
                    phase = .idle
                }
            }
        case .failure(let error):
            installError = error.localizedDescription
        }
    }
}
