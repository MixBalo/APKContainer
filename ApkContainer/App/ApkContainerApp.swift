//
//  ApkContainerApp.swift
//  ApkContainer
//
//  Implemented: real SwiftUI app entry point. Creates the shared AppCatalog
//  as a @StateObject and injects it via .environmentObject so every tab and
//  sheet/fullScreenCover can observe it. Also initializes ThemeManager for
//  dark mode support.
//  Stubbed: nothing in this file.
//  Unsupported: nothing in this file.
//

import SwiftUI

@main
struct ApkContainerApp: App {
    // AppCatalog lives in Core/Catalog and is implemented by Task 3-b.
    // Assumed contract: no-arg init, ObservableObject.
    @StateObject private var catalog = AppCatalog()
    
    // Theme manager for dark mode support
    @StateObject private var themeManager = ThemeManager()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(catalog)
                .environmentObject(themeManager)
                .onAppear {
                    themeManager.initializeTheme()
                }
        }
    }
}
