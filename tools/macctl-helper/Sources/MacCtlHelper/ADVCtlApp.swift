import AppKit
import ApplicationServices
import AVFoundation
import CoreAudio
import Darwin
import Foundation
import IOKit.hid
import MacCtlCore
import SwiftUI

private let advCtlVendorID = 0x16C0
private let advCtlProductID = 0x05DF
private let advCtlProductName = "ADVCtl"
private let advCtlBundleIdentifier = "dev.cardputer.advctl"
private let advCtlShowSettingsNotification = Notification.Name("dev.cardputer.advctl.showSettings")
private let advCtlShowInstallNotification = Notification.Name("dev.cardputer.advctl.showInstall")
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
private let advCtlAudioStateReport: UInt8 = 0xA1
private let advCtlAudioAckRetryInterval: TimeInterval = 2.0
private let advCtlAudioMaxConcealedPackets = 16
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
    var expectedAudioFrames: Int?
    var audioE2ETimeoutSeconds: TimeInterval?

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
            case "--e2e-audio-frames":
                values["ADVCTL_E2E_EXPECTED_AUDIO_FRAMES"] = value
            case "--e2e-audio-timeout":
                values["ADVCTL_E2E_AUDIO_TIMEOUT_SECONDS"] = value
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
        config.expectedAudioFrames = (values["ADVCTL_E2E_EXPECTED_AUDIO_FRAMES"] ?? values["MACCTL_E2E_EXPECTED_AUDIO_FRAMES"]).flatMap(Int.init)
        config.audioE2ETimeoutSeconds = (values["ADVCTL_E2E_AUDIO_TIMEOUT_SECONDS"] ?? values["MACCTL_E2E_AUDIO_TIMEOUT_SECONDS"]).flatMap(TimeInterval.init)
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

    init(state: AppleTVNativeState) {
        active = state.active
        playing = state.playing
        muted = state.volumePercent == 0
        volumePercent = state.volumePercent
        progressPercent = state.progressPercent
        title = state.title.isEmpty && active ? "Playing" : state.title
        artist = state.artist
    }
}

private protocol ADVCtlBridgeDelegate: AnyObject {
    func bridgeDidUpdateConnection(deviceCount: Int)
    func bridgeDidReceive(command: MacCtlCommand)
    func bridgeDidReceiveKnobKey(_ keyCode: UInt8)
    func bridgeDidUpdateAppleTVVolume(_ volumePercent: Int)
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
    private let appleTVNative = AppleTVNativeClient()
    private var appleTVCommandSession: AppleTVNativeCommandSession?
    private var appleTVCommandSessionIdentifier = ""
    private let audioSink = ADVCtlAudioRingSink()
    private var handledCommands = 0
    private var hidManager: IOHIDManager?
    private var hidDevices: [HIDDeviceRegistration] = []
    private var pressedKnobKeys = Set<UInt8>()
    private var nowPlayingTask: Task<Void, Never>?
    private var audioDemandTimer: Timer?
    private var audioE2ETimeoutTimer: Timer?
    private var manualAudioTestActive = false
    private var driverAudioDemandActive = false
    private var advAudioBridgeActive = false
    private var advAudioStateAcknowledged = true
    private var lastAudioBridgeCommandAt: TimeInterval = 0
    private var audioFrameCount = 0
    private var expectedAudioFrameSequence: UInt8?
    private var concealedAudioPacketCount = 0
    private var lastNowPlayingTextSignature = ""
    private var lastPublishedAppleTVVolumePercent: UInt8?
    weak var delegate: ADVCtlBridgeDelegate?

    init(config: HelperConfig) {
        self.config = config
        if let haConfig = config.homeAssistant {
            self.homeAssistant = HomeAssistantClient(config: haConfig)
        } else {
            self.homeAssistant = nil
        }
    }

    private var configuredAppleTVIdentifier: String {
        UserDefaults.standard.string(forKey: "advctl.appleTVIdentifier") ?? ""
    }

    private var knobVolumeStep: Int {
        let saved = UserDefaults.standard.integer(forKey: "advctl.knobVolumeStep")
        return saved == 0 ? 1 : min(20, max(1, saved))
    }

    private func commandSession(for identifier: String) throws -> AppleTVNativeCommandSession {
        if let session = appleTVCommandSession,
           session.isRunning,
           appleTVCommandSessionIdentifier == identifier {
            return session
        }
        appleTVCommandSession?.stop()
        let session = try appleTVNative.startCommandSession(identifier: identifier) { [weak self] message in
            guard !message.isEmpty else { return }
            log("Apple TV worker: \(message)")
            self?.handleAppleTVWorkerMessage(message)
            if message.contains("\"event\": \"ready\"") || message.contains("\"event\":\"ready\"") {
                self?.updateMessage("Apple TV control connected")
            }
        }
        appleTVCommandSession = session
        appleTVCommandSessionIdentifier = identifier
        return session
    }

