// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "OXRSysLauncher",
    platforms: [.macOS("14.4")],
    targets: [
        .target(
            name: "TapCore",
            path: "Sources/TapCore",
            cSettings: [.unsafeFlags(["-fobjc-arc"])],
            linkerSettings: [.linkedFramework("CoreAudio"), .linkedFramework("Foundation")]
        ),
        .executableTarget(
            name: "TapProbe",
            dependencies: ["TapCore"],
            path: "Sources/TapProbe"
        ),
        .executableTarget(
            name: "OXRSysLauncher",
            dependencies: ["TapCore"],
            path: "Sources/OXRSysLauncher",
            linkerSettings: [.linkedFramework("IOKit"), .linkedFramework("AppKit")]
        ),
    ]
)
