import Foundation

/// Talks to the macOS RDP daemon's per-user LaunchAgent: reads/writes the
/// LaunchAgent plist's `EnvironmentVariables`, queries running state via
/// `launchctl print`, and starts/stops the agent with bootstrap/bootout.
///
/// Everything here is intentionally defensive: launchctl and plist access can
/// fail for many reasons (agent not installed, plist missing, permissions),
/// and the UI must never crash. Mutating calls therefore return a `Result` so
/// the caller can surface a readable alert.
struct AgentController {

    // MARK: Configuration constants

    /// Service label used by the live per-user LaunchAgent (the label in
    /// ~/Library/LaunchAgents/com.macosrdp.agent.user.plist and registered
    /// under gui/<uid>/com.macosrdp.agent.user by launchctl bootstrap).
    /// NOTE: scripts/install-user.sh historically used "com.macosrdp.agent" —
    /// that is the discrepancy to reconcile; the live deploy uses .agent.user.
    static let label = "com.macosrdp.agent.user"

    /// LaunchAgent plist path for the current user.
    static var plistPath: String {
        (NSHomeDirectory() as NSString)
            .appendingPathComponent("Library/LaunchAgents/\(label).plist")
    }

    /// `gui/<uid>` domain target used by launchctl for per-user agents.
    static var guiDomain: String {
        "gui/\(getuid())"
    }

    /// Fully-qualified service target, e.g. `gui/501/com.macosrdp.agent`.
    static var serviceTarget: String {
        "\(guiDomain)/\(label)"
    }

    /// Environment-variable keys the daemon actually reads (confirmed against
    /// the C/ObjC sources: display/ScreenCapture.m reads RDP_SHOW_CURSOR,
    /// daemon/main.m reads RDP_LOG_LEVEL; RDP_SHARED_MODE is added by parallel
    /// work and read by the daemon's shared-mode path).
    enum EnvKey {
        static let showCursor  = "RDP_SHOW_CURSOR"
        static let sharedMode  = "RDP_SHARED_MODE"
        static let logLevel    = "RDP_LOG_LEVEL"
    }

    // MARK: Process helper

    /// Runs an executable and captures combined stdout/stderr + exit status.
    @discardableResult
    private static func run(_ launchPath: String,
                            _ args: [String]) -> (status: Int32, output: String) {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: launchPath)
        proc.arguments = args

        let pipe = Pipe()
        proc.standardOutput = pipe
        proc.standardError = pipe