    deinit {
        nowPlayingTask?.cancel()
        appleTVCommandSession?.stop()
        audioDemandTimer?.invalidate()
        audioE2ETimeoutTimer?.invalidate()
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
        if configuredAppleTVIdentifier.isEmpty {
            log("ADVCtl Apple TV native control disabled; pair Apple TV in settings")
        }
        startAppleTVCommandSessionIfConfigured()
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
        startAudioE2ETimeoutIfNeeded()
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

    func mediaConfigurationDidChange() {
        lastPublishedAppleTVVolumePercent = nil
        startAppleTVCommandSessionIfConfigured()
        if nowPlayingTask == nil {
            startNowPlayingSync()
        }
    }

    private func startAppleTVCommandSessionIfConfigured() {
        let identifier = configuredAppleTVIdentifier
        guard !identifier.isEmpty else {
            return
        }
        do {
            _ = try commandSession(for: identifier)
            updateMessage("Connecting Apple TV control")
        } catch {
            updateMessage("Apple TV worker failed: \(error.localizedDescription)")
        }
    }

    private func publishAppleTVVolumeIfChanged(_ volumePercent: UInt8) {
        guard lastPublishedAppleTVVolumePercent != volumePercent else {
            return
        }
        lastPublishedAppleTVVolumePercent = volumePercent
        DispatchQueue.main.async {
            self.delegate?.bridgeDidUpdateAppleTVVolume(Int(volumePercent))
        }
    }

    @MainActor private func sendNowPlaying(_ snapshot: NowPlayingSnapshot) {
        publishAppleTVVolumeIfChanged(snapshot.volumePercent)
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
        guard nowPlayingTask == nil, homeAssistant != nil || !configuredAppleTVIdentifier.isEmpty else {
            return
        }
        nowPlayingTask = Task { [weak self] in
            while !Task.isCancelled {
                do {
                    if let identifier = self?.configuredAppleTVIdentifier, !identifier.isEmpty {
                        let state = try await self?.appleTVNative.fetchState(identifier: identifier)
                        if let state {
                            await self?.sendNowPlaying(NowPlayingSnapshot(state: state))
                        }
                    } else if let homeAssistant = self?.homeAssistant {
                        let state = try await homeAssistant.fetchState()
                        let snapshot = NowPlayingSnapshot(state: state)
                        await self?.sendNowPlaying(snapshot)
                    }
                } catch {
                    await self?.sendNowPlaying(NowPlayingSnapshot(active: false,
                                                                  playing: false,
                                                                  muted: false,
                                                                  volumePercent: 0,
                                                                  progressPercent: 0,
                                                                  title: "",
                                                                  artist: ""))
                }
                let hasAppleTV = !(self?.configuredAppleTVIdentifier ?? "").isEmpty
                try? await Task.sleep(nanoseconds: hasAppleTV ? 2_000_000_000 : 3_000_000_000)
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

    private func startAudioE2ETimeoutIfNeeded() {
        guard let expectedFrames = config.expectedAudioFrames, expectedFrames > 0, audioE2ETimeoutTimer == nil else {
            return
        }
        let timeout = max(3.0, config.audioE2ETimeoutSeconds ?? 30.0)
        updateMessage("E2E waiting for \(expectedFrames) ADV audio frames within \(Int(timeout))s")
        audioE2ETimeoutTimer = Timer.scheduledTimer(withTimeInterval: timeout, repeats: false) { _ in
            log("E2E audio timeout; received \(self.audioFrameCount) of \(expectedFrames) frames")
            fflush(stderr)
            exit(2)
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
        let retryStart = shouldRetryAudioStart()
        if retryStart {
            updateMessage("ADV microphone start not acknowledged; retrying")
        }
        sendAudioBridgeActive(true, force: forceControl || retryStart)

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
        let stateChanging = advAudioBridgeActive != active
        guard force || stateChanging else {
            return
        }
        advAudioBridgeActive = active
        advAudioStateAcknowledged = false
        resetAudioPacketTracking()
        lastAudioBridgeCommandAt = Date().timeIntervalSince1970
        sendControlPayload([advCtlAudioTestCommand, active ? 1 : 0, 0, 0],
                           successMessage: active ? "ADV microphone bridge activated" : "ADV microphone bridge stopped",
                           updateStatus: updateStatus)
        sendKeyboardLedAudioControl(active: active)
    }

    private func shouldRetryAudioStart() -> Bool {
        guard advAudioBridgeActive && !advAudioStateAcknowledged else {
            return false
        }
        return Date().timeIntervalSince1970 - lastAudioBridgeCommandAt >= advCtlAudioAckRetryInterval
    }

    private func resetAudioPacketTracking() {
        expectedAudioFrameSequence = nil
        audioFrameCount = 0
        concealedAudioPacketCount = 0
    }

    private func concealMissingAudioPackets(before sequence: UInt8, payloadBytes: Int) -> Int {
        guard let expected = expectedAudioFrameSequence, payloadBytes > 0 else {
            expectedAudioFrameSequence = sequence &+ 1
            return 0
        }

        let distance = (Int(sequence) - Int(expected) + 256) % 256
        expectedAudioFrameSequence = sequence &+ 1
        guard distance > 0 else {
            return 0
        }

        guard distance <= 127 else {
            log("ADV audio sequence reset or stale packet: expected=\(expected) got=\(sequence)")
            return 0
        }

        let missingPackets = min(distance, advCtlAudioMaxConcealedPackets)
        concealedAudioPacketCount += missingPackets
        let written = audioSink.enqueueSilence(uLawSampleCount: missingPackets * payloadBytes)
        if distance > advCtlAudioMaxConcealedPackets {
            log("ADV audio gap capped: missing=\(distance) concealed=\(missingPackets)")
        }
        return written
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
            (kIOHIDReportTypeOutput, true, 0),
            (kIOHIDReportTypeOutput, false, CFIndex(advCtlReportID)),
            (kIOHIDReportTypeOutput, true, CFIndex(advCtlReportID)),
            (kIOHIDReportTypeFeature, false, CFIndex(advCtlReportID)),
            (kIOHIDReportTypeFeature, true, 0),
            (kIOHIDReportTypeFeature, true, CFIndex(advCtlReportID)),
        ]
        var lastStatus = kIOReturnUnsupported
        var outputReportSent = false
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
                if reportType == kIOHIDReportTypeOutput {
                    outputReportSent = true
                    continue
                }
                return status
            }
            lastStatus = status
        }
        if outputReportSent {
            return kIOReturnSuccess
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
        if payload.count >= 2, payload[0] == advCtlAudioStateReport {
            let active = payload[1] != 0
            advAudioStateAcknowledged = active == advAudioBridgeActive
            resetAudioPacketTracking()
            updateMessage(active ? "ADV microphone started" : "ADV microphone stopped")
            DispatchQueue.main.async {
                self.delegate?.bridgeDidUpdateAudioStatus(active ? "ADV microphone streaming" : self.audioSink.statusText,
                                                          active: active)
            }
            return
        }

        if payload.count >= 3, payload[0] == advCtlAudioFrameReport {
            let sequence = payload[1]
            let byteCount = min(Int(payload[2]), payload.count - 3)
            if byteCount > 0 {
                let concealedFrames = concealMissingAudioPackets(before: sequence, payloadBytes: byteCount)
                audioFrameCount += 1
                let writtenFrames = audioSink.enqueueULaw(payload.subdata(in: 3..<(3 + byteCount)))
                if let expectedFrames = config.expectedAudioFrames, expectedFrames > 0, audioFrameCount >= expectedFrames {
                    log("E2E audio complete; received \(audioFrameCount) frames, wrote \(writtenFrames) samples, concealed \(concealedAudioPacketCount) packets")
                    fflush(stdout)
                    exit(0)
                }
                if audioFrameCount == 1 || audioFrameCount % 100 == 0 {
                    updateMessage("Received ADV audio frames: \(audioFrameCount), wrote \(writtenFrames) samples, concealed \(concealedAudioPacketCount) packets")
                } else if concealedFrames > 0 {
                    updateMessage("Concealed ADV audio gap before seq \(sequence): wrote \(concealedFrames) silence samples")
                }
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

        let delta = keyCode == hidKeyF13 ? knobVolumeStep : -knobVolumeStep
        let command: MacCtlCommand = .volumeDelta(delta)
        Task {
            do {
                let appleTVIdentifier = self.configuredAppleTVIdentifier
                if !appleTVIdentifier.isEmpty {
                    try commandSession(for: appleTVIdentifier).sendVolumeDelta(delta)
                    self.updateMessage("Handled \(keyCode == hidKeyF13 ? "F13" : "F14") as \(delta) via Apple TV")
                } else if let homeAssistant {
                    try await homeAssistant.handle(command)
                    self.updateMessage("Handled \(keyCode == hidKeyF13 ? "F13" : "F14") as \(command)")
                } else {
                    self.updateMessage("Received knob key; pair Apple TV or set ADVCTL_HA_TOKEN")
                }
                self.markCommandHandled()
            } catch {
                self.updateMessage("Media request failed: \(error.localizedDescription)")
            }
        }
    }

    private func handle(_ command: MacCtlCommand) {
        DispatchQueue.main.async {
            self.delegate?.bridgeDidReceive(command: command)
        }

        Task {
            do {
                let appleTVIdentifier = self.configuredAppleTVIdentifier
                if !appleTVIdentifier.isEmpty {
                    let session = try commandSession(for: appleTVIdentifier)
                    switch command {
                    case .volumeDelta(let delta):
                        session.sendVolumeDelta(delta * self.knobVolumeStep)
                    case .playPause:
                        session.sendPlayPause()
                    }
                    self.updateMessage("Handled \(command) via Apple TV")
                } else if let homeAssistant {
                    switch command {
                    case .volumeDelta(let delta):
                        try await homeAssistant.handle(.volumeDelta(delta * self.knobVolumeStep))
                    case .playPause:
                        try await homeAssistant.handle(command)
                    }
                    self.updateMessage("Handled \(command)")
                } else {
                    self.updateMessage("Received \(command); pair Apple TV or set ADVCTL_HA_TOKEN")
                }
                self.markCommandHandled()
            } catch {
                self.updateMessage("Media request failed: \(error.localizedDescription)")
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

    private func handleAppleTVWorkerMessage(_ message: String) {
        for line in message.split(whereSeparator: \.isNewline) {
            guard let data = String(line).data(using: .utf8),
                  let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let event = object["event"] as? String,
                  event == "ok",
                  let command = object["command"] as? String,
                  command == "volume",
                  let volume = object["volume"] as? Double else {
                continue
            }
            let percent = min(100, max(0, Int(volume.rounded())))
            publishAppleTVVolumeIfChanged(UInt8(percent))
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
                <string>/usr/bin/open</string>
                <string>-gj</string>
                <string>/Applications/ADVCtl.app</string>
            </array>
            <key>RunAtLoad</key>
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
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
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

private enum ADVCtlSettingsPage: Hashable, CaseIterable {
    case install
    case general
    case adv
    case pointer
    case knob
    case audio
    case power
    case privacy

    var title: String {
        switch self {
        case .install: return "安装向导"
        case .general: return "通用"
        case .adv: return "ADV"
        case .pointer: return "指针"
        case .knob: return "旋钮"
        case .audio: return "音频"
        case .power: return "省电"
        case .privacy: return "隐私与安全性"
        }
    }

    var symbol: String {
        switch self {
        case .install: return "externaldrive.badge.checkmark"
        case .general: return "gearshape"
        case .adv: return "dot.radiowaves.left.and.right"
        case .pointer: return "cursorarrow.motionlines"
        case .knob: return "dial.low"
        case .audio: return "waveform"
        case .power: return "battery.75percent"
        case .privacy: return "hand.raised.fill"
        }
    }
}

private final class ADVCtlSettingsViewModel: ObservableObject {
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
    var onInstallAppleTVRuntime: (() -> Void)?
    var onScanAppleTV: (() -> Void)?
    var onStartAppleTVPairing: ((String, String) -> Void)?
    var onSubmitAppleTVPin: ((String) -> Void)?

    @Published var selectedPage: ADVCtlSettingsPage = .install
    @Published var appInstallStatus = "检查中"
    @Published var appInstalled = false
    @Published var audioDriverStatus = "检查中"
    @Published var audioDriverInstalled = false
    @Published var loginStartupStatus = "检查中"
    @Published var launchAtLoginEnabled = true
    @Published var launchAtLoginLoaded = false
    @Published var inputMonitoringStatus = "检查中"
    @Published var inputMonitoringGranted = false
    @Published var accessibilityStatus = "检查中"
    @Published var accessibilityGranted = false
    @Published var microphonePermissionStatus = "检查中"
    @Published var microphonePermissionGranted = false
    @Published var connectionStatus = "未连接"
    @Published var connected = false
    @Published var knobStatus = "无输入"
    @Published var messageStatus = "启动中"
    @Published var microphoneStatus = "检查 ADVCtlAudio"
    @Published var audioTestActive = false
    @Published var swapAxes = false
    @Published var invertX = false
    @Published var invertY = false
    @Published var sensitivity = 50
    @Published var knobMode = 0
    @Published var screenTimeoutSeconds = 30
    @Published var powerSaveTimeoutMinutes = 3
    @Published var appleTVRuntimeInstalled = false
    @Published var appleTVStatus = "未配置"
    @Published var appleTVDevices: [AppleTVNativeDevice] = []
    @Published var selectedAppleTVID = ""
    @Published var appleTVPairingProtocol = "airplay"
    @Published var appleTVPin = ""
    @Published var appleTVPairingActive = false
    @Published var knobVolumeStep = 1

    init(settings: JoystickSettings) {
        self.settings = settings
        refresh()
    }

    var needsInstallGuide: Bool {
        !appInstalled || !audioDriverInstalled || !launchAtLoginEnabled ||
            !inputMonitoringGranted || !accessibilityGranted || !microphonePermissionGranted
    }

    var sensitivityLabel: String {
        "\(sensitivity)"
    }

    var knobVolumeStepLabel: String {
        "\(knobVolumeStep)%"
    }

    var audioStateText: String {
        audioTestActive ? "已激活" : "未激活"
    }

    var launchAtLoginDetail: String {
        if launchAtLoginEnabled {
            return launchAtLoginLoaded ? "已开启，ADVCtl 会在登录后自动启动。" : "已开启，下次登录生效。"
        }
        return "已关闭。建议保持开启，让状态栏控制和音频驱动自动可用。"
    }

    func refresh() {
        appInstalled = FileManager.default.fileExists(atPath: advCtlInstalledAppURL.path)
        appInstallStatus = appInstalled ? "已安装在 /Applications" : "未安装"

        audioDriverInstalled = FileManager.default.fileExists(atPath: advCtlAudioDriverURL.path)
        audioDriverStatus = audioDriverInstalled ? "已安装" : "未安装"

        launchAtLoginEnabled = ADVCtlInstaller.isLaunchAtLoginEnabled()
        launchAtLoginLoaded = ADVCtlInstaller.isLaunchAtLoginLoaded()
        loginStartupStatus = launchAtLoginEnabled ? (launchAtLoginLoaded ? "已开启" : "已开启，下次登录生效") : "已关闭"

        inputMonitoringGranted = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted
        inputMonitoringStatus = inputMonitoringGranted ? "已允许" : "需要设置"

        accessibilityGranted = AXIsProcessTrusted()
        accessibilityStatus = accessibilityGranted ? "已允许" : "需要设置"

        let microphoneStatus = AVCaptureDevice.authorizationStatus(for: .audio)
        microphonePermissionGranted = microphoneStatus == .authorized
        switch microphoneStatus {
        case .authorized:
            microphonePermissionStatus = "已允许"
        case .denied:
            microphonePermissionStatus = "已拒绝"
        case .restricted:
            microphonePermissionStatus = "受限制"
        case .notDetermined:
            microphonePermissionStatus = "未请求"
        @unknown default:
            microphonePermissionStatus = "未知"
        }

        swapAxes = settings.swapAxes
        invertX = settings.invertX
        invertY = settings.invertY
        sensitivity = settings.sensitivity
        knobMode = settings.knobMode
        screenTimeoutSeconds = settings.screenTimeoutSeconds
        powerSaveTimeoutMinutes = settings.powerSaveTimeoutMinutes
        knobVolumeStep = settings.knobVolumeStep
        appleTVRuntimeInstalled = AppleTVNativeClient().isRuntimeInstalled
        selectedAppleTVID = selectedAppleTVID.isEmpty ? settings.appleTVIdentifier : selectedAppleTVID
        appleTVPairingProtocol = settings.appleTVPairingProtocol
        if settings.appleTVConfigured {
            let name = settings.appleTVName.isEmpty ? settings.appleTVIdentifier : settings.appleTVName
            appleTVStatus = appleTVRuntimeInstalled ? "已配置 \(name)" : "需要安装运行时"
        } else if appleTVRuntimeInstalled {
            appleTVStatus = "可扫描 Apple TV"
        } else {
            appleTVStatus = "需要安装运行时"
        }
    }

    var selectedAppleTVDevice: AppleTVNativeDevice? {
        appleTVDevices.first { $0.id == selectedAppleTVID }
    }

    var selectedAppleTVProtocols: [String] {
        let protocols = selectedAppleTVDevice?.services
            .filter { $0.enabled && $0.pairing != "Unsupported" && $0.pairing != "Disabled" }
            .map(\.proto) ?? []
        return protocols.isEmpty ? ["airplay"] : protocols
    }

    var canStartAppleTVPairing: Bool {
        appleTVRuntimeInstalled && !selectedAppleTVID.isEmpty && !appleTVPairingActive
    }

    func updateAppleTVStatus(_ status: String) {
        appleTVStatus = status
    }

    func updateAppleTVPin(_ pin: String) {
        setAppleTVPin(pin)
    }

    func updateAppleTVDevices(_ devices: [AppleTVNativeDevice]) {
        appleTVDevices = devices
        if selectedAppleTVID.isEmpty || !devices.contains(where: { $0.id == selectedAppleTVID }) {
            selectedAppleTVID = devices.first?.id ?? ""
        }
        if let device = selectedAppleTVDevice {
            appleTVPairingProtocol = device.preferredPairingProtocol
        }
        appleTVStatus = devices.isEmpty ? "没有发现 Apple TV" : "发现 \(devices.count) 台设备"
    }

    func setSelectedAppleTVID(_ id: String) {
        selectedAppleTVID = id
        if let device = selectedAppleTVDevice {
            appleTVPairingProtocol = device.preferredPairingProtocol
        }
    }

    func setAppleTVPairingProtocol(_ value: String) {
        appleTVPairingProtocol = value
    }

    func setAppleTVPin(_ value: String) {
        appleTVPin = String(value.filter(\.isNumber).prefix(8))
    }

    func updateStatus(connected: Bool, knobStatus: String) {
        self.connected = connected
        connectionStatus = connected ? "已连接" : "未连接"
        self.knobStatus = knobStatus
            .replacingOccurrences(of: "Knob: ", with: "")
            .replacingOccurrences(of: "旋钮：", with: "")
    }

    func updateMessage(_ message: String) {
        messageStatus = message
    }

    func updateAudioStatus(_ message: String, active: Bool) {
        microphoneStatus = message
        audioTestActive = active
        refresh()
    }

    func setSwapAxes(_ value: Bool) {
        swapAxes = value
        commitInputSettings()
    }

    func setInvertX(_ value: Bool) {
        invertX = value
        commitInputSettings()
    }

    func setInvertY(_ value: Bool) {
        invertY = value
        commitInputSettings()
    }

    func setSensitivity(_ value: Double) {
        sensitivity = min(100, max(1, Int(round(value))))
        commitInputSettings()
    }

    func setKnobMode(_ value: Int) {
        knobMode = value
        commitInputSettings()
    }

    func setScreenTimeout(_ value: Double) {
        screenTimeoutSeconds = Int(round(value))
        commitPowerSettings()
    }

    func setPowerSaveTimeout(_ value: Double) {
        powerSaveTimeoutMinutes = Int(round(value))
        commitPowerSettings()
    }

    func setKnobVolumeStep(_ value: Double) {
        knobVolumeStep = min(20, max(1, Int(round(value))))
        settings.knobVolumeStep = knobVolumeStep
        settings.synchronize()
    }

    func applyHardwareSettings(flags: UInt8, sensitivity: UInt8, knobMode: UInt8) {
        settings.applyHardware(flags: flags, sensitivity: sensitivity, knobMode: knobMode)
        refresh()
    }

    func applyPowerSettings(screenTimeoutSeconds: UInt8, powerSaveTimeoutMinutes: UInt8) {
        settings.applyPower(screenTimeoutSeconds: screenTimeoutSeconds, powerSaveTimeoutMinutes: powerSaveTimeoutMinutes)
        refresh()
    }

    private func commitInputSettings() {
        settings.swapAxes = swapAxes
        settings.invertX = invertX
        settings.invertY = invertY
        settings.sensitivity = sensitivity
        settings.knobMode = knobMode
        settings.synchronize()
        refresh()
        onSettingsChanged?()
    }

    private func commitPowerSettings() {
        settings.screenTimeoutSeconds = screenTimeoutSeconds
        settings.powerSaveTimeoutMinutes = powerSaveTimeoutMinutes
        settings.synchronize()
        refresh()
        onPowerSettingsChanged?()
    }
}

private final class SettingsWindowController: NSWindowController, NSToolbarDelegate {
    private let viewModel: ADVCtlSettingsViewModel

    var onSettingsChanged: (() -> Void)? {
        get { viewModel.onSettingsChanged }
        set { viewModel.onSettingsChanged = newValue }
    }
    var onPowerSettingsChanged: (() -> Void)? {
        get { viewModel.onPowerSettingsChanged }
        set { viewModel.onPowerSettingsChanged = newValue }
    }
    var onAudioTestChanged: ((Bool) -> Void)? {
        get { viewModel.onAudioTestChanged }
        set { viewModel.onAudioTestChanged = newValue }
    }
    var onRefreshSettings: (() -> Void)? {
        get { viewModel.onRefreshSettings }
        set { viewModel.onRefreshSettings = newValue }
    }
    var onResetHardwareSettings: (() -> Void)? {
        get { viewModel.onResetHardwareSettings }
        set { viewModel.onResetHardwareSettings = newValue }
    }
    var onInstallApp: (() -> Void)? {
        get { viewModel.onInstallApp }
        set { viewModel.onInstallApp = newValue }
    }
    var onInstallAudioDriver: (() -> Void)? {
        get { viewModel.onInstallAudioDriver }
        set { viewModel.onInstallAudioDriver = newValue }
    }
    var onLaunchAtLoginChanged: ((Bool) -> Void)? {
        get { viewModel.onLaunchAtLoginChanged }
        set { viewModel.onLaunchAtLoginChanged = newValue }
    }
    var onRequestInputMonitoring: (() -> Void)? {
        get { viewModel.onRequestInputMonitoring }
        set { viewModel.onRequestInputMonitoring = newValue }
    }
    var onRequestAccessibility: (() -> Void)? {
        get { viewModel.onRequestAccessibility }
        set { viewModel.onRequestAccessibility = newValue }
    }
    var onRequestMicrophone: (() -> Void)? {
        get { viewModel.onRequestMicrophone }
        set { viewModel.onRequestMicrophone = newValue }
    }
    var onOpenBluetooth: (() -> Void)? {
        get { viewModel.onOpenBluetooth }
        set { viewModel.onOpenBluetooth = newValue }
    }
    var onInstallAppleTVRuntime: (() -> Void)? {
        get { viewModel.onInstallAppleTVRuntime }
        set { viewModel.onInstallAppleTVRuntime = newValue }
    }
    var onScanAppleTV: (() -> Void)? {
        get { viewModel.onScanAppleTV }
        set { viewModel.onScanAppleTV = newValue }
    }
    var onStartAppleTVPairing: ((String, String) -> Void)? {
        get { viewModel.onStartAppleTVPairing }
        set { viewModel.onStartAppleTVPairing = newValue }
    }
    var onSubmitAppleTVPin: ((String) -> Void)? {
        get { viewModel.onSubmitAppleTVPin }
        set { viewModel.onSubmitAppleTVPin = newValue }
    }

    init(settings: JoystickSettings) {
        viewModel = ADVCtlSettingsViewModel(settings: settings)
        let rootView = ADVCtlSettingsRootView(model: viewModel)
            .frame(minWidth: 840, minHeight: 620)
        let hostingController = NSHostingController(rootView: rootView)
        let window = NSWindow(contentViewController: hostingController)
        window.setContentSize(NSSize(width: 900, height: 680))
        window.styleMask = [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView]
        window.title = ""
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        window.isMovableByWindowBackground = true
        window.isReleasedWhenClosed = false
        super.init(window: window)
        let toolbar = NSToolbar(identifier: NSToolbar.Identifier("ADVCtlSettingsToolbar"))
        toolbar.delegate = self
        toolbar.displayMode = .iconOnly
        toolbar.showsBaselineSeparator = false
        window.toolbar = toolbar
        window.toolbarStyle = .unified
        window.title = ""
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        refresh()
    }

    required init?(coder: NSCoder) {
        nil
    }

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.toggleSidebar, .flexibleSpace]
    }

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.toggleSidebar]
    }

    func toolbar(_ toolbar: NSToolbar,
                 itemForItemIdentifier itemIdentifier: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        guard itemIdentifier == .toggleSidebar else {
            return nil
        }
        let item = NSToolbarItem(itemIdentifier: .toggleSidebar)
        item.label = "边栏"
        item.paletteLabel = "边栏"
        item.toolTip = "显示或隐藏边栏"
        item.target = nil
        item.action = #selector(NSSplitViewController.toggleSidebar(_:))
        return item
    }

    func updateStatus(connected: Bool, knobStatus: String) {
        viewModel.updateStatus(connected: connected, knobStatus: knobStatus)
    }

    func updateMessage(_ message: String) {
        viewModel.updateMessage(message)
    }

    func updateAudioStatus(_ message: String, active: Bool) {
        viewModel.updateAudioStatus(message, active: active)
    }

    func updateAppleTVStatus(_ message: String, pairingActive: Bool? = nil) {
        viewModel.updateAppleTVStatus(message)
        if let pairingActive {
            viewModel.appleTVPairingActive = pairingActive
        }
    }

    func updateAppleTVPin(_ pin: String) {
        viewModel.updateAppleTVPin(pin)
    }

    func updateAppleTVDevices(_ devices: [AppleTVNativeDevice]) {
        viewModel.updateAppleTVDevices(devices)
    }

    func refresh() {
        viewModel.refresh()
    }

    func applyHardwareSettings(flags: UInt8, sensitivity: UInt8, knobMode: UInt8) {
        viewModel.applyHardwareSettings(flags: flags, sensitivity: sensitivity, knobMode: knobMode)
    }

    func applyPowerSettings(screenTimeoutSeconds: UInt8, powerSaveTimeoutMinutes: UInt8) {
        viewModel.applyPowerSettings(screenTimeoutSeconds: screenTimeoutSeconds,
                                     powerSaveTimeoutMinutes: powerSaveTimeoutMinutes)
    }

    func selectInstallPage() {
        viewModel.selectedPage = .install
    }
}

private struct ADVCtlSettingsRootView: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        NavigationSplitView {
            List(selection: $model.selectedPage) {
                ForEach(ADVCtlSettingsPage.allCases, id: \.self) { page in
                    Label(page.title, systemImage: page.symbol)
                        .tag(page)
                }
            }
            .listStyle(.sidebar)
            .navigationSplitViewColumnWidth(180)
        } detail: {
            ADVCtlSettingsDetailView(model: model)
        }
        .onAppear {
            model.refresh()
        }
    }
}

private struct ADVCtlSettingsDetailView: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        switch model.selectedPage {
        case .install:
            ADVCtlInstallGuideView(model: model)
        case .general:
            ADVCtlGeneralView(model: model)
        case .adv:
            ADVCtlStatusView(model: model)
        case .pointer:
            ADVCtlPointerView(model: model)
        case .knob:
            ADVCtlKnobView(model: model)
        case .audio:
            ADVCtlAudioView(model: model)
        case .power:
            ADVCtlPowerView(model: model)
        case .privacy:
            ADVCtlPrivacyView(model: model)
        }
    }
}

private struct ADVCtlInstallGuideView: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        SettingsScrollPage {
            SettingsHeader(title: "安装向导",
                           subtitle: "ADVCtl 会在应用、音频驱动或系统权限缺失时打开这里，按顺序完成本机安装和 macOS 权限。")

            if model.needsInstallGuide {
                SettingsCallout(title: "设置未完成",
                                message: "完成本页项目后，状态栏应用、ADV 输入控制和虚拟麦克风会在登录后自动可用。",
                                symbol: "exclamationmark.triangle.fill")
            }

            SettingsGroup("安装与启动") {
                ActionStatusRow(title: "ADVCtl.app",
                                detail: model.appInstallStatus,
                                symbol: model.appInstalled ? "checkmark.circle.fill" : "app.badge",
                                ok: model.appInstalled,
                                buttonTitle: "安装到 /Applications",
                                action: { model.onInstallApp?() })
                ActionStatusRow(title: "ADVCtlAudio.driver",
                                detail: model.audioDriverStatus,
                                symbol: model.audioDriverInstalled ? "checkmark.circle.fill" : "waveform.badge.mic",
                                ok: model.audioDriverInstalled,
                                buttonTitle: model.audioDriverInstalled ? "重新安装驱动" : "安装音频驱动",
                                action: { model.onInstallAudioDriver?() })
                ToggleStatusRow(title: "登录启动",
                                detail: model.launchAtLoginDetail,
                                symbol: "power.circle.fill",
                                isOn: Binding(get: { model.launchAtLoginEnabled },
                                              set: { model.onLaunchAtLoginChanged?($0) }))
            }

            SettingsGroup("权限") {
                PermissionStatusRow(title: "输入监听",
                                    detail: model.inputMonitoringStatus,
                                    ok: model.inputMonitoringGranted,
                                    action: { model.onRequestInputMonitoring?() })
                PermissionStatusRow(title: "辅助功能",
                                    detail: model.accessibilityStatus,
                                    ok: model.accessibilityGranted,
                                    action: { model.onRequestAccessibility?() })
                PermissionStatusRow(title: "麦克风",
                                    detail: model.microphonePermissionStatus,
                                    ok: model.microphonePermissionGranted,
                                    action: { model.onRequestMicrophone?() })
                ActionStatusRow(title: "蓝牙",
                                detail: "配对 ADVCtl",
                                symbol: "dot.radiowaves.left.and.right",
                                ok: true,
                                buttonTitle: "打开蓝牙",
                                action: { model.onOpenBluetooth?() })
            }

            HStack {
                Button {
                    model.refresh()
                } label: {
                    Label("刷新", systemImage: "arrow.clockwise")
                }
            }
        }
    }

    private func permissionsSummary(_ model: ADVCtlSettingsViewModel) -> String {
        let missing = [
            model.inputMonitoringGranted ? nil : "输入监听",
            model.accessibilityGranted ? nil : "辅助功能",
            model.microphonePermissionGranted ? nil : "麦克风",
        ].compactMap { $0 }
        return missing.isEmpty ? "已允许" : "缺少 \(missing.joined(separator: "、"))"
    }

    private func permissionsReady(_ model: ADVCtlSettingsViewModel) -> Bool {
        model.inputMonitoringGranted && model.accessibilityGranted && model.microphonePermissionGranted
    }
}

private struct ADVCtlGeneralView: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        SettingsScrollPage {
            SettingsHeader(title: "通用", subtitle: "管理 ADVCtl 的安装位置、登录启动和蓝牙配对入口。")
            SettingsGroup("ADVCtl") {
                ActionStatusRow(title: "应用",
                                detail: model.appInstallStatus,
                                symbol: model.appInstalled ? "checkmark.circle.fill" : "app.badge",
                                ok: model.appInstalled,
                                buttonTitle: "安装到 /Applications",
                                action: { model.onInstallApp?() })
                ToggleStatusRow(title: "登录后自动启动",
                                detail: model.launchAtLoginDetail,
                                symbol: "power.circle.fill",
                                isOn: Binding(get: { model.launchAtLoginEnabled },
                                              set: { model.onLaunchAtLoginChanged?($0) }))
                ActionStatusRow(title: "蓝牙",
                                detail: "配对 ADVCtl",
                                symbol: "dot.radiowaves.left.and.right",
                                ok: true,
                                buttonTitle: "打开蓝牙",
                                action: { model.onOpenBluetooth?() })
            }
        }
    }
}

