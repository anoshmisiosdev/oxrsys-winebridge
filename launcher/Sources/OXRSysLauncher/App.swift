import AppKit
import SwiftUI
@preconcurrency import TapCore

@main
struct OXRSysLauncherApp: App {
    @StateObject private var status = StatusMonitor()
    @StateObject private var library = LibraryModel()
    @StateObject private var session = LaunchSession()
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate

    init() {
        CLI.handleIfRequested()
    }

    var body: some Scene {
        Window("OXRSys Launcher", id: "main") {
            ContentView()
                .environmentObject(status)
                .environmentObject(library)
                .environmentObject(session)
                .environmentObject(LauncherSettings.shared)
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in
                    session.stopAudioSync()
                }
        }
        .windowToolbarStyle(.unified)
        Settings {
            SettingsView().environmentObject(LauncherSettings.shared)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

/// Debug / scripting entry points. They print JSON and exit before any UI appears.
///   --dump-library          VR games found in the bottle
///   --dump-status           one status snapshot
///   --dump-game-pids <appid> Wine PIDs of a running game (and which have audio)
///   --test-ring <game|global|pid,...> <seconds>  tap + write the shared ring briefly
///   --set-transport <auto|wifi|usb_adb> [toml]   exercise the line-preserving edit
enum CLI {
    static func handleIfRequested() {
        let args = CommandLine.arguments
        func out(_ obj: some Encodable) {
            let enc = JSONEncoder()
            enc.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
            if let d = try? enc.encode(obj) { print(String(decoding: d, as: UTF8.self)) }
        }
        if args.contains("--dump-library") {
            out(LibraryScanner.scan())
            exit(0)
        }
        if args.contains("--dump-status") {
            out(StatusCollector.collect())
            exit(0)
        }
        if let i = args.firstIndex(of: "--dump-game-pids"), i + 1 < args.count {
            let games = LibraryScanner.scan(includeNonVR: true).filter { $0.appID == args[i + 1] }
            for g in games {
                let pids = GameProcesses.pids(for: g)
                print("{\"appID\": \"\(args[i + 1])\", \"matchers\": \(g.processMatchers), \"pids\": \(pids), \"audioPIDs\": \(GameProcesses.audioPIDs(among: pids))}")
            }
            exit(0)
        }
        if let i = args.firstIndex(of: "--set-transport"), i + 1 < args.count {
            let path = i + 2 < args.count ? args[i + 2] : Paths.runtimeToml
            do {
                let changed = try RuntimeConfig.setTransport(args[i + 1], path: path)
                print(changed ? "changed" : "unchanged")
                exit(0)
            } catch {
                print("error: \(error)")
                exit(1)
            }
        }
        if let i = args.firstIndex(of: "--test-ring") {
            let what = i + 1 < args.count ? args[i + 1] : "global"
            let secs = i + 2 < args.count ? Double(args[i + 2]) ?? 3 : 3
            let pids: [NSNumber]? = what == "global" ? nil
                : what.split(separator: ",").compactMap { Int32($0) }.map { NSNumber(value: $0) }
            let audio = OXHeadsetAudio()
            do {
                try audio.start(withPIDs: pids, mute: false, scope: pids == nil ? .system : .game, ringPath: nil)
            } catch {
                print("test-ring: start failed: \(error.localizedDescription)")
                exit(1)
            }
            print("test-ring: rate=\(audio.sampleRate) pids=\(audio.tappedPIDs)")
            for s in 1...max(1, Int(secs)) {
                Thread.sleep(forTimeInterval: 1)
                print("t=\(s)s framesWritten=\(audio.framesWritten) peak=\(audio.takePeak())")
            }
            audio.stop()
            print("test-ring: done, ring=\(OXAudioRingWriter.defaultPath())")
            exit(0)
        }
    }
}
