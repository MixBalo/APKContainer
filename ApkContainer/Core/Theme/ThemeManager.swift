//
//  ThemeManager.swift
//  ApkContainer
//
//  Manages app-wide dark mode preference and applies it to all views.
//  Uses @AppStorage to persist the user's choice across app launches.
//

import SwiftUI

class ThemeManager: ObservableObject {
    @AppStorage("apkcontainer.darkmode.enabled")
    var isDarkModeEnabled: Bool = false {
        didSet {
            applyTheme()
        }
    }
    
    private func applyTheme() {
        // Set the window scene user interface style
        guard let windowScene = UIApplication.shared.connectedScenes.first as? UIWindowScene else {
            return
        }
        
        for window in windowScene.windows {
            window.overrideUserInterfaceStyle = isDarkModeEnabled ? .dark : .light
        }
    }
    
    /// Initialize theme from stored preference
    func initializeTheme() {
        applyTheme()
    }
}