private struct ADVCtlStatusView: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        SettingsScrollPage {
            SettingsHeader(title: "ADV", subtitle: "查看 ADV 蓝牙连接、旋钮输入和最近的硬件同步状态。")
            SettingsGroup("状态与同步") {
                StatusRow(title: "连接", detail: model.connectionStatus, ok: model.connected)
                StatusRow(title: "旋钮", detail: model.knobStatus, ok: model.connected)
                ActionStatusRow(title: "硬件配置",
                                detail: model.messageStatus,
                                symbol: "arrow.triangle.2.circlepath",
                                ok: true,
                                buttonTitle: "从 ADV 刷新",
                                action: { model.onRefreshSettings?() })
            }
        }
    }
}

private struct ADVCtlPointerView: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        SettingsScrollPage {
            SettingsHeader(title: "指针", subtitle: "调整摇杆方向、轴映射和灵敏度。这些设置会写入 ADV 固件。")
            SettingsGroup("指针") {
                ToggleRow(title: "交换 X/Y 轴",
                          isOn: Binding(get: { model.swapAxes }, set: { model.setSwapAxes($0) }))
                ToggleRow(title: "反转左右",
                          isOn: Binding(get: { model.invertX }, set: { model.setInvertX($0) }))
                ToggleRow(title: "反转上下",
                          isOn: Binding(get: { model.invertY }, set: { model.setInvertY($0) }))
                SliderRow(title: "摇杆灵敏度",
                          valueText: model.sensitivityLabel,
                          value: Binding(get: { Double(model.sensitivity) }, set: { model.setSensitivity($0) }),
                          range: 1...100,
                          step: 1)
            }
        }
    }
}

