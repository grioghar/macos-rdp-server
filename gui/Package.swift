// swift-tools-version:5.7
import PackageDescription

// SwiftPM manifest for the macOS RDP menu-bar configuration app.
//
// This builds a plain executable (`MacOSRDPConfig`). For day-to-day use you
// normally want a proper .app bundle so that LSUIElement (no Dock icon) takes
// effect — see gui/build.sh, which wraps the compiled binary in a bundle.
// The raw `swift build` here is what CI uses to prove the Swift/AppKit code
// compiles on the macOS runner without pulling in Xcode project tooling.
let package = Package(
    name: "MacOSRDPConfig",
    platforms: [
        .macOS(.v12)
    ],
    targets: [
        .executableTarget(
            name: "MacOSRDPConfig",
            path: "Sources/MacOSRDPConfig"
        )
    ]
)
