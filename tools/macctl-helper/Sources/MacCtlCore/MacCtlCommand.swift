import Foundation

public enum MacCtlSystemKey: UInt8, Equatable {
    case controlCenter = 1
    case spotlight = 2
}

public enum MacCtlCommand: Equatable {
    case volumeDelta(Int)
    case playPause
    case systemKey(MacCtlSystemKey)

    public init?(payload: Data) {
        guard payload.count >= 2 else {
            return nil
        }

        switch payload[0] {
        case 1:
            self = .volumeDelta(Int(Int8(bitPattern: payload[1])))
        case 2:
            self = .playPause
        case 3:
            guard let key = MacCtlSystemKey(rawValue: payload[1]) else {
                return nil
            }
            self = .systemKey(key)
        default:
            return nil
        }
    }
}
