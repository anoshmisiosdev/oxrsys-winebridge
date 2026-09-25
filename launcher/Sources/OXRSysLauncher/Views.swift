import AppKit
import SwiftUI

struct ContentView: View {
    @EnvironmentObject var status: StatusMonitor
    @EnvironmentObject var library: LibraryModel
    @EnvironmentObject var session: LaunchSession
    @EnvironmentObject var settings: LauncherSettings
    @State private var showAdd = false

    var body: some View {
        VStack(spacing: 0) {
            StatusBar()
                .padding(.horizontal, 20)
                .padding(.vertical, 14)
            WarningsView(warnings: status.snapshot.warnings)
            Divider()
            if session.active || !session.steps.isEmpty {
                SessionPanel()
                Divider()
            }
            GameGrid(showAdd: $showAdd)
        }
        .frame(minWidth: 820, minHeight: 600)
        .toolbar {
            ToolbarItemGroup {
                Toggle(isOn: $library.showHidden) { Label("Show hidden", systemImage: "eye") }
                    .help("Show hidden games")
                Button { library.rescan() } label: { Label("Rescan", systemImage: "arrow.clockwise") }
                    .help("Rescan the Steam library")
                Button { showAdd = true } label: { Label("Add Game", systemImage: "plus") }
                    .help("Add a game by Steam app id or .exe")
            }
        }
        .sheet(isPresented: $showAdd) { AddGameSheet() }
        .sheet(item: $session.pendingOpenComposite) { game in
            OpenCompositeSheet(game: game)
        }
        .onAppear {
            status.start()
            library.rescan()
        }
    }
}

// MARK: - Status

struct StatusChip: View {
    var icon: String
    var title: String
    var detail: String
    var ok: Bool?
    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: icon)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(ok == nil ? Color.secondary : (ok! ? Color.green : Color.orange))
                .frame(width: 20)
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.system(size: 12, weight: .semibold))
                Text(detail).font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 7)
        .background(RoundedRectangle(cornerRadius: 9).fill(Color.primary.opacity(0.05)))
    }
}

struct StatusBar: View {
    @EnvironmentObject var status: StatusMonitor
    @EnvironmentObject var session: LaunchSession

    var body: some View {
        let s = status.snapshot
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                StatusChip(icon: "visionpro", title: "Headset", detail: headsetDetail(s),
                           ok: status.loaded ? s.headsetConnected : nil)
                StatusChip(icon: "shippingbox", title: "Runtime",
                           detail: s.runtimeInstalled ? "Installed · \(s.configuredTransport ?? "?")" : "Not installed",
                           ok: status.loaded ? s.runtimeInstalled : nil)
                StatusChip(icon: "gamecontroller", title: "Steam", detail: steamDetail(s),
                           ok: status.loaded ? (s.steamRunning ? s.xrEnvOK : nil) : nil)
                StatusChip(icon: powerIcon(s.power), title: "Power", detail: powerDetail(s.power),
                           ok: status.loaded ? (s.power.onAC && (s.power.adapterWatts ?? 0) >= 90 && !s.power.drainingOnAC) : nil)
                StatusChip(icon: "speaker.wave.2", title: "Audio", detail: audioDetail(), ok: audioOK())
                Spacer(minLength: 0)
                if !s.steamRunning && !s.crossOverRunning {
                    Button("Start CrossOver") { session.startCrossOver() }
                        .help("Opens CrossOver with XR_RUNTIME_JSON exported, like OXRSys Home's launcher")
                }
            }
            if s.runtime.fresh, s.runtime.state == "streaming" {
                LiveStats(r: s.runtime)
            }
        }
    }

    func headsetDetail(_ s: StatusSnapshot) -> String {
        if let usb = s.usbHeadset { return "USB · \(usb.serial)" }
        if s.runtime.fresh, s.runtime.state == "streaming" || s.runtime.state == "connected" {
            return "\(s.runtime.clientName ?? "Connected") · \(s.runtime.transport ?? "wifi")"
        }
        if !s.adbAvailable { return "adb not found · Wi-Fi: waiting" }
        return "Not connected (USB or Wi-Fi)"
    }

    func steamDetail(_ s: StatusSnapshot) -> String {
        if s.steamRunning { return s.xrEnvOK ? "Running · XR_RUNTIME_JSON set" : "Running · no XR_RUNTIME_JSON" }
        return s.crossOverRunning ? "CrossOver open, Steam not running" : "Not running"
    }

    func powerIcon(_ p: PowerInfo) -> String { p.onAC ? "powerplug" : "battery.50" }
    func powerDetail(_ p: PowerInfo) -> String {
        var parts: [String] = []
        if p.onAC { parts.append(p.adapterWatts.map { "\($0) W" } ?? "AC") } else { parts.append("Battery") }
        if let b = p.batteryPercent { parts.append("\(b)%\(p.charging ? " ⚡︎" : "")") }
        return parts.joined(separator: " · ")
    }

    func audioDetail() -> String {
        switch session.audio {
        case .off: return LauncherSettings.shared.audioScope.label + (LauncherSettings.shared.muteMac ? " · mute Mac" : "")
        case .waitingForGame: return "Waiting for game audio…"
        case let .capturing(pids, rate, scope):
            return scope == "game" ? "Game PID \(pids.map(String.init).joined(separator: ",")) · \(Int(rate / 1000)) kHz"
                                   : "All Mac audio · \(Int(rate / 1000)) kHz"
        case let .failed(e): return e
        }
    }

    func audioOK() -> Bool? {
        switch session.audio {
        case .capturing: return true
        case .failed: return false
        default: return nil
        }
    }
}

