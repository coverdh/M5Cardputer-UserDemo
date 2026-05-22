import AppKit
import Foundation
import IOKit.hid
import MacCtlCore

private let advCtlVendorID = 0x16C0
private let advCtlProductID = 0x05DF
private let advCtlProductName = "ADVCtl"
private let advCtlUsagePage = 0xFF00
private let advCtlUsage = 0x01
private let hidUsagePageGenericDesktop = 0x01
private let hidUsageKeyboard = 0x06
private let advCtlReportID: UInt32 = 4
private let advCtlReportLength = 64
private let advCtlRequestConfigCommand: UInt8 = 0x80
private let advCtlSetInputCommand: UInt8 = 0x81
private let advCtlAudioTestCommand: UInt8 = 0x82
private let advCtlResetConfigCommand: UInt8 = 0x83
private let advCtlTimeSyncCommand: UInt8 = 0x84
private let advCtlSetPowerCommand: UInt8 = 0x86
private let advCtlNowPlayingCommand: UInt8 = 0x87
private let advCtlNowPlayingTitleCommand: UInt8 = 0x88
private let advCtlNowPlayingArtistCommand: UInt8 = 0x89
private let advCtlConfigReport: UInt8 = 0x90
private let advCtlPowerReport: UInt8 = 0x91
private let advCtlKeyboardReportID: UInt32 = 1
private let hidKeyF13: UInt8 = 0x68
private let hidKeyF14: UInt8 = 0x69
private let hidKeyF15: UInt8 = 0x6A
private let macKeyF13: UInt16 = 0x69
private let macKeyF14: UInt16 = 0x6B
private let macKeyF15: UInt16 = 0x71
private let systemDefinedKeyDownSubtype: Int16 = 8
private let systemDefinedKeyStateDown = 0x0A
private let nxKeyTypeBrightnessDown = 3
private let nxKeyTypeBrightnessUp = 2
private let advCtlTimeSyncEpoch: TimeInterval = 1_704_067_200
private let advCtlLogURL = URL(fileURLWithPath: NSHomeDirectory())
    .appendingPathComponent(".config/karabiner/advctl.log")

private func log(_ message: String) {
    let line = "\(Date()) \(message)\n"
    let data = Data(line.utf8)
    FileHandle.standardOutput.write(data)
    try? FileManager.default.createDirectory(at: advCtlLogURL.deletingLastPathComponent(),
                                             withIntermediateDirectories: true)
    if let handle = try? FileHandle(forWritingTo: advCtlLogURL) {
        _ = try? handle.seekToEnd()
        try? handle.write(contentsOf: data)
        try? handle.close()
    } else {
        try? data.write(to: advCtlLogURL)
    }
}

private func hidProperty<T>(_ device: IOHIDDevice, _ key: CFString, as type: T.Type) -> T? {
    IOHIDDeviceGetProperty(device, key) as? T
}

private func clippedUTF8Bytes(_ string: String, maxBytes: Int) -> [UInt8] {
    var result: [UInt8] = []
    for scalar in string.unicodeScalars {
        let bytes = Array(String(scalar).utf8)
        if result.count + bytes.count > maxBytes {
            break
        }
        result.append(contentsOf: bytes)
    }
    return result
}

private func separatorBox() -> NSBox {
    let box = NSBox()
    box.boxType = .separator
    return box
}

private struct HelperConfig {
    var homeAssistant: HomeAssistantConfig?
    var expectedCommands: Int?

    static func load(arguments: [String] = CommandLine.arguments, environment: [String: String] = ProcessInfo.processInfo.environment) -> HelperConfig {
        var config = HelperConfig()
        var values = environment

        var iterator = arguments.dropFirst().makeIterator()
        while let arg = iterator.next() {
            guard let value = iterator.next() else {
                continue
            }
            switch arg {
            case "--ha-url":
                values["ADVCTL_HA_URL"] = value
            case "--ha-token":
                values["ADVCTL_HA_TOKEN"] = value
            case "--ha-token-file":
                values["ADVCTL_HA_TOKEN_FILE"] = value
            case "--entity":
                values["ADVCTL_HA_ENTITY"] = value
            default:
                continue
            }
        }

        let urlString = values["ADVCTL_HA_URL"] ?? values["MACCTL_HA_URL"] ?? "http://127.0.0.1:8123"
        let entity = values["ADVCTL_HA_ENTITY"] ?? values["MACCTL_HA_ENTITY"] ?? "media_player.appletv_2"
        values["ADVCTL_HA_TOKEN_FILE"] = values["ADVCTL_HA_TOKEN_FILE"] ?? values["MACCTL_HA_TOKEN_FILE"] ?? "\(NSHomeDirectory())/.config/karabiner/.homeassistant_token"
        if values["ADVCTL_HA_TOKEN"] == nil, let legacyToken = values["MACCTL_HA_TOKEN"] {
            values["ADVCTL_HA_TOKEN"] = legacyToken
        }
        if values["ADVCTL_HA_TOKEN"] == nil, let tokenFile = values["ADVCTL_HA_TOKEN_FILE"] {
            values["ADVCTL_HA_TOKEN"] = try? String(contentsOfFile: tokenFile).trimmingCharacters(in: .whitespacesAndNewlines)
        }

        config.expectedCommands = (values["ADVCTL_E2E_EXPECTED_COMMANDS"] ?? values["MACCTL_E2E_EXPECTED_COMMANDS"]).flatMap(Int.init)
        if let url = URL(string: urlString), let token = values["ADVCTL_HA_TOKEN"], !token.isEmpty {
            config.homeAssistant = HomeAssistantConfig(baseURL: url, token: token, entityID: entity)
        }
        return config
    }
}

private struct NowPlayingSnapshot: Equatable {
    var active: Bool
    var playing: Bool
    var muted: Bool
    var volumePercent: UInt8
    var progressPercent: UInt8
    var title: String
    var artist: String

    init(active: Bool, playing: Bool, muted: Bool, volumePercent: UInt8, progressPercent: UInt8, title: String, artist: String) {
        self.active = active
        self.playing = playing
        self.muted = muted
        self.volumePercent = volumePercent
        self.progressPercent = progressPercent
        self.title = title
        self.artist = artist
    }

    init(state: HomeAssistantState) {
        let attributes = state.attributes
        playing = state.state == "playing"
        muted = attributes.isVolumeMuted ?? ((attributes.volumeLevel ?? 0) <= 0.001)
        let hasMedia = !(attributes.mediaTitle ?? "").isEmpty || !(attributes.mediaArtist ?? "").isEmpty
        active = (state.state == "playing" || state.state == "paused") && hasMedia
        volumePercent = UInt8(min(100, max(0, Int(((attributes.volumeLevel ?? 0) * 100).rounded()))))
        if let position = attributes.mediaPosition, let duration = attributes.mediaDuration, duration > 0 {
            progressPercent = UInt8(min(100, max(0, Int((position / duration * 100).rounded()))))
        } else {
            progressPercent = 0
        }
        title = attributes.mediaTitle ?? (active ? "Playing" : "")
        artist = attributes.mediaArtist ?? attributes.mediaAlbumName ?? ""
    }
}

