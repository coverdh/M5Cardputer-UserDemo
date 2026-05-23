import AppKit
import ApplicationServices
import AVFoundation
import CoreAudio
import Darwin
import Foundation
import IOKit.hid
import MacCtlCore

private let advCtlVendorID = 0x16C0
private let advCtlProductID = 0x05DF
private let advCtlProductName = "ADVCtl"
private let advCtlBundleIdentifier = "dev.cardputer.advctl"
private let advCtlInstalledAppURL = URL(fileURLWithPath: "/Applications/ADVCtl.app", isDirectory: true)
private let advCtlLaunchAgentLabel = "dev.cardputer.advctl"
private let advCtlAudioDriverURL = URL(fileURLWithPath: "/Library/Audio/Plug-Ins/HAL/ADVCtlAudio.driver", isDirectory: true)
private let advCtlUsagePage = 0xFF00
private let advCtlUsage = 0x01
private let hidUsagePageGenericDesktop = 0x01
private let hidUsageKeyboard = 0x06
private let advCtlReportID: UInt32 = 4
private let advCtlReportLength = 64
private let advCtlPayloadLength = 63
private let advCtlRequestConfigCommand: UInt8 = 0x80
private let advCtlSetInputCommand: UInt8 = 0x81
private let advCtlAudioTestCommand: UInt8 = 0x82
private let advCtlResetConfigCommand: UInt8 = 0x83
private let advCtlTimeSyncCommand: UInt8 = 0x84
private let advCtlSetPowerCommand: UInt8 = 0x86
private let advCtlNowPlayingCommand: UInt8 = 0x87
private let advCtlNowPlayingTitleCommand: UInt8 = 0x88
private let advCtlNowPlayingArtistCommand: UInt8 = 0x89
private let advCtlAudioFrameReport: UInt8 = 0xA0
private let advCtlConfigReport: UInt8 = 0x90
private let advCtlPowerReport: UInt8 = 0x91
private let advCtlKeyboardReportID: UInt32 = 1
private let advCtlKeyboardLedAudioPrefix: UInt8 = 0x1F
private let advCtlKeyboardLedAudioStop: UInt8 = 0x10
private let advCtlKeyboardLedAudioStart: UInt8 = 0x11
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

func log(_ message: String) {
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

private func requestHIDListenAccessIfNeeded() -> Bool {
    let access = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)
    if access == kIOHIDAccessTypeGranted {
        return true
    }

    let requested = IOHIDRequestAccess(kIOHIDRequestTypeListenEvent)
    log("Requested HID listen access: current=\(access.rawValue) requested=\(requested)")
    return IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted
}

private func openInputMonitoringPreferences() {
    guard let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent") else {
        return
    }
    NSWorkspace.shared.open(url)
}

private func openAccessibilityPreferences() {
    guard let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility") else {
        return
    }
    NSWorkspace.shared.open(url)
}

private func openMicrophonePreferences() {
    guard let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone") else {
        return
    }
    NSWorkspace.shared.open(url)
}

