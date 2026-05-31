import AppKit

// Entry point. LSUIElement (set in the bundled Info.plist via build.sh) keeps
// this out of the Dock; we also set the activation policy to .accessory so a
// raw `swift run` binary behaves the same — a status-bar-only agent app.
let app = NSApplication.shared
app.setActivationPolicy(.accessory)

let delegate = AppDelegate()
app.delegate = delegate
app.run()
