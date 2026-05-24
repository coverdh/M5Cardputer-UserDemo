# Native HomePod and Apple TV Audio Control Research

Branch: `codex/homepod-native-control`

## Goal

Find a non-Home-Assistant path for ADVCtl to control HomePod volume and, where possible, fetch now-playing metadata. Pairing is acceptable. Volume control is the top priority, including the case where the HomePod is not independently playing audio but is the Apple TV audio output.

## Current Conclusion

The best first target is Apple TV volume control, not direct HomePod playback control.

When Apple TV 4K uses HomePod or a HomePod stereo pair as its default audio output, Apple documents that all Apple TV sound can be routed to the HomePod, including system/game audio and eARC audio. In that setup, volume commands should be sent to the Apple TV control plane, because the Apple TV is the active sender/controller and the HomePod is the output device.

This matters for ADVCtl because the knob/F13/F14 path should still work when:

- Apple TV is playing video and outputting to HomePod.
- HomePod is silent but configured as Apple TV default audio output.
- eARC audio is routed through Apple TV to HomePod.

## Native Apple Options

### 1. Music.app AppleScript / Apple Events

Status: Useful, but not enough for Apple TV-to-HomePod volume.

The local Music.app scripting dictionary exposes `AirPlay device` objects with:

- `kind`, including `HomePod`
- `selected`
- `active`
- `available`
- per-device `sound volume` from 0 to 100
- `current AirPlay devices`
- `current track`, `player state`, `current stream title`

This can control HomePod volume when Music.app is the AirPlay sender. It can also provide Music now-playing metadata. It does not cover the important Apple TV case, because the Mac Music app is not the controller when Apple TV is streaming to HomePod.

Implementation fit:

- Good as a secondary provider named `musicAirPlay`.
- Requires macOS Automation permission for ADVCtl to control Music.
- No custom Home Assistant dependency.

### 2. MediaPlayer / MPNowPlayingInfoCenter / MPRemoteCommandCenter

Status: Not suitable for reading or controlling another device's active session.

Apple documents `MPNowPlayingInfoCenter` as a way for an app to publish information about media that the app plays. `MPRemoteCommandCenter` is for the app to respond to external media controls. These APIs do not provide a public "inspect whatever the HomePod or Apple TV is playing" API.

Implementation fit:

- Not a volume backend for ADVCtl.
- Not a HomePod/Apple TV now-playing reader.

### 3. HomeKit public framework

Status: Not enough for the target.

HomeKit is public for controlling home accessories and scenes, but public HomeKit does not expose a complete HomePod media session API for "current title/artist" or reliable Apple TV-to-HomePod volume control. HomePod/Apple TV can be home hubs and media endpoints, but the useful media-control path is not exposed as a simple public HomeKit characteristic set.

Implementation fit:

- Avoid as the main path for this feature.
- It would add entitlement and permission complexity without solving the core volume case.

## Pairing-Based Community Option

### pyatv-style Apple TV pairing and control

Status: Best match for the Apple TV-to-HomePod volume requirement.

`pyatv` supports discovery, pairing, persistent credentials, volume controls, and now-playing metadata for Apple TV-class devices. Its docs say it supports Apple TV, HomePod, the macOS Music app, and AirPlay devices, but also clearly states it relies on private and reverse-engineered protocols. This is the practical non-Home-Assistant route if ADVCtl must control Apple TV/HomePod volume even when Music.app is not involved.

Important protocol notes:

- Apple TV/tvOS uses MRP and AirPlay 2 tunneling for metadata/control on modern versions.
- `pyatv` exposes an audio API with `volume`, `set_volume`, `volume_up`, and `volume_down`.
- `pyatv` metadata is strongest via MRP/DMAP. AirPlay/Companion metadata support is limited.
- Pairing is expected for Apple TV. HomePod direct support is more limited, so Apple TV should be the primary target when it is the HomePod sender.

Implementation fit:

- Best first implementation target: an `appleTVPaired` provider in ADVCtl.
- Store pairing credentials locally in the ADVCtl app support directory.
- ADVCtl sends knob volume deltas to the paired Apple TV device. If that Apple TV is outputting to HomePod, the HomePod volume changes through the active Apple TV route.
- Fetch now-playing from Apple TV when available. Fall back to Music.app metadata only when Music.app is the active sender.

## Recommended ADVCtl Plan

1. Add a provider enum for media volume:
   - `homeAssistant` for the existing behavior.
   - `appleTVPaired` for the new priority path.
   - `musicAirPlay` as a native Mac fallback for Music.app-to-HomePod.

2. Implement Apple TV paired volume first:
   - Discover Apple TV devices via Bonjour/Zeroconf.
   - Pair once with a PIN flow.
   - Persist credentials in Application Support.
   - Implement `volumeDelta`, `setVolume`, and `fetchVolume`.
   - Keep all keyboard/mouse/joystick behavior unchanged.

3. Route ADVCtl knob volume commands:
   - If `appleTVPaired` is configured, send volume up/down to Apple TV first.
   - Do not require HomePod to be currently playing.
   - Do not require Music.app to be open.
   - If Apple TV is unavailable, optionally fall back to `musicAirPlay` or Home Assistant only if configured.

4. Implement now-playing as best effort:
   - Apple TV paired metadata first.
   - Music.app metadata second.
   - HomePod direct metadata should be considered experimental unless hardware tests show it works reliably.

5. Hardware validation:
   - Apple TV 4K paired with HomePod default audio output.
   - Apple TV idle/silent: volume up/down still changes route volume UI or next playback volume.
   - Apple TV playing video/audio: ADVCtl knob changes HomePod volume.
   - eARC route if available: ADVCtl knob changes HomePod volume.
   - Existing BLE HID keyboard/mouse/joystick reports are unaffected.

## Source Notes

- Apple Support documents Apple TV 4K routing all sound to HomePod when HomePod is selected as default audio output, including eARC-routed external device audio.
- Apple Support Community confirms the practical control model: when Apple TV plays through HomePod, volume commands are received by Apple TV and affect the HomePod output.
- Apple Developer MediaPlayer docs describe Now Playing APIs as publishing/control APIs for the app's own playback, not as cross-device media inspection APIs.
- `pyatv` docs expose the needed discovery, pairing, volume, and metadata APIs, while warning that the protocols are private/reverse engineered.