private func openBluetoothPreferences() {
    guard let url = URL(string: "x-apple.systempreferences:com.apple.BluetoothSettings") else {
        return
    }
    NSWorkspace.shared.open(url)
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

private protocol ADVCtlBridgeDelegate: AnyObject {
    func bridgeDidUpdateConnection(deviceCount: Int)
    func bridgeDidReceive(command: MacCtlCommand)
    func bridgeDidReceiveKnobKey(_ keyCode: UInt8)
    func bridgeDidReceiveHardwareSettings(flags: UInt8, sensitivity: UInt8, knobMode: UInt8)
    func bridgeDidReceivePowerSettings(screenTimeoutSeconds: UInt8, powerSaveTimeoutMinutes: UInt8)
    func bridgeDidUpdateAudioStatus(_ message: String, active: Bool)
    func bridgeDidUpdateMessage(_ message: String)
}

private enum HIDEndpointKind {
    case control
    case composite

    var supportsControl: Bool {
        self == .control || self == .composite
    }

    var supportsKeyboard: Bool {
        self == .composite
    }
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
    private let audioSink = ADVCtlAudioRingSink()
    private var handledCommands = 0
    private var hidManager: IOHIDManager?
    private var hidDevices: [HIDDeviceRegistration] = []
    private var pressedKnobKeys = Set<UInt8>()
    private var nowPlayingTask: Task<Void, Never>?
    private var audioDemandTimer: Timer?
    private var manualAudioTestActive = false
    private var driverAudioDemandActive = false
    private var advAudioBridgeActive = false
    private var audioFrameCount = 0
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
        audioDemandTimer?.invalidate()
        sendAudioBridgeActive(false, force: true, updateStatus: false)
        audioSink.stop()
        for registration in hidDevices {
            registration.reportBuffer.deallocate()
        }
    }

    func start() {
        log("ADVCtl starting")
        if homeAssistant == nil {
            log("ADVCtl HA disabled; token not configured")
        }
        let hidAccessGranted = requestHIDListenAccessIfNeeded()

        let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        hidManager = manager

        let vendorProductMatching: [String: Any] = [
            kIOHIDVendorIDKey: advCtlVendorID,
            kIOHIDProductIDKey: advCtlProductID,
        ]
        let productKeyboardMatching: [String: Any] = [
            kIOHIDProductKey: advCtlProductName,
            kIOHIDDeviceUsagePageKey: hidUsagePageGenericDesktop,
            kIOHIDDeviceUsageKey: hidUsageKeyboard,
        ]
        let productControlMatching: [String: Any] = [
            kIOHIDProductKey: advCtlProductName,
            kIOHIDDeviceUsagePageKey: advCtlUsagePage,
            kIOHIDDeviceUsageKey: advCtlUsage,
        ]
        IOHIDManagerSetDeviceMatchingMultiple(manager, [vendorProductMatching,
                                                        productKeyboardMatching,
                                                        productControlMatching] as CFArray)

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
        if status == kIOReturnSuccess {
            updateMessage("Listening for ADVCtl")
        } else if status == kIOReturnNotPermitted || !hidAccessGranted {
            updateMessage("Grant ADVCtl Input Monitoring, then reopen ADVCtl")
            openInputMonitoringPreferences()
            return
        } else {
            updateMessage("HID manager open failed: \(status)")
        }

        if let devices = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> {
            devices.forEach(attach)
        }
        startNowPlayingSync()
        startAudioDemandMonitor()
        syncAudioBridgeDemand(forceControl: true)
        delegate?.bridgeDidUpdateAudioStatus(audioSink.statusText, active: audioSink.isRunning)
    }

    func sendSettings(_ settings: JoystickSettings) {
        sendControlPayload([advCtlSetInputCommand, settings.flags, UInt8(settings.sensitivity), UInt8(settings.knobMode)],
                           successMessage: "Sent joystick settings to ADV")
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
        manualAudioTestActive = active
        syncAudioBridgeDemand(forceControl: true)
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

    private func startAudioDemandMonitor() {
        guard audioDemandTimer == nil else {
            return
        }
        audioDemandTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.syncAudioBridgeDemand()
        }
    }

    private func syncAudioBridgeDemand(forceControl: Bool = false) {
        let driverAvailable = audioSink.refreshDevice()
        driverAudioDemandActive = driverAvailable && audioSink.isInputRequested()

        guard manualAudioTestActive || driverAudioDemandActive else {
            stopAudioBridge(status: driverAvailable ? "ADV microphone inactive" : audioSink.statusText,
                            forceControl: forceControl)
            return
        }

        let driverReady = driverAvailable && audioSink.start()
        if !driverReady {
            audioSink.stop()
        }
        sendAudioBridgeActive(true, force: forceControl)

        let status: String
        if driverAudioDemandActive {
            status = driverReady ? "ADVCtlAudio requested input" : "ADVCtlAudio requested input; ring unavailable"
        } else {
            status = driverReady ? "ADV recording test active" : "ADV recording active; ADVCtlAudio unavailable"
        }
        delegate?.bridgeDidUpdateAudioStatus(status, active: true)
    }

    private func stopAudioBridge(status: String, forceControl: Bool = false) {
        audioSink.stop()
        sendAudioBridgeActive(false, force: forceControl)
        delegate?.bridgeDidUpdateAudioStatus(status, active: false)
    }

    private func sendAudioBridgeActive(_ active: Bool, force: Bool = false, updateStatus: Bool = true) {
        guard force || advAudioBridgeActive != active else {
            return
        }
        advAudioBridgeActive = active
        sendControlPayload([advCtlAudioTestCommand, active ? 1 : 0, 0, 0],
                           successMessage: active ? "ADV microphone bridge activated" : "ADV microphone bridge stopped",
                           updateStatus: updateStatus)
        sendKeyboardLedAudioControl(active: active)
    }

    private func sendKeyboardLedAudioControl(active: Bool) {
        for registration in hidDevices where registration.kind.supportsKeyboard {
            let prefixStatus = setKeyboardOutputReport(registration.device, value: advCtlKeyboardLedAudioPrefix)
            let commandStatus = setKeyboardOutputReport(registration.device,
                                                        value: active ? advCtlKeyboardLedAudioStart : advCtlKeyboardLedAudioStop)
            if prefixStatus == kIOReturnSuccess && commandStatus == kIOReturnSuccess {
                updateMessage("Sent ADV microphone \(active ? "start" : "stop") via keyboard output")
                return
            }
            if prefixStatus != kIOReturnUnsupported || commandStatus != kIOReturnUnsupported {
                updateMessage("Keyboard output audio control failed: \(prefixStatus)/\(commandStatus)")
            }
        }
    }

    private func setKeyboardOutputReport(_ device: IOHIDDevice, value: UInt8) -> IOReturn {
        var report = [value]
        let reportLength = report.count
        return report.withUnsafeMutableBytes { buffer in
            guard let base = buffer.baseAddress else { return kIOReturnBadArgument }
            return IOHIDDeviceSetReport(device,
                                        kIOHIDReportTypeOutput,
                                        CFIndex(advCtlKeyboardReportID),
                                        base.assumingMemoryBound(to: UInt8.self),
                                        reportLength)
        }
    }

    private func sendControlPayload(_ bytes: [UInt8], successMessage: String, updateStatus: Bool = true) {
        var payload = Array(bytes.prefix(advCtlPayloadLength))
        if payload.count < advCtlPayloadLength {
            payload.append(contentsOf: repeatElement(0, count: advCtlPayloadLength - payload.count))
        }
        var reportWithID = [UInt8(advCtlReportID)]
        reportWithID.append(contentsOf: payload)
        var sent = false
        for registration in hidDevices {
            guard registration.kind.supportsControl else {
                continue
            }
            let status = setControlReport(registration.device, payload: &payload, reportWithID: &reportWithID)
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

    private func setControlReport(_ device: IOHIDDevice,
                                  payload: inout [UInt8],
                                  reportWithID: inout [UInt8]) -> IOReturn {
        let attempts: [(IOHIDReportType, Bool, CFIndex)] = [
            (kIOHIDReportTypeFeature, true, 0),
            (kIOHIDReportTypeOutput, true, 0),
            (kIOHIDReportTypeFeature, false, CFIndex(advCtlReportID)),
            (kIOHIDReportTypeOutput, false, CFIndex(advCtlReportID)),
            (kIOHIDReportTypeFeature, true, CFIndex(advCtlReportID)),
            (kIOHIDReportTypeOutput, true, CFIndex(advCtlReportID)),
        ]
        var lastStatus = kIOReturnUnsupported
        for (reportType, includeReportID, reportID) in attempts {
            let status: IOReturn
            if includeReportID {
                let reportLength = reportWithID.count
                status = reportWithID.withUnsafeMutableBytes { buffer in
                    guard let base = buffer.baseAddress else { return kIOReturnBadArgument }
                    return IOHIDDeviceSetReport(device,
                                                reportType,
                                                reportID,
                                                base.assumingMemoryBound(to: UInt8.self),
                                                reportLength)
                }
            } else {
                let reportLength = payload.count
                status = payload.withUnsafeMutableBytes { buffer in
                    guard let base = buffer.baseAddress else { return kIOReturnBadArgument }
                    return IOHIDDeviceSetReport(device,
                                                reportType,
                                                reportID,
                                                base.assumingMemoryBound(to: UInt8.self),
                                                reportLength)
                }
            }
            if status == kIOReturnSuccess {
                updateMessage("Sent control payload using \(reportType == kIOHIDReportTypeFeature ? "feature" : "output") report\(includeReportID ? " with id" : "") param=\(reportID)")
                return status
            }
            lastStatus = status
        }
        return lastStatus
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
            kind = .composite
        } else {
            updateMessage("Ignoring non-control HID endpoint usagePage=\(usagePage) usage=\(usage)")
            return
        }

        let openStatus = IOHIDDeviceOpen(device, IOOptionBits(kIOHIDOptionsTypeNone))
        if openStatus != kIOReturnSuccess && openStatus != kIOReturnExclusiveAccess {
            if openStatus == kIOReturnNotPermitted {
                updateMessage("Grant ADVCtl Input Monitoring, then reopen ADVCtl")
                openInputMonitoringPreferences()
                return
            }
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
        if kind.supportsControl {
            sendTimeSync()
            requestSettings()
            syncAudioBridgeDemand(forceControl: true)
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
        case .composite:
            handleControlReport(reportID: reportID, report: report, length: length)
            handleKeyboardReport(reportID: reportID, report: report, length: length)
        }
    }

    private func handleControlReport(reportID: UInt32, report: UnsafeMutablePointer<UInt8>, length: CFIndex) {
        let payload: Data
        if length >= 4, report[0] == UInt8(advCtlReportID) {
            payload = Data(bytes: report.advanced(by: 1), count: min(advCtlPayloadLength, Int(length) - 1))
        } else if reportID == advCtlReportID, length >= 3 {
            payload = Data(bytes: report, count: min(advCtlPayloadLength, Int(length)))
        } else {
            return
        }

        if payload.count >= 3, payload[0] == advCtlAudioFrameReport {
            let byteCount = min(Int(payload[2]), payload.count - 3)
            if byteCount > 0 {
                audioFrameCount += 1
                if audioFrameCount == 1 || audioFrameCount % 100 == 0 {
                    updateMessage("Received ADV audio frames: \(audioFrameCount)")
                }
                audioSink.enqueueULaw(payload.subdata(in: 3..<(3 + byteCount)))
            }
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

private enum ADVCtlInstaller {
    static var launchAgentURL: URL {
        URL(fileURLWithPath: NSHomeDirectory())
            .appendingPathComponent("Library/LaunchAgents", isDirectory: true)
            .appendingPathComponent("\(advCtlLaunchAgentLabel).plist")
    }

    static var bundledAudioDriverURL: URL? {
        Bundle.main.resourceURL?.appendingPathComponent("ADVCtlAudio.driver", isDirectory: true)
    }

    static func isLaunchAtLoginEnabled() -> Bool {
        FileManager.default.fileExists(atPath: launchAgentURL.path)
    }

    static func isLaunchAtLoginLoaded() -> Bool {
        runLaunchctl(["print", "\(launchDomain())/\(advCtlLaunchAgentLabel)"]) == 0
    }

    static func installAppToApplications() throws {
        let current = Bundle.main.bundleURL
        guard current.pathExtension == "app" else {
            throw NSError(domain: "ADVCtlInstaller", code: 1, userInfo: [
                NSLocalizedDescriptionKey: "ADVCtl must be running from an app bundle."
            ])
        }
        if current.standardizedFileURL == advCtlInstalledAppURL.standardizedFileURL {
            return
        }
        let command = "rm -rf \(shellQuote(advCtlInstalledAppURL.path)) && ditto \(shellQuote(current.path)) \(shellQuote(advCtlInstalledAppURL.path))"
        try runAdminShell(command)
    }

    static func installAudioDriver() throws {
        guard let bundledAudioDriverURL, FileManager.default.fileExists(atPath: bundledAudioDriverURL.path) else {
            throw NSError(domain: "ADVCtlInstaller", code: 2, userInfo: [
                NSLocalizedDescriptionKey: "Bundled ADVCtlAudio.driver is missing. Rebuild ADVCtl with tools/macctl-helper/build-app.sh."
            ])
        }
        let command = [
            "mkdir -p /Library/Audio/Plug-Ins/HAL",
            "rm -rf \(shellQuote(advCtlAudioDriverURL.path))",
            "rm -f /tmp/advctl_audio_pcm.ring",
            "ditto \(shellQuote(bundledAudioDriverURL.path)) \(shellQuote(advCtlAudioDriverURL.path))",
            "chown -R root:wheel \(shellQuote(advCtlAudioDriverURL.path))",
            "chmod -R go-w \(shellQuote(advCtlAudioDriverURL.path))",
            "killall coreaudiod || true",
        ].joined(separator: " && ")
        try runAdminShell(command)
    }

    static func setLaunchAtLogin(_ enabled: Bool) throws {
        if enabled {
            try FileManager.default.createDirectory(at: launchAgentURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            try launchAgentPlist().write(to: launchAgentURL, atomically: true, encoding: .utf8)
            _ = runLaunchctl(["bootout", launchDomain(), launchAgentURL.path])
            let result = runLaunchctl(["bootstrap", launchDomain(), launchAgentURL.path])
            if result != 0 {
                throw NSError(domain: "ADVCtlInstaller", code: Int(result), userInfo: [
                    NSLocalizedDescriptionKey: "launchctl bootstrap failed with status \(result)."
                ])
            }
            _ = runLaunchctl(["kickstart", "-k", "\(launchDomain())/\(advCtlLaunchAgentLabel)"])
        } else {
            _ = runLaunchctl(["bootout", launchDomain(), launchAgentURL.path])
            if FileManager.default.fileExists(atPath: launchAgentURL.path) {
                try FileManager.default.removeItem(at: launchAgentURL)
            }
        }
    }

    private static func launchAgentPlist() -> String {
        """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
            <key>Label</key>
            <string>\(advCtlLaunchAgentLabel)</string>
            <key>ProgramArguments</key>
            <array>
                <string>/Applications/ADVCtl.app/Contents/MacOS/ADVCtl</string>
            </array>
            <key>RunAtLoad</key>
            <true/>
            <key>KeepAlive</key>
            <true/>
        </dict>
        </plist>
        """
    }

    private static func launchDomain() -> String {
        "gui/\(getuid())"
    }

    private static func runLaunchctl(_ arguments: [String]) -> Int32 {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        process.arguments = arguments
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus
        } catch {
            return -1
        }
    }

    private static func runAdminShell(_ command: String) throws {
        let script = "do shell script \(appleScriptQuote(command)) with administrator privileges"
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = ["-e", script]
        try process.run()
        process.waitUntilExit()
        if process.terminationStatus != 0 {
            throw NSError(domain: "ADVCtlInstaller", code: Int(process.terminationStatus), userInfo: [
                NSLocalizedDescriptionKey: "Administrator command failed with status \(process.terminationStatus)."
            ])
        }
    }

    private static func shellQuote(_ value: String) -> String {
        "'\(value.replacingOccurrences(of: "'", with: "'\\''"))'"
    }

    private static func appleScriptQuote(_ value: String) -> String {
        "\"\(value.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\""))\""
    }
}

private final class SettingsWindowController: NSWindowController {
    private struct SettingsSection {
        let identifier: String
        let sidebarTitle: String
        let symbol: String
        let pageTitle: String
        let pageSubtitle: String
        let groups: [NSView]
    }

    private let settings: JoystickSettings
    var onSettingsChanged: (() -> Void)?
    var onPowerSettingsChanged: (() -> Void)?
    var onAudioTestChanged: ((Bool) -> Void)?
    var onRefreshSettings: (() -> Void)?
    var onResetHardwareSettings: (() -> Void)?
    var onInstallApp: (() -> Void)?
    var onInstallAudioDriver: (() -> Void)?
    var onLaunchAtLoginChanged: ((Bool) -> Void)?
    var onRequestInputMonitoring: (() -> Void)?
    var onRequestAccessibility: (() -> Void)?
    var onRequestMicrophone: (() -> Void)?
    var onOpenBluetooth: (() -> Void)?

    private let appInstallStatusLabel = NSTextField(labelWithString: "检查中")
    private let audioDriverStatusLabel = NSTextField(labelWithString: "检查中")
    private let loginStartupStatusLabel = NSTextField(labelWithString: "检查中")
    private let inputMonitoringStatusLabel = NSTextField(labelWithString: "检查中")
    private let accessibilityStatusLabel = NSTextField(labelWithString: "检查中")
    private let microphonePermissionStatusLabel = NSTextField(labelWithString: "检查中")
    private let bluetoothStatusLabel = NSTextField(labelWithString: "蓝牙设置")
    private let installAppButton = NSButton(title: "安装到 /Applications", target: nil, action: nil)
    private let installAudioDriverButton = NSButton(title: "安装音频驱动", target: nil, action: nil)
    private let launchAtLoginButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let inputMonitoringButton = NSButton(title: "设置", target: nil, action: nil)
    private let accessibilityButton = NSButton(title: "设置", target: nil, action: nil)
    private let microphonePermissionButton = NSButton(title: "设置", target: nil, action: nil)
    private let bluetoothButton = NSButton(title: "打开蓝牙", target: nil, action: nil)
    private let connectionValueLabel = NSTextField(labelWithString: "未连接")
    private let knobValueLabel = NSTextField(labelWithString: "无输入")
    private let messageValueLabel = NSTextField(labelWithString: "启动中")
    private let audioStateLabel = NSTextField(labelWithString: "未激活")
    private let microphoneStatusLabel = NSTextField(labelWithString: "检查 ADVCtlAudio")
    private let swapAxesButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let invertXButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let invertYButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let sensitivitySlider = NSSlider()
    private let sensitivityValueLabel = NSTextField(labelWithString: "1x")
    private let knobModePopup = NSPopUpButton()
    private let resetButton = NSButton(title: "恢复硬件默认值", target: nil, action: nil)
    private let refreshButton = NSButton(title: "从 ADV 刷新", target: nil, action: nil)
    private let waveformView = WaveformView(frame: .zero)
    private let recordButton = NSButton(title: "启用 ADV 麦克风", target: nil, action: nil)
    private let screenTimeoutSlider = NSSlider()
    private let screenTimeoutValueLabel = NSTextField(labelWithString: "30s")
    private let powerSaveSlider = NSSlider()
    private let powerSaveValueLabel = NSTextField(labelWithString: "3m")
    private let searchField = NSSearchField()
    private let sidebarTable = NSTableView()
    private let sidebarScrollView = NSScrollView()
    private let tabView = NSTabView()
    private var sections: [SettingsSection] = []
    private var audioTestActive = false

    init(settings: JoystickSettings) {
        self.settings = settings
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 840, height: 768),
                              styleMask: [.titled, .closable, .miniaturizable, .fullSizeContentView],
                              backing: .buffered,
                              defer: false)
        window.title = "ADVCtl"
        window.isReleasedWhenClosed = false
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
        connectionValueLabel.stringValue = connected ? "已连接" : "未连接"
        connectionValueLabel.textColor = connected ? .systemGreen : .secondaryLabelColor
        knobValueLabel.stringValue = knobStatus
            .replacingOccurrences(of: "Knob: ", with: "")
            .replacingOccurrences(of: "旋钮：", with: "")
    }

    func updateMessage(_ message: String) {
        messageValueLabel.stringValue = message
    }

    func updateAudioStatus(_ message: String, active: Bool) {
        microphoneStatusLabel.stringValue = message
        microphoneStatusLabel.textColor = active ? .systemGreen : .secondaryLabelColor
        audioTestActive = active
        refresh()
    }

    private func buildContent() {
        guard let contentView = window?.contentView else { return }

        contentView.wantsLayer = true
        contentView.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor

        let root = NSStackView()
        root.orientation = .horizontal
        root.spacing = 0
        root.translatesAutoresizingMaskIntoConstraints = false

        let sidebarEffect = NSVisualEffectView()
        sidebarEffect.material = .sidebar
        sidebarEffect.blendingMode = .withinWindow
        sidebarEffect.state = .active
        sidebarEffect.translatesAutoresizingMaskIntoConstraints = false

        let sidebarStack = NSStackView()
        sidebarStack.orientation = .vertical
        sidebarStack.alignment = .leading
        sidebarStack.spacing = 10
        sidebarStack.edgeInsets = NSEdgeInsets(top: 72, left: 12, bottom: 18, right: 10)
        sidebarStack.translatesAutoresizingMaskIntoConstraints = false
        sidebarEffect.addSubview(sidebarStack)

        searchField.placeholderString = "搜索"
        searchField.font = .systemFont(ofSize: 13)
        searchField.translatesAutoresizingMaskIntoConstraints = false
        sidebarStack.addArrangedSubview(searchField)

        sections = makeSections()
        configureSidebarTable()
        sidebarStack.addArrangedSubview(sidebarScrollView)

        let contentEffect = NSVisualEffectView()
        contentEffect.material = .contentBackground
        contentEffect.blendingMode = .withinWindow
        contentEffect.state = .active
        contentEffect.translatesAutoresizingMaskIntoConstraints = false

        tabView.tabViewType = .noTabsNoBorder
        tabView.translatesAutoresizingMaskIntoConstraints = false
        sections.forEach { tabView.addTabViewItem(tabItem($0)) }
        contentEffect.addSubview(tabView)
        root.addArrangedSubview(sidebarEffect)
        root.addArrangedSubview(contentEffect)
        contentView.addSubview(root)

        installAppButton.target = self
        installAppButton.action = #selector(installApp)
        installAudioDriverButton.target = self
        installAudioDriverButton.action = #selector(installAudioDriver)
        launchAtLoginButton.target = self
        launchAtLoginButton.action = #selector(toggleLaunchAtLogin)
        inputMonitoringButton.target = self
        inputMonitoringButton.action = #selector(requestInputMonitoring)
        accessibilityButton.target = self
        accessibilityButton.action = #selector(requestAccessibility)
        microphonePermissionButton.target = self
        microphonePermissionButton.action = #selector(requestMicrophone)
        bluetoothButton.target = self
        bluetoothButton.action = #selector(openBluetooth)

        swapAxesButton.target = self
        swapAxesButton.action = #selector(settingsChanged)
        invertXButton.target = self
        invertXButton.action = #selector(settingsChanged)
        invertYButton.target = self
        invertYButton.action = #selector(settingsChanged)
        knobModePopup.addItems(withTitles: ["系统音量", "HomePod 音量", "禁用"])
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
            sidebarEffect.widthAnchor.constraint(equalToConstant: 260),
            sidebarStack.leadingAnchor.constraint(equalTo: sidebarEffect.leadingAnchor),
            sidebarStack.trailingAnchor.constraint(equalTo: sidebarEffect.trailingAnchor),
            sidebarStack.topAnchor.constraint(equalTo: sidebarEffect.topAnchor),
            sidebarStack.bottomAnchor.constraint(equalTo: sidebarEffect.bottomAnchor),
            searchField.widthAnchor.constraint(equalToConstant: 224),
            searchField.heightAnchor.constraint(equalToConstant: 32),
            sidebarScrollView.widthAnchor.constraint(equalToConstant: 232),
            sidebarScrollView.topAnchor.constraint(equalTo: searchField.bottomAnchor, constant: 10),
            sidebarScrollView.bottomAnchor.constraint(equalTo: sidebarStack.bottomAnchor),
            tabView.leadingAnchor.constraint(equalTo: contentEffect.leadingAnchor),
            tabView.trailingAnchor.constraint(equalTo: contentEffect.trailingAnchor),
            tabView.topAnchor.constraint(equalTo: contentEffect.topAnchor),
            tabView.bottomAnchor.constraint(equalTo: contentEffect.bottomAnchor),
        ])
    }

    private func configureSidebarTable() {
        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("settings"))
        column.resizingMask = .autoresizingMask
        sidebarTable.addTableColumn(column)
        sidebarTable.headerView = nil
        sidebarTable.style = .sourceList
        sidebarTable.rowHeight = 40
        sidebarTable.intercellSpacing = NSSize(width: 0, height: 2)
        sidebarTable.backgroundColor = .clear
        sidebarTable.delegate = self
        sidebarTable.dataSource = self
        sidebarTable.translatesAutoresizingMaskIntoConstraints = false

        sidebarScrollView.documentView = sidebarTable
        sidebarScrollView.drawsBackground = false
        sidebarScrollView.hasVerticalScroller = true
        sidebarScrollView.borderType = .noBorder
        sidebarScrollView.translatesAutoresizingMaskIntoConstraints = false

        sidebarTable.reloadData()
        sidebarTable.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
    }

    private func makeSections() -> [SettingsSection] {
        [
            SettingsSection(identifier: "general",
                            sidebarTitle: "通用",
                            symbol: "gearshape",
                            pageTitle: "通用",
                            pageSubtitle: "管理 ADVCtl 的安装位置、登录启动和蓝牙配对入口。",
                            groups: [
                                group("ADVCtl", rows: [
                                    valueRow("ADVCtl.app", appInstallStatusLabel),
                                    controlRow("安装位置", installAppButton),
                                    valueRow("登录启动", loginStartupStatusLabel),
                                    controlRow("登录后自动启动", launchAtLoginButton),
                                    permissionRow("蓝牙", bluetoothStatusLabel, bluetoothButton),
                                ]),
                            ]),
            SettingsSection(identifier: "status",
                            sidebarTitle: "ADV",
                            symbol: "dot.radiowaves.left.and.right",
                            pageTitle: "ADV",
                            pageSubtitle: "查看 ADV 蓝牙连接、旋钮输入和最近的硬件同步状态。",
                            groups: [
                                group("连接", rows: [
                                    valueRow("ADV", connectionValueLabel),
                                    valueRow("旋钮", knobValueLabel),
                                    valueRow("最近消息", messageValueLabel),
                                    controlRow("硬件配置", refreshButton),
                                ]),
                            ]),
            SettingsSection(identifier: "pointer",
                            sidebarTitle: "指针",
                            symbol: "cursorarrow.motionlines",
                            pageTitle: "指针",
                            pageSubtitle: "调整摇杆方向、轴映射和鼠标移动速度。这些设置会写入 ADV 固件。",
                            groups: [
                                group("指针", rows: [
                                    controlRow("交换 X/Y 轴", swapAxesButton),
                                    controlRow("反转左右", invertXButton),
                                    controlRow("反转上下", invertYButton),
                                    sensitivityRow(),
                                ]),
                            ]),
            SettingsSection(identifier: "knob",
                            sidebarTitle: "旋钮",
                            symbol: "dial.low",
                            pageTitle: "旋钮",
                            pageSubtitle: "设置旋转行为。HomePod 音量由 ADVCtl 通过 Home Assistant 执行。",
                            groups: [
                                group("旋钮", rows: [
                                    controlRow("旋转", knobModePopup),
                                    valueRow("按下", NSTextField(labelWithString: "系统模式静音，HomePod 模式发送 F15")),
                                    controlRow("默认值", resetButton),
                                ]),
                            ]),
            SettingsSection(identifier: "audio",
                            sidebarTitle: "音频",
                            symbol: "waveform",
                            pageTitle: "音频",
                            pageSubtitle: "系统应用读取 ADVCtlAudio 麦克风时自动激活 ADV，并通过本地环形缓冲传输音频。",
                            groups: [
                                group("系统音频驱动", rows: [
                                    valueRow("ADVCtlAudio", audioDriverStatusLabel),
                                    controlRow("驱动", installAudioDriverButton),
                                ]),
                                group("音频", rows: [
                                    valueRow("录制测试", audioStateLabel),
                                    valueRow("系统麦克风", microphoneStatusLabel),
                                    waveformRow(),
                                ]),
                            ]),
            SettingsSection(identifier: "power",
                            sidebarTitle: "省电",
                            symbol: "battery.75percent",
                            pageTitle: "省电",
                            pageSubtitle: "配置屏幕关闭和深度省电时间。深度省电会关闭 BLE，按键后自动恢复连接。",
                            groups: [
                                group("省电", rows: [
                                    screenTimeoutRow(),
                                    powerSaveTimeoutRow(),
                                ]),
                            ]),
            SettingsSection(identifier: "privacy",
                            sidebarTitle: "隐私与安全性",
                            symbol: "hand.raised.fill",
                            pageTitle: "隐私与安全性",
                            pageSubtitle: "集中管理 ADVCtl 需要的 macOS 权限。",
                            groups: [
                                group("权限", rows: [
                                    permissionRow("输入监听", inputMonitoringStatusLabel, inputMonitoringButton),
                                    permissionRow("辅助功能", accessibilityStatusLabel, accessibilityButton),
                                    permissionRow("麦克风", microphonePermissionStatusLabel, microphonePermissionButton),
                                ]),
                            ]),
        ]
    }

    private func tabItem(_ section: SettingsSection) -> NSTabViewItem {
        let item = NSTabViewItem(identifier: section.identifier)
        item.label = section.sidebarTitle
        item.view = tabPage(title: section.pageTitle, subtitle: section.pageSubtitle, groups: section.groups)
        return item
    }

    private func tabPage(title: String, subtitle: String, groups: [NSView]) -> NSView {
        let page = NSView()
        page.translatesAutoresizingMaskIntoConstraints = false

        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 14
        stack.edgeInsets = NSEdgeInsets(top: 18, left: 24, bottom: 28, right: 24)
        stack.translatesAutoresizingMaskIntoConstraints = false

        stack.addArrangedSubview(pageHeader(title: title, subtitle: subtitle))
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

    func selectInstallPage() {
        selectPage("general")
    }

    private func selectPage(_ identifier: String) {
        tabView.selectTabViewItem(withIdentifier: identifier)
        if let index = sections.firstIndex(where: { $0.identifier == identifier }) {
            sidebarTable.selectRowIndexes(IndexSet(integer: index), byExtendingSelection: false)
        }
    }

    private func pageHeader(title: String, subtitle: String) -> NSView {
        let container = NSView()
        container.translatesAutoresizingMaskIntoConstraints = false

        let titleLabel = NSTextField(labelWithString: title)
        titleLabel.font = .systemFont(ofSize: 22, weight: .semibold)
        titleLabel.textColor = .labelColor
        titleLabel.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(titleLabel)

        let subtitleLabel = NSTextField(wrappingLabelWithString: subtitle)
        subtitleLabel.font = .systemFont(ofSize: 13)
        subtitleLabel.textColor = .secondaryLabelColor
        subtitleLabel.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(subtitleLabel)

        NSLayoutConstraint.activate([
            container.widthAnchor.constraint(equalToConstant: 520),
            container.heightAnchor.constraint(equalToConstant: 72),
            titleLabel.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            titleLabel.topAnchor.constraint(equalTo: container.topAnchor),
            subtitleLabel.leadingAnchor.constraint(equalTo: titleLabel.leadingAnchor),
            subtitleLabel.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            subtitleLabel.topAnchor.constraint(equalTo: titleLabel.bottomAnchor, constant: 8),
        ])
        return container
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
        card.layer?.cornerRadius = 12
        card.layer?.backgroundColor = NSColor.controlBackgroundColor.withAlphaComponent(0.72).cgColor
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
            wrapper.widthAnchor.constraint(equalToConstant: 532),
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

    private func permissionRow(_ title: String, _ status: NSTextField, _ button: NSButton) -> NSView {
        status.font = .systemFont(ofSize: 13)
        status.textColor = .secondaryLabelColor
        let stack = NSStackView()
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = 10
        stack.addArrangedSubview(status)
        stack.addArrangedSubview(button)
        status.widthAnchor.constraint(equalToConstant: 160).isActive = true
        button.widthAnchor.constraint(greaterThanOrEqualToConstant: 96).isActive = true
        return row(title, trailing: stack)
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
        return row("指针速度", trailing: stack)
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
        return row("关闭屏幕", trailing: stack)
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
        return row("深度省电", trailing: stack)
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
        return row("ADV 麦克风", trailing: stack, minHeight: 162)
    }

    private func row(_ title: String, trailing: NSView, minHeight: CGFloat = 44) -> NSView {
        let label = NSTextField(labelWithString: title)
        label.font = .systemFont(ofSize: 13.5)
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
            label.topAnchor.constraint(equalTo: container.topAnchor, constant: 14),
            trailing.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -16),
            trailing.centerYAnchor.constraint(equalTo: container.centerYAnchor),
            trailing.leadingAnchor.constraint(greaterThanOrEqualTo: label.trailingAnchor, constant: 18),
        ])
        return container
    }

    func refresh() {
        let appInstalled = FileManager.default.fileExists(atPath: advCtlInstalledAppURL.path)
        setStatus(appInstallStatusLabel,
                  appInstalled ? "已安装在 /Applications" : "未安装",
                  ok: appInstalled)

        let driverInstalled = FileManager.default.fileExists(atPath: advCtlAudioDriverURL.path)
        setStatus(audioDriverStatusLabel,
                  driverInstalled ? "已安装" : "未安装",
                  ok: driverInstalled)

        let launchAtLoginEnabled = ADVCtlInstaller.isLaunchAtLoginEnabled()
        let launchAtLoginLoaded = ADVCtlInstaller.isLaunchAtLoginLoaded()
        launchAtLoginButton.state = launchAtLoginEnabled ? .on : .off
        setStatus(loginStartupStatusLabel,
                  launchAtLoginEnabled ? (launchAtLoginLoaded ? "已开启" : "已开启，下次登录生效") : "已关闭",
                  ok: launchAtLoginEnabled)

        let inputMonitoringGranted = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted
        setStatus(inputMonitoringStatusLabel,
                  inputMonitoringGranted ? "已允许" : "需要设置",
                  ok: inputMonitoringGranted)

        let accessibilityGranted = AXIsProcessTrusted()
        setStatus(accessibilityStatusLabel,
                  accessibilityGranted ? "已允许" : "需要设置",
                  ok: accessibilityGranted)

        let microphoneStatus = AVCaptureDevice.authorizationStatus(for: .audio)
        let microphoneGranted = microphoneStatus == .authorized
        let microphoneText: String
        switch microphoneStatus {
        case .authorized:
            microphoneText = "已允许"
        case .denied:
            microphoneText = "已拒绝"
        case .restricted:
            microphoneText = "受限制"
        case .notDetermined:
            microphoneText = "未请求"
        @unknown default:
            microphoneText = "未知"
        }
        setStatus(microphonePermissionStatusLabel, microphoneText, ok: microphoneGranted)

        setStatus(bluetoothStatusLabel, "配对 ADVCtl", ok: true)

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
        audioStateLabel.stringValue = audioTestActive ? "已激活" : "未激活"
        audioStateLabel.textColor = audioTestActive ? .systemBlue : .secondaryLabelColor
        recordButton.title = audioTestActive ? "停止 ADV 麦克风" : "启用 ADV 麦克风"
        waveformView.isActive = audioTestActive
    }

    private func setStatus(_ label: NSTextField, _ text: String, ok: Bool) {
        label.stringValue = text
        label.textColor = ok ? .systemGreen : .secondaryLabelColor
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
        onAudioTestChanged?(!audioTestActive)
    }

    @objc private func installApp() {
        onInstallApp?()
    }

    @objc private func installAudioDriver() {
        onInstallAudioDriver?()
    }

    @objc private func toggleLaunchAtLogin() {
        onLaunchAtLoginChanged?(launchAtLoginButton.state == .on)
    }

    @objc private func requestInputMonitoring() {
        onRequestInputMonitoring?()
    }

    @objc private func requestAccessibility() {
        onRequestAccessibility?()
    }

    @objc private func requestMicrophone() {
        onRequestMicrophone?()
    }

    @objc private func openBluetooth() {
        onOpenBluetooth?()
    }
}

