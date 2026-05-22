import Foundation

public enum MacCtlCommand: Equatable {
    case volumeDelta(Int)
    case playPause

    public init?(payload: Data) {
        guard payload.count >= 2 else {
            return nil
        }

        switch payload[0] {
        case 1:
            self = .volumeDelta(Int(Int8(bitPattern: payload[1])))
        case 2:
            self = .playPause
        default:
            return nil
        }
    }

    public init?(hotKeyID: UInt32) {
        guard let hotKey = MacCtlHotKey(rawValue: hotKeyID) else {
            return nil
        }

        switch hotKey {
        case .volumeUp:
            self = .volumeDelta(1)
        case .volumeDown:
            self = .volumeDelta(-1)
        case .playPause:
            self = .playPause
        }
    }
}

public enum MacCtlHotKey: UInt32 {
    case volumeUp = 1
    case volumeDown = 2
    case playPause = 3

    public static let signature: UInt32 = 0x4D_43_54_4C
}

public func targetVolumePercent(current: Int, delta: Int, stepPercent: Int = 2) -> Int {
    min(100, max(0, current + delta * stepPercent))
}
