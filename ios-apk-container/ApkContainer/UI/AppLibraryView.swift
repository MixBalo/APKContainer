//
//  AppLibraryView.swift
//  ApkContainer
//
//  Implemented: real SwiftUI grid of installed apps (icon + name + package
//  id). Toolbar + button opens InstallApkView sheet. Tap → RunningAppView
//  fullScreenCover. Long-press context menu → AppDetailView sheet. Pull-to-
//  refresh and on-appear both call catalog.load(). Empty state with install
//  CTA. Adaptive LazyVGrid (min 96pt columns), 16pt spacing, cards with
//  .regularMaterial + 16pt corner radius + shadow.
//  Stubbed: nothing in this file itself; runtime launch is delegated to
//  RunningAppView/RuntimeEngine (Core/Runtime).
//  Unsupported: nothing in this file.
//
//  Notes:
//  - AppRecord must conform to Identifiable (uses .fullScreenCover(item:)).
//  - .fileImporter for .apk filtering is handled in InstallApkView.
//

import SwiftUI
import UniformTypeIdentifiers

struct AppLibraryView: View {
    @EnvironmentObject private var catalog: AppCatalog

    @State private var presentingInstaller = false
    @State private var launchingApp: AppRecord? = nil
    @State private var detailApp: AppRecord? = nil

    private let columns = [GridItem(.adaptive(minimum: 96), spacing: 16)]

    var body: some View {
        Group {
            if catalog.installedApps.isEmpty {
                emptyState
            } else {
                grid
            }
        }
        .navigationTitle("Apps")
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    presentingInstaller = true
                } label: {
                    Image(systemName: "plus")
                }
                .accessibilityLabel("Install APK")
            }
        }
        .sheet(isPresented: $presentingInstaller) {
            InstallApkView()
                .environmentObject(catalog)
        }
        .fullScreenCover(item: $launchingApp) { app in
            RunningAppView(packageId: app.packageId, record: app)
        }
        .sheet(item: $detailApp) { app in
            NavigationStack {
                AppDetailView(app: app)
                    .environmentObject(catalog)
            }
        }
        .refreshable {
            await catalog.load()
        }
        .task {
            await catalog.load()
        }
    }

    private var grid: some View {
        ScrollView {
            LazyVGrid(columns: columns, spacing: 16) {
                ForEach(catalog.installedApps) { app in
                    AppCard(app: app)
                        .onTapGesture {
                            launchingApp = app
                        }
                        .contextMenu {
                            Button {
                                detailApp = app
                            } label: {
                                Label("App Details", systemImage: "info.circle")
                            }
                        }
                }
            }
            .padding(16)
        }
    }

    private var emptyState: some View {
        VStack(spacing: 20) {
            Image(systemName: "square.and.arrow.down.on.square")
                .font(.system(size: 64))
                .foregroundStyle(.secondary)
            Text("No APKs Installed")
                .font(.title2.bold())
            Text("Tap + to install an .apk file from Files.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button {
                presentingInstaller = true
            } label: {
                Label("Install APK", systemImage: "plus")
                    .padding(.horizontal, 16)
                    .padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent)
        }
        .padding(40)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct AppCard: View {
    let app: AppRecord

    var body: some View {
        VStack(spacing: 8) {
            AppIconView(path: app.iconPath)
                .frame(width: 60, height: 60)
            VStack(spacing: 2) {
                Text(app.name)
                    .font(.footnote.weight(.semibold))
                    .lineLimit(1)
                Text(app.packageId)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity)
        .background(
            .regularMaterial,
            in: RoundedRectangle(cornerRadius: 16, style: .continuous)
        )
        .shadow(color: .black.opacity(0.08), radius: 4, y: 2)
    }
}

#Preview {
    NavigationStack {
        AppLibraryView()
    }
    .environmentObject(AppCatalog())
}