private final class JoystickSettings {
    private let defaults = UserDefaults.standard

    var swapAxes: Bool {
        get { defaults.bool(forKey: "advctl.swapAxes") }
        set { defaults.set(newValue, forKey: "advctl.swapAxes") }
    }

    var invertX: Bool {
        get { defaults.bool(forKey: "advctl.invertX") }
        set { defaults.set(newValue, forKey: "advctl.invertX") }
    }

    var invertY: Bool {
        get { defaults.bool(forKey: "advctl.invertY") }
        set { defaults.set(newValue, forKey: "advctl.invertY") }
    }

    var sensitivity: Int {
        get {
            let saved = defaults.integer(forKey: "advctl.sensitivityHalfStep")
            if saved >= 2 {
                return min(6, max(2, saved))
            }
            let legacySaved = defaults.integer(forKey: "advctl.sensitivity")
            return legacySaved == 0 ? 2 : min(6, max(2, legacySaved * 2))
        }
        set { defaults.set(min(6, max(2, newValue)), forKey: "advctl.sensitivityHalfStep") }
    }

    var flags: UInt8 {
        var value: UInt8 = 0
        if swapAxes { value |= 0x01 }
        if invertX { value |= 0x02 }
        if invertY { value |= 0x04 }
        return value
    }

    var sensitivityLabel: String {
        sensitivity.isMultiple(of: 2) ? "\(sensitivity / 2)x" : "\(sensitivity / 2).5x"
    }

    var knobMode: Int {
        get { defaults.object(forKey: "advctl.knobMode") as? Int ?? 0 }
        set { defaults.set(min(2, max(0, newValue)), forKey: "advctl.knobMode") }
    }

    var knobModeLabel: String {
        switch knobMode {
        case 1: return "HomePod"
        case 2: return "Disabled"
        default: return "System volume"
        }
    }

    var screenTimeoutSeconds: Int {
        get {
            let saved = defaults.integer(forKey: "advctl.screenTimeoutSeconds")
            return saved == 0 ? 30 : min(255, max(5, saved))
        }
        set { defaults.set(min(255, max(5, newValue)), forKey: "advctl.screenTimeoutSeconds") }
    }

    var powerSaveTimeoutMinutes: Int {
        get {
            let saved = defaults.integer(forKey: "advctl.powerSaveTimeoutMinutes")
            return saved == 0 ? 3 : min(255, max(1, saved))
        }
        set { defaults.set(min(255, max(1, newValue)), forKey: "advctl.powerSaveTimeoutMinutes") }
    }

    func applyHardware(flags: UInt8, sensitivity: UInt8, knobMode: UInt8) {
        swapAxes = (flags & 0x01) != 0
        invertX = (flags & 0x02) != 0
        invertY = (flags & 0x04) != 0
        self.sensitivity = Int(sensitivity)
        self.knobMode = Int(knobMode)
        synchronize()
    }

    func applyPower(screenTimeoutSeconds: UInt8, powerSaveTimeoutMinutes: UInt8) {
        self.screenTimeoutSeconds = Int(screenTimeoutSeconds)
        self.powerSaveTimeoutMinutes = Int(powerSaveTimeoutMinutes)
        synchronize()
    }

    func synchronize() {
        defaults.synchronize()
    }
}

private protocol ADVCtlBridgeDelegate: AnyObject {
    func bridgeDidUpdateConnection(deviceCount: Int)
    func bridgeDidReceive(command: MacCtlCommand)
    func bridgeDidReceiveKnobKey(_ keyCode: UInt8)
    func bridgeDidReceiveHardwareSettings(flags: UInt8, sensitivity: UInt8, knobMode: UInt8)
    func bridgeDidReceivePowerSettings(screenTimeoutSeconds: UInt8, powerSaveTimeoutMinutes: UInt8)
    func bridgeDidUpdateMessage(_ message: String)
}

private enum HIDEndpointKind {
    case control
    case keyboard
}

private final class HIDDeviceRegistration {
    let device: IOHIDDevice
    let reportBuffer: UnsafeMutablePointer<UInt8>
    let kind: HIDEndpointKind

    weak var bridge: ADVCtlBridge?

    init(device: IOHIDDevice, reportBuffer: UnsafeMutablePointer<UInt8>, kind: HIDEndpointKind, bridge: ADVCtlBridge) {
        self.device = device
        self.reportBuffer = reportBuffer
        self.kind = kind
        self.bridge = bridge
    }
}

private final class ADVCtlBridge {
    private let config: HelperConfig
    private let homeAssistant: HomeAssistantClient?
    private var handledCommands = 0
    private var hidManager: IOHIDManager?
    private var hidDevices: [HIDDeviceRegistration] = []
    private var pressedKnobKeys = Set<UInt8>()
    private var nowPlayingTask: Task<Void, Never>?
    private var lastNowPlayingTextSignature = ""
    weak var delegate: ADVCtlBridgeDelegate?

    init(config: HelperConfig) {
        self.config = config
        if let haConfig = config.homeAssistant {
            self.homeAssistant = HomeAssistantClient(config: haConfig)
        } else {
            self.homeAssistant = nil
        }
    }

    deinit {
        nowPlayingTask?.cancel()
        for registration in hidDevices {
            registration.reportBuffer.deallocate()
        }
    }

    func start() {
        log("ADVCtl starting")
        if homeAssistant == nil {
            log("ADVCtl HA disabled; token not configured")
        }

        let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        hidManager = manager

        let matching: [String: Any] = [
            kIOHIDVendorIDKey: advCtlVendorID,
            kIOHIDProductIDKey: advCtlProductID,
        ]
        IOHIDManagerSetDeviceMatching(manager, matching as CFDictionary)

        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        IOHIDManagerRegisterDeviceMatchingCallback(manager, { context, _, _, device in
            guard let context else { return }
            Unmanaged<ADVCtlBridge>.fromOpaque(context).takeUnretainedValue().attach(device)
        }, selfPtr)
        IOHIDManagerRegisterDeviceRemovalCallback(manager, { context, _, _, device in
            guard let context else { return }
            Unmanaged<ADVCtlBridge>.fromOpaque(context).takeUnretainedValue().detach(device)
        }, selfPtr)

        IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
        let status = IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        updateMessage(status == kIOReturnSuccess ? "Listening for ADVCtl" : "HID manager open failed: \(status)")

        if let devices = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> {
            devices.forEach(attach)
        }
        startNowPlayingSync()
    }

