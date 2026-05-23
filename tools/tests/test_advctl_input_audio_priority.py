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

    def test_app_entry_requests_chain_joystick_center_calibration(self):
        app_source = (ROOT / "main/apps/app_home_control/app_home_control.cpp").read_text()
        on_open = app_source[app_source.index("void AppHomeControl::onOpen()"):app_source.index("void AppHomeControl::onRunning()")]
        self.assertIn("GetHAL().externalInput.calibrateJoystickCenter();", on_open)

        input_source = (ROOT / "main/hal/external_input.cpp").read_text()
        self.assertIn("void ExternalInput::calibrateJoystickCenter()", input_source)
        self.assertIn("_chain_joystick_center_pending = true;", input_source)
        self.assertIn("rawX - _chain_joystick_center_x", input_source)
        self.assertIn("rawY - _chain_joystick_center_y", input_source)
        self.assertIn("CHAIN_JOYSTICK_DEAD_ZONE", input_source[input_source.index("bool ExternalInput::readChainJoystick"):])

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
        self.assertLess(attempts.index("kIOHIDReportTypeFeature"), attempts.index("kIOHIDReportTypeOutput"))
        self.assertIn("(kIOHIDReportTypeFeature, true, 0)", attempts)
        self.assertIn("(kIOHIDReportTypeOutput, true, 0)", attempts)
        self.assertLess(attempts.index("(kIOHIDReportTypeFeature, true, 0)"),
                        attempts.index("(kIOHIDReportTypeFeature, false, CFIndex(advCtlReportID))"))
        self.assertLess(attempts.index("(kIOHIDReportTypeOutput, true, 0)"),
                        attempts.index("(kIOHIDReportTypeOutput, false, CFIndex(advCtlReportID))"))
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
        self.assertIn("safeAreaInset(edge: .top)", source)
        self.assertIn(".ignoresSafeArea(.container, edges: .top)", source)
        self.assertIn("ADVCtlInstallGuideView", source)
        self.assertIn("安装向导", source)
        self.assertIn("ADVCtlSettingsPage.allCases", source)
        self.assertIn("NSImage(systemSymbolName: \"waveform\"", source)
        self.assertIn("打开 ADVCtl", source)
        self.assertIn("runningApplications(withBundleIdentifier: advCtlBundleIdentifier)", source)
        self.assertIn("DistributedNotificationCenter.default().postNotificationName", source)
        self.assertIn("handleShowSettingsNotification", source)
        self.assertNotIn("menu.addItem(connectionMenuItem)", source)
        self.assertNotIn("menu.addItem(knobMenuItem)", source)
        self.assertNotIn("menu.addItem(messageMenuItem)", source)
        self.assertNotIn("sidebarTable = NSTableView()", source)
        self.assertNotIn("NSTabView()", source)
        self.assertNotIn("SettingsSidebarCell", source)

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
        self.assertNotIn("ADVCTL_GATT_SERVICE_UUID", source)
        self.assertNotIn("ble_gatts_notify_custom", source)
        self.assertNotIn("ble_hid_device_helper_register_advctl_gatt", source)
        self.assertIn("#define MACCTL_BLE_STORE_SCHEMA_VERSION 4", source)
        self.assertIn("#define BLE_HID_ENUMERATION_TIMEOUT_MS 10000", source)
        self.assertIn("ble_hid_device_helper_enumeration_watchdog", source)
        self.assertIn("HID did not enumerate", source)
        self.assertIn("ble_store_clear();", source)
        self.assertIn("ble_gap_terminate", source)

        defaults = (ROOT / "sdkconfig.defaults").read_text()
        self.assertIn("CONFIG_BT_NIMBLE_ENABLED=y", defaults)
        self.assertNotIn("CONFIG_BT_CLASSIC_ENABLED=y", defaults)

    def test_audio_ring_overrun_does_not_convert_negative_counts_to_uint(self):
        source = (ROOT / "tools/macctl-helper/Sources/MacCtlHelper/ADVCtlAudioRingSink.swift").read_text()

        self.assertIn("let overflow = max(0, available + samples.count - capacity)", source)
        self.assertIn("readIndex += UInt64(overflow)", source)
        self.assertNotIn("UInt64(samples.count - capacity)", source)

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
        self.assertIn("security state: encrypted=%u authenticated=%u bonded=%u", gap_source)

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
