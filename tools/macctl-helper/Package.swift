// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "ADVCtl",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .executable(name: "ADVCtl", targets: ["MacCtlHelper"])
    ],
    targets: [
        .target(name: "MacCtlCore"),
        .executableTarget(
            name: "MacCtlHelper",
            dependencies: ["MacCtlCore"],
            exclude: ["Info.plist"],
            linkerSettings: [
                .linkedFramework("IOKit"),
                .linkedFramework("AppKit"),
                .linkedFramework("CoreAudio"),
                .unsafeFlags([
                    "-Xlinker", "-sectcreate",
                    "-Xlinker", "__TEXT",
                    "-Xlinker", "__info_plist",
                    "-Xlinker", "Sources/MacCtlHelper/Info.plist",
                ])
            ]
        ),
        .testTarget(name: "MacCtlCoreTests", dependencies: ["MacCtlCore"]),
    ]
)