    func sendSettings(_ settings: JoystickSettings) {
        var payload: [UInt8] = [advCtlSetInputCommand, settings.flags, UInt8(settings.sensitivity), UInt8(settings.knobMode)]
        for registration in hidDevices {
            guard registration.kind == .control else {
                continue
            }
            let status = IOHIDDeviceSetReport(registration.device,
                                              kIOHIDReportTypeOutput,
                                              CFIndex(advCtlReportID),
                                              &payload,
                                              payload.count)
            if status == kIOReturnSuccess {
                updateMessage("Sent joystick settings to ADV")
            } else if status != kIOReturnUnsupported {
                updateMessage("Set joystick settings failed: \(status)")
            }
        }
    }

    func sendPowerSettings(_ settings: JoystickSettings) {
        sendControlPayload([advCtlSetPowerCommand,
                            UInt8(settings.screenTimeoutSeconds),
                            UInt8(settings.powerSaveTimeoutMinutes),
                            0],
                           successMessage: "Sent power settings to ADV")
    }

    func requestSettings() {
        sendControlPayload([advCtlRequestConfigCommand, 0, 0, 0], successMessage: "Requested hardware settings")
    }

    func sendTimeSync() {
        let elapsed = max(0, Date().timeIntervalSince1970 - advCtlTimeSyncEpoch)
        let minutes = UInt32(elapsed / 60) & 0x00FF_FFFF
        sendControlPayload([advCtlTimeSyncCommand,
                            UInt8(minutes & 0xFF),
                            UInt8((minutes >> 8) & 0xFF),
                            UInt8((minutes >> 16) & 0xFF)],
                           successMessage: "Synced ADV time")
    }

    func resetHardwareSettings() {
        sendControlPayload([advCtlResetConfigCommand, 0, 0, 0], successMessage: "Reset hardware settings")
    }

    func sendAudioTest(active: Bool) {
        sendControlPayload([advCtlAudioTestCommand, active ? 1 : 0, 0, 0],
                           successMessage: active ? "ADV recording test activated" : "ADV recording test stopped")
    }

    @MainActor private func sendNowPlaying(_ snapshot: NowPlayingSnapshot) {
        var flags: UInt8 = 0
        if snapshot.active { flags |= 0x01 }
        if snapshot.playing { flags |= 0x02 }
        if snapshot.muted { flags |= 0x04 }
        sendControlPayload([advCtlNowPlayingCommand, flags, snapshot.volumePercent, snapshot.progressPercent],
                           successMessage: "Synced now playing",
                           updateStatus: false)

        let signature = "\(snapshot.title)\u{1f}\(snapshot.artist)"
        guard snapshot.active, signature != lastNowPlayingTextSignature else {
            if !snapshot.active {
                lastNowPlayingTextSignature = ""
            }
            return
        }
        lastNowPlayingTextSignature = signature
        sendText(command: advCtlNowPlayingTitleCommand, text: snapshot.title)
        sendText(command: advCtlNowPlayingArtistCommand, text: snapshot.artist)
    }

    @MainActor private func sendText(command: UInt8, text: String) {
        let bytes = clippedUTF8Bytes(text, maxBytes: 32)
        let packetCount = max(1, (bytes.count + 1) / 2)
        for index in 0..<packetCount {
            let firstIndex = index * 2
            let first = firstIndex < bytes.count ? bytes[firstIndex] : 0
            let second = firstIndex + 1 < bytes.count ? bytes[firstIndex + 1] : 0
            sendControlPayload([command, UInt8(index), first, second],
                               successMessage: "Synced now playing text",
                               updateStatus: false)
        }
    }

    private func startNowPlayingSync() {
        guard let homeAssistant, nowPlayingTask == nil else {
            return
        }
        nowPlayingTask = Task { [weak self] in
            while !Task.isCancelled {
                do {
                    let state = try await homeAssistant.fetchState()
                    let snapshot = NowPlayingSnapshot(state: state)
                    await self?.sendNowPlaying(snapshot)
                } catch {
                    await self?.sendNowPlaying(NowPlayingSnapshot(active: false,
                                                                  playing: false,
                                                                  muted: false,
                                                                  volumePercent: 0,
                                                                  progressPercent: 0,
                                                                  title: "",
                                                                  artist: ""))
                }
                try? await Task.sleep(nanoseconds: 3_000_000_000)
            }
        }
    }

    private func sendControlPayload(_ bytes: [UInt8], successMessage: String, updateStatus: Bool = true) {
        var payload = bytes
        var sent = false
        for registration in hidDevices {
            guard registration.kind == .control else {
                continue
            }
            let status = IOHIDDeviceSetReport(registration.device,
                                              kIOHIDReportTypeOutput,
                                              CFIndex(advCtlReportID),
                                              &payload,
                                              payload.count)
            if status == kIOReturnSuccess {
                sent = true
            } else if status != kIOReturnUnsupported {
                updateMessage("Set control payload failed: \(status)")
            }
        }
        if updateStatus {
            updateMessage(sent ? successMessage : "ADV control endpoint not connected")
        }
    }

    private func attach(_ device: IOHIDDevice) {
        if hidDevices.contains(where: { CFEqual($0.device, device) }) {
            return
        }
        let product = hidProperty(device, kIOHIDProductKey as CFString, as: NSString.self) as String?
        if product != advCtlProductName {
            updateMessage("Ignoring stale HID product: \(product ?? "unknown")")
            return
        }
        let usagePage = hidProperty(device, kIOHIDPrimaryUsagePageKey as CFString, as: NSNumber.self)?.intValue ?? -1
        let usage = hidProperty(device, kIOHIDPrimaryUsageKey as CFString, as: NSNumber.self)?.intValue ?? -1
        let kind: HIDEndpointKind
        if usagePage == advCtlUsagePage && usage == advCtlUsage {
            kind = .control
        } else if usagePage == hidUsagePageGenericDesktop && usage == hidUsageKeyboard {
            kind = .keyboard
        } else {
            updateMessage("Ignoring non-control HID endpoint usagePage=\(usagePage) usage=\(usage)")
            return
        }

        let openStatus = IOHIDDeviceOpen(device, IOOptionBits(kIOHIDOptionsTypeNone))
        if openStatus != kIOReturnSuccess && openStatus != kIOReturnExclusiveAccess {
            updateMessage("HID device open failed: \(openStatus)")
            return
        }

        let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: advCtlReportLength)
        buffer.initialize(repeating: 0, count: advCtlReportLength)
        let registration = HIDDeviceRegistration(device: device, reportBuffer: buffer, kind: kind, bridge: self)
        hidDevices.append(registration)