struct LiveStats: View {
    var r: RuntimeLive
    var body: some View {
        HStack(spacing: 18) {
            Label("Streaming \(r.applicationName ?? "")", systemImage: "dot.radiowaves.left.and.right")
                .foregroundStyle(.green)
            stat("Refresh", r.refreshHz.map { "\(Int($0)) Hz" })
            stat("Bitrate", r.bitrateMbps.map { "\(Int($0)) Mbps" })
            stat("Encode", r.encodeTotalMs.map { String(format: "%.1f ms", $0) })
            stat("Decode", r.clientDecodeMs.map { String(format: "%.1f ms", $0) })
            stat("Headset audio", r.headsetAudio.map { $0 ? "on" : "off" })
            Spacer()
        }
        .font(.system(size: 11))
    }

    @ViewBuilder func stat(_ k: String, _ v: String?) -> some View {
        if let v {
            HStack(spacing: 4) {
                Text(k).foregroundStyle(.secondary)
                Text(v).monospacedDigit()
            }
        }
    }
}

struct WarningsView: View {
    var warnings: [StatusSnapshot.Warning]
    var body: some View {
        if !warnings.isEmpty {
            VStack(alignment: .leading, spacing: 6) {
                ForEach(warnings) { w in
                    HStack(alignment: .top, spacing: 8) {
                        Image(systemName: w.severe ? "exclamationmark.octagon.fill" : "exclamationmark.triangle.fill")
                            .foregroundStyle(w.severe ? .red : .orange)
                        VStack(alignment: .leading, spacing: 1) {
                            Text(w.title).font(.system(size: 12, weight: .semibold))
                            Text(w.detail).font(.system(size: 11)).foregroundStyle(.secondary)
                        }
                        Spacer()
                    }
                }
            }
            .padding(.horizontal, 20)
            .padding(.bottom, 12)
        }
    }
}

// MARK: - Session

struct SessionPanel: View {
    @EnvironmentObject var session: LaunchSession

    var body: some View {
        HStack(alignment: .top, spacing: 20) {
            VStack(alignment: .leading, spacing: 6) {
                Text(session.game?.name ?? "").font(.headline)
                ForEach(session.steps) { step in
                    HStack(spacing: 8) {
                        icon(step.state).frame(width: 16)
                        Text(step.title).font(.system(size: 12))
                        if let note = step.note ?? failure(step.state) {
                            Text(note).font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(1)
                        }
                    }
                }
            }
            Spacer()
            VStack(alignment: .trailing, spacing: 8) {
                if case .capturing = session.audio {
                    LevelMeter(level: session.level).frame(width: 160, height: 8)
                }
                HStack {
                    if session.active {
                        Button("Stop audio") { session.stopSession() }
                            .help("Stops headset audio capture. The game keeps running.")
                    } else {
                        Button("Dismiss") { session.steps = []; session.game = nil }
                    }
                }
                if session.gameRunning {
                    Text("Game running").font(.system(size: 11)).foregroundStyle(.green)
                }
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 12)
        .background(Color.primary.opacity(0.03))
    }

    func failure(_ s: LaunchSession.StepState) -> String? {
        if case let .failed(msg) = s { return msg }
        return nil
    }

    @ViewBuilder func icon(_ s: LaunchSession.StepState) -> some View {
        switch s {
        case .pending: Image(systemName: "circle").foregroundStyle(.secondary)
        case .running: ProgressView().controlSize(.small)
        case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
        case .skipped: Image(systemName: "minus.circle").foregroundStyle(.secondary)
        case .failed: Image(systemName: "xmark.octagon.fill").foregroundStyle(.red)
        }
    }
}

struct LevelMeter: View {
    var level: Float
    var body: some View {
        GeometryReader { g in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.primary.opacity(0.1))
                Capsule().fill(level > 0.9 ? Color.red : Color.green)
                    .frame(width: g.size.width * CGFloat(min(1, sqrt(Double(level)))))
            }
        }
        .animation(.linear(duration: 0.1), value: level)
    }
}