extension SettingsWindowController: NSTableViewDataSource, NSTableViewDelegate {
    func numberOfRows(in tableView: NSTableView) -> Int {
        sections.count
    }

    func tableView(_ tableView: NSTableView, heightOfRow row: Int) -> CGFloat {
        38
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard row >= 0, row < sections.count else {
            return nil
        }
        let identifier = NSUserInterfaceItemIdentifier("SettingsSidebarCell")
        let section = sections[row]

        let cell = tableView.makeView(withIdentifier: identifier, owner: self) as? NSTableCellView ?? {
            let cell = NSTableCellView()
            cell.identifier = identifier

            let imageView = NSImageView()
            imageView.translatesAutoresizingMaskIntoConstraints = false
            imageView.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 16, weight: .regular)
            imageView.contentTintColor = .labelColor

            let textField = NSTextField(labelWithString: "")
            textField.translatesAutoresizingMaskIntoConstraints = false
            textField.font = .systemFont(ofSize: 13)
            textField.lineBreakMode = .byTruncatingTail

            cell.addSubview(imageView)
            cell.addSubview(textField)
            cell.imageView = imageView
            cell.textField = textField

            NSLayoutConstraint.activate([
                imageView.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 10),
                imageView.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
                imageView.widthAnchor.constraint(equalToConstant: 22),
                imageView.heightAnchor.constraint(equalToConstant: 22),
                textField.leadingAnchor.constraint(equalTo: imageView.trailingAnchor, constant: 8),
                textField.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -8),
                textField.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
            ])
            return cell
        }()

        cell.textField?.stringValue = section.sidebarTitle
        cell.imageView?.image = NSImage(systemSymbolName: section.symbol, accessibilityDescription: section.sidebarTitle)
        return cell
    }

    func tableViewSelectionDidChange(_ notification: Notification) {
        let row = sidebarTable.selectedRow
        guard row >= 0, row < sections.count else {
            return
        }
        tabView.selectTabViewItem(withIdentifier: sections[row].identifier)
    }
}

