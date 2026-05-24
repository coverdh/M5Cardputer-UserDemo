import Foundation

final class JoystickSettings {
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
            let saved = defaults.integer(forKey: "advctl.joystickSensitivity")
            return saved == 0 ? 50 : min(100, max(1, saved))
        }
        set { defaults.set(min(100, max(1, newValue)), forKey: "advctl.joystickSensitivity") }
    }

    var flags: UInt8 {
        var value: UInt8 = 0
        if swapAxes { value |= 0x01 }
        if invertX { value |= 0x02 }
        if invertY { value |= 0x04 }
        return value
    }

    var sensitivityLabel: String {
        "\(sensitivity)"
    }

    var knobMode: Int {
        get { defaults.object(forKey: "advctl.knobMode") as? Int ?? 0 }
        set { defaults.set(min(2, max(0, newValue)), forKey: "advctl.knobMode") }
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