        let registrationPtr = Unmanaged.passUnretained(registration).toOpaque()
        IOHIDDeviceRegisterInputReportCallback(device, buffer, advCtlReportLength, { context, result, _, _, reportID, report, reportLength in
            guard result == kIOReturnSuccess, let context else { return }
            let registration = Unmanaged<HIDDeviceRegistration>.fromOpaque(context).takeUnretainedValue()
            registration.bridge?.handleReport(registration: registration,
                                              reportID: reportID,
                                              report: report,
                                              length: reportLength)
        }, registrationPtr)
        IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)

        let maxInput = hidProperty(device, kIOHIDMaxInputReportSizeKey as CFString, as: NSNumber.self)?.intValue ?? -1
        updateConnection()
        updateMessage("Attached \(kind) usagePage=\(usagePage) usage=\(usage) maxInput=\(maxInput) openStatus=\(openStatus)")
        if kind == .control {
            sendTimeSync()
            requestSettings()
        }
    }

    private func detach(_ device: IOHIDDevice) {
        guard let index = hidDevices.firstIndex(where: { CFEqual($0.device, device) }) else {
            return
        }
        hidDevices[index].reportBuffer.deallocate()
        hidDevices.remove(at: index)
        updateConnection()
        updateMessage("Detached ADVCtl HID device")
    }

    private func handleReport(registration: HIDDeviceRegistration, reportID: UInt32, report: UnsafeMutablePointer<UInt8>, length: CFIndex) {
        switch registration.kind {
        case .control:
            handleControlReport(reportID: reportID, report: report, length: length)
        case .keyboard:
            handleKeyboardReport(reportID: reportID, report: report, length: length)
        }
    }

    private func handleControlReport(reportID: UInt32, report: UnsafeMutablePointer<UInt8>, length: CFIndex) {
        let payload: Data
        if length >= 4, report[0] == UInt8(advCtlReportID) {
            payload = Data(bytes: report.advanced(by: 1), count: min(4, Int(length) - 1))
        } else if reportID == advCtlReportID, length >= 3 {
            payload = Data(bytes: report, count: min(4, Int(length)))
        } else {
            return
        }

        guard let command = MacCtlCommand(payload: payload) else {
            if payload.count >= 4, payload[0] == advCtlConfigReport {
                DispatchQueue.main.async {
                    self.delegate?.bridgeDidReceiveHardwareSettings(flags: payload[1],
                                                                    sensitivity: payload[2],
                                                                    knobMode: payload[3])
                }
                updateMessage("Hardware settings received")
                return
            }
            if payload.count >= 4, payload[0] == advCtlPowerReport {
                DispatchQueue.main.async {
                    self.delegate?.bridgeDidReceivePowerSettings(screenTimeoutSeconds: payload[1],
                                                                 powerSaveTimeoutMinutes: payload[2])
                }
                updateMessage("Power settings received")
                return
            }
            updateMessage("Ignored unknown report: \(payload as NSData)")
            return
        }
        handle(command)
    }

    private func handleKeyboardReport(reportID: UInt32, report: UnsafeMutablePointer<UInt8>, length: CFIndex) {
        let startIndex: Int
        if length >= 9, report[0] == UInt8(advCtlKeyboardReportID) {
            startIndex = 3
        } else if reportID == advCtlKeyboardReportID, length >= 8 {
            startIndex = 2
        } else {
            return
        }

        let totalLength = Int(length)
        let keys = Set((startIndex..<min(totalLength, startIndex + 6)).map { report[$0] }.filter { $0 != 0 })
        for key in [hidKeyF13, hidKeyF14, hidKeyF15] where keys.contains(key) && !pressedKnobKeys.contains(key) {
            handleKnobKey(key)
        }
        pressedKnobKeys = keys.intersection([hidKeyF13, hidKeyF14, hidKeyF15])
    }

    func handleKnobKey(_ keyCode: UInt8) {
        DispatchQueue.main.async {
            self.delegate?.bridgeDidReceiveKnobKey(keyCode)
        }

        guard keyCode == hidKeyF13 || keyCode == hidKeyF14 else {
            updateMessage("Received knob button F15")
            markCommandHandled()
            return
        }

        let command: MacCtlCommand = keyCode == hidKeyF13 ? .volumeDelta(1) : .volumeDelta(-1)
        Task {
            do {
                if let homeAssistant {
                    try await homeAssistant.handle(command)
                    self.updateMessage("Handled \(keyCode == hidKeyF13 ? "F13" : "F14") as \(command)")
                } else {
                    self.updateMessage("Received knob key; set ADVCTL_HA_TOKEN to enable HA control")
                }
                self.markCommandHandled()
            } catch {
                self.updateMessage("HA request failed: \(error.localizedDescription)")
            }
        }
    }

    private func handle(_ command: MacCtlCommand) {
        DispatchQueue.main.async {
            self.delegate?.bridgeDidReceive(command: command)
        }

        Task {
            do {
                if let homeAssistant {
                    try await homeAssistant.handle(command)
                    self.updateMessage("Handled \(command)")
                } else {
                    self.updateMessage("Received \(command); set ADVCTL_HA_TOKEN to enable HA control")
                }
                self.markCommandHandled()
            } catch {
                self.updateMessage("HA request failed: \(error.localizedDescription)")
            }
        }
    }

    private func markCommandHandled() {
        handledCommands += 1
        if let expectedCommands = config.expectedCommands, handledCommands >= expectedCommands {
            log("E2E complete; handled \(handledCommands) commands")
            fflush(stdout)
            exit(0)
        }
    }

    private func updateConnection() {
        let count = hidDevices.count
        DispatchQueue.main.async {
            self.delegate?.bridgeDidUpdateConnection(deviceCount: count)
        }
    }

    private func updateMessage(_ message: String) {
        log(message)
        DispatchQueue.main.async {
            self.delegate?.bridgeDidUpdateMessage(message)
        }
    }
}

private final class WaveformView: NSView {
    var isActive = false {
        didSet {
            if isActive {
                phase = 0
                startTimer()
            } else {
                stopTimer()
            }
            needsDisplay = true
        }
    }

    private var phase: Double = 0
    private var timer: Timer?

    deinit {
        stopTimer()
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.cornerRadius = 8
        layer?.backgroundColor = NSColor.controlBackgroundColor.withAlphaComponent(0.72).cgColor
    }

    required init?(coder: NSCoder) {
        nil
    }

