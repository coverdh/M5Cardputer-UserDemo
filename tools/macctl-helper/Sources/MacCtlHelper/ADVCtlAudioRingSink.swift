import Darwin
import Foundation

private let advCtlAudioRingPath = "/tmp/advctl_audio_pcm.ring"
private let advCtlAudioRingMagic: UInt32 = 0x41445641
private let advCtlAudioRingVersion: UInt32 = 1
private let advCtlAudioRingHeaderSize = 64
private let advCtlAudioRingCapacityFrames = 48_000 * 5

final class ADVCtlAudioRingSink {
    private var mapped: UnsafeMutableRawPointer?
    private var mappedSize = 0
    private var fileHandle: FileHandle?
    private var ringDevice: UInt64 = 0
    private var ringInode: UInt64 = 0

    var isRunning: Bool {
        isInputRequested()
    }

    var statusText: String {
        if isInputRequested() {
            return "ADVCtlAudio is requesting input"
        }
        return isMapped ? "ADVCtlAudio driver idle" : "ADVCtlAudio driver not loaded"
    }

    private var isMapped: Bool {
        mapped != nil
    }

    init() {
        refreshDevice()
    }

    deinit {
        unmapRing()
    }

    @discardableResult func refreshDevice() -> Bool {
        if mapped != nil, isCurrentRingFileMapped() {
            return true
        }
        if mapped != nil {
            log("ADVCtlAudio ring file changed; remapping")
            unmapRing()
        }
        mappedSize = advCtlAudioRingHeaderSize + advCtlAudioRingCapacityFrames * MemoryLayout<Float32>.size
        if !FileManager.default.fileExists(atPath: advCtlAudioRingPath) {
            return false
        }
        guard let handle = try? FileHandle(forUpdating: URL(fileURLWithPath: advCtlAudioRingPath)) else {
            log("ADVCtlAudio open ring file failed: \(advCtlAudioRingPath)")
            return false
        }
        do {
            try handle.truncate(atOffset: UInt64(mappedSize))
        } catch {
            log("ADVCtlAudio truncate ring failed: \(error)")
            try? handle.close()
            return false
        }
        let fd = handle.fileDescriptor
        let pointer = mmap(nil, mappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
        guard pointer != MAP_FAILED else {
            log("ADVCtlAudio mmap failed: \(errno)")
            try? handle.close()
            return false
        }
        var fileStat = stat()
        if fstat(fd, &fileStat) == 0 {
            ringDevice = UInt64(fileStat.st_dev)
            ringInode = UInt64(fileStat.st_ino)
        }
        fileHandle = handle
        mapped = pointer
        initializeRingIfNeeded()
        return true
    }

    @discardableResult func start() -> Bool {
        refreshDevice()
    }

    func stop() {}

    func isInputRequested() -> Bool {
        guard refreshDevice(), let mapped else {
            return false
        }
        return mapped.load(fromByteOffset: 12, as: UInt32.self) != 0
    }

    func enqueueULaw(_ data: Data) {
        guard refreshDevice(), !data.isEmpty else {
            return
        }
        var samples = [Float32]()
        samples.reserveCapacity(data.count * 6)
        for byte in data {
            let decoded = Float32(Self.decodeULaw(byte)) / 32768.0
            for _ in 0..<6 {
                samples.append(decoded)
            }
        }
        write(samples)
    }

    private func initializeRingIfNeeded() {
        guard let mapped else {
            return
        }
        let magic = mapped.load(fromByteOffset: 0, as: UInt32.self)
        let version = mapped.load(fromByteOffset: 4, as: UInt32.self)
        let capacity = mapped.load(fromByteOffset: 8, as: UInt32.self)
        guard magic != advCtlAudioRingMagic ||
              version != advCtlAudioRingVersion ||
              capacity != UInt32(advCtlAudioRingCapacityFrames) else {
            return
        }
        memset(mapped, 0, mappedSize)
        mapped.storeBytes(of: advCtlAudioRingMagic, toByteOffset: 0, as: UInt32.self)
        mapped.storeBytes(of: advCtlAudioRingVersion, toByteOffset: 4, as: UInt32.self)
        mapped.storeBytes(of: UInt32(advCtlAudioRingCapacityFrames), toByteOffset: 8, as: UInt32.self)
    }

    private func isCurrentRingFileMapped() -> Bool {
        guard ringDevice != 0 || ringInode != 0 else {
            return true
        }
        var pathStat = stat()
        guard stat(advCtlAudioRingPath, &pathStat) == 0 else {
            return false
        }
        return UInt64(pathStat.st_dev) == ringDevice && UInt64(pathStat.st_ino) == ringInode
    }

    private func unmapRing() {
        if let mapped {
            munmap(mapped, mappedSize)
            self.mapped = nil
        }
        try? fileHandle?.close()
        fileHandle = nil
        ringDevice = 0
        ringInode = 0
    }

    private func write(_ samples: [Float32]) {
        guard let mapped, !samples.isEmpty else {
            return
        }
        let capacity = Int(mapped.load(fromByteOffset: 8, as: UInt32.self))
        guard capacity > 0 else {
            return
        }

        var writeIndex = mapped.load(fromByteOffset: 32, as: UInt64.self)
        var readIndex = mapped.load(fromByteOffset: 40, as: UInt64.self)
        let available = writeIndex >= readIndex ? Int(writeIndex - readIndex) : 0
        let overflow = max(0, available + samples.count - capacity)
        if overflow > 0 {
            readIndex += UInt64(overflow)
            mapped.storeBytes(of: readIndex, toByteOffset: 40, as: UInt64.self)
            let overrun = mapped.load(fromByteOffset: 56, as: UInt64.self) + 1
            mapped.storeBytes(of: overrun, toByteOffset: 56, as: UInt64.self)
        }

        for (offset, sample) in samples.enumerated() {
            let frame = Int((writeIndex + UInt64(offset)) % UInt64(capacity))
            mapped.storeBytes(of: sample,
                              toByteOffset: advCtlAudioRingHeaderSize + frame * MemoryLayout<Float32>.size,
                              as: Float32.self)
        }
        writeIndex += UInt64(samples.count)
        mapped.storeBytes(of: writeIndex, toByteOffset: 32, as: UInt64.self)
    }

    private static func decodeULaw(_ byte: UInt8) -> Int16 {
        let value = ~byte
        var sample = Int16(((Int(value & 0x0F) << 3) + 0x84) << Int((value & 0x70) >> 4))
        sample -= 0x84
        return (value & 0x80) != 0 ? -sample : sample
    }
}
