# ADVCtl Audio Path

ADVCtl cannot become a system microphone from the status bar app alone. macOS only exposes system-wide input devices through CoreAudio HAL drivers, so ADVCtl uses the open-source BlackHole virtual audio driver as the system-visible input device.

Install the 2-channel driver:

```sh
brew install --cask blackhole-2ch
```

Runtime chain:

1. ADVCtl.app activates the ADV microphone only when recording is requested.
2. ADV streams compressed voice frames to the Mac helper over the BLE control/audio channel.
3. ADVCtl.app decodes frames into 16 kHz mono PCM.
4. ADVCtl.app writes PCM to the BlackHole output device.
5. macOS apps select `BlackHole 2ch` as their microphone input.

Current status:

- Recording activation and ADV-side waveform test are verified.
- BlackHole is the chosen HAL driver. The local experimental `ADVCtlAudio.driver` path is retired.
- The remaining implementation work is ADVCtl's BLE audio receive/decode path and the CoreAudio writer that feeds `BlackHole 2ch`.
