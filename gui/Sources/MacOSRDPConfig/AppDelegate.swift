import AppKit

/// Menu-bar (NSStatusItem) controller for the macOS RDP daemon. Shows daemon
/// status, lets the user start/stop it, and toggles the EnvironmentVariables
/// options the daemon reads. Changes write to the LaunchAgent plist and reload
/// the daemon so settings take effect on the next connection.
final class AppDelegate: NSObject, NSApplicationDelegate {

    private var statusItem: NSStatusItem!
    private let menu = NSMenu()

    // Menu items we update on each rebuild.
    private var statusMenuItem: NSMenuItem!
    private var startItem: NSMenuItem!
    private var stopItem: NSMenuItem!
    // Video
    private var showCursorItem: NSMenuItem!
    private var cursorShapesItem: NSMenuItem!
    private var sharedModeItem: NSMenuItem!
    private var privacyBlankItem: NSMenuItem!
    // Drives
    private var rdpdrEnabledItem: NSMenuItem!
    // Audio
    private var audioLocalItem: NSMenuItem!
    private var audioInputItem: NSMenuItem!
    // Logging
    private var logDebugItem: NSMenuItem!

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem.button {
            // SF Symbol with a text fallback for older runtimes.
            if let img = NSImage(systemSymbolName: "display",
                                 accessibilityDescription: "macOS RDP") {
                img.isTemplate = true
                button.image = img
            } else {
                button.title = "RDP"
            }
        }
        buildMenu()
        statusItem.menu = menu
        refresh()
    }

    // MARK: Menu construction

    private func buildMenu() {
        menu.removeAllItems()

        statusMenuItem = NSMenuItem(title: "Daemon: …", action: nil, keyEquivalent: "")
        statusMenuItem.isEnabled = false
        menu.addItem(statusMenuItem)

        menu.addItem(.separator())

        startItem = NSMenuItem(title: "Start Daemon",
                               action: #selector(startDaemon), keyEquivalent: "")
        startItem.target = self
        menu.addItem(startItem)

        stopItem = NSMenuItem(title: "Stop Daemon",
                              action: #selector(stopDaemon), keyEquivalent: "")
        stopItem.target = self
        menu.addItem(stopItem)

        menu.addItem(.separator())

        // ── Video ─────────────────────────────────────────────────────────
        let videoHeader = NSMenuItem(title: "Video", action: nil, keyEquivalent: "")
        videoHeader.isEnabled = false
        menu.addItem(videoHeader)

        showCursorItem = NSMenuItem(title: "Show macOS cursor overlay in video",
                                    action: #selector(toggleShowCursor),
                                    keyEquivalent: "")
        showCursorItem.target = self
        menu.addItem(showCursorItem)

        cursorShapesItem = NSMenuItem(title: "Stream cursor shapes to client",
                                      action: #selector(toggleCursorShapes),
                                      keyEquivalent: "")
        cursorShapesItem.target = self
        menu.addItem(cursorShapesItem)

        sharedModeItem = NSMenuItem(title: "Shared mode (local + remote see same screen)",
                                    action: #selector(toggleSharedMode),
                                    keyEquivalent: "")
        sharedModeItem.target = self
        menu.addItem(sharedModeItem)

        privacyBlankItem = NSMenuItem(title: "Privacy blank local display on connect",
                                      action: #selector(togglePrivacyBlank),
                                      keyEquivalent: "")
        privacyBlankItem.target = self
        menu.addItem(privacyBlankItem)

        menu.addItem(.separator())

        // ── Drives ────────────────────────────────────────────────────────
        let drivesHeader = NSMenuItem(title: "Drives", action: nil, keyEquivalent: "")
        drivesHeader.isEnabled = false
        menu.addItem(drivesHeader)

        rdpdrEnabledItem = NSMenuItem(title: "Enable Windows drive redirection",
                                      action: #selector(toggleRdpdr),
                                      keyEquivalent: "")
        rdpdrEnabledItem.target = self
        menu.addItem(rdpdrEnabledItem)

        menu.addItem(.separator())

        // ── Audio ─────────────────────────────────────────────────────────
        let audioHeader = NSMenuItem(title: "Audio", action: nil, keyEquivalent: "")
        audioHeader.isEnabled = false
        menu.addItem(audioHeader)

        audioLocalItem = NSMenuItem(title: "Also play audio on local Mac speakers",
                                    action: #selector(toggleAudioLocal),
                                    keyEquivalent: "")
        audioLocalItem.target = self
        menu.addItem(audioLocalItem)

        audioInputItem = NSMenuItem(title: "Receive mic input from Windows client",
                                    action: #selector(toggleAudioInput),
                                    keyEquivalent: "")
        audioInputItem.target = self
        menu.addItem(audioInputItem)

        menu.addItem(.separator())

        // ── Logging ───────────────────────────────────────────────────────
        let loggingHeader = NSMenuItem(title: "Logging", action: nil, keyEquivalent: "")
        loggingHeader.isEnabled = false
        menu.addItem(loggingHeader)

        logDebugItem = NSMenuItem(title: "Verbose logging (debug/verbose)",
                                  action: #selector(toggleLogLevel),
                                  keyEquivalent: "")
        logDebugItem.target = self
        menu.addItem(logDebugItem)

        menu.addItem(.separator())

        let refreshItem = NSMenuItem(title: "Refresh",
                                     action: #selector(refresh), keyEquivalent: "r")
        refreshItem.target = self
        menu.addItem(refreshItem)

        let quitItem = NSMenuItem(title: "Quit RDP Config",
                                  action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)
    }

    // MARK: State refresh

    /// Re-reads daemon status and option values, updating titles/checkmarks.
    @objc private func refresh() {
        switch AgentController.status() {
        case .running:
            statusMenuItem.title = "Daemon: Running"
            startItem.isEnabled = false
            stopItem.isEnabled = true
        case .stopped:
            statusMenuItem.title = "Daemon: Stopped"
            startItem.isEnabled = true
            stopItem.isEnabled = false
        case .notInstalled:
            statusMenuItem.title = "Daemon: Not installed"
            startItem.isEnabled = false
            stopItem.isEnabled = false
        case let .unknown(msg):
            statusMenuItem.title = "Daemon: Unknown (\(msg))"
            startItem.isEnabled = true
            stopItem.isEnabled = true
        }

        // Reflect option state from the plist. On failure, leave items
        // unchecked but enabled so the user can attempt to set them.
        switch AgentController.readEnvironment() {
        case let .success(env):
            showCursorItem.state    = (env[AgentController.EnvKey.showCursor]   == "1") ? .on : .off
            cursorShapesItem.state  = (env[AgentController.EnvKey.cursorShapes] == "1") ? .on : .off
            sharedModeItem.state    = (env[AgentController.EnvKey.sharedMode]   == "1") ? .on : .off
            privacyBlankItem.state  = (env[AgentController.EnvKey.privacyBlank] == "1") ? .on : .off
            rdpdrEnabledItem.state  = (env[AgentController.EnvKey.rdpdrEnabled] == "1") ? .on : .off
            // RDP_AUDIO_LOCAL: "1" = both local+remote (default), "0" = remote only
            audioLocalItem.state    = (env[AgentController.EnvKey.audioLocal] != "0") ? .on : .off
            audioInputItem.state    = (env[AgentController.EnvKey.audioInput]  == "1") ? .on : .off
            let level = (env[AgentController.EnvKey.logLevel] ?? "info").lowercased()
            logDebugItem.state      = (level == "debug" || level == "verbose") ? .on : .off
            setOptionItems(enabled: true)
        case .failure:
            showCursorItem.state    = .off
            cursorShapesItem.state  = .off
            sharedModeItem.state    = .off
            privacyBlankItem.state  = .off
            rdpdrEnabledItem.state  = .off
            audioLocalItem.state    = .off
            audioInputItem.state    = .off
            logDebugItem.state      = .off
            setOptionItems(enabled: false)
        }
    }

    private func setOptionItems(enabled: Bool) {
        showCursorItem.isEnabled   = enabled
        cursorShapesItem.isEnabled = enabled
        sharedModeItem.isEnabled   = enabled
        privacyBlankItem.isEnabled = enabled
        rdpdrEnabledItem.isEnabled = enabled
        audioLocalItem.isEnabled   = enabled
        audioInputItem.isEnabled   = enabled
        logDebugItem.isEnabled     = enabled
    }

    // MARK: Actions

    @objc private func startDaemon() {
        handle(AgentController.start(), success: "Daemon started.")
    }

    @objc private func stopDaemon() {
        handle(AgentController.stop(), success: "Daemon stopped.")
    }

    @objc private func toggleShowCursor() {
        let newValue = (showCursorItem.state == .on) ? "0" : "1"
        setOptionAndReload(key: AgentController.EnvKey.showCursor, value: newValue)
    }

    @objc private func toggleCursorShapes() {
        let newValue = (cursorShapesItem.state == .on) ? "0" : "1"
        setOptionAndReload(key: AgentController.EnvKey.cursorShapes, value: newValue)
    }

    @objc private func toggleSharedMode() {
        let newValue = (sharedModeItem.state == .on) ? "0" : "1"
        setOptionAndReload(key: AgentController.EnvKey.sharedMode, value: newValue)
    }

    @objc private func togglePrivacyBlank() {
        let newValue = (privacyBlankItem.state == .on) ? "0" : "1"
        setOptionAndReload(key: AgentController.EnvKey.privacyBlank, value: newValue)
    }

    @objc private func toggleRdpdr() {
        let newValue = (rdpdrEnabledItem.state == .on) ? "0" : "1"
        setOptionAndReload(key: AgentController.EnvKey.rdpdrEnabled, value: newValue)
    }

    @objc private func toggleAudioLocal() {
        // RDP_AUDIO_LOCAL=1 means play on local speakers too; 0 = remote only
        let newValue = (audioLocalItem.state == .on) ? "0" : "1"
        setOptionAndReload(key: AgentController.EnvKey.audioLocal, value: newValue)
    }

    @objc private func toggleAudioInput() {
        // RDP_AUDIO_INPUT=1 enables receiving mic audio from the Windows client
        let newValue = (audioInputItem.state == .on) ? "0" : "1"
        setOptionAndReload(key: AgentController.EnvKey.audioInput, value: newValue)
    }

    @objc private func toggleLogLevel() {
        let newValue = (logDebugItem.state == .on) ? "info" : "debug"
        setOptionAndReload(key: AgentController.EnvKey.logLevel, value: newValue)
    }

    /// Writes an option then, if the daemon is currently running, reloads it
    /// so the change takes effect immediately.
    private func setOptionAndReload(key: String, value: String) {
        if case let .failure(err) = AgentController.setEnvironment(key: key, value: value) {
            showAlert(title: "Could not update option", message: "\(err)")
            return
        }

        let wasRunning: Bool
        if case .running = AgentController.status() { wasRunning = true } else { wasRunning = false }

        if wasRunning {
            if case let .failure(err) = AgentController.reload() {
                showAlert(title: "Saved, but reload failed",
                          message: "The setting was written but the daemon could "
                                 + "not be restarted:\n\(err)")
            }
        }
        refresh()
    }

    private func handle(_ result: Result<Void, AgentError>, success: String) {
        switch result {
        case .success:
            refresh()
        case let .failure(err):
            showAlert(title: "Operation failed", message: "\(err)")
        }
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }

    // MARK: Alerts

    private func showAlert(title: String, message: String) {
        // Bring the (accessory) app forward so the modal alert is visible.
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.addButton(withTitle: "OK")
        alert.runModal()
    }
}
