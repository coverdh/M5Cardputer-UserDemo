import Foundation

public struct AppleTVNativeService: Equatable, Codable, Sendable {
    public var proto: String
    public var port: Int?
    public var pairing: String
    public var enabled: Bool
    public var hasCredentials: Bool

    public init(proto: String, port: Int?, pairing: String, enabled: Bool, hasCredentials: Bool) {
        self.proto = proto
        self.port = port
        self.pairing = pairing
        self.enabled = enabled
        self.hasCredentials = hasCredentials
    }
}

public struct AppleTVNativeDevice: Equatable, Identifiable, Codable, Sendable {
    public var id: String
    public var name: String
    public var address: String
    public var model: String
    public var services: [AppleTVNativeService]

    public init(id: String, name: String, address: String, model: String, services: [AppleTVNativeService]) {
        self.id = id
        self.name = name
        self.address = address
        self.model = model
        self.services = services
    }

    public var preferredPairingProtocol: String {
        let candidates = ["airplay", "mrp", "companion", "raop"]
        for candidate in candidates {
            if services.contains(where: { $0.proto == candidate && $0.enabled && $0.pairing != "Unsupported" && $0.pairing != "Disabled" }) {
                return candidate
            }
        }
        return services.first(where: { $0.enabled })?.proto ?? "airplay"
    }

    public var isAppleTVCandidate: Bool {
        let haystack = "\(name) \(model)".lowercased()
        return haystack.contains("appletv") || haystack.contains("apple tv")
    }
}

public struct AppleTVNativeState: Equatable, Codable, Sendable {
    public var active: Bool
    public var playing: Bool
    public var volumePercent: UInt8
    public var title: String
    public var artist: String
    public var progressPercent: UInt8

    public init(active: Bool, playing: Bool, volumePercent: UInt8, title: String, artist: String, progressPercent: UInt8) {
        self.active = active
        self.playing = playing
        self.volumePercent = volumePercent
        self.title = title
        self.artist = artist
        self.progressPercent = progressPercent
    }
}

public struct AppleTVNativePairingEvent: Equatable, Codable, Sendable {
    public var event: String
    public var message: String?
    public var deviceProvidesPin: Bool?
    public var pin: String?
    public var paired: Bool?
}

public enum AppleTVNativeError: Error, LocalizedError {
    case runtimeMissing
    case scriptMissing(URL)
    case deviceNotConfigured
    case processFailed(String)
    case badResponse(String)

    public var errorDescription: String? {
        switch self {
        case .runtimeMissing:
            return "pyatv runtime is not installed"
        case .scriptMissing(let url):
            return "Apple TV bridge script is missing at \(url.path)"
        case .deviceNotConfigured:
            return "Apple TV is not configured"
        case .processFailed(let message), .badResponse(let message):
            return message
        }
    }
}

public final class AppleTVNativeClient {
    public let supportDirectory: URL
    public let scriptURL: URL

    public init(supportDirectory: URL = AppleTVNativeClient.defaultSupportDirectory(),
                scriptURL: URL = AppleTVNativeClient.defaultScriptURL()) {
        self.supportDirectory = supportDirectory
        self.scriptURL = scriptURL
    }

