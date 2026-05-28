#!/usr/bin/env python3
import pathlib
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


def read_mac_helper_sources() -> str:
    source_dir = ROOT / "tools/macctl-helper/Sources/MacCtlHelper"
    return "\n".join(path.read_text() for path in sorted(source_dir.glob("*.swift")))


class AdvCtlInputAudioPriorityTests(unittest.TestCase):
    def test_audio_test_polls_external_input_before_streaming_audio(self):
        source = (ROOT / "main/apps/app_home_control/app_home_control.cpp").read_text()
        mode_pos = source.index("if (_mode == Mode::AudioTest)")
        block = source[mode_pos:source.index("if (_mode == Mode::Dashboard)", mode_pos)]

        self.assertIn("handleExternalInput();", block)
        self.assertLess(block.index("handleExternalInput();"), block.index("renderAudioWaveform();"))

        header = (ROOT / "main/apps/app_home_control/app_home_control.h").read_text()
        self.assertIn("AUDIO_TEST_LENGTH    = 120", header)
        self.assertIn("AUDIO_TEST_RATE      = 16000", header)
        self.assertIn("AUDIO_STREAM_PAYLOAD = 60", header)

    def test_chain_joystick_center_and_sensitivity_are_managed_by_input_layer(self):
        app_source = (ROOT / "main/apps/app_home_control/app_home_control.cpp").read_text()
        on_open = app_source[app_source.index("void AppHomeControl::onOpen()"):app_source.index("void AppHomeControl::onRunning()")]
        self.assertNotIn("GetHAL().externalInput.calibrateJoystickCenter();", on_open)

        input_header = (ROOT / "main/hal/external_input.h").read_text()
        self.assertIn("DEFAULT_JOYSTICK_SENSITIVITY = 50", input_header)
        self.assertIn("void setJoystickSensitivity(uint8_t sensitivity);", input_header)
        self.assertIn("uint8_t getJoystickSensitivity() const", input_header)

        input_source = (ROOT / "main/hal/external_input.cpp").read_text()
        self.assertIn("void ExternalInput::calibrateJoystickCenter()", input_source)
        self.assertIn("void ExternalInput::setJoystickSensitivity(uint8_t sensitivity)", input_source)
        self.assertIn("settings.GetInt(\"ext_joy_sens\", DEFAULT_JOYSTICK_SENSITIVITY)", input_source)
        self.assertIn("_settings->SetInt(\"ext_joy_sens\", _joystick_sensitivity)", input_source)
        self.assertIn("CHAIN_JOYSTICK_MIN_SENSITIVITY", input_source)
        self.assertIn("CHAIN_JOYSTICK_MAX_SENSITIVITY", input_source)
        self.assertIn("CHAIN_JOYSTICK_BASE_DEAD_ZONE_X", input_source)
        self.assertIn("CHAIN_JOYSTICK_BASE_DEAD_ZONE_Y", input_source)
        self.assertIn("_chain_joystick_center_pending = true;", input_source)
        self.assertIn("CHAIN_JOYSTICK_CENTER_ACCEPTANCE", input_source)
        self.assertIn("CHAIN_JOYSTICK_CENTER_STABLE_SAMPLES", input_source)
        self.assertIn("chainJoystickAxisNearCenter(rawX)", input_source)
        self.assertIn("chainJoystickAxisNearCenter(rawY)", input_source)
        self.assertIn("chainJoystickAxisInAutoCenterRange(rawX)", input_source)
        self.assertIn("chainJoystickNormalizeMapped16", input_source)
        self.assertIn("chainJoystickNormalizeRawAdc", input_source)
        self.assertIn("chainCommand(_chain_joystick_index, 0x30", input_source)
        self.assertIn("chainCommand(_chain_joystick_index, 0x34", input_source)
        self.assertIn("mappedX16   = chainJoystickReadInt16LE(&data[0]);", input_source)
        self.assertIn("mappedY16   = chainJoystickReadInt16LE(&data[2]);", input_source)
        self.assertIn("updateChainJoystickCenter(rawX, rawY)", input_source)
        self.assertIn("rawX - _chain_joystick_center_x", input_source)
        self.assertIn("rawY - _chain_joystick_center_y", input_source)
        self.assertIn("chain joystick center deferred", input_source)
        self.assertIn("chain joystick center waiting for release", input_source)
        self.assertNotIn("_chain_joystick_center_raw_x", input_source)
        self.assertNotIn("_chain_joystick_center_raw_y", input_source)
        self.assertNotIn("still using default center", input_source)
        read_chain = input_source[input_source.index("bool ExternalInput::readChainJoystick"):]
        self.assertIn("chainJoystickDeadZone(CHAIN_JOYSTICK_BASE_DEAD_ZONE_X)", read_chain)
        self.assertIn("chainJoystickDeadZone(CHAIN_JOYSTICK_BASE_DEAD_ZONE_Y)", read_chain)
        self.assertIn("sensitivity={} deadzone=({}, {})", read_chain)

        hal_source = (ROOT / "main/hal/hal.cpp").read_text()
        self.assertIn("GetHAL().externalInput.getJoystickSensitivity()", hal_source)
        self.assertIn("GetHAL().externalInput.setJoystickSensitivity(data[2]);", hal_source)

        mac_source = read_mac_helper_sources()
        self.assertIn("advctl.joystickSensitivity", mac_source)
        self.assertIn("range: 1...100", mac_source)
        self.assertIn("SliderRow(title: \"摇杆灵敏度\"", mac_source)

    def test_power_save_keeps_hid_connected_and_input_wake_active(self):
        source = (ROOT / "main/apps/app_home_control/app_home_control.cpp").read_text()
        running = source[source.index("void AppHomeControl::onRunning()"):source.index("void AppHomeControl::onClose()")]
        screen_off_block = running[running.index("if (_screen_off)"):running.index("if (_mode == Mode::Setup)")]
        self.assertIn("handleExternalInput();", screen_off_block)
        self.assertNotIn("!_power_save_active", screen_off_block)

        power_save = source[source.index("void AppHomeControl::enterPowerSave()"):source.index("void AppHomeControl::exitPowerSave()")]
        self.assertNotIn("bleControlStop", power_save)
        self.assertNotIn("externalInput.setPaused(true)", power_save)

    def test_mac_helper_treats_keyboard_hid_endpoint_as_composite_control(self):
        source = read_mac_helper_sources()

        self.assertIn("IOHIDManagerSetDeviceMatchingMultiple", source)
        self.assertIn("kIOHIDProductKey: advCtlProductName", source)
        self.assertIn("kIOHIDDeviceUsagePageKey: hidUsagePageGenericDesktop", source)
        self.assertIn("kIOHIDDeviceUsagePageKey: advCtlUsagePage", source)
        self.assertIn("IOHIDRequestAccess(kIOHIDRequestTypeListenEvent)", source)
        self.assertIn("kIOReturnNotPermitted", source)
        self.assertIn("openInputMonitoringPreferences()", source)
        self.assertIn("case composite", source)
        self.assertIn("var supportsControl: Bool", source)
        self.assertIn("registration.kind.supportsControl", source)
        self.assertNotIn("import CoreBluetooth", source)
        self.assertNotIn("ADVCtlBLEControlClient", source)
        self.assertIn("kIOHIDReportTypeFeature", source)
        self.assertIn("reportWithID.append(contentsOf: payload)", source)
        self.assertIn("sendKeyboardLedAudioControl(active: active)", source)
        self.assertIn("kIOHIDReportTypeOutput", source[source.index("private func setKeyboardOutputReport"):])
        self.assertIn("CFIndex(advCtlKeyboardReportID)", source[source.index("private func setKeyboardOutputReport"):])
        attempts = source[source.index("let attempts: [(IOHIDReportType, Bool, CFIndex)]"):source.index("var lastStatus", source.index("let attempts: [(IOHIDReportType, Bool, CFIndex)]"))]
        self.assertLess(attempts.index("kIOHIDReportTypeOutput"), attempts.index("kIOHIDReportTypeFeature"))
        self.assertIn("(kIOHIDReportTypeFeature, true, 0)", attempts)
        self.assertIn("(kIOHIDReportTypeOutput, true, 0)", attempts)
        self.assertLess(attempts.index("(kIOHIDReportTypeOutput, true, 0)"),
                        attempts.index("(kIOHIDReportTypeOutput, false, CFIndex(advCtlReportID))"))
        self.assertLess(attempts.index("(kIOHIDReportTypeOutput, false, CFIndex(advCtlReportID))"),
                        attempts.index("(kIOHIDReportTypeFeature, false, CFIndex(advCtlReportID))"))
        open_failed_block = source[source.index('updateMessage("HID manager open failed: \\(status)")'):source.index("if let devices = IOHIDManagerCopyDevices", source.index('updateMessage("HID manager open failed: \\(status)")'))]
        self.assertNotIn("return", open_failed_block)
        keyboard_match = "usagePage == hidUsagePageGenericDesktop && usage == hidUsageKeyboard"
        self.assertIn(keyboard_match, source)
        self.assertIn("kind = .composite", source[source.index(keyboard_match):])
        composite_case = source[source.index("case .composite:"):source.index("private func handleControlReport")]
        self.assertIn("handleControlReport", composite_case)
        self.assertIn("handleKeyboardReport", composite_case)

    def test_mac_helper_settings_use_system_sidebar_and_install_guide(self):
        source = read_mac_helper_sources()

        self.assertIn("import SwiftUI", source)
        self.assertIn("NavigationSplitView", source)
        self.assertIn("List(selection: $model.selectedPage)", source)
        self.assertIn("window.titlebarAppearsTransparent = true", source)
        self.assertIn("window.titleVisibility = .hidden", source)
        self.assertIn("window.title = \"\"", source)
        self.assertIn(".listStyle(.sidebar)", source)
        self.assertIn("NSToolbar.Identifier(\"ADVCtlSettingsToolbar\")", source)
        self.assertIn("NSToolbarItem(itemIdentifier: .toggleSidebar)", source)
        self.assertIn("window.toolbarStyle = .unified", source)
        self.assertNotIn("safeAreaInset(edge: .top)", source)
        self.assertNotIn(".ignoresSafeArea(.container, edges: .top)", source)
        self.assertIn("ADVCtlInstallGuideView", source)
        self.assertIn("安装向导", source)
        self.assertIn("ADVCtlSettingsPage.allCases", source)
        self.assertIn("AudioBridgePanel", source)
        self.assertIn("ModernWaveformView", source)
        self.assertIn("Canvas", source)
        self.assertNotIn("WaveformView(frame:", source)
        self.assertNotIn("NSViewRepresentable", source)
        self.assertIn("MenuBarExtra(\"ADVCtl\", systemImage: \"waveform\")", source)
        self.assertIn("Label(\"打开 ADVCtl\", systemImage: \"macwindow\")", source)
        self.assertIn("Label(\"安装向导\", systemImage: \"externaldrive.badge.checkmark\")", source)
        self.assertIn("runningApplications(withBundleIdentifier: advCtlBundleIdentifier)", source)
        self.assertIn("DistributedNotificationCenter.default().postNotificationName", source)
        self.assertIn("deliverImmediately: true", source)
        self.assertIn("applicationShouldHandleReopen", source)
        self.assertIn("handleShowSettingsNotification", source)
        self.assertNotIn("NSStatusBar.system.statusItem", source)
        self.assertNotIn("NSMenuItem(title:", source)
        self.assertNotIn("menu.addItem(connectionMenuItem)", source)
        self.assertNotIn("menu.addItem(knobMenuItem)", source)
        self.assertNotIn("menu.addItem(messageMenuItem)", source)
        self.assertNotIn("sidebarTable = NSTableView()", source)
        self.assertNotIn("NSTabView()", source)
        self.assertNotIn("SettingsSidebarCell", source)

    def test_adv_audio_stays_speech_oriented_hid_transport(self):
        source = (ROOT / "main/hal/utils/ble_hid_device/ble_hid_device_helper.c").read_text()
        app = (ROOT / "main/apps/app_home_control/app_home_control.cpp").read_text()
        header = (ROOT / "main/apps/app_home_control/app_home_control.h").read_text()
        mac = read_mac_helper_sources()
        agents = (ROOT / "AGENTS.md").read_text()

        self.assertIn("ESP_HID_APPEARANCE_KEYBOARD", source)
        self.assertIn("GATT_SVR_SVC_HID_UUID 0x1812", (ROOT / "main/hal/utils/ble_hid_device/ble_hid_gap.c").read_text())
        self.assertIn('.device_name = "ADVCtl"', source)
        self.assertIn("ble_svc_gap_device_name_set(device_name)", (ROOT / "main/hal/utils/ble_hid_device/ble_hid_gap.c").read_text())
        self.assertIn("static constexpr size_t AUDIO_TEST_RATE      = 16000", header)
        self.assertIn("static constexpr size_t AUDIO_STREAM_PAYLOAD = 60", header)
        self.assertIn("for (size_t i = 0; i < _audio_test_buffer.size(); i += 2)", app)
        self.assertIn("MACCTL_REPORT_AUDIO_STATE", source)
        self.assertIn("bool ble_hid_device_helper_send_macctl_audio_state(bool active)", source)
        self.assertIn("GetHAL().bleMacCtlAudioState(true);", app)
        self.assertIn("GetHAL().bleMacCtlAudioState(false);", app)
        self.assertIn("private let advCtlAudioStateReport: UInt8 = 0xA1", mac)
        self.assertIn("ADV microphone start not acknowledged; retrying", mac)
        self.assertIn("ADV microphone streaming", mac)
        self.assertIn("Received ADV audio frames", mac)
        self.assertIn("audio request consumed", app)
        self.assertIn("audio frame sent", app)
        self.assertIn("@discardableResult func enqueueULaw(_ data: Data) -> Int", mac)
        self.assertIn("speech recognition", agents)
        self.assertNotIn("A2DP", source)
        self.assertNotIn("Bluetooth audio", source)

    def test_ble_report_queue_splits_control_from_best_effort_reports(self):
        source = (ROOT / "main/hal/utils/ble_hid_device/ble_hid_device_helper.c").read_text()

        self.assertNotIn("xQueueSendToFront", source)
        self.assertIn("s_ble_hid_control_report_queue", source)
        self.assertIn("s_ble_hid_best_effort_report_queue", source)
        self.assertIn("xTaskNotifyGive(s_ble_hid_report_task)", source)
        self.assertIn("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)", source)
        self.assertIn("ble_hid_report_is_best_effort", source)
        self.assertIn("ble_hid_report_should_drop_audio", source)
        self.assertIn("0xB1, 0x02", source)
        feature_case = source[source.index("case ESP_HIDD_FEATURE_EVENT:"):source.index("case ESP_HIDD_DISCONNECT_EVENT:")]
        self.assertIn("s_ble_hid_output_callback(param->feature.data, param->feature.length)", feature_case)
        output_case = source[source.index("case ESP_HIDD_OUTPUT_EVENT:"):source.index("case ESP_HIDD_FEATURE_EVENT:")]
        self.assertIn("ble_hid_device_helper_handle_keyboard_output(param->output.data, param->output.length)", output_case)
        self.assertIn("MACCTL_LED_CMD_AUDIO_START", source)
        self.assertIn("uint8_t command[4] = {0x82", source)
        self.assertIn("ble_hid_device_helper_poll_output_reports", source)
        self.assertIn("ble_hid_device_helper_poll_nimble_output_reports", source)
        self.assertIn("ble_att_svr_find_by_uuid", source)
        self.assertIn("BLE_SVC_HID_DSC_UUID16_RPT_REF", source)
        self.assertIn("MACCTL write poll", source)
        self.assertIn("ble_hid_device_helper_poll_output_reports();", (ROOT / "main/hal/hal.cpp").read_text())
        self.assertNotIn("ADVCTL_GATT_SERVICE_UUID", source)
        self.assertNotIn("ble_gatts_notify_custom", source)
        self.assertNotIn("ble_hid_device_helper_register_advctl_gatt", source)
        self.assertIn("#define MACCTL_BLE_STORE_SCHEMA_VERSION 4", source)
        self.assertIn("#define BLE_HID_ENUMERATION_TIMEOUT_MS 10000", source)
        self.assertIn("ble_hid_device_helper_enumeration_watchdog", source)
        self.assertIn("HID did not enumerate", source)
        watchdog_start = source.index("ble_hid_device_helper_enumeration_watchdog")
        watchdog_block = source[watchdog_start:source.index("static void ble_hid_device_helper_ensure_enumeration_watchdog", watchdog_start)]
        self.assertNotIn("ble_store_clear()", watchdog_block)
        self.assertNotIn("ble_gap_terminate", watchdog_block)

        defaults = (ROOT / "sdkconfig.defaults").read_text()
        self.assertIn("CONFIG_BT_NIMBLE_ENABLED=y", defaults)
        self.assertNotIn("CONFIG_BT_CLASSIC_ENABLED=y", defaults)

    def test_audio_ring_overrun_does_not_convert_negative_counts_to_uint(self):
        source = (ROOT / "tools/macctl-helper/Sources/MacCtlHelper/ADVCtlAudioRingSink.swift").read_text()

        self.assertIn("let overflow = max(0, available + samples.count - capacity)", source)
        self.assertIn("readIndex += UInt64(overflow)", source)
        self.assertNotIn("UInt64(samples.count - capacity)", source)

    def test_mac_audio_bridge_enhances_speech_pcm_before_ring_write(self):
        source = (ROOT / "tools/macctl-helper/Sources/MacCtlHelper/ADVCtlAudioRingSink.swift").read_text()

        self.assertIn("private let advCtlAudioSpeechGain: Float32", source)
        self.assertIn("private let advCtlAudioHighPassAlpha: Float32", source)
        self.assertIn("private let advCtlAudioSoftClipKnee: Float32", source)
        self.assertIn("appendUpsampledSpeechSample(enhanceSpeech(decoded), to: &samples)", source)
        self.assertIn("let fraction = Float32(step) / Float32(advCtlAudioUpsampleFactor)", source)
        self.assertIn("let highPassed = sample - highPassPreviousInput", source)
        self.assertIn("let boosted = highPassed * advCtlAudioSpeechGain", source)
        self.assertIn("resetSpeechEnhancer(to: 0)", source)

    def test_mac_audio_bridge_conceals_best_effort_hid_packet_loss(self):
        app = (ROOT / "tools/macctl-helper/Sources/MacCtlHelper/ADVCtlApp.swift").read_text()
        sink = (ROOT / "tools/macctl-helper/Sources/MacCtlHelper/ADVCtlAudioRingSink.swift").read_text()

        self.assertIn("private let advCtlAudioMaxConcealedPackets = 16", app)
        self.assertIn("private var expectedAudioFrameSequence: UInt8?", app)
        self.assertIn("private func concealMissingAudioPackets(before sequence: UInt8, payloadBytes: Int) -> Int", app)
        self.assertIn("let distance = (Int(sequence) - Int(expected) + 256) % 256", app)
        self.assertIn("guard distance <= 127 else", app)
        self.assertIn("audioSink.enqueueSilence(uLawSampleCount: missingPackets * payloadBytes)", app)
        self.assertIn("resetAudioPacketTracking()", app[app.index("if payload.count >= 2, payload[0] == advCtlAudioStateReport"):])
        self.assertIn("private let advCtlAudioUpsampleFactor = 6", sink)
        self.assertIn("@discardableResult func enqueueSilence(uLawSampleCount: Int) -> Int", sink)

    def test_voice_e2e_automation_uses_driver_demand_and_hid_audio(self):
        script = (ROOT / "tools/tests/advctl_voice_e2e.py").read_text()
        app = (ROOT / "tools/macctl-helper/Sources/MacCtlHelper/ADVCtlApp.swift").read_text()

        self.assertIn("ADVCtl Audio", script)
        self.assertIn("AudioQueueNewInput", script)
        self.assertIn("kAudioQueueProperty_CurrentDevice", script)
        self.assertIn("speechLikely", script)
        self.assertIn('result = run(["open", "-gj", str(args.helper_app)])', script)
        self.assertIn("ADV microphone bridge activated", script)
        self.assertIn("Sent ADV microphone start", script)
        self.assertIn("Received ADV audio frames", script)
        self.assertIn("--e2e-audio-frames", script)
        self.assertIn("ADVCTL_E2E_EXPECTED_AUDIO_FRAMES", app)
        self.assertIn("ADVCTL_E2E_AUDIO_TIMEOUT_SECONDS", app)
        self.assertIn("E2E audio complete; received", app)
        self.assertNotIn("CoreBluetooth", script)
        driver = (ROOT / "tools/macctl-helper/AudioDriver/Sources/Driver.cpp").read_text()
        self.assertIn('deviceParams.ConfigurationApplicationBundleID = "dev.cardputer.advctl"', driver)

    def test_install_scripts_automate_firmware_flash_and_mac_audio_driver_install(self):
        flash = (ROOT / "tools/flash-firmware.sh").read_text()
        build_app = (ROOT / "tools/macctl-helper/build-app.sh").read_text()
        install = (ROOT / "tools/macctl-helper/install.sh").read_text()

        self.assertIn("find_idf_export()", flash)
        self.assertIn("find_serial_port()", flash)
        self.assertIn("idf.py set-target esp32s3", flash)
        self.assertIn('idf.py -p "${PORT}" -b "${BAUD}" flash', flash)
        self.assertIn('idf.py -p "${PORT}" monitor', flash)
        self.assertIn("ADVCTL_INSTALL_AUDIO_DRIVER", build_app)
        self.assertIn("install_audio_driver()", build_app)
        self.assertIn("/Library/Audio/Plug-Ins/HAL", build_app)
        self.assertIn("ADVCtlAudio.driver", build_app)
        self.assertIn("killall coreaudiod || true", build_app)
        self.assertIn("./build-app.sh", install)

    def test_keyboard_report_map_keeps_output_byte_aligned(self):
        source = (ROOT / "main/hal/utils/ble_hid_device/ble_hid_device_helper.c").read_text()
        start = source.index("const unsigned char keyboardReportMap[]")
        report_map = source[start:source.index("};", start)]

        normalized = "\n".join(line.strip() for line in report_map.splitlines())
        self.assertIn(
            "\n".join(
                [
                    "0x95, 0x05,  //   Report Count (5)",
                    "0x75, 0x01,  //   Report Size (1)",
                    "0x05, 0x08,  //   Usage Page (LEDs)",
                    "0x19, 0x01,  //   Usage Minimum (Num Lock)",
                    "0x29, 0x05,  //   Usage Maximum (Kana)",
                    "0x91, 0x02,  //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)",
                    "0x95, 0x01,  //   Report Count (1)",
                    "0x75, 0x03,  //   Report Size (3)",
                    "0x91, 0x03,  //   Output (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)",
                    "0x95, 0x06,  //   Report Count (6)",
                    "0x75, 0x08,  //   Report Size (8)",
                ]
            ),
            normalized,
        )

    def test_nimble_uses_single_composite_hid_report_map(self):
        source = (ROOT / "main/hal/utils/ble_hid_device/ble_hid_device_helper.c").read_text()
        defaults = (ROOT / "sdkconfig.defaults").read_text()

        self.assertIn("#define BLE_HID_MAP_INDEX_MOUSE    0", source)
        self.assertIn("#define BLE_HID_MAP_INDEX_MACCTL   0", source)
        self.assertIn("CONFIG_BT_NIMBLE_SVC_HID_MAX_RPTS=8", defaults)
        maps_start = source.index("static esp_hid_raw_report_map_t ble_report_maps[]")
        maps_block = source[maps_start:source.index("};", maps_start)]
        self.assertIn("#if CONFIG_EXAMPLE_HID_DEVICE_ROLE == 2 && CONFIG_BT_NIMBLE_ENABLED", maps_block)
        self.assertIn("{.data = compositeReportMap, .len = sizeof(compositeReportMap)}", maps_block)

        gap_source = (ROOT / "main/hal/utils/ble_hid_device/ble_hid_gap.c").read_text()
        self.assertIn("#define GATT_SVR_SVC_HID_UUID 0x1812", gap_source)
        self.assertNotIn("#define GATT_SVR_SVC_ADVCTL_UUID", gap_source)
        self.assertIn("fields.num_uuids16         = 1", gap_source)
        self.assertIn("ble_gap_security_initiate(event->connect.conn_handle)", gap_source)
        failed_connect_block = gap_source[gap_source.index("} else {", gap_source.index("BLE_GAP_EVENT_CONNECT:")):gap_source.index("return 0;", gap_source.index("BLE_GAP_EVENT_CONNECT:"))]
        self.assertIn("ble_hid_device_helper_gap_disconnected(BLE_HS_CONN_HANDLE_NONE)", failed_connect_block)
        self.assertIn("esp_hid_ble_gap_adv_start()", failed_connect_block)
        self.assertIn("security state: encrypted=%u authenticated=%u bonded=%u", gap_source)
        self.assertNotIn("ble_store_util_bonded_peers", gap_source)
        self.assertNotIn("BLE_GAP_CONN_MODE_DIR", gap_source)
        self.assertIn("ble_hid_device_helper_ensure_advertising", source)
        self.assertIn("ensure BLE HID advertising started", source)
        self.assertIn("ble_gap_conn_find(s_ble_hid_conn_handle", source)
        self.assertIn("clearing stale BLE HID connection handle before advertising", source)
        hal_source = (ROOT / "main/hal/hal.cpp").read_text()
        update_block = hal_source[hal_source.index("void Hal::update()"):hal_source.index("void Hal::feedTheDog()")]
        self.assertIn("_is_ble_keyboard_inited && !bleKeyboardIsConnected()", update_block)
        self.assertIn("ble_hid_device_helper_ensure_advertising();", update_block)
        ble_connected = hal_source[hal_source.index("bool Hal::bleKeyboardIsConnected() const"):hal_source.index("void Hal::bleKeyboardSendReport")]
        self.assertIn("ble_hid_device_helper_get_state() == BLE_HID_DEVICE_STATE_CONNECTED", ble_connected)
        self.assertNotIn("ble_hid_device_helper_is_ready()", ble_connected)
        send_report = hal_source[hal_source.index("void Hal::bleKeyboardSendReport"):hal_source.index("void Hal::bleKeyboardTap")]
        self.assertIn("ble_hid_device_helper_is_ready()", send_report)
        self.assertNotIn("bleControlEnsureAdvertising", (ROOT / "main/apps/app_home_control/app_home_control.cpp").read_text())
        self.assertIn("connection parameter update requested after security", gap_source)
        self.assertLess(
            gap_source.index("security state: encrypted=%u authenticated=%u bonded=%u"),
            gap_source.index("ble_gap_update_params(event->enc_change.conn_handle"),
        )
        disconnect_block = gap_source[gap_source.index("case BLE_GAP_EVENT_DISCONNECT:"):gap_source.index("case BLE_GAP_EVENT_CONN_UPDATE:")]
        self.assertIn("esp_hid_ble_gap_adv_start()", disconnect_block)
        self.assertIn("advertising restarted after disconnect", disconnect_block)
        helper_disconnect = source[source.index("void ble_hid_device_helper_gap_disconnected"):source.index("void ble_hid_device_helper_gap_subscribe")]
        self.assertIn("s_ble_hid_keyboard_state   = BLE_HID_DEVICE_STATE_IDLE;", helper_disconnect)
        helper_connect = source[source.index("void ble_hid_device_helper_gap_connected"):source.index("void ble_hid_device_helper_gap_disconnected")]
        self.assertIn("s_ble_hid_keyboard_state = BLE_HID_DEVICE_STATE_CONNECTED;", helper_connect)
        helper_ready = source[source.index("bool ble_hid_device_helper_is_ready"):source.index("void ble_hid_device_helper_gap_connected")]
        self.assertIn("s_ble_hid_keyboard_state == BLE_HID_DEVICE_STATE_CONNECTED", helper_ready)
        self.assertNotIn("s_ble_hid_notify_ready", helper_ready)
        self.assertIn("keeping BLE connection and bonds", source)
        watchdog_start = source.index("ble_hid_device_helper_enumeration_watchdog")
        watchdog_block = source[watchdog_start:source.index("static void ble_hid_device_helper_ensure_enumeration_watchdog", watchdog_start)]
        self.assertNotIn("ble_store_clear()", watchdog_block)
        self.assertNotIn("ble_gap_terminate", watchdog_block)

    def test_ble_report_policy_keeps_audio_from_starving_controls(self):
        clang = shutil.which("clang") or shutil.which("cc")
        if clang is None:
            self.skipTest("no C compiler available")

        include_dir = ROOT / "main/hal/utils/ble_hid_device"
        code = textwrap.dedent(
            r"""
            #include "ble_hid_report_queue_policy.h"
            #include <assert.h>
            #include <stdint.h>

            int main(void) {
                const uint8_t macctl_map = 3;
                const uint8_t macctl_report = 4;
                const uint8_t mouse_map = 1;
                const uint8_t mouse_report = 2;
                const uint8_t audio[] = {0xA0, 0x01, 0x02};
                const uint8_t command[] = {0x01, 0x01, 0x00};
                const uint8_t keyboard[] = {0x01, 0x00, 0x04};
                const uint8_t mouse_move[] = {0x00, 0x05, 0x00, 0x00};
                const uint8_t mouse_click[] = {0x01, 0x00, 0x00, 0x00};

                assert(ble_hid_report_is_audio_frame(macctl_map, macctl_report, macctl_map, macctl_report,
                                                     audio, sizeof(audio)));
                assert(ble_hid_report_is_mouse_movement(mouse_map, mouse_report, mouse_map, mouse_report,
                                                        mouse_move, sizeof(mouse_move)));
                assert(!ble_hid_report_is_mouse_movement(mouse_map, mouse_report, mouse_map, mouse_report,
                                                         mouse_click, sizeof(mouse_click)));
                assert(!ble_hid_report_is_audio_frame(macctl_map, macctl_report, macctl_map, macctl_report,
                                                      command, sizeof(command)));
                assert(ble_hid_report_should_drop_audio(BLE_HID_REPORT_AUDIO_QUEUE_RESERVE));
                assert(!ble_hid_report_should_drop_audio(BLE_HID_REPORT_AUDIO_QUEUE_RESERVE + 1));
                assert(ble_hid_report_should_drop_mouse_movement(BLE_HID_REPORT_MOUSE_QUEUE_RESERVE));
                assert(!ble_hid_report_should_drop_mouse_movement(BLE_HID_REPORT_MOUSE_QUEUE_RESERVE + 1));
                assert(!ble_hid_report_should_prioritize(macctl_map, macctl_report, macctl_map, macctl_report,
                                                        mouse_map, mouse_report, audio, sizeof(audio)));
                assert(!ble_hid_report_should_prioritize(mouse_map, mouse_report, macctl_map, macctl_report,
                                                        mouse_map, mouse_report, mouse_move, sizeof(mouse_move)));
                assert(ble_hid_report_should_prioritize(macctl_map, macctl_report, macctl_map, macctl_report,
                                                       mouse_map, mouse_report, command, sizeof(command)));
                assert(ble_hid_report_should_prioritize(mouse_map, mouse_report, macctl_map, macctl_report,
                                                       mouse_map, mouse_report, mouse_click, sizeof(mouse_click)));
                assert(ble_hid_report_should_prioritize(0, 1, macctl_map, macctl_report,
                                                       mouse_map, mouse_report, keyboard, sizeof(keyboard)));
                assert(ble_hid_report_is_best_effort(macctl_map, macctl_report, macctl_map, macctl_report,
                                                     mouse_map, mouse_report, audio, sizeof(audio)));
                assert(ble_hid_report_is_best_effort(mouse_map, mouse_report, macctl_map, macctl_report,
                                                     mouse_map, mouse_report, mouse_move, sizeof(mouse_move)));
                assert(!ble_hid_report_is_best_effort(macctl_map, macctl_report, macctl_map, macctl_report,
                                                      mouse_map, mouse_report, command, sizeof(command)));
                assert(BLE_HID_REPORT_CONTROL_QUEUE_LEN >= 8);
                assert(BLE_HID_REPORT_BEST_EFFORT_QUEUE_LEN >= 16);
                return 0;
            }
            """
        )

        with tempfile.TemporaryDirectory() as tmp:
            test_c = pathlib.Path(tmp) / "test_ble_hid_report_policy.c"
            test_bin = pathlib.Path(tmp) / "test_ble_hid_report_policy"
            test_c.write_text(code)
            subprocess.run([clang, "-std=c11", "-I", str(include_dir), str(test_c), "-o", str(test_bin)], check=True)
            subprocess.run([str(test_bin)], check=True)


if __name__ == "__main__":
    unittest.main()
