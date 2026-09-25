import AppKit
import Foundation
import IOKit.ps
@preconcurrency import TapCore

struct AdbDevice: Codable, Hashable {
    var serial: String
    var state: String   // "device", "unauthorized", "offline", ...
    var isWireless: Bool { serial.contains(":") || serial.contains("._adb-tls") }
}

struct PowerInfo: Codable {
    var onAC = false
    var adapterWatts: Int?
    var batteryPercent: Int?
    var charging = false
    var drainingOnAC = false
}

struct RuntimeLive: Codable {
    var state: String = "unknown"
    var transport: String?
    var clientName: String?
    var applicationName: String?
    var processID: Int?
    var fresh = false               // updated within the last few seconds by a live process
    var refreshHz: Double?
    var bitrateMbps: Double?
    var encodeTotalMs: Double?
    var serverPipelineMs: Double?
    var clientDecodeMs: Double?
    var headsetAudio: Bool?
    var droppedFrames: Double?
}

struct StatusSnapshot: Codable {
    var adbDevices: [AdbDevice] = []
    var adbAvailable = false
    var runtimeInstalled = false
    var runtimeManifest = ""
    var configuredTransport: String?
    var headsetAudioEnabled: Bool?
    var crossOverRunning = false
    var crossOverHasXRRuntimeEnv = false
    var bottleHasXRRuntimeEnv = false
    var steamRunning = false
    var steamPID: Int32?
    var videoDecodersRunning: [String] = []   // scrcpy / OXRSys Simulator
    var power = PowerInfo()
    var runtime = RuntimeLive()

    var usbHeadset: AdbDevice? { adbDevices.first { $0.state == "device" && !$0.isWireless } }
    var headsetConnected: Bool {
        usbHeadset != nil || (runtime.fresh && ["streaming", "connected"].contains(runtime.state))
    }
    var xrEnvOK: Bool { crossOverHasXRRuntimeEnv || bottleHasXRRuntimeEnv }
    var wantedTransport: String { usbHeadset != nil ? "usb_adb" : "wifi" }

    struct Warning: Identifiable, Hashable {
        var id: String
        var title: String
        var detail: String
        var severe: Bool
    }

    var warnings: [Warning] {
        var w: [Warning] = []
        if !runtimeInstalled {
            w.append(.init(id: "runtime", title: "OXRSys runtime not found",
                           detail: "Expected \(runtimeManifest). Install it with scripts/install.sh.", severe: true))
        }
        if steamRunning && !xrEnvOK {
            w.append(.init(id: "xrenv", title: "Steam may not see the OXRSys runtime",
                           detail: "XR_RUNTIME_JSON is neither in the bottle's environment nor CrossOver's. Quit Steam from its menu, then use \"Start CrossOver\" so it is exported first.",
                           severe: true))
        }
        if power.onAC, let watts = power.adapterWatts, watts < 90 {
            w.append(.init(id: "charger", title: "\(watts) W charger",
                           detail: "A charger under ~90 W cannot sustain VR streaming; the battery will drain even on AC.",
                           severe: false))
        } else if !power.onAC {
            w.append(.init(id: "battery", title: "Running on battery",
                           detail: "Plug in a 96 W+ charger for sustained streaming.", severe: false))
        }
        if power.drainingOnAC {
            w.append(.init(id: "drain", title: "Battery draining on AC",
                           detail: "The charger cannot keep up with the load.", severe: false))
        }
        if !videoDecodersRunning.isEmpty {
            w.append(.init(id: "decoders", title: "\(videoDecodersRunning.joined(separator: ", ")) running",
                           detail: "Decoding video on the Mac starves the encoder (14 → 45 ms, corruption at 120 Hz). Close it while playing.",
                           severe: false))
        }
        if runtime.fresh, runtime.state == "streaming", let live = runtime.transport,
           live != wantedTransport, usbHeadset != nil || live == "usb_adb" {
            w.append(.init(id: "transport", title: "Stream is on \(live), headset is on \(wantedTransport == "usb_adb" ? "USB" : "Wi-Fi")",
                           detail: "The transport is fixed at session start. Restart the game after switching USB/Wi-Fi.",
                           severe: false))
        }
        if adbDevices.contains(where: { $0.state == "unauthorized" }) {
            w.append(.init(id: "adbauth", title: "Headset USB debugging not authorized",
                           detail: "Put on the headset and allow USB debugging for this Mac.", severe: false))
        }
        return w
    }
}

enum StatusCollector {
    static var lastBattery: (percent: Int, at: Date)?

