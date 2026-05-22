// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "MacCtlHelper",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .executable(name: "macctl-helper", targets: ["MacCtlHelper"])
    ],
    targets: [
        .target(name: "MacCtlCore"),
        .executableTarget(
            name: "MacCtlHelper",
            dependencies: ["MacCtlCore"],
            exclude: ["Info.plist"],
            linkerSettings: [
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
