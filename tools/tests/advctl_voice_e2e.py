#!/usr/bin/env python3
"""End-to-end ADVCtl voice transport test.

The test reads the macOS `ADVCtl Audio` input device. That CoreAudio read sets
driver demand, ADVCtl.app forwards the demand over the existing HID control
report, ADV streams narrowband u-law speech frames back over HID report 4, and
the helper writes decoded PCM into the driver ring.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_HELPER_APP = pathlib.Path("/Applications/ADVCtl.app")
DEFAULT_DRIVER = pathlib.Path("/Library/Audio/Plug-Ins/HAL/ADVCtlAudio.driver")
DEFAULT_HELPER_LOG = pathlib.Path.home() / ".config" / "karabiner" / "advctl.log"


SWIFT_PROBE = r"""
import AudioToolbox
import CoreAudio
import Foundation

struct Options {
    var deviceName = "ADVCtl Audio"
    var duration = 12.0
    var minRMS = 0.002
    var minPeak = 0.02
    var minActiveRatio = 0.05
}

struct AudioDevice {
    let id: AudioObjectID
    let name: String
    let uid: String
}

func parseOptions() -> Options {
    var options = Options()
    var args = CommandLine.arguments.dropFirst().makeIterator()
    while let arg = args.next() {
        guard let value = args.next() else { continue }
        switch arg {
        case "--device":
            options.deviceName = value
        case "--duration":
            options.duration = Double(value) ?? options.duration
        case "--min-rms":
            options.minRMS = Double(value) ?? options.minRMS
        case "--min-peak":
            options.minPeak = Double(value) ?? options.minPeak
        case "--min-active-ratio":
            options.minActiveRatio = Double(value) ?? options.minActiveRatio
        default:
            continue
        }
    }
    return options
}

func propertyAddress(_ selector: AudioObjectPropertySelector,
                     _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: selector,
                               mScope: scope,
                               mElement: kAudioObjectPropertyElementMain)
}

func stringProperty(_ objectID: AudioObjectID, selector: AudioObjectPropertySelector) -> String? {
    var address = propertyAddress(selector)
    var value: CFString = "" as CFString
    var size = UInt32(MemoryLayout<CFString>.size)
    let status = withUnsafeMutablePointer(to: &value) {
        AudioObjectGetPropertyData(objectID, &address, 0, nil, &size, $0)
    }
    return status == noErr ? value as String : nil
}

func hasInputStreams(_ objectID: AudioObjectID) -> Bool {
    var address = propertyAddress(kAudioDevicePropertyStreams, kAudioDevicePropertyScopeInput)
    var size: UInt32 = 0
    let status = AudioObjectGetPropertyDataSize(objectID, &address, 0, nil, &size)
    return status == noErr && size >= UInt32(MemoryLayout<AudioObjectID>.size)
}

func inputDevices() -> [AudioDevice] {
    var address = propertyAddress(kAudioHardwarePropertyDevices)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size) == noErr else {
        return []
    }
    let count = Int(size) / MemoryLayout<AudioObjectID>.size
    var ids = [AudioObjectID](repeating: 0, count: count)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &ids) == noErr else {
        return []
    }
    return ids.compactMap { id in
        guard hasInputStreams(id),
              let name = stringProperty(id, selector: kAudioObjectPropertyName),
              let uid = stringProperty(id, selector: kAudioDevicePropertyDeviceUID) else {
            return nil
        }
        return AudioDevice(id: id, name: name, uid: uid)
    }
}

final class AudioStats {
    let minRMS: Double
    let minPeak: Double
    let minActiveRatio: Double

    private var sampleRate = 48_000.0
    private var sampleCount = 0
    private var sumSquares = 0.0
    private var peak = 0.0
    private var clippedSamples = 0
    private var zeroCrossings = 0
    private var previousSign = 0
    private var totalWindows = 0
    private var activeWindows = 0
    private var consecutiveActiveWindows = 0
    private var maxConsecutiveActiveWindows = 0
    private var windowSamples = 0
    private var windowSquares = 0.0
    private var callbackCount = 0

