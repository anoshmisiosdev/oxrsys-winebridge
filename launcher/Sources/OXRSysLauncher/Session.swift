import Foundation
@preconcurrency import TapCore

// MARK: - OpenComposite

enum OpenComposite {
    struct Target { var dir: String; var installed: Bool }

    /// 64-bit openvr_api.dll locations and whether each already carries the shim
    /// (a .stock backup exists and differs from the live DLL).
    static func targets(for game: Game) -> [Target] {
        guard let dir = game.installDir else { return [] }
        var dirs: [String] = []
        for rel in game.vrEvidence where rel.lowercased().hasSuffix("openvr_api.dll")
            || rel.lowercased().hasSuffix("openvr_api.dll.stock") {
            let d = ((dir + "/" + rel) as NSString).deletingLastPathComponent
            let last = (d as NSString).lastPathComponent.lowercased()
            if last == "win32" || last == "x86" { continue }
            if !dirs.contains(d) { dirs.append(d) }
        }
        let fm = FileManager.default
        return dirs.map { d in
            let live = d + "/openvr_api.dll", stock = d + "/openvr_api.dll.stock"
            var installed = false
            if fm.fileExists(atPath: stock), fm.fileExists(atPath: live) {
                installed = !fm.contentsEqual(atPath: live, andPath: stock)
            }
            return Target(dir: d, installed: installed)
        }
    }

    static func isInstalled(for game: Game) -> Bool {
        let t = targets(for: game)
        return !t.isEmpty && t.allSatisfy(\.installed)
    }

    static var script: String { Paths.repoRoot + "/scripts/install-opencomposite.sh" }

    /// Runs scripts/install-opencomposite.sh (keeps the .stock backup). Blocking.
    static func install(for game: Game) -> CommandResult {
        guard let dir = game.installDir else { return CommandResult(status: -1, output: "No install dir") }
        guard FileManager.default.fileExists(atPath: script) else {
            return CommandResult(status: -1, output: "Not found: \(script) (set the repo path in Settings)")
        }
        // The script needs a PATH with Homebrew for the optional mingw strip.
        return Shell.run("/bin/bash", [script, "--bottle", Paths.bottleName, dir],
                         env: ["PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"],
                         timeout: 120)
    }
}

// MARK: - Game process discovery

enum GameProcesses {
    /// PIDs whose argv0 (Wine rewrites it to the Windows exe path) matches the game.
    static func pids(for game: Game) -> [pid_t] {
        let matchers = game.processMatchers
        guard !matchers.isEmpty else { return [] }
        var out: [pid_t] = []
        for entry in OXListProcesses() {
            guard entry.count == 2, let pid = entry[0] as? NSNumber, let argv0 = entry[1] as? String else { continue }
            let lower = argv0.lowercased()
            guard lower.hasSuffix(".exe") else { continue }
            if lower.contains("unitycrashhandler") || lower.contains("crashreport") { continue }
            if matchers.contains(where: { lower.contains($0) }) { out.append(pid.int32Value) }
        }
        return out
    }

    /// The subset that currently has a Core Audio process object.
    static func audioPIDs(among pids: [pid_t]) -> [pid_t] {
        let known = Set(OXTapCapture.audioProcesses().map(\.pid))
        return pids.filter { known.contains($0) }
    }
}

// MARK: - Launch session

@MainActor
final class LaunchSession: ObservableObject {
    enum StepState: Equatable { case pending, running, done, skipped, failed(String) }
    struct Step: Identifiable, Equatable {
        var id: String
        var title: String
        var state: StepState = .pending
        var note: String?
    }
    enum AudioState: Equatable {
        case off, waitingForGame, capturing(pids: [Int32], rate: Double, scope: String), failed(String)
    }

    @Published var game: Game?
    @Published var steps: [Step] = []
    @Published var audio: AudioState = .off
    @Published var level: Float = 0
    @Published var gameRunning = false
    @Published var active = false
    @Published var pendingOpenComposite: Game?   // drives the confirmation sheet
    @Published var openCompositeLog: String?

    private let headsetAudio = OXHeadsetAudio()
    private var monitor: Task<Void, Never>?
    private var meter: Timer?

    // MARK: launch

    func launch(_ game: Game, status: StatusSnapshot) {
        if game.api == .openVR, !OpenComposite.isInstalled(for: game), !OpenComposite.targets(for: game).isEmpty {
            pendingOpenComposite = game
            return
        }
        begin(game, status: status)
    }

