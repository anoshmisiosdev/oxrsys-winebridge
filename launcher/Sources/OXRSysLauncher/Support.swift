import Foundation

/// Well-known locations and defaults for the OXRSys + CrossOver setup.
enum Paths {
    static let home = FileManager.default.homeDirectoryForCurrentUser.path
    static let oxrsysSupport = home + "/Library/Application Support/OXRSys"
    static let runtimeToml = oxrsysSupport + "/oxrsys-runtime.toml"
    static let runtimeStatus = oxrsysSupport + "/runtime_status.json"
    static let launcherSupport = oxrsysSupport + "/Launcher"
    static let gamesJson = launcherSupport + "/games.json"
    static let crossOverApp = "/Applications/CrossOver.app"
    static let wine = crossOverApp + "/Contents/SharedSupport/CrossOver/bin/wine"
    static let bottlesDir = home + "/Library/Application Support/CrossOver/Bottles"

    static var bottleName: String { LauncherSettings.shared.bottleName }
    static var bottleDir: String { bottlesDir + "/" + bottleName }
    static var driveC: String { bottleDir + "/drive_c" }
    static var steamDir: String { driveC + "/Program Files (x86)/Steam" }
    static var repoRoot: String { (LauncherSettings.shared.repoRoot as NSString).expandingTildeInPath }
    static var runtimeDir: String { (LauncherSettings.shared.runtimeDir as NSString).expandingTildeInPath }
    static var runtimeManifest: String { runtimeDir + "/oxrsys-runtime.json" }

    static var adb: String? {
        for p in ["/opt/homebrew/bin/adb", "/usr/local/bin/adb",
                  home + "/Library/Android/sdk/platform-tools/adb"]
        where FileManager.default.isExecutableFile(atPath: p) {
            return p
        }
        return nil
    }

    /// Map a POSIX path inside the bottle's drive_c to a Windows path.
    static func windowsPath(_ posix: String) -> String? {
        let c = driveC
        guard posix.hasPrefix(c) else { return nil }
        return "C:" + posix.dropFirst(c.count).replacingOccurrences(of: "/", with: "\\")
    }
}

enum AudioScopeSetting: String, CaseIterable, Identifiable, Codable {
    case game, system, off
    var id: String { rawValue }
    var label: String {
        switch self {
        case .game: return "Game only"
        case .system: return "All Mac audio"
        case .off: return "Off"
        }
    }
}

/// User preferences (UserDefaults-backed).
final class LauncherSettings: ObservableObject {
    static let shared = LauncherSettings()
    private let d = UserDefaults.standard

    @Published var audioScope: AudioScopeSetting {
        didSet { d.set(audioScope.rawValue, forKey: "audioScope") }
    }
    @Published var muteMac: Bool { didSet { d.set(muteMac, forKey: "muteMac") } }
    @Published var autoTransport: Bool { didSet { d.set(autoTransport, forKey: "autoTransport") } }
    @Published var bottleName: String { didSet { d.set(bottleName, forKey: "bottleName") } }
    @Published var repoRoot: String { didSet { d.set(repoRoot, forKey: "repoRoot") } }
    @Published var runtimeDir: String { didSet { d.set(runtimeDir, forKey: "runtimeDir") } }

    private init() {
        audioScope = AudioScopeSetting(rawValue: d.string(forKey: "audioScope") ?? "") ?? .game
        muteMac = d.object(forKey: "muteMac") as? Bool ?? true
        autoTransport = d.object(forKey: "autoTransport") as? Bool ?? true
        bottleName = d.string(forKey: "bottleName") ?? "VR"
        repoRoot = d.string(forKey: "repoRoot") ?? "~/oxrsys-winebridge"
        runtimeDir = d.string(forKey: "runtimeDir") ?? "~/liboxrsys-runtime-1.1.0"
    }
}

struct CommandResult {
    var status: Int32
    var output: String
}

enum Shell {
    /// Run a command synchronously with a timeout (the process is sent SIGTERM,
    /// never SIGKILL, if it overruns). Call off the main thread.
    @discardableResult
    static func run(_ exe: String, _ args: [String], env: [String: String]? = nil,
                    timeout: TimeInterval = 10) -> CommandResult {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        if let env {
            p.environment = ProcessInfo.processInfo.environment.merging(env) { $1 }
        }
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        p.standardInput = FileHandle.nullDevice
        var data = Data()
        let lock = NSLock()
        pipe.fileHandleForReading.readabilityHandler = { h in
            let chunk = h.availableData
            lock.lock(); data.append(chunk); lock.unlock()
        }
        do { try p.run() } catch {
            pipe.fileHandleForReading.readabilityHandler = nil
            return CommandResult(status: -1, output: "\(error)")
        }
        let deadline = Date().addingTimeInterval(timeout)
        while p.isRunning && Date() < deadline { usleep(20_000) }
        if p.isRunning { p.terminate() }
        p.waitUntilExit()
        pipe.fileHandleForReading.readabilityHandler = nil
        let rest = pipe.fileHandleForReading.readDataToEndOfFile()
        lock.lock(); data.append(rest); lock.unlock()
        return CommandResult(status: p.terminationStatus,
                             output: String(decoding: data, as: UTF8.self))
    }

    /// Start a command detached (we do not wait for it; it is reaped on exit).
    static func spawn(_ exe: String, _ args: [String], env: [String: String]? = nil) throws {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        if let env {
            p.environment = ProcessInfo.processInfo.environment.merging(env) { $1 }
        }
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        p.standardInput = FileHandle.nullDevice
        try p.run()
    }
}