    public static func defaultSupportDirectory() -> URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first ??
            URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support")
        return base.appendingPathComponent("ADVCtl/AppleTV", isDirectory: true)
    }

    public static func defaultScriptURL() -> URL {
        if let resourceURL = Bundle.main.url(forResource: "advctl_pyatv_bridge", withExtension: "py") {
            return resourceURL
        }

        let cwdURL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        let sourceRelative = cwdURL.appendingPathComponent("Sources/MacCtlHelper/Resources/advctl_pyatv_bridge.py")
        if FileManager.default.fileExists(atPath: sourceRelative.path) {
            return sourceRelative
        }

        return cwdURL.appendingPathComponent("tools/macctl-helper/Sources/MacCtlHelper/Resources/advctl_pyatv_bridge.py")
    }

    public var storageURL: URL {
        supportDirectory.appendingPathComponent("pyatv.conf")
    }

    public var venvPythonURL: URL {
        supportDirectory.appendingPathComponent("venv/bin/python3")
    }

    public var isRuntimeInstalled: Bool {
        FileManager.default.fileExists(atPath: venvPythonURL.path)
    }

    public func installRuntime(progress: @escaping (String) -> Void = { _ in }) async throws {
        try FileManager.default.createDirectory(at: supportDirectory, withIntermediateDirectories: true)
        let systemPython = try AppleTVNativeClient.preferredSystemPythonURL()
        progress("Creating Python runtime")
        _ = try await runProcess(executable: systemPython, arguments: ["-m", "venv", supportDirectory.appendingPathComponent("venv").path])
        progress("Installing pyatv")
        _ = try await runProcess(executable: venvPythonURL, arguments: ["-m", "pip", "install", "--upgrade", "pip", "pyatv"])
        progress("pyatv runtime ready")
    }

    public static func preferredSystemPythonURL() throws -> URL {
        if let override = ProcessInfo.processInfo.environment["ADVCTL_PYTHON"], !override.isEmpty {
            return URL(fileURLWithPath: override)
        }
        for path in ["/usr/bin/python3", "/opt/homebrew/bin/python3", "/usr/local/bin/python3"] {
            if FileManager.default.isExecutableFile(atPath: path) {
                let url = URL(fileURLWithPath: path)
                if isPythonCompatibleForPyATV(url) {
                    return url
                }
            }
        }
        throw AppleTVNativeError.processFailed("compatible python3 was not found")
    }

    private static func isPythonCompatibleForPyATV(_ url: URL) -> Bool {
        let process = Process()
        process.executableURL = url
        process.arguments = ["-c", "import inspect; raise SystemExit(0 if hasattr(inspect, 'getargspec') else 1)"]
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus == 0
        } catch {
            return false
        }
    }

    public func scan(timeout: Int = 5) async throws -> [AppleTVNativeDevice] {
        let output = try await runBridge(arguments: ["scan", "--timeout", "\(timeout)"])
        struct Response: Decodable { var devices: [AppleTVNativeDevice] }
        return try decode(Response.self, from: output).devices.filter(\.isAppleTVCandidate)
    }

    public func handle(_ command: MacCtlCommand, identifier: String) async throws {
        switch command {
        case .volumeDelta(let delta):
            try await applyVolumeDelta(delta, identifier: identifier)
        case .playPause:
            _ = try await runBridge(arguments: ["remote", "--identifier", identifier, "--command", "play_pause"])
        }
    }

    public func applyVolumeDelta(_ delta: Int, identifier: String) async throws {
        guard delta != 0 else {
            return
        }
        _ = try await runBridge(arguments: ["volume", "--identifier", identifier, "--delta", "\(delta)"])
    }

    public func fetchState(identifier: String) async throws -> AppleTVNativeState {
        let output = try await runBridge(arguments: ["state", "--identifier", identifier])
        return try decode(AppleTVNativeState.self, from: output)
    }

    public func startPairing(identifier: String,
                             proto: String,
                             eventHandler: @escaping (AppleTVNativePairingEvent) -> Void) throws -> AppleTVNativePairingSession {
        try ensureRuntimeReady()
        try ensureScriptReady()
        try FileManager.default.createDirectory(at: supportDirectory, withIntermediateDirectories: true)
        return try AppleTVNativePairingSession(pythonURL: venvPythonURL,
                                               scriptURL: scriptURL,
                                               storageURL: storageURL,
                                               identifier: identifier,
                                               proto: proto,
                                               eventHandler: eventHandler)
    }

    public func startCommandSession(identifier: String,
                                    eventHandler: @escaping (String) -> Void = { _ in }) throws -> AppleTVNativeCommandSession {
        try ensureRuntimeReady()
        try ensureScriptReady()
        try FileManager.default.createDirectory(at: supportDirectory, withIntermediateDirectories: true)
        return try AppleTVNativeCommandSession(pythonURL: venvPythonURL,
                                               scriptURL: scriptURL,
                                               storageURL: storageURL,
                                               identifier: identifier,
                                               eventHandler: eventHandler)
    }

    private func runBridge(arguments: [String]) async throws -> String {
        try ensureRuntimeReady()
        try ensureScriptReady()
        try FileManager.default.createDirectory(at: supportDirectory, withIntermediateDirectories: true)
        return try await runProcess(executable: venvPythonURL,
                                    arguments: [scriptURL.path, "--storage", storageURL.path] + arguments)
    }

    private func ensureRuntimeReady() throws {
        guard isRuntimeInstalled else {
            throw AppleTVNativeError.runtimeMissing
        }
    }

    private func ensureScriptReady() throws {
        guard FileManager.default.fileExists(atPath: scriptURL.path) else {
            throw AppleTVNativeError.scriptMissing(scriptURL)
        }
    }

    private func runProcess(executable: URL, arguments: [String]) async throws -> String {
        try await withCheckedThrowingContinuation { continuation in
            let process = Process()
            process.executableURL = executable
            process.arguments = arguments
            let stdout = Pipe()
            let stderr = Pipe()
            process.standardOutput = stdout
            process.standardError = stderr
            process.terminationHandler = { process in
                let output = String(data: stdout.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
                let error = String(data: stderr.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
                if process.terminationStatus == 0 {
                    continuation.resume(returning: output)
                } else {
                    continuation.resume(throwing: AppleTVNativeError.processFailed(error.isEmpty ? output : error))
                }
            }
            do {
                try process.run()
            } catch {
                continuation.resume(throwing: error)
            }
        }
    }

    private func decode<T: Decodable>(_ type: T.Type, from output: String) throws -> T {
        let trimmed = output.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let data = trimmed.data(using: .utf8), !data.isEmpty else {
            throw AppleTVNativeError.badResponse("empty Apple TV bridge response")
        }
        do {
            return try JSONDecoder().decode(type, from: data)
        } catch {
            throw AppleTVNativeError.badResponse("invalid Apple TV bridge response: \(trimmed)")
        }
    }
}

public final class AppleTVNativeCommandSession {
    private let process: Process
    private let input: Pipe
    private let output: Pipe
    private let error: Pipe
    private let eventHandler: (String) -> Void
    private let writeQueue = DispatchQueue(label: "dev.cardputer.advctl.apple-tv-command-session")

    init(pythonURL: URL,
         scriptURL: URL,
         storageURL: URL,
         identifier: String,
         eventHandler: @escaping (String) -> Void) throws {
        self.process = Process()
        self.input = Pipe()
        self.output = Pipe()
        self.error = Pipe()
        self.eventHandler = eventHandler
        process.executableURL = pythonURL
        process.arguments = [scriptURL.path, "--storage", storageURL.path, "serve", "--identifier", identifier]
        process.standardInput = input
        process.standardOutput = output
        process.standardError = error
        output.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else {
                return
            }
            self?.eventHandler(text.trimmingCharacters(in: .whitespacesAndNewlines))
        }
        error.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else {
                return
            }
            self?.eventHandler(text.trimmingCharacters(in: .whitespacesAndNewlines))
        }
        process.terminationHandler = { [weak self] process in
            self?.output.fileHandleForReading.readabilityHandler = nil
            self?.error.fileHandleForReading.readabilityHandler = nil
            self?.eventHandler("Apple TV worker exited with \(process.terminationStatus)")
        }
        try process.run()
    }

    public var isRunning: Bool {
        process.isRunning
    }

    public func sendVolumeDelta(_ delta: Int) {
        send(["command": "volume", "delta": delta])
    }

    public func sendPlayPause() {
        send(["command": "remote", "name": "play_pause"])
    }

    public func stop() {
        send(["command": "stop"])
        if process.isRunning {
            process.terminate()
        }
    }

    private func send(_ object: [String: Any]) {
        writeQueue.async { [input] in
            guard JSONSerialization.isValidJSONObject(object),
                  let data = try? JSONSerialization.data(withJSONObject: object, options: []),
                  var line = String(data: data, encoding: .utf8)?.data(using: .utf8) else {
                return
            }
            line.append(0x0A)
            input.fileHandleForWriting.write(line)
        }
    }
}