private struct ADVCtlKnobView: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        SettingsScrollPage {
            SettingsHeader(title: "旋钮", subtitle: "设置旋转行为。HomePod/Apple TV 音量由 ADVCtl.app 本机配对后执行。")
            SettingsGroup("旋钮") {
                PickerRow(title: "旋转",
                          selection: Binding(get: { model.knobMode }, set: { model.setKnobMode($0) }),
                          options: ["系统音量", "媒体音量", "禁用"])
                StatusRow(title: "按下", detail: "系统模式静音，媒体模式发送 F15", ok: true)
                ActionRow(title: "默认值", buttonTitle: "恢复硬件默认值") {
                    model.onResetHardwareSettings?()
                }
            }
            SettingsGroup("Apple TV") {
                ActionStatusRow(title: "运行时",
                                detail: model.appleTVRuntimeInstalled ? "已安装" : "未安装",
                                symbol: model.appleTVRuntimeInstalled ? "checkmark.circle.fill" : "shippingbox",
                                ok: model.appleTVRuntimeInstalled,
                                buttonTitle: model.appleTVRuntimeInstalled ? "重新安装" : "安装 pyatv",
                                action: { model.onInstallAppleTVRuntime?() })
                ActionStatusRow(title: "发现",
                                detail: model.appleTVStatus,
                                symbol: model.appleTVDevices.isEmpty ? "appletv" : "checkmark.circle.fill",
                                ok: !model.appleTVDevices.isEmpty,
                                buttonTitle: "扫描",
                                action: { model.onScanAppleTV?() })
                SliderRow(title: "每格音量",
                          valueText: model.knobVolumeStepLabel,
                          value: Binding(get: { Double(model.knobVolumeStep) }, set: { model.setKnobVolumeStep($0) }),
                          range: 1...20,
                          step: 1)
                if !model.appleTVDevices.isEmpty {
                    AppleTVDevicePickerRow(model: model)
                    AppleTVProtocolPickerRow(model: model)
                    ActionRow(title: "配对", buttonTitle: model.appleTVPairingActive ? "等待 PIN" : "开始配对") {
                        model.onStartAppleTVPairing?(model.selectedAppleTVID, model.appleTVPairingProtocol)
                    }
                    AppleTVPinRow(model: model)
                }
            }
        }
    }
}

