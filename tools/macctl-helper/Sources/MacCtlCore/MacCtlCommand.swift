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
}