        do {
            try proc.run()
        } catch {
            return (-1, "failed to launch \(launchPath): \(error.localizedDescription)")
        }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        proc.waitUntilExit()
        let out = String(data: data, encoding: .utf8) ?? ""
        return (proc.terminationStatus, out)
    }

    // MARK: Status

    enum Status {
        case running
        case stopped
        case notInstalled
        case unknown(String)
    }

    /// Determines whether the agent is loaded/running. Uses `launchctl print`
    /// on the service target; a zero exit means the service exists in the
    /// domain. We then look for a PID to distinguish loaded-and-running from
    /// loaded-but-not-running.
    static func status() -> Status {
        guard FileManager.default.fileExists(atPath: plistPath) else {
            return .notInstalled
        }
        let r = run("/bin/launchctl", ["print", serviceTarget])
        if r.status != 0 {
            // Non-zero usually means "could not find service" => not loaded.
            return .stopped
        }
        // Loaded. Check for a live PID line: `pid = 1234`.
        for rawLine in r.output.split(separator: "\n") {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("pid = ") {
                let value = line.dropFirst("pid = ".count).trimmingCharacters(in: .whitespaces)
                if Int(value) != nil { return .running }
            }
        }
        // Loaded but no pid => registered yet not currently running.
        return .stopped
    }

    // MARK: Start / stop

    static func start() -> Result<Void, AgentError> {
        guard FileManager.default.fileExists(atPath: plistPath) else {
            return .failure(.notInstalled)
        }
        // Clear any prior `disable` override, then bootstrap.
        _ = run("/bin/launchctl", ["enable", serviceTarget])
        let r = run("/bin/launchctl", ["bootstrap", guiDomain, plistPath])
        // bootstrap returns 5 ("Input/output error") when the service is
        // already loaded — treat that as success rather than an error.
        if r.status == 0 || isAlreadyLoaded(r) {
            return .success(())
        }
        return .failure(.launchctl("bootstrap", r.status, r.output))
    }

    static func stop() -> Result<Void, AgentError> {
        let r = run("/bin/launchctl", ["bootout", serviceTarget])
        // bootout returns 3 ("No such process") when not loaded — that means
        // it's already stopped, which is the desired end state.
        if r.status == 0 || isNotLoaded(r) {
            return .success(())
        }
        return .failure(.launchctl("bootout", r.status, r.output))
    }

    /// bootout + bootstrap so the daemon re-reads its EnvironmentVariables.
    static func reload() -> Result<Void, AgentError> {
        _ = run("/bin/launchctl", ["bootout", serviceTarget])
        return start()
    }

    private static func isAlreadyLoaded(_ r: (status: Int32, output: String)) -> Bool {
        r.output.localizedCaseInsensitiveContains("already") ||
        r.output.localizedCaseInsensitiveContains("service already loaded")
    }

    private static func isNotLoaded(_ r: (status: Int32, output: String)) -> Bool {
        r.output.localizedCaseInsensitiveContains("No such process") ||
        r.output.localizedCaseInsensitiveContains("Could not find")
    }

    // MARK: Plist environment variables

    /// Reads the `EnvironmentVariables` dict from the LaunchAgent plist.
    /// Returns an empty dict if the plist or key is absent (non-fatal).
    static func readEnvironment() -> Result<[String: String], AgentError> {
        guard FileManager.default.fileExists(atPath: plistPath) else {
            return .failure(.notInstalled)
        }
        guard let data = FileManager.default.contents(atPath: plistPath) else {
            return .failure(.plist("could not read \(plistPath)"))
        }
        do {
            let obj = try PropertyListSerialization.propertyList(
                from: data, options: [], format: nil)
            guard let root = obj as? [String: Any] else {
                return .failure(.plist("plist root is not a dictionary"))
            }
            let env = root[envKey] as? [String: Any] ?? [:]
            var result: [String: String] = [:]
            for (k, v) in env { result[k] = "\(v)" }
            return .success(result)
        } catch {
            return .failure(.plist("parse error: \(error.localizedDescription)"))
        }
    }

    /// Sets (or removes, when `value` is nil) a single key inside the plist's
    /// `EnvironmentVariables`, preserving every other key in the file. Writes
    /// the plist back in XML format. Does NOT reload the agent — the caller
    /// decides when to reload.
    static func setEnvironment(key: String, value: String?) -> Result<Void, AgentError> {
        guard FileManager.default.fileExists(atPath: plistPath) else {
            return .failure(.notInstalled)
        }
        guard let data = FileManager.default.contents(atPath: plistPath) else {
            return .failure(.plist("could not read \(plistPath)"))
        }
        do {
            var format = PropertyListSerialization.PropertyListFormat.xml
            let obj = try PropertyListSerialization.propertyList(
                from: data, options: [.mutableContainersAndLeaves], format: &format)
            guard var root = obj as? [String: Any] else {
                return .failure(.plist("plist root is not a dictionary"))
            }
            var env = (root[envKey] as? [String: Any]) ?? [:]
            if let value = value {
                env[key] = value
            } else {
                env.removeValue(forKey: key)
            }
            root[envKey] = env

            let outData = try PropertyListSerialization.data(
                fromPropertyList: root, format: .xml, options: 0)
            try outData.write(to: URL(fileURLWithPath: plistPath))
            return .success(())
        } catch {
            return .failure(.plist("write error: \(error.localizedDescription)"))
        }
    }

    private static let envKey = "EnvironmentVariables"
}

/// Readable errors surfaced to the user via an alert.
enum AgentError: Error, CustomStringConvertible {
    case notInstalled
    case launchctl(String, Int32, String)
    case plist(String)

    var description: String {
        switch self {
        case .notInstalled:
            return "The RDP daemon LaunchAgent is not installed.\n"
                + "Run scripts/install-user.sh on this Mac first."
        case let .launchctl(cmd, status, output):
            let trimmed = output.trimmingCharacters(in: .whitespacesAndNewlines)
            return "launchctl \(cmd) failed (exit \(status)).\n\(trimmed)"
        case let .plist(msg):
            return "LaunchAgent plist problem: \(msg)"
        }
    }
}
