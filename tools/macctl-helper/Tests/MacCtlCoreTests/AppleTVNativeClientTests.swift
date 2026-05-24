import XCTest
@testable import MacCtlCore

final class AppleTVNativeClientTests: XCTestCase {
    func testPreferredPairingProtocolUsesAirPlayBeforeMRP() {
        let device = AppleTVNativeDevice(id: "id",
                                         name: "Living Room",
                                         address: "10.0.0.2",
                                         model: "Apple TV",
                                         services: [
                                            AppleTVNativeService(proto: "mrp",
                                                                 port: 49152,
                                                                 pairing: "Mandatory",
                                                                 enabled: true,
                                                                 hasCredentials: false),
                                            AppleTVNativeService(proto: "airplay",
                                                                 port: 7000,
                                                                 pairing: "Mandatory",
                                                                 enabled: true,
                                                                 hasCredentials: false),
                                         ])

        XCTAssertEqual(device.preferredPairingProtocol, "airplay")
    }

    func testPreferredPairingProtocolSkipsDisabledServices() {
        let device = AppleTVNativeDevice(id: "id",
                                         name: "Living Room",
                                         address: "10.0.0.2",
                                         model: "Apple TV",
                                         services: [
                                            AppleTVNativeService(proto: "airplay",
                                                                 port: 7000,
                                                                 pairing: "Disabled",
                                                                 enabled: true,
                                                                 hasCredentials: false),
                                            AppleTVNativeService(proto: "mrp",
                                                                 port: 49152,
                                                                 pairing: "Mandatory",
                                                                 enabled: true,
                                                                 hasCredentials: false),
                                         ])

        XCTAssertEqual(device.preferredPairingProtocol, "mrp")
    }

    func testAppleTVCandidateUsesModelOrName() {
        let appleTV = AppleTVNativeDevice(id: "atv",
                                          name: "Living Room",
                                          address: "10.0.0.2",
                                          model: "DeviceModel.AppleTV4KGen2",
                                          services: [])
        let renamedAppleTV = AppleTVNativeDevice(id: "atv-name",
                                                 name: "AppleTV (2)",
                                                 address: "10.0.0.3",
                                                 model: "DeviceModel.Unknown",
                                                 services: [])
        let homePod = AppleTVNativeDevice(id: "homepod",
                                          name: "客厅",
                                          address: "10.0.0.4",
                                          model: "DeviceModel.HomePodGen2",
                                          services: [])

        XCTAssertTrue(appleTV.isAppleTVCandidate)
        XCTAssertTrue(renamedAppleTV.isAppleTVCandidate)
        XCTAssertFalse(homePod.isAppleTVCandidate)
    }
}
