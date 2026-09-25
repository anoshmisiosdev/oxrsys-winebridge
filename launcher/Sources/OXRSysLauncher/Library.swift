import Foundation

enum VRAPI: String, Codable {
    case openVR = "OpenVR"
    case openXR = "OpenXR"
    case unknown = "Unknown"
}

struct Game: Identifiable, Codable, Hashable {
    var appID: String?          // Steam app id (nil for manual exe entries)
    var name: String
    var installDir: String?     // POSIX path
    var exePath: String?        // POSIX path, manual entries
    var api: VRAPI
    var vrEvidence: [String] = []   // relative paths of the DLLs that marked it as VR
    var artPath: String?
    var manual: Bool = false

    var id: String { appID.map { "steam:\($0)" } ?? "exe:\(exePath ?? name)" }

    /// Case-insensitive fragments that identify this game's Wine processes by argv0.
    var processMatchers: [String] {
        var m: [String] = []
        if let dir = installDir, let win = Paths.windowsPath(dir) {
            m.append((win + "\\").lowercased())
        }
        if let exe = exePath {
            m.append(((exe as NSString).lastPathComponent).lowercased())
        }
        return m
    }
}

/// Persisted manual entries and hidden ids.
struct LibraryPrefs: Codable {
    var manualGames: [Game] = []
    var hiddenIDs: [String] = []

    static func load() -> LibraryPrefs {
        guard let data = FileManager.default.contents(atPath: Paths.gamesJson),
              let p = try? JSONDecoder().decode(LibraryPrefs.self, from: data) else { return LibraryPrefs() }
        return p
    }

    func save() {
        try? FileManager.default.createDirectory(atPath: Paths.launcherSupport,
                                                 withIntermediateDirectories: true)
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        if let data = try? enc.encode(self) {
            try? data.write(to: URL(fileURLWithPath: Paths.gamesJson), options: .atomic)
        }
    }
}

enum LibraryScanner {
    /// Minimal Valve KeyValues (VDF/ACF) reader: returns flattened "a/b/c" -> value.
    static func parseVDF(_ text: String) -> [String: String] {
        var result: [String: String] = [:]
        var stack: [String] = []
        var pending: String?
        var tokens: [String] = []
        var i = text.startIndex
        while i < text.endIndex {
            let c = text[i]
            if c == "\"" {
                var s = ""
                i = text.index(after: i)
                while i < text.endIndex, text[i] != "\"" {
                    if text[i] == "\\", text.index(after: i) < text.endIndex {
                        i = text.index(after: i)
                    }
                    s.append(text[i])
                    i = text.index(after: i)
                }
                tokens.append(s)
            } else if c == "{" || c == "}" {
                tokens.append(String(c))
            }
            if i < text.endIndex { i = text.index(after: i) }
        }
        for t in tokens {
            if t == "{" {
                if let k = pending { stack.append(k) }
                pending = nil
            } else if t == "}" {
                _ = stack.popLast()
                pending = nil
            } else if let k = pending {
                result[(stack + [k]).joined(separator: "/").lowercased()] = t
                pending = nil
            } else {
                pending = t
            }
        }
        return result
    }

    static func libraryFolders() -> [String] {
        var folders = [Paths.steamDir]
        let vdfPath = Paths.steamDir + "/steamapps/libraryfolders.vdf"
        if let text = try? String(contentsOfFile: vdfPath, encoding: .utf8) {
            let kv = parseVDF(text)
            for (k, v) in kv where k.hasSuffix("/path") {
                // Windows path such as "C:\\Program Files (x86)\\Steam" or "D:\\SteamLibrary"
                var posix: String
                let drive = v.prefix(1).lowercased()
                let rest = v.dropFirst(2).replacingOccurrences(of: "\\", with: "/")
                if drive == "c" {
                    posix = Paths.driveC + rest
                } else {
                    posix = Paths.bottleDir + "/dosdevices/\(drive):" + rest
                }
                posix = (posix as NSString).resolvingSymlinksInPath
                if !folders.contains(where: { ($0 as NSString).resolvingSymlinksInPath == posix }) {
                    folders.append(posix)
                }
            }
        }
        return folders.filter { FileManager.default.fileExists(atPath: $0 + "/steamapps") }
    }

