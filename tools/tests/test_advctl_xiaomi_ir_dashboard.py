#!/usr/bin/env python3
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class AdvCtlXiaomiIrDashboardTests(unittest.TestCase):
    def test_xiaomi_ir_protocol_is_available_without_replacing_nec(self):
        helper = (ROOT / "main/hal/utils/ir_nec/ir_helper.c").read_text()
        header = (ROOT / "main/hal/utils/ir_nec/ir_helper.h").read_text()
        hal_header = (ROOT / "main/hal/hal.h").read_text()
        hal_source = (ROOT / "main/hal/hal.cpp").read_text()

        self.assertIn("#define XIAOMI_IR_CARRIER_HZ 36000", helper)
        self.assertIn("#define XIAOMI_IR_UNIT_US    290", helper)
        self.assertIn("ir_helper_xiaomi_checksum", helper)
        self.assertIn("(device >> 4) ^ device ^ (function >> 4) ^ function", helper)
        self.assertIn("rmt_new_copy_encoder", helper)
        self.assertIn("ir_helper_send_xiaomi", helper)
        self.assertIn("XIAOMI_IR_FRAME_US - elapsed_us", helper)
        self.assertIn("ir_helper_send_raw", helper)
        self.assertIn("ir_helper_send(uint8_t addr, uint8_t cmd)", header)
        self.assertIn("ir_helper_send_xiaomi(uint8_t device, uint8_t function, uint8_t repeats)", header)
        self.assertIn("ir_helper_send_raw(uint32_t carrier_hz, const uint32_t* durations_us", header)
        self.assertIn("void irSend(uint8_t addr, uint8_t cmd);", hal_header)
        self.assertIn("void irSendXiaomi(uint8_t device, uint8_t function, uint8_t repeats = 3);", hal_header)
        self.assertIn("void irSendRaw(uint32_t carrier_hz, const uint32_t* durations_us", hal_header)
        self.assertIn("ir_helper_send(addr, cmd);", hal_source)
        self.assertIn("ir_helper_send_xiaomi(device, function, repeats);", hal_source)
        self.assertIn("ir_helper_send_raw(carrier_hz, durations_us, duration_count);", hal_source)

    def test_advctl_tv_shortcuts_default_to_xiaomi_tv_pro_codes(self):
        source = (ROOT / "main/apps/app_home_control/app_home_control.cpp").read_text()
        header = (ROOT / "main/apps/app_home_control/app_home_control.h").read_text()

        self.assertIn("XIAOMI_TV_POWER_DEVICE = 0x3C", source)
        self.assertIn("XIAOMI_TV_POWER_FUNC   = 0xCC", source)
        self.assertIn("XIAOMI_TV_INPUT_DEVICE = 0x86", source)
        self.assertIn("XIAOMI_TV_INPUT_FUNC   = 0x01", source)
        self.assertIn("MITV_RAW_CARRIER_HZ   = 38028", source)
        self.assertIn("MITV_RAW_POWER[]", source)
        self.assertIn("_tv_power_addr         = 0x3C", header)
        self.assertIn("_tv_power_cmd          = 0xCC", header)
        self.assertIn("_tv_input_addr         = 0x86", header)
        self.assertIn("_tv_input_cmd          = 0x01", header)
        self.assertIn("sendTvInputSource();", header)
        self.assertIn("parseTvInputConfig", header)
        self.assertIn("settings.GetInt(\"tv_input_addr\", XIAOMI_TV_INPUT_DEVICE)", source)
        self.assertIn("settings.SetInt(\"tv_input_addr\", _tv_input_addr)", source)
        self.assertIn("GetHAL().irSendXiaomi(_tv_power_addr, _tv_power_cmd, XIAOMI_TV_IR_REPEATS);", source)
        self.assertIn("GetHAL().irSendRaw(MITV_RAW_CARRIER_HZ, MITV_RAW_POWER", source)
        self.assertIn("GetHAL().irSendXiaomi(_tv_input_addr, _tv_input_cmd, XIAOMI_TV_IR_REPEATS);", source)
        self.assertIn("key == \"input\" || key == \"src\"", source)

        dashboard_fn = source[source.index("bool AppHomeControl::handleDashboardFnControl"):
                              source.index("void AppHomeControl::forwardKeyboardEvent")]
        space_branch = dashboard_fn[dashboard_fn.index("keyEvent.keyCode == KEY_SPACE"):
                                    dashboard_fn.index("keyEvent.keyCode == KEY_W")]
        self.assertIn("sendTvInputSource();", space_branch)
        self.assertNotIn("bleMacCtlPlayPause", space_branch)

    def test_home_dashboard_hides_system_bars_and_uses_fullscreen_canvas(self):
        source = (ROOT / "main/apps/app_home_control/app_home_control.cpp").read_text()
        dashboard = source[source.index("void AppHomeControl::renderDashboard()"):
                           source.index("void AppHomeControl::renderNowPlaying()")]
        render = source[source.index("void AppHomeControl::render()"):
                        source.index("void AppHomeControl::renderSetup()")]
        on_open = source[source.index("void AppHomeControl::onOpen()"):
                         source.index("void AppHomeControl::onRunning()")]

        self.assertIn("GetHAL().setFullscreenMode(true);", on_open)
        self.assertIn("GetHAL().setFullscreenMode(true);", render)
        self.assertNotIn("GetHAL().setFullscreenMode(false);", dashboard)
        self.assertIn("auto& canvas = GetHAL().canvas;", dashboard)
        self.assertIn("GetHAL().pushCanvas();", dashboard)
        self.assertIn("renderStatusBar();", dashboard)
        self.assertNotIn("renderNowPlaying();", dashboard)
        self.assertNotIn("LGFX_Sprite frame(&display);", dashboard)
        self.assertIn("ADVCtl B:%s A:%s", dashboard)
        self.assertIn("Wi:%s T:%s Mic:%s M:%s", dashboard)
        self.assertIn("TV Ent/T:Pwr Sp:In", dashboard)
        self.assertIn("Ptr E/S:Spd A:XY D:X R:Reset", dashboard)
        self.assertIn("Sys W:WiFi C:Cfg S:Sleep", dashboard)
        self.assertIn("Snd M:On [/]Vol  B:Pair", dashboard)

        hal_header = (ROOT / "main/hal/hal.h").read_text()
        push_canvas = hal_header[hal_header.index("inline void pushCanvas()"):
                                 hal_header.index("void setFullscreenMode", hal_header.index("inline void pushCanvas()"))]
        self.assertIn("if (_fullscreen_mode)", push_canvas)
        self.assertIn("canvas.pushSprite(0, 0);", push_canvas)

        hal_source = (ROOT / "main/hal/hal.cpp").read_text()
        fullscreen = hal_source[hal_source.index("void Hal::setFullscreenMode"):
                                hal_source.index("void Hal::setDeviceBrightnessPercent")]
        self.assertIn("canvas.deleteSprite();", fullscreen)
        self.assertIn("canvas.createSprite(display.width(), display.height());", fullscreen)
        self.assertIn("canvas.createSprite(204, 109);", fullscreen)

        system_bar = (ROOT / "main/apps/app_launcher/view/system_bar/system_bar.cpp").read_text()
        keyboard_bar = (ROOT / "main/apps/app_launcher/view/keyboard_bar/keyboard_bar.cpp").read_text()
        self.assertIn("if (GetHAL().isFullscreenMode())", system_bar)
        self.assertIn("if (GetHAL().isFullscreenMode())", keyboard_bar)

    def test_ble_hid_identity_is_unchanged(self):
        hid = (ROOT / "main/hal/utils/ble_hid_device/ble_hid_device_helper.c").read_text()
        gap = (ROOT / "main/hal/utils/ble_hid_device/ble_hid_gap.c").read_text()

        self.assertIn("ESP_HID_APPEARANCE_KEYBOARD", hid)
        self.assertIn('.device_name = "ADVCtl"', hid)
        self.assertIn("GATT_SVR_SVC_HID_UUID 0x1812", gap)
        self.assertIn("ble_svc_gap_device_name_set(device_name)", gap)
        self.assertNotIn("XIAOMI_IR", hid)

    def test_advctl_battery_level_is_not_reported_as_static_full(self):
        hal_header = (ROOT / "main/hal/hal.h").read_text()
        hal_source = (ROOT / "main/hal/hal.cpp").read_text()
        system_bar = (ROOT / "main/apps/app_launcher/view/system_bar/system_bar.cpp").read_text()
        ble_header = (ROOT / "main/hal/utils/ble_hid_device/ble_hid_device_helper.h").read_text()
        ble_source = (ROOT / "main/hal/utils/ble_hid_device/ble_hid_device_helper.c").read_text()

        self.assertIn("inline int getBatLevel()", hal_header)
        self.assertIn("if (level < 0)", hal_header)
        self.assertIn("ble_hid_device_helper_set_battery_level(uint8_t level)", ble_header)
        self.assertIn("ble_hid_device_helper_set_battery_level(0);", ble_source)
        self.assertNotIn("Setting battery level to 100", ble_source)
        self.assertIn("void Hal::updateBleBatteryLevel(bool force)", hal_source)
        self.assertIn("const int level = getBatLevel();", hal_source)
        self.assertIn("ble_hid_device_helper_set_battery_level(batteryLevel);", hal_source)
        self.assertIn("updateBleBatteryLevel(true);", hal_source)
        self.assertIn("updateBleBatteryLevel();", hal_source)
        self.assertIn('bat_level < 0', system_bar)
        self.assertIn('bat_level = "--"', system_bar)


if __name__ == "__main__":
    unittest.main()