private final class ADVCtlAppDelegate: NSObject, NSApplicationDelegate, ADVCtlBridgeDelegate {
    private let launchAtLoginDefaultsKey = "launchAtLoginEnabled"
    private let settings = JoystickSettings()
    private lazy var bridge = ADVCtlBridge(config: HelperConfig.load())
    private lazy var settingsWindow = SettingsWindowController(settings: settings)
    private var statusItem: NSStatusItem?
    private let connectionMenuItem = NSMenuItem(title: "未连接", action: nil, keyEquivalent: "")
    private let knobMenuItem = NSMenuItem(title: "旋钮：无输入", action: nil, keyEquivalent: "")
    private let messageMenuItem = NSMenuItem(title: "启动中", action: nil, keyEquivalent: "")
    private var connected = false
    private var knobStatus = "旋钮：无输入"
    private var globalKeyMonitor: Any?
    private var localKeyMonitor: Any?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let launchArgumentText = ProcessInfo.processInfo.arguments.joined(separator: " ")
        let shouldOpenInstallGuide = launchArgumentText.contains("--show-install")
        let shouldOpenSettings = launchArgumentText.contains("--show-settings")
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
        settingsWindow.onInstallApp = { [weak self] in
            self?.installApp()
        }
        settingsWindow.onInstallAudioDriver = { [weak self] in
            self?.installAudioDriver()
        }
        settingsWindow.onLaunchAtLoginChanged = { [weak self] enabled in
            self?.setLaunchAtLogin(enabled)
        }
        settingsWindow.onRequestInputMonitoring = { [weak self] in
            self?.requestInputMonitoring()
        }
        settingsWindow.onRequestAccessibility = { [weak self] in
            self?.requestAccessibility()
        }
        settingsWindow.onRequestMicrophone = { [weak self] in
            self?.requestMicrophone()
        }
        settingsWindow.onOpenBluetooth = {
            openBluetoothPreferences()
        }
        buildStatusItem()
        if !shouldOpenInstallGuide && !shouldOpenSettings {
            applyDefaultLaunchAtLogin()
        }
        bridge.delegate = self
        installKeyMonitors()
        bridge.start()
        if shouldOpenInstallGuide {
            DispatchQueue.main.async { [weak self] in
                log("Opening install guide from launch argument: \(CommandLine.arguments)")
                self?.openInstallGuide()
            }
        } else if shouldOpenSettings {
            DispatchQueue.main.async { [weak self] in
                log("Opening settings from launch argument: \(CommandLine.arguments)")
                self?.openSettings()
            }
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        bridge.sendAudioTest(active: false)
    }