    static func collect() -> StatusSnapshot {
        var s = StatusSnapshot()
        let fm = FileManager.default

        // Headset over USB.
        if let adb = Paths.adb {
            s.adbAvailable = true
            let r = Shell.run(adb, ["devices"], timeout: 4)
            for line in r.output.split(separator: "\n").dropFirst() {
                let parts = line.split(whereSeparator: { $0 == "\t" || $0 == " " })
                if parts.count >= 2 { s.adbDevices.append(AdbDevice(serial: String(parts[0]), state: String(parts[1]))) }
            }
        }

        // Runtime install + config.
        s.runtimeManifest = Paths.runtimeManifest
        s.runtimeInstalled = fm.fileExists(atPath: Paths.runtimeManifest)
            && fm.fileExists(atPath: Paths.runtimeDir + "/liboxrsys-runtime.dylib")
        if let toml = try? String(contentsOfFile: Paths.runtimeToml, encoding: .utf8) {
            s.configuredTransport = RuntimeConfig.value(of: "transport", in: toml)
            s.headsetAudioEnabled = RuntimeConfig.value(of: "headset_audio", in: toml).map { $0 == "true" }
        }

        // Processes.
        let procs = OXListProcesses()
        for entry in procs {
            guard entry.count == 2, let pid = entry[0] as? NSNumber, let argv0 = entry[1] as? String else { continue }
            let lower = argv0.lowercased()
            if lower.hasSuffix("\\steam\\steam.exe") { s.steamRunning = true; s.steamPID = pid.int32Value }
            if lower.contains("scrcpy") && !s.videoDecodersRunning.contains("scrcpy") {
                s.videoDecodersRunning.append("scrcpy")
            }
            if lower.contains("oxrsys") && lower.contains("simulator") && !lower.contains("launcher")
                && !s.videoDecodersRunning.contains("OXRSys Simulator") {
                s.videoDecodersRunning.append("OXRSys Simulator")
            }
        }
        let co = NSRunningApplication.runningApplications(withBundleIdentifier: "com.codeweavers.CrossOver")
        s.crossOverRunning = !co.isEmpty
        if let pid = co.first?.processIdentifier {
            // Wine processes clobber their argv/env area, but the CrossOver app itself does not.
            let r = Shell.run("/bin/ps", ["-E", "-p", "\(pid)", "-o", "command="], timeout: 3)
            s.crossOverHasXRRuntimeEnv = r.output.contains("XR_RUNTIME_JSON=")
        }
        if let conf = try? String(contentsOfFile: Paths.bottleDir + "/cxbottle.conf", encoding: .utf8) {
            s.bottleHasXRRuntimeEnv = bottleEnvHasXR(conf)
        }

        s.power = power()
        s.runtime = runtimeLive()
        return s
    }

    static func bottleEnvHasXR(_ conf: String) -> Bool {
        var inEnv = false
        for raw in conf.split(separator: "\n") {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("[") { inEnv = line == "[EnvironmentVariables]"; continue }
            if inEnv && line.hasPrefix("\"XR_RUNTIME_JSON\"") { return true }
        }
        return false
    }

    static func power() -> PowerInfo {
        var p = PowerInfo()
        if let details = IOPSCopyExternalPowerAdapterDetails()?.takeRetainedValue() as? [String: Any] {
            p.adapterWatts = details[kIOPSPowerAdapterWattsKey] as? Int
        }
        if let blob = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
           let list = IOPSCopyPowerSourcesList(blob)?.takeRetainedValue() as? [CFTypeRef] {
            for ps in list {
                guard let d = IOPSGetPowerSourceDescription(blob, ps)?.takeUnretainedValue() as? [String: Any] else { continue }
                if let state = d[kIOPSPowerSourceStateKey] as? String { p.onAC = state == kIOPSACPowerValue }
                if let cur = d[kIOPSCurrentCapacityKey] as? Int, let max = d[kIOPSMaxCapacityKey] as? Int, max > 0 {
                    p.batteryPercent = cur * 100 / max
                }
                p.charging = d[kIOPSIsChargingKey] as? Bool ?? false
            }
        }
        if let pct = p.batteryPercent {
            if p.onAC, let last = lastBattery, pct < last.percent, !p.charging {
                p.drainingOnAC = true
            }
            if lastBattery == nil || lastBattery!.percent != pct || Date().timeIntervalSince(lastBattery!.at) > 600 {
                lastBattery = (pct, Date())
            }
        }
        return p
    }

    static func runtimeLive() -> RuntimeLive {
        var r = RuntimeLive()
        guard let data = FileManager.default.contents(atPath: Paths.runtimeStatus),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return r }
        r.state = obj["state"] as? String ?? "unknown"
        r.transport = obj["transport"] as? String
        r.clientName = obj["client_name"] as? String
        r.applicationName = obj["application_name"] as? String
        r.processID = obj["process_id"] as? Int
        let updated = (obj["updated_at_unix_ms"] as? Double) ?? 0
        let ageOK = Date().timeIntervalSince1970 * 1000 - updated < 5000
        let alive = r.processID.map { kill(pid_t($0), 0) == 0 } ?? false
        r.fresh = ageOK || alive
        if let st = obj["streaming_stats"] as? [String: Any] {
            r.refreshHz = st["refresh_rate_hz"] as? Double
            r.bitrateMbps = st["current_bitrate_mbps"] as? Double
            r.headsetAudio = st["headset_audio"] as? Bool
            if let enc = st["encode_ms"] as? [String: Any] { r.encodeTotalMs = enc["total_avg"] as? Double }
            if let lat = st["latency_ms"] as? [String: Any] {
                r.serverPipelineMs = lat["server_pipeline"] as? Double
                r.clientDecodeMs = lat["client_decode"] as? Double
            }
            if let c = st["counters"] as? [String: Any] { r.droppedFrames = c["encoder_dropped_frames_total"] as? Double }
        }
        return r
    }
}

@MainActor
final class StatusMonitor: ObservableObject {
    @Published var snapshot = StatusSnapshot()
    @Published var loaded = false
    private var task: Task<Void, Never>?

    func start() {
        guard task == nil else { return }
        task = Task.detached(priority: .utility) { [weak self] in
            while !Task.isCancelled {
                let snap = StatusCollector.collect()
                await self?.apply(snap)
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
    }

    func refreshNow() {
        Task.detached(priority: .userInitiated) { [weak self] in
            let snap = StatusCollector.collect()
            await self?.apply(snap)
        }
    }

    private func apply(_ snap: StatusSnapshot) {
        snapshot = snap
        loaded = true
    }
}