// MARK: - Library grid

struct GameGrid: View {
    @EnvironmentObject var library: LibraryModel
    @Binding var showAdd: Bool

    var body: some View {
        ScrollView {
            if library.games.isEmpty {
                VStack(spacing: 10) {
                    if library.scanning {
                        ProgressView("Scanning the \(Paths.bottleName) bottle's Steam library…")
                    } else {
                        Text("No VR games found in the \(Paths.bottleName) bottle.")
                            .foregroundStyle(.secondary)
                        Button("Add Game…") { showAdd = true }
                    }
                }
                .frame(maxWidth: .infinity, minHeight: 300)
            } else {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 250, maximum: 340), spacing: 18)], spacing: 18) {
                    ForEach(library.visibleGames) { GameCard(game: $0) }
                }
                .padding(20)
            }
        }
    }
}

struct GameCard: View {
    var game: Game
    @EnvironmentObject var library: LibraryModel
    @EnvironmentObject var session: LaunchSession
    @EnvironmentObject var status: StatusMonitor
    @State private var hovering = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            art
                .frame(height: 130)
                .clipped()
            HStack(alignment: .center) {
                VStack(alignment: .leading, spacing: 3) {
                    Text(game.name).font(.system(size: 13, weight: .semibold)).lineLimit(1)
                    HStack(spacing: 6) {
                        Badge(text: game.api.rawValue)
                        if game.api == .openVR {
                            Badge(text: OpenComposite.isInstalled(for: game) ? "OpenComposite" : "Needs OC",
                                  tint: OpenComposite.isInstalled(for: game) ? .green : .orange)
                        }
                        if library.isHidden(game) { Badge(text: "Hidden", tint: .secondary) }
                    }
                }
                Spacer()
                Button {
                    session.launch(game, status: status.snapshot)
                } label: {
                    Label("Play", systemImage: "play.fill").font(.system(size: 13, weight: .semibold))
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .disabled(session.active)
            }
            .padding(12)
        }
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(nsColor: .controlBackgroundColor)))
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Color.primary.opacity(hovering ? 0.25 : 0.08)))
        .shadow(color: .black.opacity(hovering ? 0.2 : 0.08), radius: hovering ? 10 : 4, y: 2)
        .onHover { hovering = $0 }
        .contextMenu {
            Button(library.isHidden(game) ? "Unhide" : "Hide") { library.toggleHidden(game) }
            if let dir = game.installDir {
                Button("Show in Finder") { NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: dir) }
            }
            if game.manual { Button("Remove", role: .destructive) { library.remove(game) } }
        }
    }

    @ViewBuilder var art: some View {
        if let p = game.artPath, let img = NSImage(contentsOfFile: p) {
            Image(nsImage: img).resizable().aspectRatio(contentMode: .fill)
        } else {
            ZStack {
                LinearGradient(colors: [.indigo, .purple], startPoint: .topLeading, endPoint: .bottomTrailing)
                Text(game.name).font(.system(size: 20, weight: .bold)).foregroundStyle(.white.opacity(0.9))
                    .multilineTextAlignment(.center).padding()
            }
        }
    }
}

struct Badge: View {
    var text: String
    var tint: Color = .accentColor
    var body: some View {
        Text(text)
            .font(.system(size: 10, weight: .medium))
            .lineLimit(1)
            .fixedSize()
            .padding(.horizontal, 6).padding(.vertical, 2)
            .background(Capsule().fill(tint.opacity(0.15)))
            .foregroundStyle(tint)
    }
}