    func bridgeDidUpdateConnection(deviceCount: Int) {
        connected = deviceCount > 0
        connectionMenuItem.title = connected ? "已连接 (\(deviceCount))" : "未连接"
        settingsWindow.updateStatus(connected: connected, knobStatus: knobStatus)
    }

    func bridgeDidReceive(command: MacCtlCommand) {
        switch command {
        case .volumeDelta(let delta):
            knobStatus = delta > 0 ? "旋钮：音量 +\(delta)" : "旋钮：音量 \(delta)"
        case .playPause:
            knobStatus = "旋钮：按下"
        }
        knobMenuItem.title = knobStatus
        settingsWindow.updateStatus(connected: connected, knobStatus: knobStatus)
    }

    func bridgeDidReceiveKnobKey(_ keyCode: UInt8) {
        switch keyCode {
        case hidKeyF13:
            knobStatus = "旋钮：F13 -> HomePod 增大音量"
        case hidKeyF14:
            knobStatus = "旋钮：F14 -> HomePod 减小音量"
        case hidKeyF15:
            knobStatus = "旋钮：F15 按键"
        default:
            knobStatus = "旋钮：按键 \(keyCode)"
        }
        knobMenuItem.title = knobStatus
        settingsWindow.updateStatus(connected: connected, knobStatus: knobStatus)
    }

