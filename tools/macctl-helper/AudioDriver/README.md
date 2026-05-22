# ADVCtlAudio HAL Driver

`ADVCtlAudio.driver` exposes a real system input device named `ADVCtl Audio`.
It replaces the earlier BlackHole bridge for ADV microphone capture.

Runtime chain:

1. A macOS app starts reading `ADVCtl Audio`.
2. CoreAudio calls `StartIO` in this HAL driver.
3. The driver sets demand in `/tmp/advctl_audio_pcm.ring`.
4. ADVCtl sees demand, sends the BLE microphone activation command to ADV, and writes decoded Float32 PCM into the ring.
5. CoreAudio reads the ring in `ReadInput`.
6. `StopIO` clears demand, and ADVCtl tells ADV to stop recording.

Useful logs:

```sh
log show --last 5m --predicate 'eventMessage CONTAINS "ADVCtlAudio"'
log stream --predicate 'eventMessage CONTAINS "ADVCtlAudio"'
```

Build:

```sh
./build-driver.sh
```

Install for the current Mac:

```sh
./install-driver.sh
```