private struct AppleTVDevicePickerRow: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        SettingsRow(title: "设备") {
            Picker("", selection: Binding(get: { model.selectedAppleTVID },
                                          set: { model.setSelectedAppleTVID($0) })) {
                ForEach(model.appleTVDevices) { device in
                    Text(device.name).tag(device.id)
                }
            }
            .labelsHidden()
            .frame(width: 220)
        }
    }
}

private struct AppleTVProtocolPickerRow: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        SettingsRow(title: "协议") {
            Picker("", selection: Binding(get: { model.appleTVPairingProtocol },
                                          set: { model.setAppleTVPairingProtocol($0) })) {
                ForEach(model.selectedAppleTVProtocols, id: \.self) { proto in
                    Text(proto.uppercased()).tag(proto)
                }
            }
            .labelsHidden()
            .frame(width: 140)
        }
    }
}

private struct AppleTVPinRow: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        SettingsRow(title: "PIN") {
            HStack(spacing: 10) {
                TextField("0000", text: Binding(get: { model.appleTVPin },
                                                set: { model.setAppleTVPin($0) }))
                    .frame(width: 82)
                    .multilineTextAlignment(.center)
                Button("完成") {
                    model.onSubmitAppleTVPin?(model.appleTVPin)
                }
                .disabled(model.appleTVPin.isEmpty || !model.appleTVPairingActive)
            }
        }
    }
}

