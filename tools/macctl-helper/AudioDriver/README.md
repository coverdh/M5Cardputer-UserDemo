# ADVCtl Audio Driver

ADVCtl cannot become a system microphone from the status bar app alone. macOS only exposes system-wide input devices through CoreAudio HAL drivers, so the ADV microphone path needs an `AudioServerPlugIn` bundle installed under:

`/Library/Audio/Plug-Ins/HAL/ADVCtlAudio.driver`

Planned chain:

1. ADVCtl.app activates the ADV microphone only when recording is requested.
2. ADV streams compressed voice frames to the Mac helper over the BLE control/audio channel.
3. ADVCtl.app decodes frames into a shared ring buffer.
4. `ADVCtlAudio.driver` exposes one mono input stream named `ADV Microphone`.
5. CoreAudio pulls PCM frames from that ring buffer so Speech Recognition and other apps can select ADVCtl as a normal microphone.

Current status:

- Recording activation and ADV-side waveform test are verified.
- System-level device exposure is not implemented yet.
- The next implementation step is a minimal HAL driver that registers `ADV Microphone` and outputs silence until the app-provided ring buffer is connected.