    func bridgeDidReceiveHardwareSettings(flags: UInt8, sensitivity: UInt8, knobMode: UInt8) {
        settings.applyHardware(flags: flags, sensitivity: sensitivity, knobMode: knobMode)
        settingsWindow.applyHardwareSettings(flags: flags, sensitivity: sensitivity, knobMode: knobMode)
        messageMenuItem.title = "已收到硬件设置"
    }

    func bridgeDidReceivePowerSettings(screenTimeoutSeconds: UInt8, powerSaveTimeoutMinutes: UInt8) {
        settings.applyPower(screenTimeoutSeconds: screenTimeoutSeconds, powerSaveTimeoutMinutes: powerSaveTimeoutMinutes)
        settingsWindow.applyPowerSettings(screenTimeoutSeconds: screenTimeoutSeconds,
                                          powerSaveTimeoutMinutes: powerSaveTimeoutMinutes)
        messageMenuItem.title = "已收到省电设置"
    }

    func bridgeDidUpdateAudioStatus(_ message: String, active: Bool) {
        settingsWindow.updateAudioStatus(message, active: active)
        messageMenuItem.title = message
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

        let installItem = NSMenuItem(title: "安装向导...", action: #selector(openInstallGuide), keyEquivalent: "i")
        installItem.target = self
        menu.addItem(installItem)

        let settingsItem = NSMenuItem(title: "设置...", action: #selector(openSettings), keyEquivalent: ",")
        settingsItem.target = self
        menu.addItem(settingsItem)

        let quitItem = NSMenuItem(title: "退出 ADVCtl", action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)

        item.menu = menu
        statusItem = item
    }

    private func applyDefaultLaunchAtLogin() {
        if UserDefaults.standard.object(forKey: launchAtLoginDefaultsKey) == nil {
            UserDefaults.standard.set(true, forKey: launchAtLoginDefaultsKey)
        }
        guard UserDefaults.standard.bool(forKey: launchAtLoginDefaultsKey) else {
            return
        }
        guard !ADVCtlInstaller.isLaunchAtLoginLoaded() else {
            return
        }
        do {
            try ADVCtlInstaller.setLaunchAtLogin(true)
        } catch {
            log("Launch at Login setup failed: \(error.localizedDescription)")
        }
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
        settingsWindow.window?.makeKeyAndOrderFront(nil)
        settingsWindow.window?.orderFrontRegardless()
        NSApp.activate(ignoringOtherApps: true)
    }

    @objc private func openInstallGuide() {
        openSettings()
        settingsWindow.selectInstallPage()
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }

    private func installApp() {
        do {
            try ADVCtlInstaller.installAppToApplications()
            messageMenuItem.title = "已安装 ADVCtl.app"
        } catch {
            messageMenuItem.title = "应用安装失败"
            log("App install failed: \(error.localizedDescription)")
        }
        settingsWindow.refresh()
    }

    private func installAudioDriver() {
        do {
            try ADVCtlInstaller.installAudioDriver()
            messageMenuItem.title = "已安装 ADVCtlAudio"
        } catch {
            messageMenuItem.title = "音频驱动安装失败"
            log("Audio driver install failed: \(error.localizedDescription)")
        }
        settingsWindow.refresh()
    }

    private func setLaunchAtLogin(_ enabled: Bool) {
        UserDefaults.standard.set(enabled, forKey: launchAtLoginDefaultsKey)
        do {
            try ADVCtlInstaller.setLaunchAtLogin(enabled)
            messageMenuItem.title = enabled ? "登录启动已开启" : "登录启动已关闭"
        } catch {
            messageMenuItem.title = "登录启动设置失败"
            log("Launch at Login update failed: \(error.localizedDescription)")
        }
        settingsWindow.refresh()
    }

    private func requestInputMonitoring() {
        _ = requestHIDListenAccessIfNeeded()
        if IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted {
            openInputMonitoringPreferences()
        }
        settingsWindow.refresh()
    }

    private func requestAccessibility() {
        let options = ["AXTrustedCheckOptionPrompt": true] as CFDictionary
        AXIsProcessTrustedWithOptions(options)
        settingsWindow.refresh()
    }

    private func requestMicrophone() {
        AVCaptureDevice.requestAccess(for: .audio) { [weak self] _ in
            DispatchQueue.main.async {
                self?.settingsWindow.refresh()
                if AVCaptureDevice.authorizationStatus(for: .audio) == .denied {
                    openMicrophonePreferences()
                }
            }
        }
    }
}

let app = NSApplication.shared
private let delegate = ADVCtlAppDelegate()
app.delegate = delegate
app.run()