    private func startTimer() {
        guard timer == nil else { return }
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.phase += 0.18
            self.needsDisplay = true
        }
    }

    private func stopTimer() {
        timer?.invalidate()
        timer = nil
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)

        let rect = bounds.insetBy(dx: 16, dy: 14)
        guard rect.width > 1, rect.height > 1 else { return }

        NSColor.separatorColor.withAlphaComponent(0.35).setStroke()
        let mid = rect.midY
        let centerLine = NSBezierPath()
        centerLine.move(to: NSPoint(x: rect.minX, y: mid))
        centerLine.line(to: NSPoint(x: rect.maxX, y: mid))
        centerLine.lineWidth = 1
        centerLine.stroke()

        let path = NSBezierPath()
        let samples = max(64, Int(rect.width / 3))
        for index in 0...samples {
            let x = rect.minX + CGFloat(index) / CGFloat(samples) * rect.width
            let t = Double(index) / Double(samples)
            let envelope = isActive ? (0.35 + 0.45 * abs(sin(phase * 0.7 + t * 5.0))) : 0.05
            let wave = sin(t * 38.0 + phase) * 0.55 + sin(t * 77.0 + phase * 1.6) * 0.25
            let y = mid + CGFloat(wave * envelope) * rect.height * 0.46
            if index == 0 {
                path.move(to: NSPoint(x: x, y: y))
            } else {
                path.line(to: NSPoint(x: x, y: y))
            }
        }
        NSColor.systemBlue.setStroke()
        path.lineWidth = 2.2
        path.stroke()

        if !isActive {
            let label = "Idle"
            let attrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: 12, weight: .medium),
                .foregroundColor: NSColor.secondaryLabelColor,
            ]
            let size = label.size(withAttributes: attrs)
            label.draw(at: NSPoint(x: rect.midX - size.width / 2, y: rect.midY - size.height / 2), withAttributes: attrs)
        }
    }
}

private final class SettingsWindowController: NSWindowController {
    private let settings: JoystickSettings
    var onSettingsChanged: (() -> Void)?
    var onPowerSettingsChanged: (() -> Void)?
    var onAudioTestChanged: ((Bool) -> Void)?
    var onRefreshSettings: (() -> Void)?
    var onResetHardwareSettings: (() -> Void)?

    private let connectionValueLabel = NSTextField(labelWithString: "Disconnected")
    private let knobValueLabel = NSTextField(labelWithString: "No input")
    private let messageValueLabel = NSTextField(labelWithString: "Starting")
    private let audioStateLabel = NSTextField(labelWithString: "Inactive")
    private let microphoneStatusLabel = NSTextField(labelWithString: "Virtual microphone driver not installed")
    private let swapAxesButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let invertXButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let invertYButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let sensitivitySlider = NSSlider()
    private let sensitivityValueLabel = NSTextField(labelWithString: "1x")
    private let knobModePopup = NSPopUpButton()
    private let resetButton = NSButton(title: "Reset Hardware Defaults", target: nil, action: nil)
    private let refreshButton = NSButton(title: "Refresh from ADV", target: nil, action: nil)
    private let waveformView = WaveformView(frame: .zero)
    private let recordButton = NSButton(title: "Activate Recording Test", target: nil, action: nil)
    private let screenTimeoutSlider = NSSlider()
    private let screenTimeoutValueLabel = NSTextField(labelWithString: "30s")
    private let powerSaveSlider = NSSlider()
    private let powerSaveValueLabel = NSTextField(labelWithString: "3m")
    private let tabView = NSTabView()
    private var sidebarButtons: [NSButton] = []
    private var audioTestActive = false

