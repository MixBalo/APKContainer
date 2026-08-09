//
//  ContentView.swift
//  ApkContainer
//
//  Implemented: real SwiftUI TabView root with three tabs — Apps
//  (AppLibraryView), Running (RunningAppsView), Settings (SettingsView).
//  Each tab is wrapped in its own NavigationStack so push/dismiss state is
//  isolated per tab. SF Symbols used for tab icons. System colors only, so
//  dark mode is handled by SwiftUI.
//  Stubbed: nothing in this file.
//  Unsupported: nothing in this file.
//

import SwiftUI

struct ContentView: View {
    var body: some View {
        TabView {
            NavigationStack {
                AppLibraryView()
            }
            .tabItem {
                Label("Apps", systemImage: "square.grid.2x2")
            }

            NavigationStack {
                RunningAppsView()
            }
            .tabItem {
                Label("Running", systemImage: "play.circle")
            }

            NavigationStack {
                SettingsView()
            }
            .tabItem {
                Label("Settings", systemImage: "gearshape")
            }
        }
    }
}

#Preview {
    ContentView()
        .environmentObject(AppCatalog())
}