    func confirmOpenComposite(_ game: Game, status: StatusSnapshot) {
        pendingOpenComposite = nil
        openCompositeLog = "Installing OpenComposite…"
        Task.detached {
            let r = OpenComposite.install(for: game)
            await MainActor.run {
                self.openCompositeLog = r.output
                if r.status == 0 {
                    self.begin(game, status: status, openCompositeNote: "Installed (stock DLL kept as .stock)")
                } else {
                    self.game = game
                    self.steps = [Step(id: "oc", title: "Install OpenComposite",
                                       state: .failed("install-opencomposite.sh exited \(r.status)"))]
                    self.active = false
                }
            }
        }
    }

    private func set(_ id: String, _ state: StepState, _ note: String? = nil) {
        if let i = steps.firstIndex(where: { $0.id == id }) {
            steps[i].state = state
            if let note { steps[i].note = note }
        }
    }

    private func begin(_ game: Game, status: StatusSnapshot, openCompositeNote: String? = nil) {
        stopAudio()
        self.game = game
        active = true
        gameRunning = false
        steps = [
            Step(id: "transport", title: "Match stream transport to headset"),
            Step(id: "oc", title: "OpenComposite (OpenVR titles)"),
            Step(id: "steam", title: "CrossOver + Steam with XR_RUNTIME_JSON"),
            Step(id: "launch", title: "Launch game"),
            Step(id: "audio", title: "Headset audio capture"),
        ]

        // 1. Transport.
        if LauncherSettings.shared.autoTransport {
            let wanted = status.wantedTransport
            do {
                let changed = try RuntimeConfig.setTransport(wanted)
                set("transport", .done, changed ? "Set transport = \(wanted) (backup saved)" : "Already \(wanted)")
            } catch {
                set("transport", .failed("\(error.localizedDescription)"))
            }
        } else {
            set("transport", .skipped, "Automatic transport is off in Settings")
        }

        // 2. OpenComposite.
        if game.api == .openVR {
            if OpenComposite.targets(for: game).isEmpty {
                set("oc", .skipped, "No openvr_api.dll found")
            } else {
                set("oc", OpenComposite.isInstalled(for: game) ? .done : .failed("Not installed"),
                    openCompositeNote ?? "Installed")
            }
        } else {
            set("oc", .skipped, "Native \(game.api.rawValue)")
        }

        // 3. CrossOver / Steam.
        if status.steamRunning && !status.xrEnvOK {
            set("steam", .failed("Steam is running without XR_RUNTIME_JSON"),
                "Quit Steam from its menu, then click Start CrossOver.")
            active = false
            return
        }
        if !status.steamRunning && !status.crossOverRunning {
            startCrossOver()
            set("steam", .done, "Started CrossOver with XR_RUNTIME_JSON; Steam starts with the game")
        } else {
            set("steam", .done, status.steamRunning ? "Steam running" : "CrossOver running; Steam starts with the game")
        }

        // 4. Launch.
        set("launch", .running)
        do {
            try launchProcess(game)
            set("launch", .done, game.appID.map { "steam://rungameid/\($0)" } ?? "Started exe")
        } catch {
            set("launch", .failed(error.localizedDescription))
            active = false
            return
        }

        // 5. Audio + game-exit monitoring.
        startMonitoring(game)
    }

    func startCrossOver() {
        try? Shell.spawn("/usr/bin/open", ["-g", "--env", "XR_RUNTIME_JSON=\(Paths.runtimeManifest)",
                                           "-a", Paths.crossOverApp])
    }

