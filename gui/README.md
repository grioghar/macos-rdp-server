# macOS RDP Config (menu-bar app)

A tiny macOS menu-bar (status-item) app that **controls the `macos-rdp-daemon`
LaunchAgent** — it does not serve RDP itself. Use it to see whether the daemon
is running, start/stop it, and toggle the daemon options that are passed via
the LaunchAgent plist's `EnvironmentVariables`.

It is an agent app (`LSUIElement = YES`): it lives in the menu bar with **no
Dock icon and no window**.

## What it manages

The daemon reads these environment variables (confirmed against the C/ObjC
sources — `display/ScreenCapture.m` and `daemon/main.m`). The app writes them
into `~/Library/LaunchAgents/com.macosrdp.agent.plist` under
`EnvironmentVariables`, preserving every other key, then reloads the agent so
the change takes effect:

| Menu item                              | Env var            | Values        |
|----------------------------------------|--------------------|---------------|
| Show macOS cursor in video             | `RDP_SHOW_CURSOR`  | `0` / `1`     |
| Shared mode (desk + remote at once)    | `RDP_SHARED_MODE`  | `0` / `1`     |
| Verbose logging (debug)                | `RDP_LOG_LEVEL`    | `info`/`debug`|

(`RDP_SHARED_MODE` is consumed by the daemon's shared-mode path added by
parallel work; this app only writes the key.)

## Daemon control

- **Status**: `launchctl print gui/$(id -u)/com.macosrdp.agent` (a live `pid =`
  line means running).
- **Start**: `launchctl enable …` then `launchctl bootstrap gui/$(id -u) <plist>`.
- **Stop**: `launchctl bootout gui/$(id -u)/com.macosrdp.agent`.
- **Reload after an option change**: bootout + bootstrap.

All launchctl/plist failures surface as an alert (or a disabled/labelled menu
item) — the app never crashes if the agent isn't installed yet.

## Build & run

Requires the Swift toolchain (Xcode or Command Line Tools).

```sh
# Build a .app bundle (LSUIElement applies) into gui/build/
gui/build.sh
open gui/build/MacOSRDPConfig.app
```

Or run straight from SwiftPM (still status-bar-only via `.accessory`):

```sh
cd gui
swift run        # Ctrl-C to quit, or use the menu's "Quit"
```

CI compiles the app with `swift build` (see `--compile-only` in `build.sh`) on
the macOS runner to prove it builds; it does not affect the daemon build.

## Notes

- The app assumes the daemon was installed with `scripts/install-user.sh`,
  which writes the `com.macosrdp.agent` LaunchAgent. If the plist is missing,
  the menu shows **Daemon: Not installed**.
- Toggling an option only restarts the daemon if it is currently running.