public final class AppleTVNativePairingSession {
    private let process: Process
    private let input: Pipe
    private let output: Pipe
    private let error: Pipe
    private let eventHandler: (AppleTVNativePairingEvent) -> Void
    private var outputBuffer = Data()
    private var errorBuffer = Data()

    init(pythonURL: URL,
         scriptURL: URL,
         storageURL: URL,
         identifier: String,
         proto: String,
         eventHandler: @escaping (AppleTVNativePairingEvent) -> Void) throws {
        self.process = Process()
        self.input = Pipe()
        self.output = Pipe()
        self.error = Pipe()
        self.eventHandler = eventHandler
        process.executableURL = pythonURL
        process.arguments = [scriptURL.path, "--storage", storageURL.path, "pair", "--identifier", identifier, "--protocol", proto]
        process.standardInput = input
        process.standardOutput = output
        process.standardError = error
        output.fileHandleForReading.readabilityHandler = { [weak self] handle in
            self?.consume(handle.availableData)
        }
        error.fileHandleForReading.readabilityHandler = { [weak self] handle in
            guard let self else { return }
            let data = handle.availableData
            if data.isEmpty { return }
            self.errorBuffer.append(data)
        }
        process.terminationHandler = { [weak self] process in
            self?.output.fileHandleForReading.readabilityHandler = nil
            self?.error.fileHandleForReading.readabilityHandler = nil
            if process.terminationStatus != 0 {
                let text = self.flatMap { String(data: $0.errorBuffer, encoding: .utf8) }?
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                self?.eventHandler(AppleTVNativePairingEvent(event: "error",
                                                             message: text?.isEmpty == false ? text! : "Pairing process exited with \(process.terminationStatus)",
                                                             deviceProvidesPin: nil,
                                                             pin: nil,
                                                             paired: false))
            }
        }
        try process.run()
    }

    deinit {
        cancel()
    }

    public func submit(pin: String) {
        guard let data = "\(pin)\n".data(using: .utf8) else {
            return
        }
        input.fileHandleForWriting.write(data)
    }

    public func cancel() {
        if process.isRunning {
            process.terminate()
        }
    }

    private func consume(_ data: Data) {
        guard !data.isEmpty else {
            return
        }
        outputBuffer.append(data)
        while let newline = outputBuffer.firstIndex(of: 0x0A) {
            let lineData = outputBuffer[..<newline]
            outputBuffer.removeSubrange(...newline)
            guard !lineData.isEmpty else {
                continue
            }
            do {
                let event = try JSONDecoder().decode(AppleTVNativePairingEvent.self, from: Data(lineData))
                eventHandler(event)
            } catch {
                let text = String(data: Data(lineData), encoding: .utf8) ?? "invalid pairing output"
                eventHandler(AppleTVNativePairingEvent(event: "error", message: text, deviceProvidesPin: nil, pin: nil, paired: nil))
            }
        }
    }
}