    init(minRMS: Double, minPeak: Double, minActiveRatio: Double, sampleRate: Double) {
        self.minRMS = minRMS
        self.minPeak = minPeak
        self.minActiveRatio = minActiveRatio
        self.sampleRate = sampleRate
    }

    func consumeFloat32Buffer(_ buffer: AudioQueueBufferRef) {
        callbackCount += 1
        let sampleTotal = Int(buffer.pointee.mAudioDataByteSize) / MemoryLayout<Float>.size
        let samples = buffer.pointee.mAudioData.assumingMemoryBound(to: Float.self)
        for index in 0..<sampleTotal {
            consume(Double(samples[index]))
        }
    }

    private func consume(_ rawSample: Double) {
        let sample = max(-1.0, min(1.0, rawSample))
        let magnitude = abs(sample)
        sampleCount += 1
        sumSquares += sample * sample
        peak = max(peak, magnitude)
        if magnitude >= 0.98 {
            clippedSamples += 1
        }

        let sign = sample > 0 ? 1 : (sample < 0 ? -1 : 0)
        if sign != 0 {
            if previousSign != 0 && sign != previousSign {
                zeroCrossings += 1
            }
            previousSign = sign
        }

        windowSamples += 1
        windowSquares += sample * sample
        let windowSize = max(160, Int(sampleRate * 0.02))
        if windowSamples >= windowSize {
            finishWindow()
        }
    }

    private func finishWindow() {
        guard windowSamples > 0 else { return }
        totalWindows += 1
        let rms = sqrt(windowSquares / Double(windowSamples))
        if rms >= minRMS {
            activeWindows += 1
            consecutiveActiveWindows += 1
            maxConsecutiveActiveWindows = max(maxConsecutiveActiveWindows, consecutiveActiveWindows)
        } else {
            consecutiveActiveWindows = 0
        }
        windowSamples = 0
        windowSquares = 0.0
    }

    func result(device: AudioDevice, duration: Double) -> [String: Any] {
        finishWindow()
        let rms = sampleCount > 0 ? sqrt(sumSquares / Double(sampleCount)) : 0.0
        let activeRatio = totalWindows > 0 ? Double(activeWindows) / Double(totalWindows) : 0.0
        let zcr = sampleCount > 1 ? Double(zeroCrossings) / Double(sampleCount - 1) : 0.0
        let speechLikely = rms >= minRMS && peak >= minPeak && activeRatio >= minActiveRatio
        return [
            "device": device.name,
            "deviceUID": device.uid,
            "durationSeconds": duration,
            "sampleRate": sampleRate,
            "sampleCount": sampleCount,
            "callbackCount": callbackCount,
            "rms": rms,
            "peak": peak,
            "activeWindowRatio": activeRatio,
            "activeWindows": activeWindows,
            "totalWindows": totalWindows,
            "maxConsecutiveActiveWindows": maxConsecutiveActiveWindows,
            "zeroCrossingRate": zcr,
            "clippedSamples": clippedSamples,
            "speechLikely": speechLikely,
            "thresholds": [
                "minRMS": minRMS,
                "minPeak": minPeak,
                "minActiveRatio": minActiveRatio
            ]
        ]
    }
}

private func audioQueueCallback(userData: UnsafeMutableRawPointer?,
                                queue: AudioQueueRef,
                                buffer: AudioQueueBufferRef,
                                startTime: UnsafePointer<AudioTimeStamp>,
                                packetCount: UInt32,
                                packetDescriptions: UnsafePointer<AudioStreamPacketDescription>?) {
    guard let userData else { return }
    let stats = Unmanaged<AudioStats>.fromOpaque(userData).takeUnretainedValue()
    stats.consumeFloat32Buffer(buffer)
    AudioQueueEnqueueBuffer(queue, buffer, 0, nil)
}

func check(_ status: OSStatus, _ message: String) {
    guard status == noErr else {
        fputs("\(message): \(status)\n", stderr)
        exit(5)
    }
}

let options = parseOptions()
let devices = inputDevices()
guard let device = devices.first(where: { $0.name == options.deviceName }) else {
    let names = devices.map { "\($0.name) [\($0.uid)]" }.joined(separator: ", ")
    fputs("audio input device not found: \(options.deviceName). Available: \(names)\n", stderr)
    exit(3)
}

