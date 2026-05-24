# AGENTS.md

These rules apply to this repository unless a deeper `AGENTS.md` overrides them.

## BLE Device Identity

- Baseline Bluetooth type: ADVCtl is a standard BLE HID composite keyboard/mouse device using HID service UUID `0x1812`, advertised/GAP name `ADVCtl`, and keyboard appearance `ESP_HID_APPEARANCE_KEYBOARD`. The NimBLE report map is a single composite HID map with keyboard report ID `1`, mouse report ID `2`, consumer/media report ID `3`, and ADVCtl vendor control/audio report ID `4`.
- Keep ADVCtl as a BLE HID keyboard/composite HID device. Do not change the device type, HID role, advertised HID service, or keyboard appearance for microphone work.
- BLE advertising must keep the HID service UUID `0x1812` as the advertised service. Do not advertise the ADVCtl custom service `0xFFF0` as the primary device identity.
- Keep the advertised name and GAP Device Name in sync as `ADVCtl`; call `ble_svc_gap_device_name_set(device_name)` for NimBLE HID so macOS/iOS pairing UIs resolve the device correctly.
- Do not switch ADVCtl to Bluetooth audio, A2DP, classic BT, USB audio, or another device class to carry microphone data unless the user explicitly asks for that risk and validates keyboard/joystick behavior.
- Do not add custom primary GATT services such as `0xFFF0`/`0xFFF2` to the firmware for microphone work unless the user explicitly approves the pairing-cache risk and hardware validation shows macOS still connects as a keyboard.

## Input Priority

- Keyboard and external joystick/encoder input have priority over microphone streaming.
- Microphone audio is best-effort: it may be dropped or throttled to preserve keyboard, joystick, mouse, and consumer-control responsiveness.
- ADV microphone audio is for speech recognition, dictation, and command input. Narrowband speech-oriented transport such as 8 kHz µ-law inside the existing HID vendor report is acceptable; do not optimize for music or high-fidelity complex audio by changing Bluetooth device class.
- Do not change HID report IDs, report maps, advertised appearance, or BLE GAP identity while working on audio unless the change is specifically about keyboard/joystick compatibility and is validated on device.

## Validation

- Before finishing BLE, keyboard, joystick, or microphone changes, run the focused tests and a firmware build when practical.
- On hardware, verify logs show `HID_DEV_BLE: START`, `HID_DEV_BLE: CONNECT`, `security state: encrypted=1 ... bonded=1`, and `hid input notify ready`, and that external input detection still reports the expected joystick/encoder path.
- Do not label device-side UI as `paired` merely because a BLE connection exists. macOS pairing is not complete until the security/encryption callback reports a bonded connection.
- If macOS/iOS sees ADVCtl but pairing spins forever, check for missing `ble_gap_security_initiate()` / missing `BLE_GAP_EVENT_ENC_CHANGE` before changing device type or HID descriptors.
- Do not clear BLE bonds or terminate the BLE link from automatic reconnect or enumeration watchdog paths. Clearing the device-side bond while macOS still remembers ADVCtl leaves a stale host pairing that appears in Settings but will not auto-reconnect; terminating early can make macOS drop an otherwise valid HID reconnect before it finishes subscriptions. Require an explicit forget/re-pair flow for bond repair.
- After every NimBLE GAP disconnect, restart connectable HID advertising from the GAP disconnect path as well as any higher-level HID disconnect callback. A bonded macOS/iOS host cannot auto-reconnect if ADVCtl is not advertising after the link drops.
- Follow the repo's `AppKeyboard` BLE model for reconnect: entering the app initializes BLE HID, and if it is initialized but not connected, the firmware ensures normal connectable HID advertising is running. Do not move Bluetooth reconnect responsibility into ADVCtl.app.
- With NimBLE, trust the GAP connect result for the app-visible connected state. A high-level HID connect callback can appear before a failed GAP connection completes; failed GAP connects must reset helper state and restart advertising.
- If microphone work adds or changes a GATT channel, confirm the Mac app can discover it after connecting to the existing HID device instead of requiring a different Bluetooth device type.