// MARK: - Sheets

struct OpenCompositeSheet: View {
    var game: Game
    @EnvironmentObject var session: LaunchSession
    @EnvironmentObject var status: StatusMonitor

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Install OpenComposite for \(game.name)?").font(.headline)
            Text("\(game.name) uses OpenVR. OpenComposite replaces its openvr_api.dll so it runs on OXRSys without SteamVR. The original DLL is kept as openvr_api.dll.stock and can be restored with scripts/install-opencomposite.sh --restore.")
                .font(.system(size: 12)).fixedSize(horizontal: false, vertical: true)
            ForEach(OpenComposite.targets(for: game), id: \.dir) { t in
                Text(t.dir).font(.system(size: 10, design: .monospaced)).foregroundStyle(.secondary)
                    .lineLimit(1).truncationMode(.head)
            }
            HStack {
                Spacer()
                Button("Cancel") { session.pendingOpenComposite = nil }.keyboardShortcut(.cancelAction)
                Button("Install and Play") { session.confirmOpenComposite(game, status: status.snapshot) }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 520)
    }
}

struct AddGameSheet: View {
    @Environment(\.dismiss) var dismiss
    @EnvironmentObject var library: LibraryModel
    @State private var appID = ""
    @State private var exePath = ""
    @State private var name = ""
    @State private var error: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Add a game").font(.headline)
            Form {
                TextField("Steam app id", text: $appID)
                HStack {
                    TextField("…or .exe inside the bottle", text: $exePath)
                    Button("Choose…") { chooseExe() }
                }
                TextField("Name (optional)", text: $name)
            }
            if let error { Text(error).foregroundStyle(.red).font(.system(size: 11)) }
            HStack {
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Add") { add() }.keyboardShortcut(.defaultAction)
                    .disabled(appID.isEmpty && exePath.isEmpty)
            }
        }
        .padding(20)
        .frame(width: 480)
    }

    func chooseExe() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = []
        panel.allowsOtherFileTypes = true
        panel.directoryURL = URL(fileURLWithPath: Paths.driveC)
        panel.canChooseDirectories = false
        if panel.runModal() == .OK, let url = panel.url { exePath = url.path }
    }

    func add() {
        let id = appID.trimmingCharacters(in: .whitespaces)
        if !id.isEmpty, Int(id) == nil { error = "App id must be a number"; return }
        if !exePath.isEmpty, Paths.windowsPath(exePath) == nil {
            error = "The .exe must be inside the \(Paths.bottleName) bottle's drive_c"; return
        }
        let n = name, e = exePath
        Task.detached {
            let g = LibraryScanner.manualGame(appID: id.isEmpty ? nil : id, exePath: e.isEmpty ? nil : e, name: n)
            await MainActor.run {
                if let g { library.add(g); dismiss() } else { error = "Could not add that game" }
            }
        }
    }
}

// MARK: - Settings

struct SettingsView: View {
    @EnvironmentObject var settings: LauncherSettings

    var body: some View {
        Form {
            Section("Headset audio") {
                Picker("Source", selection: $settings.audioScope) {
                    ForEach(AudioScopeSetting.allCases) { Text($0.label).tag($0) }
                }
                Toggle("Mute Mac while streaming", isOn: $settings.muteMac)
                Text("Audio is captured with a Core Audio process tap and handed to the OXRSys runtime through ~/Library/Application Support/OXRSys/headset-audio.ring. No BlackHole or Multi-Output device is needed.")
                    .font(.system(size: 11)).foregroundStyle(.secondary)
            }
            Section("Streaming") {
                Toggle("Set transport (USB / Wi-Fi) automatically before launch", isOn: $settings.autoTransport)
                Text("Edits only the transport key in oxrsys-runtime.toml, with a backup. The transport is fixed when a game session starts.")
                    .font(.system(size: 11)).foregroundStyle(.secondary)
            }
            Section("Paths") {
                TextField("CrossOver bottle", text: $settings.bottleName)
                TextField("oxrsys-winebridge repo", text: $settings.repoRoot)
                TextField("OXRSys runtime folder", text: $settings.runtimeDir)
            }
        }
        .formStyle(.grouped)
        .frame(width: 520, height: 460)
    }
}
