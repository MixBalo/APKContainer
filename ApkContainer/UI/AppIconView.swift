//
//  AppIconView.swift
//  ApkContainer
//
//  Implemented: real SwiftUI view that loads a PNG icon from disk via
//  UIImage(contentsOfFile:) and renders it with 16pt rounded corners +
//  shadow. Falls back to a .regularMaterial placeholder with SF Symbol
//  "questionmark.app" when the path is nil or the file cannot be decoded.
//  Stubbed: nothing.
//  Unsupported: adaptive-icon XML (<adaptive-icon>) is NOT parsed here. Only
//  the foreground PNG at `path` is used; legacy .webp/.9.png fallbacks and
//  mipmap density selection are handled at extraction time (Core/Installer)
//  and are out of scope for this view.
//

import SwiftUI
import UIKit

struct AppIconView: View {
    let path: String?

    var body: some View {
        Group {
            if let path, !path.isEmpty, let uiImage = UIImage(contentsOfFile: path) {
                Image(uiImage: uiImage)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
            } else {
                placeholder
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
        .shadow(color: .black.opacity(0.12), radius: 4, y: 2)
    }

    private var placeholder: some View {
        RoundedRectangle(cornerRadius: 16, style: .continuous)
            .fill(.regularMaterial)
            .overlay {
                Image(systemName: "questionmark.app")
                    .font(.title)
                    .foregroundStyle(.secondary)
            }
    }
}

#Preview {
    AppIconView(path: nil)
        .frame(width: 60, height: 60)
        .padding()
}
