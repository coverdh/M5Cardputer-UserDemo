import Carbon
import Foundation
import MacCtlCore

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
                values["MACCTL_HA_URL"] = value
            case "--ha-token":
                values["MACCTL_HA_TOKEN"] = value
            case "--ha-token-file":
                values["MACCTL_HA_TOKEN_FILE"] = value
            case "--entity":
                values["MACCTL_HA_ENTITY"] = value
            default:
                continue
            }
        }

        let urlString = values["MACCTL_HA_URL"] ?? "http://homeassistant.orb.local:8123"
        let entity = values["MACCTL_HA_ENTITY"] ?? "media_player.ke_ting"
        if values["MACCTL_HA_TOKEN"] == nil, let tokenFile = values["MACCTL_HA_TOKEN_FILE"] {
            values["MACCTL_HA_TOKEN"] = try? String(contentsOfFile: tokenFile).trimmingCharacters(in: .whitespacesAndNewlines)
        }
        config.expectedCommands = values["MACCTL_E2E_EXPECTED_COMMANDS"].flatMap(Int.init)
        if let url = URL(string: urlString), let token = values["MACCTL_HA_TOKEN"], !token.isEmpty {
            config.homeAssistant = HomeAssistantConfig(baseURL: url, token: token, entityID: entity)
        }
        return config
    }
}

private final class MacCtlBridge {
    private let config: HelperConfig
    private let homeAssistant: HomeAssistantClient?
    private var handledCommands = 0
    private var hotKeyRefs: [EventHotKeyRef?] = []

    init(config: HelperConfig) {
        self.config = config
        if let haConfig = config.homeAssistant {
            self.homeAssistant = HomeAssistantClient(config: haConfig)
        } else {
            self.homeAssistant = nil
        }
    }

    func start() {
        installHotKeyHandler()
        registerHotKey(keyCode: UInt32(kVK_F18), commandID: MacCtlHotKey.volumeUp.rawValue, label: "F18 volume up")
        registerHotKey(keyCode: UInt32(kVK_F19), commandID: MacCtlHotKey.volumeDown.rawValue, label: "F19 volume down")
        registerHotKey(keyCode: UInt32(kVK_F20), commandID: MacCtlHotKey.playPause.rawValue, label: "F20 play/pause")
        print("MacCtl Helper listening for BLE HID hotkeys")
    }

    private func installHotKeyHandler() {
        var eventType = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        let status = InstallEventHandler(GetApplicationEventTarget(), { _, eventRef, userData in
            guard let eventRef, let userData else {
                return noErr
            }

            var hotKeyID = EventHotKeyID()
            let status = GetEventParameter(
                eventRef,
                EventParamName(kEventParamDirectObject),
                EventParamType(typeEventHotKeyID),
                nil,
                MemoryLayout<EventHotKeyID>.size,
                nil,
                &hotKeyID
            )
            guard status == noErr else {
                return status
            }

            let bridge = Unmanaged<MacCtlBridge>.fromOpaque(userData).takeUnretainedValue()
            bridge.handleHotKey(commandID: hotKeyID.id)
            return noErr
        }, 1, &eventType, selfPtr, nil)

        if status != noErr {
            print("Hotkey handler install failed: \(status)")
        }
    }

    private func registerHotKey(keyCode: UInt32, commandID: UInt32, label: String) {
        let hotKeyID = EventHotKeyID(signature: MacCtlHotKey.signature, id: commandID)
        var hotKeyRef: EventHotKeyRef?
        let status = RegisterEventHotKey(keyCode, 0, hotKeyID, GetApplicationEventTarget(), 0, &hotKeyRef)
        if status == noErr {
            hotKeyRefs.append(hotKeyRef)
            print("Registered \(label)")
        } else {
            print("Failed to register \(label): \(status)")
        }
    }

    private func handleHotKey(commandID: UInt32) {
        guard let command = MacCtlCommand(hotKeyID: commandID) else {
            print("Ignored unknown hotkey \(commandID)")
            return
        }

        Task {
            do {
                if let homeAssistant {
                    try await homeAssistant.handle(command)
                    print("Handled \(command)")
                } else {
                    print("Received \(command); set MACCTL_HA_TOKEN to enable HA control")
                }
                self.handledCommands += 1
                if let expectedCommands = self.config.expectedCommands, self.handledCommands >= expectedCommands {
                    print("E2E complete; handled \(self.handledCommands) commands")
                    fflush(stdout)
                    exit(0)
                }
            } catch {
                print("HA request failed: \(error.localizedDescription)")
            }
        }
    }
}

private let config = HelperConfig.load()
private let bridge = MacCtlBridge(config: config)
bridge.start()
RunLoop.main.run()