    init(settings: JoystickSettings) {
        self.settings = settings
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 820, height: 560),
                              styleMask: [.titled, .closable, .miniaturizable, .fullSizeContentView],
                              backing: .buffered,
                              defer: false)
        window.title = "ADVCtl"
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        window.isMovableByWindowBackground = true
        super.init(window: window)
        buildContent()
        refresh()
    }

    required init?(coder: NSCoder) {
        nil
    }

    func updateStatus(connected: Bool, knobStatus: String) {
        connectionValueLabel.stringValue = connected ? "Connected" : "Disconnected"
        connectionValueLabel.textColor = connected ? .systemGreen : .secondaryLabelColor
        knobValueLabel.stringValue = knobStatus.replacingOccurrences(of: "Knob: ", with: "")
    }

    func updateMessage(_ message: String) {
        messageValueLabel.stringValue = message
        microphoneStatusLabel.stringValue = "Virtual microphone requires the ADVCtl audio driver"
    }

    private func buildContent() {
        guard let contentView = window?.contentView else { return }

        let root = NSStackView()
        root.orientation = .horizontal
        root.spacing = 0
        root.translatesAutoresizingMaskIntoConstraints = false

        let sidebarEffect = NSVisualEffectView()
        sidebarEffect.material = .sidebar
        sidebarEffect.blendingMode = .withinWindow
        sidebarEffect.state = .active
        sidebarEffect.translatesAutoresizingMaskIntoConstraints = false

        let sidebar = NSStackView()
        sidebar.orientation = .vertical
        sidebar.alignment = .leading
        sidebar.spacing = 4
        sidebar.edgeInsets = NSEdgeInsets(top: 58, left: 12, bottom: 18, right: 12)
        sidebar.translatesAutoresizingMaskIntoConstraints = false
        sidebarEffect.addSubview(sidebar)

        let brand = NSTextField(labelWithString: "ADVCtl")
        brand.font = .systemFont(ofSize: 20, weight: .semibold)
        brand.translatesAutoresizingMaskIntoConstraints = false
        sidebar.addArrangedSubview(brand)
        sidebar.setCustomSpacing(14, after: brand)
        for (index, title) in ["Status", "Pointer", "Knob", "Power", "Audio"].enumerated() {
            let button = sidebarButton(title, selected: index == 0)
            sidebarButtons.append(button)
            sidebar.addArrangedSubview(button)
        }

        let contentEffect = NSVisualEffectView()
        contentEffect.material = .contentBackground
        contentEffect.blendingMode = .withinWindow
        contentEffect.state = .active
        contentEffect.translatesAutoresizingMaskIntoConstraints = false

        tabView.tabViewType = .noTabsNoBorder
        tabView.translatesAutoresizingMaskIntoConstraints = false
        tabView.addTabViewItem(tabItem("Status", groups: [
            group("Connection", rows: [
                valueRow("ADV", connectionValueLabel),
                valueRow("Knob", knobValueLabel),
                valueRow("Last message", messageValueLabel),
                controlRow("Hardware config", refreshButton),
            ]),
        ]))
        tabView.addTabViewItem(tabItem("Pointer", groups: [
            group("Pointer", rows: [
                controlRow("Swap X/Y axes", swapAxesButton),
                controlRow("Invert horizontal", invertXButton),
                controlRow("Invert vertical", invertYButton),
                sensitivityRow(),
            ]),
        ]))
        tabView.addTabViewItem(tabItem("Knob", groups: [
            group("Knob", rows: [
                controlRow("Rotation", knobModePopup),
                valueRow("Press", NSTextField(labelWithString: "Mute in system mode, F15 in HomePod mode")),
                controlRow("Defaults", resetButton),
            ]),
        ]))
        tabView.addTabViewItem(tabItem("Power", groups: [
            group("Power Save", rows: [
                screenTimeoutRow(),
                powerSaveTimeoutRow(),
            ]),
        ]))
        tabView.addTabViewItem(tabItem("Audio", groups: [
            group("Audio", rows: [
                valueRow("Recording test", audioStateLabel),
                valueRow("System microphone", microphoneStatusLabel),
                waveformRow(),
            ]),
        ]))
        contentEffect.addSubview(tabView)
        root.addArrangedSubview(sidebarEffect)
        root.addArrangedSubview(contentEffect)
        contentView.addSubview(root)

        swapAxesButton.target = self
        swapAxesButton.action = #selector(settingsChanged)
        invertXButton.target = self
        invertXButton.action = #selector(settingsChanged)
        invertYButton.target = self
        invertYButton.action = #selector(settingsChanged)
        knobModePopup.addItems(withTitles: ["System volume", "HomePod volume", "Disabled"])
        knobModePopup.target = self
        knobModePopup.action = #selector(settingsChanged)
        resetButton.target = self
        resetButton.action = #selector(resetHardwareDefaults)
        refreshButton.target = self
        refreshButton.action = #selector(refreshHardwareSettings)
        sensitivitySlider.minValue = 2
        sensitivitySlider.maxValue = 6
        sensitivitySlider.numberOfTickMarks = 5
        sensitivitySlider.allowsTickMarkValuesOnly = true
        sensitivitySlider.target = self
        sensitivitySlider.action = #selector(settingsChanged)
        screenTimeoutSlider.minValue = 5
        screenTimeoutSlider.maxValue = 120
        screenTimeoutSlider.numberOfTickMarks = 24
        screenTimeoutSlider.allowsTickMarkValuesOnly = true
        screenTimeoutSlider.target = self
        screenTimeoutSlider.action = #selector(powerSettingsChanged)
        powerSaveSlider.minValue = 1
        powerSaveSlider.maxValue = 30
        powerSaveSlider.numberOfTickMarks = 30
        powerSaveSlider.allowsTickMarkValuesOnly = true
        powerSaveSlider.target = self
        powerSaveSlider.action = #selector(powerSettingsChanged)
        recordButton.target = self
        recordButton.action = #selector(toggleAudioTest)

        NSLayoutConstraint.activate([
            root.leadingAnchor.constraint(equalTo: contentView.leadingAnchor),
            root.trailingAnchor.constraint(equalTo: contentView.trailingAnchor),
            root.topAnchor.constraint(equalTo: contentView.topAnchor),
            root.bottomAnchor.constraint(equalTo: contentView.bottomAnchor),
            sidebarEffect.widthAnchor.constraint(equalToConstant: 190),
            sidebar.leadingAnchor.constraint(equalTo: sidebarEffect.leadingAnchor),
            sidebar.trailingAnchor.constraint(equalTo: sidebarEffect.trailingAnchor),
            sidebar.topAnchor.constraint(equalTo: sidebarEffect.topAnchor),
            sidebar.bottomAnchor.constraint(lessThanOrEqualTo: sidebarEffect.bottomAnchor),
            tabView.leadingAnchor.constraint(equalTo: contentEffect.leadingAnchor),
            tabView.trailingAnchor.constraint(equalTo: contentEffect.trailingAnchor),
            tabView.topAnchor.constraint(equalTo: contentEffect.topAnchor),
            tabView.bottomAnchor.constraint(equalTo: contentEffect.bottomAnchor),
        ])
    }

    private func sidebarButton(_ title: String, selected: Bool) -> NSButton {
        let button = NSButton(title: title, target: self, action: #selector(selectSettingsTab(_:)))
        button.bezelStyle = .regularSquare
        button.isBordered = false
        button.alignment = .left
        button.font = .systemFont(ofSize: 13, weight: selected ? .semibold : .regular)
        button.contentTintColor = selected ? .labelColor : .secondaryLabelColor
        button.setButtonType(.momentaryPushIn)
        button.identifier = NSUserInterfaceItemIdentifier(title)
        button.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            button.widthAnchor.constraint(equalToConstant: 166),
            button.heightAnchor.constraint(equalToConstant: 28),
        ])
        return button
    }

    private func tabItem(_ title: String, groups: [NSView]) -> NSTabViewItem {
        let item = NSTabViewItem(identifier: title)
        item.label = title
        item.view = tabPage(title: title, groups: groups)
        return item
    }

    private func tabPage(title: String, groups: [NSView]) -> NSView {
        let page = NSView()
        page.translatesAutoresizingMaskIntoConstraints = false

        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 16
        stack.edgeInsets = NSEdgeInsets(top: 54, left: 34, bottom: 28, right: 34)
        stack.translatesAutoresizingMaskIntoConstraints = false

        let titleLabel = NSTextField(labelWithString: title)
        titleLabel.font = .systemFont(ofSize: 28, weight: .bold)
        stack.addArrangedSubview(titleLabel)
        groups.forEach { stack.addArrangedSubview($0) }

        page.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: page.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: page.trailingAnchor),
            stack.topAnchor.constraint(equalTo: page.topAnchor),
            stack.bottomAnchor.constraint(lessThanOrEqualTo: page.bottomAnchor),
        ])
        return page
    }

    @objc private func selectSettingsTab(_ sender: NSButton) {
        guard let title = sender.identifier?.rawValue else { return }
        tabView.selectTabViewItem(withIdentifier: title)
        for button in sidebarButtons {
            let selected = button === sender
            button.font = .systemFont(ofSize: 13, weight: selected ? .semibold : .regular)
            button.contentTintColor = selected ? .labelColor : .secondaryLabelColor
        }
    }

    private func group(_ title: String, rows: [NSView]) -> NSView {
        let titleLabel = NSTextField(labelWithString: title)
        titleLabel.font = .systemFont(ofSize: 13, weight: .semibold)
        titleLabel.textColor = .secondaryLabelColor

        let cardStack = NSStackView()
        cardStack.orientation = .vertical
        cardStack.alignment = .leading
        cardStack.spacing = 0
        cardStack.translatesAutoresizingMaskIntoConstraints = false

        for (index, row) in rows.enumerated() {
            cardStack.addArrangedSubview(row)
            if index < rows.count - 1 {
                cardStack.addArrangedSubview(separator())
            }
        }

        let card = NSView()
        card.wantsLayer = true
        card.layer?.cornerRadius = 8
        card.layer?.backgroundColor = NSColor.controlBackgroundColor.withAlphaComponent(0.68).cgColor
        card.translatesAutoresizingMaskIntoConstraints = false
        card.addSubview(cardStack)

        let wrapper = NSStackView()
        wrapper.orientation = .vertical
        wrapper.alignment = .leading
        wrapper.spacing = 6
        wrapper.translatesAutoresizingMaskIntoConstraints = false
        wrapper.addArrangedSubview(titleLabel)
        wrapper.addArrangedSubview(card)

        NSLayoutConstraint.activate([
            wrapper.widthAnchor.constraint(greaterThanOrEqualToConstant: 520),
            card.widthAnchor.constraint(equalTo: wrapper.widthAnchor),
            cardStack.leadingAnchor.constraint(equalTo: card.leadingAnchor),
            cardStack.trailingAnchor.constraint(equalTo: card.trailingAnchor),
            cardStack.topAnchor.constraint(equalTo: card.topAnchor),
            cardStack.bottomAnchor.constraint(equalTo: card.bottomAnchor),
        ])
        return wrapper
    }

    private func separator() -> NSView {
        let line = NSBox()
        line.boxType = .separator
        line.translatesAutoresizingMaskIntoConstraints = false
        line.heightAnchor.constraint(equalToConstant: 1).isActive = true
        return line
    }

    private func valueRow(_ title: String, _ value: NSTextField) -> NSView {
        value.font = .systemFont(ofSize: 13)
        value.textColor = .secondaryLabelColor
        value.lineBreakMode = .byTruncatingMiddle
        return row(title, trailing: value)
    }

    private func controlRow(_ title: String, _ control: NSControl) -> NSView {
        return row(title, trailing: control)
    }

    private func sensitivityRow() -> NSView {
        let stack = NSStackView()
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = 10
        stack.addArrangedSubview(sensitivitySlider)
        stack.addArrangedSubview(sensitivityValueLabel)
        sensitivitySlider.widthAnchor.constraint(equalToConstant: 170).isActive = true
        sensitivityValueLabel.widthAnchor.constraint(equalToConstant: 34).isActive = true
        return row("Pointer speed", trailing: stack)
    }

    private func screenTimeoutRow() -> NSView {
        let stack = NSStackView()
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = 10
        stack.addArrangedSubview(screenTimeoutSlider)
        stack.addArrangedSubview(screenTimeoutValueLabel)
        screenTimeoutSlider.widthAnchor.constraint(equalToConstant: 170).isActive = true
        screenTimeoutValueLabel.widthAnchor.constraint(equalToConstant: 44).isActive = true
        return row("Turn display off", trailing: stack)
    }

    private func powerSaveTimeoutRow() -> NSView {
        let stack = NSStackView()
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = 10
        stack.addArrangedSubview(powerSaveSlider)
        stack.addArrangedSubview(powerSaveValueLabel)
        powerSaveSlider.widthAnchor.constraint(equalToConstant: 170).isActive = true
        powerSaveValueLabel.widthAnchor.constraint(equalToConstant: 44).isActive = true
        return row("Power save", trailing: stack)
    }

    private func waveformRow() -> NSView {
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        stack.addArrangedSubview(waveformView)
        stack.addArrangedSubview(recordButton)
        waveformView.widthAnchor.constraint(equalToConstant: 330).isActive = true
        waveformView.heightAnchor.constraint(equalToConstant: 118).isActive = true
        return row("ADV microphone", trailing: stack, minHeight: 162)
    }

    private func row(_ title: String, trailing: NSView, minHeight: CGFloat = 44) -> NSView {
        let label = NSTextField(labelWithString: title)
        label.font = .systemFont(ofSize: 13)
        label.textColor = .labelColor
        label.translatesAutoresizingMaskIntoConstraints = false
        trailing.translatesAutoresizingMaskIntoConstraints = false

        let container = NSView()
        container.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(label)
        container.addSubview(trailing)
        NSLayoutConstraint.activate([
            container.heightAnchor.constraint(greaterThanOrEqualToConstant: minHeight),
            label.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 16),
            label.topAnchor.constraint(equalTo: container.topAnchor, constant: 13),
            trailing.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -16),
            trailing.centerYAnchor.constraint(equalTo: container.centerYAnchor),
            trailing.leadingAnchor.constraint(greaterThanOrEqualTo: label.trailingAnchor, constant: 18),
        ])
        return container
    }

    func refresh() {
        swapAxesButton.state = settings.swapAxes ? .on : .off
        invertXButton.state = settings.invertX ? .on : .off
        invertYButton.state = settings.invertY ? .on : .off
        sensitivitySlider.integerValue = settings.sensitivity
        sensitivityValueLabel.stringValue = settings.sensitivityLabel
        knobModePopup.selectItem(at: settings.knobMode)
        screenTimeoutSlider.integerValue = settings.screenTimeoutSeconds
        screenTimeoutValueLabel.stringValue = "\(settings.screenTimeoutSeconds)s"
        powerSaveSlider.integerValue = settings.powerSaveTimeoutMinutes
        powerSaveValueLabel.stringValue = "\(settings.powerSaveTimeoutMinutes)m"
        audioStateLabel.stringValue = audioTestActive ? "Active" : "Inactive"
        audioStateLabel.textColor = audioTestActive ? .systemBlue : .secondaryLabelColor
        recordButton.title = audioTestActive ? "Stop Recording Test" : "Activate Recording Test"
        waveformView.isActive = audioTestActive
    }

    @objc private func settingsChanged() {
        settings.swapAxes = swapAxesButton.state == .on
        settings.invertX = invertXButton.state == .on
        settings.invertY = invertYButton.state == .on
        settings.sensitivity = Int(round(sensitivitySlider.doubleValue))
        settings.knobMode = knobModePopup.indexOfSelectedItem
        settings.synchronize()
        refresh()
        onSettingsChanged?()
    }

    @objc private func powerSettingsChanged() {
        settings.screenTimeoutSeconds = Int(round(screenTimeoutSlider.doubleValue))
        settings.powerSaveTimeoutMinutes = Int(round(powerSaveSlider.doubleValue))
        settings.synchronize()
        refresh()
        onPowerSettingsChanged?()
    }

    func applyHardwareSettings(flags: UInt8, sensitivity: UInt8, knobMode: UInt8) {
        settings.applyHardware(flags: flags, sensitivity: sensitivity, knobMode: knobMode)
        refresh()
    }

    func applyPowerSettings(screenTimeoutSeconds: UInt8, powerSaveTimeoutMinutes: UInt8) {
        settings.applyPower(screenTimeoutSeconds: screenTimeoutSeconds, powerSaveTimeoutMinutes: powerSaveTimeoutMinutes)
        refresh()
    }

    @objc private func refreshHardwareSettings() {
        onRefreshSettings?()
    }

    @objc private func resetHardwareDefaults() {
        onResetHardwareSettings?()
    }

    @objc private func toggleAudioTest() {
        audioTestActive.toggle()
        refresh()
        onAudioTestChanged?(audioTestActive)
    }
}

