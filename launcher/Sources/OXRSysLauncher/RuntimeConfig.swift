import Foundation

/// Line-preserving edits of ~/Library/Application Support/OXRSys/oxrsys-runtime.toml.
/// Only the requested key's value is rewritten; comments, ordering and every other
/// key stay byte-identical. A timestamped backup is written before any change.
enum RuntimeConfig {
    static let validTransports = ["auto", "wifi", "usb_adb"]

    /// Value of the first uncommented `key = value` line (quotes stripped).
    static func value(of key: String, in toml: String) -> String? {
        for line in toml.components(separatedBy: "\n") {
            guard let (k, v, _) = split(line), k == key else { continue }
            return v
        }
        return nil
    }

    private static func split(_ line: String) -> (String, String, Range<String.Index>)? {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard !trimmed.hasPrefix("#"), !trimmed.hasPrefix("["), let eq = line.firstIndex(of: "=") else { return nil }
        let key = line[..<eq].trimmingCharacters(in: .whitespaces)
        var valueStart = line.index(after: eq)
        while valueStart < line.endIndex, line[valueStart] == " " || line[valueStart] == "\t" {
            valueStart = line.index(after: valueStart)
        }
        var valueEnd = line.endIndex
        if line[valueStart...].first == "\"" {
            if let close = line[line.index(after: valueStart)...].firstIndex(of: "\"") {
                valueEnd = line.index(after: close)
            }
        } else if let hash = line[valueStart...].firstIndex(of: "#") {
            valueEnd = hash
        }
        var raw = String(line[valueStart..<valueEnd]).trimmingCharacters(in: .whitespaces)
        if raw.hasPrefix("\""), raw.hasSuffix("\""), raw.count >= 2 { raw = String(raw.dropFirst().dropLast()) }
        // Range covering the value text (without trailing spaces before a comment).
        var trimmedEnd = valueEnd
        while trimmedEnd > valueStart, line[line.index(before: trimmedEnd)] == " " {
            trimmedEnd = line.index(before: trimmedEnd)
        }
        return (key, raw, valueStart..<trimmedEnd)
    }

    /// Replace (or insert under [streaming]) `transport`. Returns true when the file changed.
    @discardableResult
    static func setTransport(_ transport: String, path: String = Paths.runtimeToml) throws -> Bool {
        precondition(validTransports.contains(transport))
        let text = try String(contentsOfFile: path, encoding: .utf8)
        var lines = text.components(separatedBy: "\n")
        var section = ""
        var replaced = false
        var streamingHeader: Int?
        for (i, line) in lines.enumerated() {
            let t = line.trimmingCharacters(in: .whitespaces)
            if t.hasPrefix("[") {
                section = t
                if t == "[streaming]" { streamingHeader = i }
                continue
            }
            guard let (k, v, range) = split(line), k == "transport",
                  section == "[streaming]" || section.isEmpty else { continue }
            if v == transport { return false }
            var l = line
            l.replaceSubrange(range, with: "\"\(transport)\"")
            lines[i] = l
            replaced = true
            break
        }
        if !replaced {
            let entry = "transport = \"\(transport)\""
            if let h = streamingHeader { lines.insert(entry, at: h + 1) }
            else { lines.append(contentsOf: ["[streaming]", entry]) }
        }
        try backup(path)
        try lines.joined(separator: "\n").write(toFile: path, atomically: true, encoding: .utf8)
        return true
    }

    /// Copy to `<path>.launcher-YYYYmmdd-HHMMSS.bak`, keeping the newest 5 launcher backups.
    static func backup(_ path: String) throws {
        let fm = FileManager.default
        let f = DateFormatter()
        f.dateFormat = "yyyyMMdd-HHmmss"
        let dest = "\(path).launcher-\(f.string(from: Date())).bak"
        if !fm.fileExists(atPath: dest) { try fm.copyItem(atPath: path, toPath: dest) }
        let dir = (path as NSString).deletingLastPathComponent
        let prefix = (path as NSString).lastPathComponent + ".launcher-"
        let backups = ((try? fm.contentsOfDirectory(atPath: dir)) ?? [])
            .filter { $0.hasPrefix(prefix) && $0.hasSuffix(".bak") }
            .sorted()
        for old in backups.dropLast(5) { try? fm.removeItem(atPath: dir + "/" + old) }
    }
}