    private func launchProcess(_ game: Game) throws {
        let env = ["XR_RUNTIME_JSON": Paths.runtimeManifest]
        if let appID = game.appID {
            try Shell.spawn(Paths.wine, ["--bottle", Paths.bottleName,
                                         "C:\\Program Files (x86)\\Steam\\steam.exe",
                                         "steam://rungameid/\(appID)"], env: env)
        } else if let exe = game.exePath {
            let win = Paths.windowsPath(exe) ?? exe
            try Shell.spawn(Paths.wine, ["--bottle", Paths.bottleName, win], env: env)
        } else {
            throw NSError(domain: "Launcher", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Nothing to launch"])
        }
    }

    // MARK: audio + monitoring

    private func startMonitoring(_ game: Game) {
        monitor?.cancel()
        let scope = LauncherSettings.shared.audioScope
        let mute = LauncherSettings.shared.muteMac
        if scope == .off {
            set("audio", .skipped, "Off in Settings")
        } else {
            set("audio", .running, scope == .game ? "Waiting for the game's audio…" : "Capturing all Mac audio")
            audio = .waitingForGame
        }
        startMeter()
        monitor = Task { [weak self] in
            var seenGame = false
            var missingSince: Date?
            let startedAt = Date()
            var tapped: Set<pid_t> = []
            while !Task.isCancelled {
                let (pids, audioPIDs) = await Task.detached { () -> ([pid_t], [pid_t]) in
                    let p = GameProcesses.pids(for: game)
                    return (p, GameProcesses.audioPIDs(among: p))
                }.value
                guard let self else { return }
                if !pids.isEmpty {
                    seenGame = true
                    missingSince = nil
                    self.gameRunning = true
                } else if seenGame {
                    missingSince = missingSince ?? Date()
                    if Date().timeIntervalSince(missingSince!) > 5 {
                        self.gameEnded()
                        return
                    }
                } else if Date().timeIntervalSince(startedAt) > 180 {
                    self.set("launch", .failed("Game process not seen after 3 minutes"))
                    self.gameEnded()
                    return
                }

                switch scope {
                case .off:
                    break
                case .system:
                    if case .waitingForGame = self.audio { self.startTap(pids: nil, mute: mute, scope: .system) }
                case .game:
                    let want = Set(audioPIDs)
                    if !want.isEmpty, want != tapped, !self.tapStarting {
                        tapped = want
                        self.startTap(pids: Array(want), mute: mute, scope: .game)
                    } else if want.isEmpty, seenGame, case .waitingForGame = self.audio,
                              Date().timeIntervalSince(startedAt) > 120 {
                        self.set("audio", .failed("The game never opened an audio device"))
                        self.audio = .failed("No audio device from the game")
                    }
                }
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
    }

    /// Tap start can block for a long time: the first capture waits in
    /// AudioDeviceStart until the user answers the System Audio Recording prompt.
    /// So start/stop run on a private serial queue, never on the main thread.
    private let audioQueue = DispatchQueue(label: "io.oxrsys.launcher.audio", qos: .userInitiated)
    private var tapStarting = false

    private func startTap(pids: [pid_t]?, mute: Bool, scope: OXHeadsetAudioScope) {
        guard !tapStarting else { return }
        tapStarting = true
        if pids == nil || audio == .waitingForGame {
            set("audio", .running, "Starting capture (first time: allow System Audio Recording)…")
        }
        let engine = headsetAudio
        audioQueue.async {
            engine.stop()
            var err: Error?
            do {
                try engine.start(withPIDs: pids?.map { NSNumber(value: $0) }, mute: mute, scope: scope,
                                 ringPath: nil)
            } catch { err = error }
            let tapped = engine.tappedPIDs.map(\.int32Value)
            let rate = engine.sampleRate
            DispatchQueue.main.async {
                self.tapStarting = false
                guard self.monitor != nil else { self.audioQueue.async { engine.stop() }; return }
                if let err {
                    self.audio = .failed(err.localizedDescription)
                    self.set("audio", .failed(err.localizedDescription))
                    return
                }
                self.audio = .capturing(pids: tapped, rate: rate, scope: scope == .game ? "game" : "system")
                self.set("audio", .done, scope == .game
                    ? "Tapping game PID \(tapped.map(String.init).joined(separator: ", "))\(mute ? ", Mac muted" : "")"
                    : "Tapping all Mac audio\(mute ? ", Mac muted" : "")")
            }
        }
    }

    private func startMeter() {
        meter?.invalidate()
        meter = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                let peak = self.headsetAudio.takePeak()
                self.level = max(peak, self.level * 0.8)
            }
        }
    }

    private func gameEnded() {
        gameRunning = false
        stopAudio()
        active = false
        if let i = steps.firstIndex(where: { $0.id == "audio" }), steps[i].state == .done {
            steps[i].note = "Stopped (game exited)"
        }
    }

    /// Stops capture and monitoring. Never touches the game, Wine or Steam.
    func stopAudio() {
        monitor?.cancel()
        monitor = nil
        meter?.invalidate()
        meter = nil
        let engine = headsetAudio
        audioQueue.async { engine.stop() }
        level = 0
        if audio != .off { audio = .off }
    }

    /// Synchronous stop for app termination, so the ring is marked inactive before exit.
    func stopAudioSync() {
        monitor?.cancel()
        monitor = nil
        let engine = headsetAudio
        audioQueue.sync { engine.stop() }
    }

    func stopSession() {
        stopAudio()
        active = false
    }
}
