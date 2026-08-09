//
//  LogViewer.swift
//  ApkContainer
//
//  Status: IMPLEMENTED. Reads the current run's log file (the path returned
//  by apkcontainer_get_log_path) and renders it as a scrolling, auto-following
//  text view. The file is written by Native/RuntimeGlue/log_file.c.
//
//  The log file is the single source of truth for "what happened during the
//  last run" — every native module logs there with timestamped, tagged, leveled
//  lines. Use it to diagnose launch failures, missing symbols, JNI stubs,
//  shader compile errors, etc.
//

import SwiftUI

/// A scrolling, auto-following view of the current run's log file.
@MainActor
struct LogViewer: View {
    /// Optional explicit path. If nil, uses RuntimeEngine.shared.logPath.
    let path: String?

    @State private var lines: [String] = []
    @State private var autoFollow = true
    @State private var lastSize: UInt64 = 0
    private let pollTimer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var resolvedPath: String {
        path ?? RuntimeEngine.shared.logPath
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text(resolvedPath)
                    .font(.caption2.monospaced())
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Spacer()
                Toggle("Follow", isOn: $autoFollow)
                    .toggleStyle(.switch)
                    .labelsHidden()
                    .help("Auto-scroll to the bottom when new lines arrive")
                Button {
                    reload()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                ShareLink(item: logText) {
                    Image(systemName: "square.and.arrow.up")
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(.bar)

            Divider()

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(Array(lines.enumerated()), id: \.offset) { idx, line in
                            Text(line)
                                .font(.system(.caption2, design: .monospaced))
                                .foregroundStyle(color(for: line))
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(.horizontal, 12)
                                .padding(.vertical, 1)
                                .id(idx)
                        }
                    }
                    .padding(.vertical, 6)
                }
                .background(Color(.systemBackground))
                .onChange(of: lines.count) { _ in
                    if autoFollow, let last = lines.indices.last {
                        withAnimation { proxy.scrollTo(last, anchor: .bottom) }
                    }
                }
            }
        }
        .navigationTitle("Log")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { reload() }
        .onReceive(pollTimer) { _ in reload() }
    }

    private var logText: String {
        lines.joined(separator: "\n")
    }

    private func reload() {
        let url = URL(fileURLWithPath: resolvedPath)
        guard FileManager.default.fileExists(atPath: resolvedPath) else {
            lines = ["(log file does not exist yet — launch an app to populate it)"]
            return
        }
        guard let data = try? Data(contentsOf: url),
              let text = String(data: data, encoding: .utf8) else {
            lines = ["(could not read log file)"]
            return
        }
        // Only update if the file grew.
        let size = UInt64(data.count)
        if size == lastSize && !lines.isEmpty { return }
        lastSize = size
        // Cap to last 5000 lines to avoid OOM in the SwiftUI list.
        let allLines = text.split(separator: "\n", omittingEmptySubsequences: false)
        let capped = allLines.suffix(5000)
        lines = capped.map(String.init)
    }

    /// Color-codes lines by level keyword.
    private func color(for line: String) -> Color {
        if line.contains("[ERROR]") || line.contains(" STUB") || line.contains("FAILED") {
            return .red
        }
        if line.contains("[WARN]") || line.contains("WARN ") {
            return .orange
        }
        if line.contains("[INFO") {
            return .primary
        }
        if line.contains("[DEBUG]") {
            return .secondary
        }
        return .primary
    }
}