private final class ADVCtlAppDelegate: NSObject, NSApplicationDelegate, ADVCtlBridgeDelegate {
    private let settings = JoystickSettings()
    private lazy var bridge = ADVCtlBridge(config: HelperConfig.load())
    private lazy var settingsWindow = SettingsWindowController(settings: settings)
    private var statusItem: NSStatusItem?
    private let connectionMenuItem = NSMenuItem(title: "Disconnected", action: nil, keyEquivalent: "")
    private let knobMenuItem = NSMenuItem(title: "Knob: none", action: nil, keyEquivalent: "")
    private let messageMenuItem = NSMenuItem(title: "Starting", action: nil, keyEquivalent: "")
    private var connected = false
    private var knobStatus = "Knob: none"
    private var globalKeyMonitor: Any?
    private var localKeyMonitor: Any?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        settingsWindow.onSettingsChanged = { [weak self] in
            guard let self else { return }
            self.bridge.sendSettings(self.settings)
        }
        settingsWindow.onPowerSettingsChanged = { [weak self] in
            guard let self else { return }
            self.bridge.sendPowerSettings(self.settings)
        }
        settingsWindow.onAudioTestChanged = { [weak self] active in
            self?.bridge.sendAudioTest(active: active)
        }
        settingsWindow.onRefreshSettings = { [weak self] in
            self?.bridge.requestSettings()
        }
        settingsWindow.onResetHardwareSettings = { [weak self] in
            self?.bridge.resetHardwareSettings()
        }
        buildStatusItem()
        bridge.delegate = self
        installKeyMonitors()
        bridge.start()
    }

    func bridgeDidUpdateConnection(deviceCount: Int) {
        connected = deviceCount > 0
        connectionMenuItem.title = connected ? "Connected (\(deviceCount))" : "Disconnected"
        settingsWindow.updateStatus(connected: connected, knobStatus: knobStatus)
    }

    func bridgeDidReceive(command: MacCtlCommand) {
        switch command {
        case .volumeDelta(let delta):
            knobStatus = delta > 0 ? "Knob: volume +\(delta)" : "Knob: volume \(delta)"
        case .playPause:
            knobStatus = "Knob: press"
        }
        knobMenuItem.title = knobStatus
        settingsWindow.updateStatus(connected: connected, knobStatus: knobStatus)
    }

    func bridgeDidReceiveKnobKey(_ keyCode: UInt8) {
        switch keyCode {
        case hidKeyF13:
            knobStatus = "Knob: F13 -> HomePod up"
        case hidKeyF14:
            knobStatus = "Knob: F14 -> HomePod down"
        case hidKeyF15:
            knobStatus = "Knob: F15 button"
        default:
            knobStatus = "Knob: key \(keyCode)"
        }
        knobMenuItem.title = knobStatus
        settingsWindow.updateStatus(connected: connected, knobStatus: knobStatus)
    }

    func bridgeDidReceiveHardwareSettings(flags: UInt8, sensitivity: UInt8, knobMode: UInt8) {
        settings.applyHardware(flags: flags, sensitivity: sensitivity, knobMode: knobMode)
        settingsWindow.applyHardwareSettings(flags: flags, sensitivity: sensitivity, knobMode: knobMode)
        messageMenuItem.title = "Hardware settings received"
    }

    func bridgeDidReceivePowerSettings(screenTimeoutSeconds: UInt8, powerSaveTimeoutMinutes: UInt8) {
        settings.applyPower(screenTimeoutSeconds: screenTimeoutSeconds, powerSaveTimeoutMinutes: powerSaveTimeoutMinutes)
        settingsWindow.applyPowerSettings(screenTimeoutSeconds: screenTimeoutSeconds,
                                          powerSaveTimeoutMinutes: powerSaveTimeoutMinutes)
        messageMenuItem.title = "Power settings received"
    }

    func bridgeDidUpdateMessage(_ message: String) {
        messageMenuItem.title = message
        settingsWindow.updateMessage(message)
    }

    private func buildStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.image = nil
        item.button?.title = "✦"
        item.button?.imagePosition = .noImage
        item.button?.font = .systemFont(ofSize: 15, weight: .semibold)

        let menu = NSMenu()
        menu.addItem(connectionMenuItem)
        menu.addItem(knobMenuItem)
        menu.addItem(messageMenuItem)
        menu.addItem(.separator())

        let settingsItem = NSMenuItem(title: "Settings...", action: #selector(openSettings), keyEquivalent: ",")
        settingsItem.target = self
        menu.addItem(settingsItem)

        let quitItem = NSMenuItem(title: "Quit ADVCtl", action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)

        item.menu = menu
        statusItem = item
    }

    private func installKeyMonitors() {
        globalKeyMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.keyDown, .systemDefined]) { [weak self] event in
            self?.handleKnobEvent(event)
        }
        localKeyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .systemDefined]) { [weak self] event in
            self?.handleKnobEvent(event)
            return event
        }
    }

    private func handleKnobEvent(_ event: NSEvent) {
        if event.type == .systemDefined {
            handleSystemDefinedKnobEvent(event)
            return
        }

        switch event.keyCode {
        case macKeyF13:
            bridge.handleKnobKey(hidKeyF13)
        case macKeyF14:
            bridge.handleKnobKey(hidKeyF14)
        case macKeyF15:
            bridge.handleKnobKey(hidKeyF15)
        default:
            break
        }
    }

    private func handleSystemDefinedKnobEvent(_ event: NSEvent) {
        guard event.subtype.rawValue == systemDefinedKeyDownSubtype else {
            return
        }

        let keyType = Int((event.data1 & 0xFFFF0000) >> 16)
        let keyState = Int((event.data1 & 0x0000FF00) >> 8)
        guard keyState == systemDefinedKeyStateDown else {
            return
        }

        switch keyType {
        case nxKeyTypeBrightnessDown:
            log("Mapped system-defined brightness down to F14")
            bridge.handleKnobKey(hidKeyF14)
        case nxKeyTypeBrightnessUp:
            log("Mapped system-defined brightness up to F15")
            bridge.handleKnobKey(hidKeyF15)
        default:
            break
        }
    }

    @objc private func openSettings() {
        bridge.requestSettings()
        settingsWindow.refresh()
        settingsWindow.updateStatus(connected: connected, knobStatus: knobStatus)
        settingsWindow.showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }
}

let app = NSApplication.shared
private let delegate = ADVCtlAppDelegate()
app.delegate = delegate
app.run()
