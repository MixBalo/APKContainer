//
//  ApkContainerApp.swift
//  ApkContainer
//
//  Implemented: real SwiftUI app entry point. Creates the shared AppCatalog
//  as a @StateObject and injects it via .environmentObject so every tab and
//  sheet/fullScreenCover can observe it.
//  Stubbed: nothing in this file.
//  Unsupported: nothing in this file.
//

import SwiftUI

@main
struct ApkContainerApp: App {
    // AppCatalog lives in Core/Catalog and is implemented by Task 3-b.
    // Assumed contract: no-arg init, ObservableObject.
    @StateObject private var catalog = AppCatalog()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(catalog)
        }
    }
}