var format = AudioStreamBasicDescription(mSampleRate: 48_000,
                                         mFormatID: kAudioFormatLinearPCM,
                                         mFormatFlags: kAudioFormatFlagIsFloat |
                                             kAudioFormatFlagIsPacked |
                                             kAudioFormatFlagsNativeEndian,
                                         mBytesPerPacket: UInt32(MemoryLayout<Float>.size),
                                         mFramesPerPacket: 1,
                                         mBytesPerFrame: UInt32(MemoryLayout<Float>.size),
                                         mChannelsPerFrame: 1,
                                         mBitsPerChannel: 32,
                                         mReserved: 0)
let stats = AudioStats(minRMS: options.minRMS,
                       minPeak: options.minPeak,
                       minActiveRatio: options.minActiveRatio,
                       sampleRate: format.mSampleRate)
var audioQueue: AudioQueueRef?
let context = Unmanaged.passUnretained(stats).toOpaque()
check(AudioQueueNewInput(&format,
                         audioQueueCallback,
                         context,
                         CFRunLoopGetCurrent(),
                         CFRunLoopMode.commonModes.rawValue,
                         0,
                         &audioQueue),
      "AudioQueueNewInput failed")
guard let audioQueue else {
    fputs("AudioQueueNewInput returned no queue\n", stderr)
    exit(5)
}

var currentDeviceUID = device.uid as CFString
check(withUnsafePointer(to: &currentDeviceUID) {
    AudioQueueSetProperty(audioQueue,
                          kAudioQueueProperty_CurrentDevice,
                          $0,
                          UInt32(MemoryLayout<CFString>.size))
}, "AudioQueueSetProperty current device failed")

let bufferByteSize = UInt32(format.mSampleRate / 10) * format.mBytesPerFrame
for _ in 0..<3 {
    var buffer: AudioQueueBufferRef?
    check(AudioQueueAllocateBuffer(audioQueue, bufferByteSize, &buffer), "AudioQueueAllocateBuffer failed")
    if let buffer {
        check(AudioQueueEnqueueBuffer(audioQueue, buffer, 0, nil), "AudioQueueEnqueueBuffer failed")
    }
}

check(AudioQueueStart(audioQueue, nil), "AudioQueueStart failed")
RunLoop.current.run(until: Date().addingTimeInterval(options.duration))
AudioQueueStop(audioQueue, true)
AudioQueueDispose(audioQueue, true)

let result = stats.result(device: device, duration: options.duration)
let data = try JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys])
print(String(data: data, encoding: .utf8)!)
exit((result["speechLikely"] as? Bool) == true ? 0 : 7)
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run an ADVCtl Audio end-to-end voice transport test.")
    parser.add_argument("--device-name", default="ADVCtl Audio")
    parser.add_argument("--duration", type=float, default=12.0)
    parser.add_argument("--warmup", type=float, default=1.5)
    parser.add_argument("--expected-frames", type=int, default=20)
    parser.add_argument("--helper-app", type=pathlib.Path, default=DEFAULT_HELPER_APP)
    parser.add_argument("--helper-log", type=pathlib.Path, default=DEFAULT_HELPER_LOG)
    parser.add_argument("--driver-path", type=pathlib.Path, default=DEFAULT_DRIVER)
    parser.add_argument("--no-launch-helper", action="store_true")
    parser.add_argument("--skip-helper-log", action="store_true")
    parser.add_argument("--min-rms", type=float, default=0.002)
    parser.add_argument("--min-peak", type=float, default=0.02)
    parser.add_argument("--min-active-ratio", type=float, default=0.05)
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--json", action="store_true", help="Print machine-readable summary only.")
    return parser.parse_args()


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, text=True, capture_output=True, **kwargs)


def helper_running() -> bool:
    result = run(["pgrep", "-fl", "ADVCtl"])
    if result.returncode != 0:
        return False
    for line in result.stdout.splitlines():
        if "advctl_voice_e2e.py" in line:
            continue
        if "/ADVCtl" in line or "ADVCtl.app" in line:
            return True
    return False