private struct ADVCtlAudioView: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        SettingsScrollPage {
            SettingsHeader(title: "音频", subtitle: "系统应用读取 ADVCtlAudio 麦克风时自动激活 ADV，并通过本地环形缓冲传输音频。")
            SettingsGroup("音频链路") {
                ActionStatusRow(title: "ADVCtlAudio",
                                detail: model.audioDriverStatus,
                                symbol: model.audioDriverInstalled ? "checkmark.circle.fill" : "waveform.badge.mic",
                                ok: model.audioDriverInstalled,
                                buttonTitle: model.audioDriverInstalled ? "重新安装驱动" : "安装音频驱动",
                                action: { model.onInstallAudioDriver?() })
                AudioBridgePanel(model: model)
            }
        }
    }
}

private struct ADVCtlPowerView: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        SettingsScrollPage {
            SettingsHeader(title: "省电", subtitle: "配置屏幕关闭和深度省电时间。深度省电会关闭 BLE，按键后自动恢复连接。")
            SettingsGroup("省电") {
                SliderRow(title: "关闭屏幕",
                          valueText: "\(model.screenTimeoutSeconds)s",
                          value: Binding(get: { Double(model.screenTimeoutSeconds) },
                                         set: { model.setScreenTimeout($0) }),
                          range: 5...120,
                          step: 5)
                SliderRow(title: "深度省电",
                          valueText: "\(model.powerSaveTimeoutMinutes)m",
                          value: Binding(get: { Double(model.powerSaveTimeoutMinutes) },
                                         set: { model.setPowerSaveTimeout($0) }),
                          range: 1...30,
                          step: 1)
            }
        }
    }
}

private struct ADVCtlPrivacyView: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        SettingsScrollPage {
            SettingsHeader(title: "隐私与安全性", subtitle: "集中管理 ADVCtl 需要的 macOS 权限。")
            SettingsGroup("权限") {
                PermissionStatusRow(title: "输入监听",
                                    detail: model.inputMonitoringStatus,
                                    ok: model.inputMonitoringGranted,
                                    action: { model.onRequestInputMonitoring?() })
                PermissionStatusRow(title: "辅助功能",
                                    detail: model.accessibilityStatus,
                                    ok: model.accessibilityGranted,
                                    action: { model.onRequestAccessibility?() })
                PermissionStatusRow(title: "麦克风",
                                    detail: model.microphonePermissionStatus,
                                    ok: model.microphonePermissionGranted,
                                    action: { model.onRequestMicrophone?() })
            }
        }
    }
}

private struct SettingsScrollPage<Content: View>: View {
    @ViewBuilder let content: () -> Content

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                content()
            }
            .padding(28)
            .frame(maxWidth: 680, alignment: .leading)
        }
        .background(Color(nsColor: .windowBackgroundColor))
    }
}

private struct SettingsHeader: View {
    let title: String
    let subtitle: String

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(title)
                .font(.largeTitle.weight(.semibold))
            Text(subtitle)
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.bottom, 2)
    }
}

private struct SettingsCallout: View {
    let title: String
    let message: String
    let symbol: String

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: symbol)
                .font(.title3)
                .foregroundStyle(.orange)
                .frame(width: 24)
            VStack(alignment: .leading, spacing: 4) {
                Text(title)
                    .font(.headline)
                Text(message)
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(14)
        .background(.orange.opacity(0.08), in: RoundedRectangle(cornerRadius: 8))
    }
}

private struct StatusGrid: View {
    let items: [(title: String, detail: String, ok: Bool)]

    var body: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 180), spacing: 12)], spacing: 12) {
            ForEach(items, id: \.title) { item in
                VStack(alignment: .leading, spacing: 8) {
                    HStack(spacing: 7) {
                        Image(systemName: item.ok ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                            .foregroundStyle(item.ok ? .green : .orange)
                        Text(item.title)
                            .font(.headline)
                    }
                    Text(item.detail)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .lineLimit(3)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(14)
                .frame(maxWidth: .infinity, minHeight: 96, alignment: .topLeading)
                .background(.quaternary.opacity(0.42), in: RoundedRectangle(cornerRadius: 8))
            }
        }
    }
}

private struct SettingsGroup<Content: View>: View {
    let title: String
    @ViewBuilder let content: () -> Content

    init(_ title: String, @ViewBuilder content: @escaping () -> Content) {
        self.title = title
        self.content = content
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            Text(title)
                .font(.headline)
            VStack(spacing: 0) {
                content()
            }
            .padding(.vertical, 2)
            .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: 8))
        }
    }
}

private struct StatusRow: View {
    let title: String
    let detail: String
    let ok: Bool

    var body: some View {
        SettingsRow(title: title) {
            Text(detail)
                .foregroundStyle(ok ? .green : .secondary)
                .lineLimit(2)
                .multilineTextAlignment(.trailing)
        }
    }
}

private struct ActionRow: View {
    let title: String
    let buttonTitle: String
    let action: () -> Void

    var body: some View {
        SettingsRow(title: title) {
            Button(buttonTitle, action: action)
        }
    }
}

private struct ActionStatusRow: View {
    let title: String
    let detail: String
    let symbol: String
    let ok: Bool
    let buttonTitle: String
    let action: () -> Void

    var body: some View {
        SettingsRow(title: title) {
            HStack(spacing: 12) {
                Image(systemName: symbol)
                    .foregroundStyle(ok ? .green : .orange)
                Text(detail)
                    .foregroundStyle(ok ? .green : .secondary)
                Button(buttonTitle, action: action)
            }
        }
    }
}

private struct ToggleRow: View {
    let title: String
    @Binding var isOn: Bool

    var body: some View {
        SettingsRow(title: title) {
            Toggle("", isOn: $isOn)
                .labelsHidden()
        }
    }
}

private struct ToggleStatusRow: View {
    let title: String
    let detail: String
    let symbol: String
    @Binding var isOn: Bool

    var body: some View {
        SettingsRow(title: title) {
            HStack(spacing: 12) {
                Image(systemName: symbol)
                    .foregroundStyle(isOn ? .green : .secondary)
                Text(detail)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
                Toggle("", isOn: $isOn)
                    .labelsHidden()
            }
        }
    }
}

private struct PermissionStatusRow: View {
    let title: String
    let detail: String
    let ok: Bool
    let action: () -> Void

