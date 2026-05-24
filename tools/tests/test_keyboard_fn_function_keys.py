#!/usr/bin/env python3
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class KeyboardFnFunctionKeyTests(unittest.TestCase):
    def test_fn_number_row_maps_to_function_keys(self):
        source = (ROOT / "main/hal/keyboard/keyboard.cpp").read_text()
        row0 = source[source.index("// Row 0"):source.index("// Row 1")]

        for number, function_key in zip("1234567890", [f"F{i}" for i in range(1, 11)]):
            pattern = (
                rf'\{{"{re.escape(number)}",\s+KEY_{number},.*?'
                rf'"{function_key}",\s+KEY_{function_key},\s+0\s+\}}'
            )
            self.assertRegex(row0, pattern)

        self.assertIn("if (_fn_state)", source)
        self.assertIn("ret.keyCode        = kv.fnKeyCode;", source)
        self.assertIn("ret.extraModifiers = kv.fnExtraModifiers;", source)

    def test_fn_function_keys_use_macos_consumer_controls_where_standard(self):
        hal = (ROOT / "main/hal/hal.cpp").read_text()
        hal_header = (ROOT / "main/hal/hal.h").read_text()
        ble_header = (ROOT / "main/hal/utils/ble_hid_device/ble_hid_device_helper.h").read_text()
        advctl = (ROOT / "main/apps/app_home_control/app_home_control.cpp").read_text()

        self.assertIn("BLE_HID_CONSUMER_BRIGHTNESS_UP       = 111", ble_header)
        self.assertIn("BLE_HID_CONSUMER_BRIGHTNESS_DOWN     = 112", ble_header)
        self.assertIn("BLE_HID_MACCTL_SYSTEM_CONTROL_CENTER = 1", ble_header)
        self.assertIn("BLE_HID_MACCTL_SYSTEM_SPOTLIGHT      = 2", ble_header)
        self.assertIn("bool bleMacSystemControlKey(const Keyboard::KeyEvent_t& keyEvent);", hal_header)
        self.assertIn("bool bleMacCtlSystemKey(uint8_t key);", hal_header)

        system_control = hal[hal.index("bool Hal::bleMacSystemControlKey"):
                             hal.index("bool Hal::bleMacCtlVolumeDelta")]
        expected = {
            "KEY_F1": "BLE_HID_CONSUMER_BRIGHTNESS_DOWN",
            "KEY_F2": "BLE_HID_CONSUMER_BRIGHTNESS_UP",
            "KEY_F7": "BLE_HID_CONSUMER_SCAN_PREVIOUS_TRACK",
            "KEY_F8": "BLE_HID_CONSUMER_PLAY_PAUSE",
            "KEY_F9": "BLE_HID_CONSUMER_SCAN_NEXT_TRACK",
            "KEY_F10": "BLE_HID_CONSUMER_MUTE",
        }
        for key, usage in expected.items():
            self.assertIn(f"case {key}:", system_control)
            self.assertIn(f"usageId = {usage};", system_control)
        self.assertIn("if (keyEvent.state)", system_control)
        self.assertIn("bleConsumerSend(usageId);", system_control)
        self.assertIn("case KEY_F3:", system_control)
        self.assertIn("bleMacCtlSystemKey(BLE_HID_MACCTL_SYSTEM_CONTROL_CENTER);", system_control)
        self.assertIn("case KEY_F4:", system_control)
        self.assertIn("bleMacCtlSystemKey(BLE_HID_MACCTL_SYSTEM_SPOTLIGHT);", system_control)

        macctl_helper = (ROOT / "main/hal/utils/ble_hid_device/ble_hid_device_helper.c").read_text()
        self.assertIn("#define MACCTL_CMD_SYSTEM_KEY   3", macctl_helper)
        self.assertIn("ble_hid_device_helper_send_macctl_system_key", macctl_helper)

        ble_forward = hal[hal.index("void Hal::handle_ble_keyboard_event"):
                          hal.index("/* -------------------------------------------------------------------------- */", hal.index("void Hal::handle_ble_keyboard_event"))]
        self.assertIn("if (bleMacSystemControlKey(keyEvent))", ble_forward)

        adv_forward = advctl[advctl.index("void AppHomeControl::forwardKeyboardEvent"):
                             advctl.index("void AppHomeControl::handlePointerKey")]
        self.assertIn("if (GetHAL().bleMacSystemControlKey(keyEvent))", adv_forward)

    def test_home_screen_only_consumes_wake_key_in_power_save(self):
        source = (ROOT / "main/apps/app_home_control/app_home_control.cpp").read_text()
        handler = source[source.index("void AppHomeControl::handleKeyEvent"):
                         source.index("void AppHomeControl::forwardKeyboardEvent")]

        self.assertIn("if (_screen_off && _power_save_active)", handler)
        self.assertNotIn("if (_screen_off) {\n        if (keyEvent.state)", handler)
        self.assertLess(handler.index("if (_screen_off && _power_save_active)"),
                        handler.index("markUserActivity();"))


if __name__ == "__main__":
    unittest.main()