def log_size(path: pathlib.Path) -> int:
    try:
        return path.stat().st_size
    except FileNotFoundError:
        return 0


def read_new_log(path: pathlib.Path, offset: int) -> str:
    try:
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            handle.seek(offset)
            return handle.read()
    except FileNotFoundError:
        return ""


def build_probe(tmpdir: pathlib.Path) -> pathlib.Path:
    source = tmpdir / "ADVCtlVoiceProbe.swift"
    binary = tmpdir / "ADVCtlVoiceProbe"
    module_cache = tmpdir / "ModuleCache"
    module_cache.mkdir()
    source.write_text(SWIFT_PROBE)
    swiftc = shutil.which("swiftc") or "/usr/bin/swiftc"
    result = run([
        swiftc,
        "-module-cache-path", str(module_cache),
        "-framework", "AudioToolbox",
        "-framework", "CoreAudio",
        str(source),
        "-o", str(binary),
    ])
    if result.returncode != 0:
        raise RuntimeError(result.stderr or result.stdout or "swiftc failed")
    return binary


def launch_helper(args: argparse.Namespace) -> None:
    if args.no_launch_helper or helper_running():
        return
    if not args.helper_app.exists():
        raise FileNotFoundError(
            f"{args.helper_app} is not installed. Run tools/macctl-helper/build-app.sh first."
        )
    # Keep the helper in normal demand-driven mode for the full probe window.
    # The helper's --e2e-audio-frames option intentionally exits once the
    # frame count is reached, which can make a long CoreAudio capture look
    # mostly silent even when the BLE audio path works.
    result = run(["open", "-gj", str(args.helper_app)])
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "failed to launch ADVCtl.app")
    time.sleep(args.warmup)
    if not helper_running():
        raise RuntimeError("ADVCtl.app did not stay running after launch")


def run_probe(probe: pathlib.Path, args: argparse.Namespace) -> tuple[int, dict[str, object], str]:
    result = run([
        str(probe),
        "--device", args.device_name,
        "--duration", str(args.duration),
        "--min-rms", str(args.min_rms),
        "--min-peak", str(args.min_peak),
        "--min-active-ratio", str(args.min_active_ratio),
    ], timeout=args.duration + 15)
    metrics: dict[str, object] = {}
    if result.stdout.strip():
        metrics = json.loads(result.stdout)
    return result.returncode, metrics, result.stderr.strip()


def main() -> int:
    args = parse_args()
    failures: list[str] = []
    helper_log_offset = log_size(args.helper_log)

    if not args.driver_path.exists():
        failures.append(f"ADVCtlAudio driver is not installed at {args.driver_path}")

    with tempfile.TemporaryDirectory(prefix="advctl-voice-e2e-") as tmp:
        probe = build_probe(pathlib.Path(tmp))
        if args.compile_only:
            print(f"compiled probe: {probe}")
            return 0

        try:
            launch_helper(args)
        except Exception as exc:
            failures.append(str(exc))

        probe_status, metrics, probe_error = run_probe(probe, args)
        if probe_status != 0:
            failures.append(probe_error or f"audio probe exited with {probe_status}")
        if not metrics.get("speechLikely", False):
            failures.append("ADVCtl Audio did not produce speech-like non-silent input")

    new_log = read_new_log(args.helper_log, helper_log_offset)
    if not args.skip_helper_log:
        for needle in [
            "ADV microphone bridge activated",
            "Sent ADV microphone start",
            "ADV microphone",
            "Received ADV audio frames",
        ]:
            if needle not in new_log:
                failures.append(f"helper log did not contain: {needle}")

    summary = {
        "ok": not failures,
        "failures": failures,
        "metrics": metrics,
        "helperLogPath": str(args.helper_log),
        "helperLogExcerpt": "\n".join(new_log.splitlines()[-20:]),
    }
    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print(json.dumps(summary, indent=2, sort_keys=True))
        if failures:
            print("\nADVCtl voice E2E failed.", file=sys.stderr)
        else:
            print("\nADVCtl voice E2E passed.")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