    var body: some View {
        SettingsRow(title: title) {
            HStack(spacing: 12) {
                Image(systemName: ok ? "checkmark.circle.fill" : "lock.open.trianglebadge.exclamationmark")
                    .foregroundStyle(ok ? .green : .orange)
                Text(detail)
                    .foregroundStyle(ok ? .green : .secondary)
                    .frame(minWidth: 72, alignment: .trailing)
                Button(ok ? "已设置" : "设置", action: action)
                    .disabled(ok)
            }
        }
    }
}

private struct SliderRow: View {
    let title: String
    let valueText: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double

    var body: some View {
        SettingsRow(title: title) {
            HStack(spacing: 10) {
                Slider(value: $value, in: range, step: step)
                    .frame(width: 190)
                Text(valueText)
                    .foregroundStyle(.secondary)
                    .frame(width: 42, alignment: .trailing)
            }
        }
    }
}

private struct PickerRow: View {
    let title: String
    @Binding var selection: Int
    let options: [String]

    var body: some View {
        SettingsRow(title: title) {
            Picker("", selection: $selection) {
                ForEach(Array(options.enumerated()), id: \.offset) { index, option in
                    Text(option).tag(index)
                }
            }
            .labelsHidden()
            .frame(width: 180)
        }
    }
}

private struct SettingsRow<Trailing: View>: View {
    let title: String
    @ViewBuilder let trailing: () -> Trailing

    var body: some View {
        HStack(alignment: .center, spacing: 18) {
            Text(title)
                .foregroundStyle(.primary)
            Spacer(minLength: 24)
            trailing()
                .font(.callout)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .frame(minHeight: 44)
    }
}

private struct AudioBridgePanel: View {
    @ObservedObject var model: ADVCtlSettingsViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                Label("ADV 麦克风", systemImage: model.audioTestActive ? "mic.fill" : "mic")
                    .font(.headline)
                Spacer(minLength: 16)
                StatusBadge(text: model.microphoneStatus, ok: model.audioTestActive)
                Toggle("", isOn: Binding(get: { model.audioTestActive },
                                         set: { model.onAudioTestChanged?($0) }))
                    .labelsHidden()
            }

            ModernWaveformView(active: model.audioTestActive)
                .frame(height: 132)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
    }
}

private struct ModernWaveformView: View {
    let active: Bool

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: !active)) { timeline in
            Canvas { context, size in
                drawWaveform(in: &context, size: size, time: timeline.date.timeIntervalSinceReferenceDate)
            }
        }
        .background(
            LinearGradient(colors: [Color(nsColor: .controlBackgroundColor),
                                    Color(nsColor: .windowBackgroundColor)],
                           startPoint: .topLeading,
                           endPoint: .bottomTrailing),
            in: RoundedRectangle(cornerRadius: 8)
        )
        .overlay {
            RoundedRectangle(cornerRadius: 8)
                .stroke(.secondary.opacity(0.16), lineWidth: 1)
        }
    }

    private func drawWaveform(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let bars = 48
        let gap: CGFloat = 3
        let barWidth = max(3, (size.width - CGFloat(bars - 1) * gap) / CGFloat(bars))
        let midY = size.height / 2
        let maxHeight = size.height * 0.72

        for index in 0..<bars {
            let progress = Double(index) / Double(max(1, bars - 1))
            let envelope = 0.42 + 0.58 * sin(progress * .pi)
            let moving = abs(sin(time * 3.1 + Double(index) * 0.34)) * 0.62 +
                abs(sin(time * 1.7 + Double(index) * 0.19)) * 0.38
            let idle = 0.16 + 0.08 * sin(Double(index) * 0.51)
            let value = active ? max(0.12, moving * envelope) : idle
            let height = max(5, CGFloat(value) * maxHeight)
            let x = CGFloat(index) * (barWidth + gap)
            let rect = CGRect(x: x, y: midY - height / 2, width: barWidth, height: height)
            let path = Path(roundedRect: rect, cornerRadius: barWidth / 2)
            let top = CGPoint(x: rect.midX, y: rect.minY)
            let bottom = CGPoint(x: rect.midX, y: rect.maxY)
            let colors = active ? [Color.accentColor, Color.green] : [Color.secondary.opacity(0.34), Color.secondary.opacity(0.18)]
            context.fill(path, with: .linearGradient(Gradient(colors: colors), startPoint: top, endPoint: bottom))
        }
    }
}

private struct StatusBadge: View {
    let text: String
    let ok: Bool

    var body: some View {
        Label(text, systemImage: ok ? "checkmark.circle.fill" : "circle")
            .font(.callout)
            .foregroundStyle(ok ? .green : .secondary)
            .lineLimit(1)
    }
}

private final class ADVCtlAppDelegate: NSObject, NSApplicationDelegate, ADVCtlBridgeDelegate, ObservableObject {
    private let launchAtLoginDefaultsKey = "launchAtLoginEnabled"
    private let settings = JoystickSettings()
    private let appleTVNative = AppleTVNativeClient()
    private lazy var bridge = ADVCtlBridge(config: HelperConfig.load())
    private lazy var settingsWindow = SettingsWindowController(settings: settings)
    private var connected = false
    private var knobStatus = "旋钮：无输入"
    private var lastMessage = "启动中"
    private var appleTVPairingSession: AppleTVNativePairingSession?
    private var appleTVDevices: [AppleTVNativeDevice] = []
    private var globalKeyMonitor: Any?
    private var localKeyMonitor: Any?
    private var didCompleteLaunchSetup = false
    private var statusItem: NSStatusItem?
    @Published var menuBarTitle = "ADVCtl"

    func applicationDidFinishLaunching(_ notification: Notification) {
        start()
    }

    func start() {
        guard !didCompleteLaunchSetup else {
            return
        }
        didCompleteLaunchSetup = true
        let launchArgumentText = ProcessInfo.processInfo.arguments.joined(separator: " ")
        let shouldOpenInstallGuide = launchArgumentText.contains("--show-install")
        let shouldOpenSettings = launchArgumentText.contains("--show-settings")
        if handOffToRunningInstance(showInstallGuide: shouldOpenInstallGuide,
                                    showSettings: shouldOpenSettings) {
            return
        }
        registerSingleInstanceNotifications()
        configureStatusItem()
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
        settingsWindow.onInstallAppleTVRuntime = { [weak self] in
            self?.installAppleTVRuntime()
        }
        settingsWindow.onScanAppleTV = { [weak self] in
            self?.scanAppleTV()
        }
        settingsWindow.onStartAppleTVPairing = { [weak self] identifier, proto in
            self?.startAppleTVPairing(identifier: identifier, proto: proto)
        }
        settingsWindow.onSubmitAppleTVPin = { [weak self] pin in
            self?.submitAppleTVPairingPin(pin)
        }
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
        DistributedNotificationCenter.default().removeObserver(self)
        bridge.sendAudioTest(active: false)
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        openSettings()
        return true
    }

    func showSettings() {
        openSettings()
    }

    func showInstallGuide() {
        openInstallGuide()
    }

    private func installAppleTVRuntime() {
        settingsWindow.updateAppleTVStatus("正在安装 pyatv")
        Task {
            do {
                try await appleTVNative.installRuntime()
                await MainActor.run {
                    settingsWindow.refresh()
                    settingsWindow.updateAppleTVStatus("pyatv 已安装")
                }
            } catch {
                await MainActor.run {
                    settingsWindow.updateAppleTVStatus("pyatv 安装失败：\(error.localizedDescription)")
                }
            }
        }
    }

    private func scanAppleTV() {
        settingsWindow.updateAppleTVStatus("正在扫描 Apple TV")
        Task {
            do {
                let devices = try await appleTVNative.scan()
                await MainActor.run {
                    appleTVDevices = devices
                    settingsWindow.updateAppleTVDevices(devices)
                }
            } catch {
                await MainActor.run {
                    settingsWindow.updateAppleTVStatus("扫描失败：\(error.localizedDescription)")
                }
            }
        }
    }

    private func startAppleTVPairing(identifier: String, proto: String) {
        guard !identifier.isEmpty else {
            settingsWindow.updateAppleTVStatus("请先选择 Apple TV")
            return
        }
        appleTVPairingSession?.cancel()
        settingsWindow.updateAppleTVPin("")
        settingsWindow.updateAppleTVStatus("正在开始配对", pairingActive: true)
        do {
            appleTVPairingSession = try appleTVNative.startPairing(identifier: identifier, proto: proto) { [weak self] event in
                DispatchQueue.main.async {
                    self?.handleAppleTVPairingEvent(event, identifier: identifier, proto: proto)
                }
            }
        } catch {
            settingsWindow.updateAppleTVStatus("配对启动失败：\(error.localizedDescription)", pairingActive: false)
        }
    }

