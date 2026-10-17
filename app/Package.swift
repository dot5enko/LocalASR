// swift-tools-version:5.10
import PackageDescription

let package = Package(
    name: "LocalASR",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(name: "LocalASR", path: "Sources/LocalASR")
    ]
)
