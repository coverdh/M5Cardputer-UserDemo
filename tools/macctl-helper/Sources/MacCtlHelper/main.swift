import CoreBluetooth
import Foundation
import MacCtlCore

private let macCtlServiceUUID = CBUUID(string: "FFF0")
private let macCtlCommandUUID = CBUUID(string: "FFF1")

private struct HelperConfig {
    var deviceName = "MacCtl"
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
            case "--device":
                config.deviceName = value
            case "--ha-url":
                values["MACCTL_HA_URL"] = value
            case "--ha-token":
                values["MACCTL_HA_TOKEN"] = value
            case "--entity":
                values["MACCTL_HA_ENTITY"] = value
            default:
                continue
            }
        }

        let urlString = values["MACCTL_HA_URL"] ?? "http://homeassistant.orb.local:8123"
        let entity = values["MACCTL_HA_ENTITY"] ?? "media_player.ke_ting"
        config.expectedCommands = values["MACCTL_E2E_EXPECTED_COMMANDS"].flatMap(Int.init)
        if let url = URL(string: urlString), let token = values["MACCTL_HA_TOKEN"], !token.isEmpty {
            config.homeAssistant = HomeAssistantConfig(baseURL: url, token: token, entityID: entity)
        }
        return config
    }
}

private final class MacCtlBridge: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private let config: HelperConfig
    private let homeAssistant: HomeAssistantClient?
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var handledCommands = 0

    init(config: HelperConfig) {
        self.config = config
        if let haConfig = config.homeAssistant {
            self.homeAssistant = HomeAssistantClient(config: haConfig)
        } else {
            self.homeAssistant = nil
        }
        super.init()
        self.central = CBCentralManager(delegate: self, queue: .main)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else {
            print("Bluetooth state: \(central.state.rawValue)")
            return
        }
        print("Scanning for \(config.deviceName)")
        central.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let advertisedName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        guard peripheral.name == config.deviceName || advertisedName == config.deviceName else {
            return
        }

        self.peripheral = peripheral
        peripheral.delegate = self
        central.stopScan()
        print("Connecting to \(config.deviceName)")
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        print("Connected; discovering MacCtl service")
        peripheral.discoverServices([macCtlServiceUUID])
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        print("Disconnected; scanning again")
        self.peripheral = nil
        central.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            print("Service discovery failed: \(error.localizedDescription)")
            return
        }
        peripheral.services?.forEach { service in
            if service.uuid == macCtlServiceUUID {
                peripheral.discoverCharacteristics([macCtlCommandUUID], for: service)
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error {
            print("Characteristic discovery failed: \(error.localizedDescription)")
            return
        }
        service.characteristics?.forEach { characteristic in
            if characteristic.uuid == macCtlCommandUUID {
                print("Subscribed to MacCtl command notifications")
                peripheral.setNotifyValue(true, for: characteristic)
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            print("Notification failed: \(error.localizedDescription)")
            return
        }
        guard let data = characteristic.value, let command = MacCtlCommand(payload: data) else {
            print("Ignored unknown command")
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
RunLoop.main.run()
