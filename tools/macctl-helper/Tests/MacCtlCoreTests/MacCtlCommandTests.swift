import Foundation
import XCTest
@testable import MacCtlCore

final class MacCtlCommandTests: XCTestCase {
    func testParsesVolumeDelta() {
        XCTAssertEqual(MacCtlCommand(payload: Data([1, 0xFE, 0])), .volumeDelta(-2))
        XCTAssertEqual(MacCtlCommand(payload: Data([1, 0x03, 0])), .volumeDelta(3))
    }

    func testParsesPlayPause() {
        XCTAssertEqual(MacCtlCommand(payload: Data([2, 0, 0])), .playPause)
    }

    func testRejectsUnknownPayloads() {
        XCTAssertNil(MacCtlCommand(payload: Data()))
        XCTAssertNil(MacCtlCommand(payload: Data([99, 0, 0])))
    }

    func testVolumeClamps() {
        XCTAssertEqual(targetVolumePercent(current: 50, delta: 3), 56)
        XCTAssertEqual(targetVolumePercent(current: 99, delta: 3), 100)
        XCTAssertEqual(targetVolumePercent(current: 1, delta: -3), 0)
    }
}