    /// Bounded search for VR runtime DLLs in a game directory.
    static func detectVR(in dir: String, maxDepth: Int = 4) -> (VRAPI, [String]) {
        let openVRNames: Set<String> = ["openvr_api.dll", "openvr_api.dll.stock"]
        let openXRNames: Set<String> = ["openxr_loader.dll", "unityopenxr.dll"]
        var openVR: [String] = []
        var openXR: [String] = []
        let fm = FileManager.default
        var queue: [(String, Int)] = [(dir, 0)]
        var visited = 0
        while !queue.isEmpty, visited < 4000 {
            let (path, depth) = queue.removeFirst()
            guard let entries = try? fm.contentsOfDirectory(atPath: path) else { continue }
            for e in entries {
                visited += 1
                let full = path + "/" + e
                let lower = e.lowercased()
                let rel = String(full.dropFirst(dir.count + 1))
                if openVRNames.contains(lower) { openVR.append(rel) }
                else if openXRNames.contains(lower) || lower == "unityopenxr" { openXR.append(rel) }
                var isDir: ObjCBool = false
                if depth < maxDepth, fm.fileExists(atPath: full, isDirectory: &isDir), isDir.boolValue {
                    queue.append((full, depth + 1))
                }
            }
        }
        // A game shipping its own OpenXR loader is native OpenXR, even if it also
        // carries an OpenVR fallback plugin.
        if !openXR.isEmpty { return (.openXR, openXR + openVR) }
        if !openVR.isEmpty { return (.openVR, openVR) }
        return (.unknown, [])
    }

    static func art(for appID: String) -> String? {
        let base = Paths.steamDir + "/appcache/librarycache"
        let fm = FileManager.default
        for flat in ["\(appID)_header.jpg", "\(appID)_library_hero.jpg"] {
            let p = base + "/" + flat
            if fm.fileExists(atPath: p) { return p }
        }
        let dir = base + "/" + appID
        let preferred = ["header.jpg", "library_header.jpg", "library_hero.jpg", "library_capsule.jpg",
                         "library_600x900.jpg"]
        var found: [String: String] = [:]
        if let top = try? fm.contentsOfDirectory(atPath: dir) {
            for e in top {
                let p = dir + "/" + e
                if preferred.contains(e) { found[e] = found[e] ?? p }
                if let sub = try? fm.contentsOfDirectory(atPath: p) {
                    for s in sub where preferred.contains(s) { found[s] = found[s] ?? p + "/" + s }
                }
            }
        }
        for name in preferred { if let p = found[name] { return p } }
        return nil
    }

    /// Scan Steam libraries for VR games. Blocking; call off the main thread.
    static func scan(includeNonVR: Bool = false) -> [Game] {
        var games: [Game] = []
        let fm = FileManager.default
        let skip: Set<String> = ["228980", "250820", "1070560", "1391110", "1628350"] // redists, SteamVR, runtimes
        for lib in libraryFolders() {
            let apps = lib + "/steamapps"
            guard let files = try? fm.contentsOfDirectory(atPath: apps) else { continue }
            for f in files where f.hasPrefix("appmanifest_") && f.hasSuffix(".acf") {
                guard let text = try? String(contentsOfFile: apps + "/" + f, encoding: .utf8) else { continue }
                let kv = parseVDF(text)
                guard let appID = kv["appstate/appid"], !skip.contains(appID),
                      let installdir = kv["appstate/installdir"] else { continue }
                let dir = apps + "/common/" + installdir
                guard fm.fileExists(atPath: dir) else { continue }
                let (api, evidence) = detectVR(in: dir)
                if api == .unknown && !includeNonVR { continue }
                games.append(Game(appID: appID, name: kv["appstate/name"] ?? installdir,
                                  installDir: dir, exePath: nil, api: api, vrEvidence: evidence,
                                  artPath: art(for: appID)))
            }
        }
        return games.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    /// Build a manual entry from a Steam app id or an exe path inside the bottle.
    static func manualGame(appID: String?, exePath: String?, name: String?) -> Game? {
        if let appID, !appID.isEmpty {
            // If Steam knows it, reuse the scanned data.
            if let g = scan(includeNonVR: true).first(where: { $0.appID == appID }) {
                var g2 = g
                g2.manual = true
                if let name, !name.isEmpty { g2.name = name }
                return g2
            }
            return Game(appID: appID, name: (name?.isEmpty == false ? name! : "App \(appID)"),
                        installDir: nil, exePath: nil, api: .unknown, artPath: art(for: appID), manual: true)
        }
        if let exePath, !exePath.isEmpty {
            let dir = (exePath as NSString).deletingLastPathComponent
            let (api, evidence) = detectVR(in: dir)
            let fallbackName = ((exePath as NSString).lastPathComponent as NSString).deletingPathExtension
            return Game(appID: nil, name: (name?.isEmpty == false ? name! : fallbackName),
                        installDir: dir, exePath: exePath, api: api, vrEvidence: evidence,
                        artPath: nil, manual: true)
        }
        return nil
    }
}