    private func submitAppleTVPairingPin(_ pin: String) {
        guard !pin.isEmpty else {
            settingsWindow.updateAppleTVStatus("请输入 Apple TV 显示的 PIN")
            return
        }
        settingsWindow.updateAppleTVStatus("正在完成配对")
        appleTVPairingSession?.submit(pin: pin)
    }

    private func handleAppleTVPairingEvent(_ event: AppleTVNativePairingEvent, identifier: String, proto: String) {
        switch event.event {
        case "pin_required":
            if event.deviceProvidesPin == false, let pin = event.pin {
                settingsWindow.updateAppleTVPin(pin)
                settingsWindow.updateAppleTVStatus("在 Apple TV 上输入 PIN \(pin)", pairingActive: true)
            } else {
                settingsWindow.updateAppleTVPin("")
                settingsWindow.updateAppleTVStatus("输入 Apple TV 屏幕上的 PIN", pairingActive: true)
            }
        case "paired":
            appleTVPairingSession = nil
            settingsWindow.updateAppleTVPin("")
            if event.paired == true {
                settings.appleTVIdentifier = identifier
                settings.appleTVPairingProtocol = proto
                if let device = appleTVDevices.first(where: { $0.id == identifier }) {
                    settings.appleTVName = device.name
                }
                settings.synchronize()
                bridge.mediaConfigurationDidChange()
                settingsWindow.refresh()
                settingsWindow.updateAppleTVStatus("Apple TV 已配对", pairingActive: false)
            } else {
                settingsWindow.updateAppleTVStatus("Apple TV 配对未完成", pairingActive: false)
            }
        case "error":
            appleTVPairingSession = nil
            settingsWindow.updateAppleTVPin("")
            settingsWindow.updateAppleTVStatus("配对失败：\(event.message ?? "未知错误")", pairingActive: false)
        default:
            settingsWindow.updateAppleTVStatus(event.message ?? event.event)
        }
    }

    func quitApplication() {
        quit()
    }

    func bridgeDidUpdateConnection(deviceCount: Int) {
        connected = deviceCount > 0
        settingsWindow.updateStatus(connected: connected, knobStatus: knobStatus)
    }

    func bridgeDidReceive(command: MacCtlCommand) {
        switch command {
        case .volumeDelta(let delta):
            knobStatus = delta > 0 ? "旋钮：音量 +\(delta)" : "旋钮：音量 \(delta)"
        case .playPause:
            knobStatus = "旋钮：按下"
        }
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
        settingsWindow.updateStatus(connected: connected, knobStatus: knobStatus)
    }

    func bridgeDidUpdateAppleTVVolume(_ volumePercent: Int) {
        updateStatusItemTitle("\(volumePercent)%")
    }

    func bridgeDidReceiveHardwareSettings(flags: UInt8, sensitivity: UInt8, knobMode: UInt8) {
        settings.applyHardware(flags: flags, sensitivity: sensitivity, knobMode: knobMode)
        settingsWindow.applyHardwareSettings(flags: flags, sensitivity: sensitivity, knobMode: knobMode)
        lastMessage = "已收到硬件设置"
        settingsWindow.updateMessage(lastMessage)
    }

    func bridgeDidReceivePowerSettings(screenTimeoutSeconds: UInt8, powerSaveTimeoutMinutes: UInt8) {
        settings.applyPower(screenTimeoutSeconds: screenTimeoutSeconds, powerSaveTimeoutMinutes: powerSaveTimeoutMinutes)
        settingsWindow.applyPowerSettings(screenTimeoutSeconds: screenTimeoutSeconds,
                                          powerSaveTimeoutMinutes: powerSaveTimeoutMinutes)
        lastMessage = "已收到省电设置"
        settingsWindow.updateMessage(lastMessage)
    }

    func bridgeDidUpdateAudioStatus(_ message: String, active: Bool) {
        settingsWindow.updateAudioStatus(message, active: active)
        lastMessage = message
    }

    func bridgeDidUpdateMessage(_ message: String) {
        lastMessage = message
        settingsWindow.updateMessage(message)
    }

    private func handOffToRunningInstance(showInstallGuide: Bool, showSettings: Bool) -> Bool {
        let otherInstances = NSRunningApplication
            .runningApplications(withBundleIdentifier: advCtlBundleIdentifier)
            .filter { $0.processIdentifier != getpid() }
        guard let runningApp = otherInstances.first else {
            return false
        }

        let notificationName = showInstallGuide ? advCtlShowInstallNotification : advCtlShowSettingsNotification
        DistributedNotificationCenter.default().postNotificationName(notificationName,
                                                                     object: advCtlBundleIdentifier,
                                                                     userInfo: nil,
                                                                     deliverImmediately: true)
        runningApp.activate(options: [.activateIgnoringOtherApps])
        NSApp.terminate(nil)
        return true
    }

    private func registerSingleInstanceNotifications() {
        DistributedNotificationCenter.default().addObserver(self,
                                                           selector: #selector(handleShowSettingsNotification),
                                                           name: advCtlShowSettingsNotification,
                                                           object: nil)
        DistributedNotificationCenter.default().addObserver(self,
                                                           selector: #selector(handleShowInstallNotification),
                                                           name: advCtlShowInstallNotification,
                                                           object: nil)
    }

    private func configureStatusItem() {
        guard statusItem == nil else {
            return
        }

        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem = item
        updateStatusItemTitle(settings.appleTVConfigured ? "--%" : "")
        if let button = item.button {
            button.image = NSImage(systemSymbolName: "waveform", accessibilityDescription: "ADVCtl")
            button.imagePosition = .imageLeading
            button.toolTip = "ADVCtl"
        }

        let menu = NSMenu()
        let settingsItem = NSMenuItem(title: "打开 ADVCtl", action: #selector(openSettings), keyEquivalent: "")
        settingsItem.target = self
        menu.addItem(settingsItem)

        let installItem = NSMenuItem(title: "安装向导", action: #selector(openInstallGuide), keyEquivalent: "")
        installItem.target = self
        menu.addItem(installItem)

        menu.addItem(.separator())

        let quitItem = NSMenuItem(title: "退出 ADVCtl", action: #selector(quit), keyEquivalent: "")
        quitItem.target = self
        menu.addItem(quitItem)

        item.menu = menu
    }

    private func updateStatusItemTitle(_ title: String) {
        menuBarTitle = title
        statusItem?.button?.title = title
    }

    @objc private func handleShowSettingsNotification(_ notification: Notification) {
        openSettings()
    }

    @objc private func handleShowInstallNotification(_ notification: Notification) {
        openInstallGuide()
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
            lastMessage = "已安装 ADVCtl.app"
            settingsWindow.updateMessage(lastMessage)
        } catch {
            lastMessage = "应用安装失败"
            settingsWindow.updateMessage(lastMessage)
            log("App install failed: \(error.localizedDescription)")
        }
        settingsWindow.refresh()
    }

    private func installAudioDriver() {
        do {
            try ADVCtlInstaller.installAudioDriver()
            lastMessage = "已安装 ADVCtlAudio"
            settingsWindow.updateMessage(lastMessage)
        } catch {
            lastMessage = "音频驱动安装失败"
            settingsWindow.updateMessage(lastMessage)
            log("Audio driver install failed: \(error.localizedDescription)")
        }
        settingsWindow.refresh()
    }

    private func setLaunchAtLogin(_ enabled: Bool) {
        UserDefaults.standard.set(enabled, forKey: launchAtLoginDefaultsKey)
        do {
            try ADVCtlInstaller.setLaunchAtLogin(enabled)
            lastMessage = enabled ? "登录启动已开启" : "登录启动已关闭"
            settingsWindow.updateMessage(lastMessage)
        } catch {
            lastMessage = "登录启动设置失败"
            settingsWindow.updateMessage(lastMessage)
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

@main
private struct ADVCtlApplication: App {
    @NSApplicationDelegateAdaptor(ADVCtlAppDelegate.self) private var appDelegate

    var body: some Scene {
        Settings {
            EmptyView()
        }
    }
}
