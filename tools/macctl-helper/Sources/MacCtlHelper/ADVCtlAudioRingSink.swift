import Darwin
import Foundation

private let advCtlAudioRingPath = "/tmp/advctl_audio_pcm.ring"
private let advCtlAudioRingMagic: UInt32 = 0x41445641
private let advCtlAudioRingVersion: UInt32 = 1
private let advCtlAudioRingHeaderSize = 64
private let advCtlAudioRingCapacityFrames = 48_000 * 5
private let advCtlAudioUpsampleFactor = 6
private let advCtlAudioAdpcmUpsampleFactor = 3
private let advCtlAudioAdpcmDecodedSamples = 320

final class ADVCtlAudioRingSink {
    private var mapped: UnsafeMutableRawPointer?
    private var mappedSize = 0
    private var fileHandle: FileHandle?
    private var ringDevice: UInt64 = 0
    private var ringInode: UInt64 = 0
    private var previousUpsampleSample: Float32?

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

    func stop() {
        resetUpsampler(to: 0)
    }

    func isInputRequested() -> Bool {
        guard refreshDevice(), let mapped else {
            return false
        }
        return mapped.load(fromByteOffset: 12, as: UInt32.self) != 0
    }

    @discardableResult func enqueueULaw(_ data: Data) -> Int {
        guard refreshDevice(), !data.isEmpty else {
            return 0
        }
        var samples = [Float32]()
        samples.reserveCapacity(data.count * advCtlAudioUpsampleFactor)
        for byte in data {
            let decoded = Float32(Self.decodeULaw(byte)) / 32768.0
            appendUpsampledSample(decoded, to: &samples)
        }
        write(samples)
        return samples.count
    }

    @discardableResult func enqueueIMAADPCM(_ data: Data) -> Int {
        guard refreshDevice(), data.count >= 4 else {
            return 0
        }
        var samples = [Float32]()
        samples.reserveCapacity((data.count - 4) * 2 * advCtlAudioAdpcmUpsampleFactor)
        let predictorBits = UInt16(data[0]) | (UInt16(data[1]) << 8)
        var predictor = Int(Int16(bitPattern: predictorBits))
        var index = min(88, max(0, Int(data[2])))
        appendUpsampledAdpcmSample(Float32(predictor) / 32768.0, to: &samples)
        var decodedSamples = 1

        for byte in data.dropFirst(4) {
            let low = byte & 0x0F
            decodeIMAADPCMNibble(low, predictor: &predictor, index: &index, into: &samples)
            decodedSamples += 1
            if decodedSamples >= advCtlAudioAdpcmDecodedSamples {
                break
            }
            let high = (byte >> 4) & 0x0F
            decodeIMAADPCMNibble(high, predictor: &predictor, index: &index, into: &samples)
            decodedSamples += 1
            if decodedSamples >= advCtlAudioAdpcmDecodedSamples {
                break
            }
        }
        write(samples)
        return samples.count
    }

    @discardableResult func enqueueSilence(uLawSampleCount: Int) -> Int {
        guard refreshDevice(), uLawSampleCount > 0 else {
            return 0
        }
        let samples = [Float32](repeating: 0.0, count: uLawSampleCount * advCtlAudioUpsampleFactor)
        previousUpsampleSample = 0
        write(samples)
        return samples.count
    }

    @discardableResult func enqueuePCMSilence(sampleCount: Int) -> Int {
        guard refreshDevice(), sampleCount > 0 else {
            return 0
        }
        let samples = [Float32](repeating: 0.0, count: sampleCount * advCtlAudioAdpcmUpsampleFactor)
        resetUpsampler(to: 0)
        write(samples)
        return samples.count
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

    private func appendUpsampledSample(_ sample: Float32, to samples: inout [Float32]) {
        guard let previous = previousUpsampleSample else {
            samples.append(contentsOf: repeatElement(sample, count: advCtlAudioUpsampleFactor))
            previousUpsampleSample = sample
            return
        }

        let delta = sample - previous
        for step in 1...advCtlAudioUpsampleFactor {
            let fraction = Float32(step) / Float32(advCtlAudioUpsampleFactor)
            samples.append(previous + delta * fraction)
        }
        previousUpsampleSample = sample
    }

    private func appendUpsampledAdpcmSample(_ sample: Float32, to samples: inout [Float32]) {
        guard let previous = previousUpsampleSample else {
            samples.append(contentsOf: repeatElement(sample, count: advCtlAudioAdpcmUpsampleFactor))
            previousUpsampleSample = sample
            return
        }

        let delta = sample - previous
        for step in 1...advCtlAudioAdpcmUpsampleFactor {
            let fraction = Float32(step) / Float32(advCtlAudioAdpcmUpsampleFactor)
            samples.append(previous + delta * fraction)
        }
        previousUpsampleSample = sample
    }

    private func resetUpsampler(to sample: Float32) {
        previousUpsampleSample = sample
    }

    private func decodeIMAADPCMNibble(_ nibble: UInt8, predictor: inout Int, index: inout Int, into samples: inout [Float32]) {
        let step = Self.imaStepTable[index]
        var delta = step >> 3
        if (nibble & 0x04) != 0 { delta += step }
        if (nibble & 0x02) != 0 { delta += step >> 1 }
        if (nibble & 0x01) != 0 { delta += step >> 2 }
        predictor += (nibble & 0x08) != 0 ? -delta : delta
        predictor = min(32767, max(-32768, predictor))
        index += Self.imaIndexTable[Int(nibble & 0x0F)]
        index = min(88, max(0, index))
        appendUpsampledAdpcmSample(Float32(predictor) / 32768.0, to: &samples)
    }

    private static func decodeULaw(_ byte: UInt8) -> Int16 {
        let value = ~byte
        var sample = Int16(((Int(value & 0x0F) << 3) + 0x84) << Int((value & 0x70) >> 4))
        sample -= 0x84
        return (value & 0x80) != 0 ? -sample : sample
    }

    private static let imaIndexTable = [
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8,
    ]

    private static let imaStepTable = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
        19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
        130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
        5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
    ]
}
