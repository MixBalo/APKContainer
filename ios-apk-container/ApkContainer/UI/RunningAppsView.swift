//
//  RunningAppsView.swift
//  ApkContainer
//
//  Implemented: real SwiftUI List of running/suspended apps sourced from
//  RuntimeEngine.shared.runningApps. Per-row: name, package id, state badge
//  (running = green, suspended = orange), force-quit button. Tap row →
//  RunningAppView fullScreenCover. Refreshes on a 1-second timer while
//  visible. Empty state with explanation.
//  Stubbed: CPU% is always shown as 0 — there is no per-process CPU
//  accounting bridge yet (Native/Runtime). Field is rendered so the layout
//  is honest and the column is ready to be wired later.
//  Unsupported: nothing in this file.
//

import SwiftUI

struct RunningAppsView: View {
    @State private var runningApps: [RunningAppInfo] = []
    @State private var launching: RunningAppInfo? = nil

    private let timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var body: some View {
        Group {
            if runningApps.isEmpty {
                emptyState
            } else {
                list
            }
        }
        .navigationTitle("Running")
        .onReceive(timer) { _ in
            runningApps = RuntimeEngine.shared.runningApps
        }
        .task {
            runningApps = RuntimeEngine.shared.runningApps
        }
        .fullScreenCover(item: $launching) { info in
            RunningAppView(packageId: info.packageId)
        }
    }

    private var list: some View {
        List {
            ForEach(runningApps) { info in
                RunningAppRow(info: info) {
                    Task { try? await RuntimeEngine.shared.forceQuit(packageId: info.packageId) }
                }
                .contentShape(Rectangle())
                .onTapGesture { launching = info }
            }
        }
        .listStyle(.insetGrouped)
    }

    private var emptyState: some View {
        VStack(spacing: 16) {
            Image(systemName: "circle.dashed")
                .font(.system(size: 56))
                .foregroundStyle(.secondary)
            Text("No Apps Running")
                .font(.title3.bold())
            Text("Launch an app from the Apps tab to see it here.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .padding(40)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct RunningAppRow: View {
    let info: RunningAppInfo
    let onForceQuit: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "app.fill")
                .font(.title2)
                .foregroundStyle(.tint)
            VStack(alignment: .leading, spacing: 2) {
                Text(info.displayName).font(.body.weight(.semibold))
                Text(info.packageId).font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            stateBadge
            // STUB: real CPU% requires a native per-process accounting
            // bridge, which is not implemented. Returning 0 keeps the column
            // honest until that bridge exists.
            Text("0%")
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)
                .frame(minWidth: 36, alignment: .trailing)
            Button(role: .destructive, action: onForceQuit) {
                Image(systemName: "xmark.circle.fill")
                    .font(.title3)
            }
            .buttonStyle(.borderless)
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder
    private var stateBadge: some View {
        switch info.state {
        case .launching:
            Label("Launching", systemImage: "arrow.triangle.2.circlepath.circle.fill")
                .font(.caption)
                .foregroundStyle(.blue)
        case .running:
            Label("Running", systemImage: "play.circle.fill")
                .font(.caption)
                .foregroundStyle(.green)
        case .suspended:
            Label("Suspended", systemImage: "pause.circle.fill")
                .font(.caption)
                .foregroundStyle(.orange)
        }
    }
}
