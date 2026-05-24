/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_home_control.h"
#include "assets/macctl_big.h"
#include "assets/macctl_small.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <hal/utils/ble_hid_device/ble_hid_device_helper.h>
#include <mooncake_log.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>

using namespace mooncake;

namespace {
constexpr uint8_t XIAOMI_TV_POWER_DEVICE = 0x3C;
constexpr uint8_t XIAOMI_TV_POWER_FUNC   = 0xCC;
constexpr uint8_t XIAOMI_TV_INPUT_DEVICE = 0x86;
constexpr uint8_t XIAOMI_TV_INPUT_FUNC   = 0x01;
constexpr uint8_t XIAOMI_TV_IR_REPEATS   = 5;
constexpr uint8_t LEGACY_NEC_POWER_ADDR  = 0x04;
constexpr uint8_t LEGACY_NEC_POWER_CMD   = 0x08;
constexpr uint32_t MITV_RAW_CARRIER_HZ   = 38028;
constexpr uint32_t MITV_RAW_POWER[] = {
    1578, 1026, 579, 605, 579, 605, 1446, 605, 1446, 605, 579, 605, 1446, 605, 579, 605, 1446, 605,
    579,  605,  1446, 605, 1446, 605, 10860, 1026, 579, 605, 579, 605, 1446, 605, 1446, 605, 579, 605,
    1446, 605,  579,  605, 1446, 605, 579,  605, 1446, 605, 1446, 605, 10860, 1026, 579, 605, 579, 605,
    1446, 605,  1446, 605, 579,  605, 1446, 605, 579,  605, 1446, 605, 579,  605, 1446, 605, 1446, 605,
    10860, 1026, 579,  605, 579,  605, 1446, 605, 1446, 605, 579,  605, 1446, 605, 579,  605, 1446, 605,
    579,  605,  1446, 605, 1446, 605, 10860, 1026, 579, 605, 579, 605, 1446, 605, 1446, 605, 579, 605,
    1446, 605,  579,  605, 1446, 605, 579,  605, 1446, 605, 1446, 605, 32767,
};
}  // namespace

const AppHomeControl::Action AppHomeControl::ACTIONS[AppHomeControl::ACTION_COUNT] = {
    {"TV Pwr", "Xiaomi IR"}, {"Pointer", "ADVCtl mouse"}, {"Keyboard", "BLE keys"}, {"TV In", "Xiaomi IR"},
    {"HP Play", "HA media"}, {"HP Vol+", "HA volume"},    {"Volume", "device"},     {"Config", "type setup"},
};

AppHomeControl::AppHomeControl()
{
    setAppInfo().name     = "ADVCtl";
    setAppInfo().userData = new AppIcon_t(image_data_macctl_big, image_data_macctl_small);
}

AppHomeControl::~AppHomeControl()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppHomeControl::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    _mode            = Mode::Setup;
    _selected_action = 0;
    _input_buffer.clear();
    _pending_volume_delta = 0;
    _auto_wifi_attempted  = false;
    _ble_start_requested  = false;
    _last_ble_retry       = 0;
    _screen_off           = false;
    _power_save_active    = false;
    _last_user_activity   = GetHAL().millis();
    resetPointerHolds();
    setStatus("Enter starts BLE");

    loadConfig();
    loadWifiConfig();

    _key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& keyEvent) { handleKeyEvent(keyEvent); });
    startConnections();
}

void AppHomeControl::onRunning()
{
    handleBleControlRequests();

    if (GetHAL().homeButton.wasClicked()) {
        markUserActivity();
        audio::play_random_tone();
        close();
        return;
    }
    updatePowerSave();
    if (_screen_off) {
        handleExternalInput();
        return;
    }

    if (_mode == Mode::Setup) {
        enterControlIfReady();
        if (!_ble_start_requested && GetHAL().millis() - _last_ble_retry > 3000) {
            startConnections();
        }
        if (GetHAL().millis() - _last_setup_render > SETUP_RENDER_MS) {
            _last_setup_render = GetHAL().millis();
            render();
        }
        return;
    }

    if (_mode == Mode::AudioTest) {
        handleExternalInput();
        renderAudioWaveform();
        return;
    }

    if (_mode == Mode::Dashboard) {
        handleExternalInput();
        repeatPointerMove();
        if (GetHAL().millis() - _last_status_refresh > DASHBOARD_RENDER_MS) {
            _last_status_refresh = GetHAL().millis();
            render();
        }
    } else if (_mode == Mode::Pointer || _mode == Mode::Keyboard || _mode == Mode::Volume) {
        handleExternalInput();
    }

}

void AppHomeControl::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    GetHAL().setFullscreenMode(false);
    exitPowerSave();
    if (_screen_off) {
        GetHAL().display.setBrightness(static_cast<uint8_t>(_display_brightness_before_sleep * 255 / 100));
        _screen_off = false;
    }
    if (_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_event_slot_id);
        _key_event_slot_id = -1;
    }
    resetPointerHolds();
    GetHAL().bleKeyboardSendReport(0, KEY_NONE);
    stopAudioTest();
}

void AppHomeControl::loadConfig()
{
    auto& settings           = GetHAL().getSettings();
    _ha_config.baseUrl       = settings.GetString("ha_url", "");
    _ha_config.token         = settings.GetString("ha_token", "");
    _ha_config.homepodEntity = settings.GetString("ha_homepod", "");
    _tv_power_addr           = static_cast<uint8_t>(settings.GetInt("tv_power_addr", XIAOMI_TV_POWER_DEVICE));
    _tv_power_cmd            = static_cast<uint8_t>(settings.GetInt("tv_power_cmd", XIAOMI_TV_POWER_FUNC));
    _tv_input_addr           = static_cast<uint8_t>(settings.GetInt("tv_input_addr", XIAOMI_TV_INPUT_DEVICE));
    _tv_input_cmd            = static_cast<uint8_t>(settings.GetInt("tv_input_cmd", XIAOMI_TV_INPUT_FUNC));
    if (_tv_power_addr == LEGACY_NEC_POWER_ADDR && _tv_power_cmd == LEGACY_NEC_POWER_CMD) {
        _tv_power_addr = XIAOMI_TV_POWER_DEVICE;
        _tv_power_cmd  = XIAOMI_TV_POWER_FUNC;
    }
    const int savedHalfStep  = settings.GetInt("ptr_sens2", 0);
    const int savedWholeStep = settings.GetInt("macctl_ptr_sens", 1);
    _pointer_sensitivity     = savedHalfStep >= 2 ? savedHalfStep : savedWholeStep * 2;
    _pointer_sensitivity     = std::max(2, std::min(6, _pointer_sensitivity));
    _knob_mode               = static_cast<uint8_t>(std::max<int32_t>(0, std::min<int32_t>(2, settings.GetInt("adv_knob_mode", 0))));
    loadPowerSettings();
    _ha.setConfig(_ha_config);
}

void AppHomeControl::loadWifiConfig()
{
    auto& settings = GetHAL().getSettings();
    _wifi_ssid     = settings.GetString("wifi_ssid", "");
    _wifi_password = settings.GetString("wifi_password", "");
}

void AppHomeControl::saveWifiConfig()
{
    auto& settings = GetHAL().getSettings();
    settings.SetString("wifi_ssid", _wifi_ssid);
    settings.SetString("wifi_password", _wifi_password);
}

void AppHomeControl::startConnections()
{
    if (!_ble_start_requested) {
        setStatus("Starting BLE");
        render();
        if (!GetHAL().bleControlInit()) {
            _last_ble_retry = GetHAL().millis();
            setStatus("BLE init failed; auto retry");
            return;
        }
        _ble_start_requested = true;
        if (!GetHAL().bleKeyboardIsConnected()) {
            setStatus("Pair ADVCtl");
        }
    }
    enterControlIfReady();
}

void AppHomeControl::saveHomeAssistantConfig()
{
    auto& settings = GetHAL().getSettings();
    settings.SetString("ha_url", _ha_config.baseUrl);
    settings.SetString("ha_token", _ha_config.token);
    settings.SetString("ha_homepod", _ha_config.homepodEntity);
    settings.SetInt("tv_power_addr", _tv_power_addr);
    settings.SetInt("tv_power_cmd", _tv_power_cmd);
    settings.SetInt("tv_input_addr", _tv_input_addr);
    settings.SetInt("tv_input_cmd", _tv_input_cmd);
    _ha.setConfig(_ha_config);
}

void AppHomeControl::tryAutoWifiConnect()
{
    if (_auto_wifi_attempted || GetHAL().isWifiConnected()) {
        return;
    }

    _auto_wifi_attempted = true;
    if (_wifi_ssid.empty()) {
        setStatus("WiFi not configured");
        return;
    }

    setStatus("WiFi connecting");
    render();
    ensureWifi();
}

bool AppHomeControl::ensureHomeAssistantReady()
{
    ensureWifi();
    if (!GetHAL().isWifiConnected()) {
        return false;
    }
    if (!_ha.isConfigured()) {
        setStatus("HA not configured");
        return false;
    }
    return true;
}

bool AppHomeControl::isConnectionReady() const
{
    return GetHAL().bleKeyboardIsConnected();
}

void AppHomeControl::enterControlIfReady()
{
    if (_mode != Mode::Setup || !isConnectionReady()) {
        return;
    }

    _mode = Mode::Dashboard;
    resetPointerHolds();
    setStatus("Control ready");
    render();
}

void AppHomeControl::resetPointerHolds()
{
    _hold_left  = false;
    _hold_right = false;
    _hold_up    = false;
    _hold_down  = false;
}

void AppHomeControl::updatePointerHold(const Keyboard::KeyEvent_t& keyEvent)
{
    if (isLeftKey(keyEvent)) {
        _hold_left = keyEvent.state;
    } else if (isRightKey(keyEvent)) {
        _hold_right = keyEvent.state;
    } else if (isUpKey(keyEvent)) {
        _hold_up = keyEvent.state;
    } else if (isDownKey(keyEvent)) {
        _hold_down = keyEvent.state;
    }
}

void AppHomeControl::repeatPointerMove()
{
    if (!_hold_left && !_hold_right && !_hold_up && !_hold_down) {
        return;
    }

    if (GetHAL().millis() - _last_pointer_repeat < POINTER_REPEAT_MS) {
        return;
    }

    int8_t dx = 0;
    int8_t dy = 0;
    if (_hold_left) {
        dx -= pointerStep();
    }
    if (_hold_right) {
        dx += pointerStep();
    }
    if (_hold_up) {
        dy -= pointerStep();
    }
    if (_hold_down) {
        dy += pointerStep();
    }

    _last_pointer_repeat = GetHAL().millis();
    markUserActivity();
    GetHAL().bleMouseMove(dx, dy);
    _pointer_status = "keyboard move";
}

void AppHomeControl::handleExternalInput()
{
    auto& input = GetHAL().externalInput;
    if (!input.isConnected() && !input.isEncoderConnected()) {
        return;
    }

    if (_screen_off) {
        if (input.getPressed() != 0 || input.getEncoderDelta() != 0 || input.getEncoderPressed()) {
            wakeDisplay();
        }
        return;
    }

    if (_mode == Mode::Dashboard || _mode == Mode::Pointer || _mode == Mode::Keyboard) {
        handleExternalPointer(input.getButtons(), input.getPressed(), input.getReleased());
    }
    handleExternalEncoder();
}

void AppHomeControl::handleExternalPointer(uint8_t buttons, uint8_t pressed, uint8_t released)
{
    (void)released;
    const uint32_t now = GetHAL().millis();
    int8_t dx = 0;
    int8_t dy = 0;
    if (buttons & ExternalInput::PAD_LEFT) {
        dx -= pointerStep();
    }
    if (buttons & ExternalInput::PAD_RIGHT) {
        dx += pointerStep();
    }
    if (buttons & ExternalInput::PAD_UP) {
        dy -= pointerStep();
    }
    if (buttons & ExternalInput::PAD_DOWN) {
        dy += pointerStep();
    }
    bool moved = false;
    if (dx != 0 || dy != 0) {
        if (now - _last_pointer_repeat >= POINTER_REPEAT_MS) {
            _last_pointer_repeat = now;
            GetHAL().bleMouseMove(dx, dy);
            moved = true;
        }
    }

    if (pressed & ExternalInput::PAD_A) {
        GetHAL().bleMouseClick(1);
        _pointer_status = "left click";
    } else if (pressed & ExternalInput::PAD_B) {
        GetHAL().bleMouseClick(2);
        _pointer_status = "right click";
    } else if (moved) {
        _pointer_status = "joystick move";
    }

    if (pressed & ExternalInput::PAD_A) {
        setStatus("Joystick left click");
    } else if (pressed & ExternalInput::PAD_B) {
        setStatus("External right click");
    } else if (moved) {
        setStatus("Joystick mouse");
    }

    if (pressed != 0 || released != 0 || moved) {
        markUserActivity();
    }

    if ((pressed != 0 || moved) && now - _last_external_render > EXTERNAL_RENDER_MS) {
        _last_external_render = now;
        render();
    }
}

void AppHomeControl::handleExternalEncoder()
{
    auto& input         = GetHAL().externalInput;
    const int16_t delta = input.getEncoderDelta();
    _knob_mode = static_cast<uint8_t>(
        std::max<int32_t>(0, std::min<int32_t>(2, GetHAL().getSettings().GetInt("adv_knob_mode", _knob_mode))));
    if (delta != 0) {
        markUserActivity();
        const int8_t step = delta > 0 ? 1 : -1;
        const int count       = std::min<int>(5, std::abs(static_cast<int>(delta)));
        for (int i = 0; i < count; ++i) {
            if (_knob_scroll_mode) {
                sendKnobWheelStep(step);
            } else {
                sendKnobControlStep(step);
            }
        }
        if (_knob_scroll_mode) {
            _knob_status = delta > 0 ? "wheel up" : "wheel down";
            setStatus(delta > 0 ? "Knob scroll up" : "Knob scroll down");
        } else if (_knob_mode == 1) {
            _knob_status = delta > 0 ? "HomePod up" : "HomePod down";
            setStatus(delta > 0 ? "Knob HomePod up" : "Knob HomePod down");
        } else if (_knob_mode == 0) {
            _knob_status = delta > 0 ? "volume up" : "volume down";
            setStatus(delta > 0 ? "Knob volume up" : "Knob volume down");
        } else {
            _knob_status = "disabled";
            setStatus("Knob disabled");
        }
    }
    if (input.getEncoderPressed()) {
        markUserActivity();
        _knob_scroll_mode = !_knob_scroll_mode;
        mclog::tagInfo(getAppInfo().name, "knob layer: {}", _knob_scroll_mode ? "mouse-wheel" : "control");
        _knob_status = _knob_scroll_mode ? "mouse wheel" : "control mode";
        setStatus(_knob_scroll_mode ? "Knob mouse wheel" : "Knob control mode");
        render();
        return;
    }
    if (delta != 0) {
        render();
    }
}

void AppHomeControl::sendKnobWheelStep(int8_t step)
{
    GetHAL().bleMouseMove(0, 0, step);
    GetHAL().delay(8);
    GetHAL().bleMouseMove(0, 0, 0);
}

void AppHomeControl::sendKnobControlStep(int8_t step)
{
    if (_knob_mode == 1) {
        if (!GetHAL().bleMacCtlVolumeDelta(step)) {
            const uint8_t keyCode = step > 0 ? KEY_F13 : KEY_F14;
            GetHAL().bleKeyboardTap(0, static_cast<KeScanCode_t>(keyCode));
        }
    } else if (_knob_mode == 0) {
        GetHAL().bleConsumerSend(step > 0 ? BLE_HID_CONSUMER_VOLUME_UP : BLE_HID_CONSUMER_VOLUME_DOWN);
    }
}

void AppHomeControl::handleBleControlRequests()
{
    bool audio_test_active = false;
    if (!GetHAL().bleConsumeAudioTestRequest(audio_test_active)) {
        return;
    }

    mclog::tagInfo(getAppInfo().name, "audio request consumed: {}", audio_test_active ? "start" : "stop");
    markUserActivity();
    if (audio_test_active) {
        startAudioTest();
    } else {
        stopAudioTest();
        _mode = Mode::Dashboard;
        render();
    }
}

void AppHomeControl::applyHardwareSettings(uint8_t flags, uint8_t sensitivity, uint8_t knobMode)
{
    const uint8_t clampedJoystickSensitivity = std::max<uint8_t>(1, std::min<uint8_t>(100, sensitivity));
    const uint8_t clampedKnobMode    = std::min<uint8_t>(2, knobMode);
    GetHAL().externalInput.setDirectionTransform((flags & 0x02) != 0, (flags & 0x04) != 0, (flags & 0x01) != 0);
    GetHAL().externalInput.setJoystickSensitivity(clampedJoystickSensitivity);
    _knob_mode = clampedKnobMode;
    GetHAL().getSettings().SetInt("adv_knob_mode", _knob_mode);
    sendHardwareSettings();
    setStatus("Hardware settings saved");
}

void AppHomeControl::resetHardwareSettings()
{
    applyHardwareSettings(0, ExternalInput::DEFAULT_JOYSTICK_SENSITIVITY, 0);
    setStatus("Hardware reset");
}

void AppHomeControl::sendHardwareSettings()
{
    uint8_t flags = 0;
    if (GetHAL().externalInput.getSwapAxes()) flags |= 0x01;
    if (GetHAL().externalInput.getFlipX()) flags |= 0x02;
    if (GetHAL().externalInput.getFlipY()) flags |= 0x04;
    GetHAL().bleMacCtlConfig(flags, GetHAL().externalInput.getJoystickSensitivity(), _knob_mode);
}

void AppHomeControl::startAudioTest()
{
    mclog::tagInfo(getAppInfo().name, "audio test start");
    if (_screen_off) {
        wakeDisplay();
    }
    if (_audio_test_active) {
        _mode = Mode::AudioTest;
        render();
        return;
    }

    audio::set_keyboard_sfx_enable(false);
    GetHAL().speaker.end();
    auto cfg = GetHAL().mic.config();
    cfg.magnification      = 128;
    cfg.noise_filter_level = 2;
    GetHAL().mic.config(cfg);
    GetHAL().mic.begin();

    _audio_test_buffer.assign(AUDIO_TEST_LENGTH, 0);
    _audio_stream_buffer.clear();
    _audio_frame_sequence = 0;
    _last_audio_error_log = 0;
    _audio_test_active = true;
    _mode              = Mode::AudioTest;
    GetHAL().bleMacCtlAudioState(true);
    setStatus("Recording test active");
    render();
}

void AppHomeControl::stopAudioTest()
{
    if (!_audio_test_active) {
        return;
    }
    mclog::tagInfo(getAppInfo().name, "audio test stop");

    while (GetHAL().mic.isRecording()) {
        GetHAL().delay(1);
    }
    GetHAL().mic.end();
    GetHAL().speaker.begin();
    GetHAL().speaker.setVolume(255);
    audio::set_keyboard_sfx_enable(true);
    _audio_test_buffer.clear();
    _audio_stream_buffer.clear();
    _audio_test_active = false;
    GetHAL().bleMacCtlAudioState(false);
    setStatus("Recording test stopped");
}

uint8_t AppHomeControl::encodeULaw(int16_t sample) const
{
    constexpr int BIAS = 0x84;
    constexpr int CLIP = 32635;
    int pcm = sample;
    int sign = 0;
    if (pcm < 0) {
        pcm = -pcm;
        sign = 0x80;
    }
    if (pcm > CLIP) {
        pcm = CLIP;
    }
    pcm += BIAS;

    int segment = 7;
    for (int threshold = 0x4000; (pcm & threshold) == 0 && segment > 0; threshold >>= 1) {
        --segment;
    }
    const int mantissa = (pcm >> (segment + 3)) & 0x0F;
    return static_cast<uint8_t>(~(sign | (segment << 4) | mantissa));
}

void AppHomeControl::streamAudioFrame()
{
    if (!GetHAL().bleMacCtlIsConnected() || _audio_test_buffer.empty()) {
        return;
    }

    for (size_t i = 0; i < _audio_test_buffer.size(); i += 2) {
        _audio_stream_buffer.push_back(encodeULaw(_audio_test_buffer[i]));
    }
    while (_audio_stream_buffer.size() >= AUDIO_STREAM_PAYLOAD) {
        const uint8_t sequence = _audio_frame_sequence++;
        const bool sent = GetHAL().bleMacCtlAudioFrame(sequence,
                                                       _audio_stream_buffer.data(),
                                                       static_cast<uint8_t>(AUDIO_STREAM_PAYLOAD));
        if (sent && (sequence == 0 || (sequence % 100) == 0)) {
            mclog::tagInfo(getAppInfo().name, "audio frame sent: seq={}", sequence);
        } else if (!sent && GetHAL().millis() - _last_audio_error_log > AUDIO_ERROR_LOG_MS) {
            _last_audio_error_log = GetHAL().millis();
            mclog::tagWarn(getAppInfo().name, "audio frame not sent: ble not ready");
        }
        _audio_stream_buffer.erase(_audio_stream_buffer.begin(), _audio_stream_buffer.begin() + AUDIO_STREAM_PAYLOAD);
    }
}

int AppHomeControl::pointerStep()
{
    _pointer_sensitivity = std::max(2, std::min(6, static_cast<int>(
        GetHAL().getSettings().GetInt("ptr_sens2", _pointer_sensitivity))));
    return std::max(1, POINTER_STEP * _pointer_sensitivity / 2);
}

std::string AppHomeControl::pointerSensitivityLabel() const
{
    const int value = std::max(2, std::min(6, _pointer_sensitivity));
    if ((value % 2) == 0) {
        return std::to_string(value / 2) + "x";
    }
    return std::to_string(value / 2) + ".5x";
}

void AppHomeControl::adjustPointerSensitivity(int delta)
{
    const int previous = _pointer_sensitivity;
    _pointer_sensitivity = std::max(2, std::min(6, _pointer_sensitivity + delta));
    if (_pointer_sensitivity != previous) {
        GetHAL().getSettings().SetInt("ptr_sens2", _pointer_sensitivity);
        sendHardwareSettings();
    }
    setStatus("Pointer sensitivity " + pointerSensitivityLabel());
}

void AppHomeControl::adjustKeyboardSfxVolume(int delta)
{
    const int volume = audio::adjust_keyboard_sfx_volume_percent(delta);
    setStatus(volume > 0 ? "Key sound " + std::to_string(volume) + "%" : "Key sound off");
}

void AppHomeControl::toggleKeyboardSfx()
{
    const bool enabled = audio::toggle_keyboard_sfx_user_enabled();
    setStatus(enabled ? "Key sound on" : "Key sound off");
}

void AppHomeControl::loadPowerSettings()
{
    auto& settings = GetHAL().getSettings();
    const int screenTimeoutS = std::max<int32_t>(
        5, std::min<int32_t>(255, settings.GetInt("adv_scr_s", DEFAULT_SCREEN_TIMEOUT_MS / 1000)));
    const int powerSaveMin = std::max<int32_t>(
        1, std::min<int32_t>(255, settings.GetInt("adv_pwr_m", DEFAULT_POWER_SAVE_TIMEOUT_MS / 60000)));
    _screen_timeout_ms     = static_cast<uint32_t>(screenTimeoutS) * 1000;
    _power_save_timeout_ms = static_cast<uint32_t>(powerSaveMin) * 60000;
}

void AppHomeControl::markUserActivity()
{
    _last_user_activity = GetHAL().millis();
}

void AppHomeControl::updatePowerSave()
{
    loadPowerSettings();
    const uint32_t idleMs = GetHAL().millis() - _last_user_activity;
    if (!_screen_off && idleMs >= _screen_timeout_ms) {
        sleepDisplay();
    }
    if (!_power_save_active && idleMs >= _power_save_timeout_ms) {
        enterPowerSave();
    }
}

void AppHomeControl::enterPowerSave()
{
    if (_power_save_active) {
        return;
    }

    mclog::tagInfo(getAppInfo().name, "enter power save");
    if (_audio_test_active) {
        stopAudioTest();
    }
    sleepDisplay();
    _power_save_active = true;
    resetPointerHolds();
    GetHAL().bleKeyboardSendReport(0, KEY_NONE);
    GetHAL().speaker.end();
}

void AppHomeControl::exitPowerSave()
{
    if (!_power_save_active) {
        return;
    }

    mclog::tagInfo(getAppInfo().name, "exit power save");
    _power_save_active = false;
    GetHAL().externalInput.setPaused(false);
    GetHAL().speaker.begin();
    GetHAL().setDeviceVolumePercent(GetHAL().getSettings().GetInt("device_volume", 35));
}

void AppHomeControl::sleepDisplay()
{
    if (_screen_off) {
        return;
    }

    _display_brightness_before_sleep = std::max(1, GetHAL().getDeviceBrightnessPercent());
    _screen_off                      = true;
    resetPointerHolds();
    GetHAL().bleKeyboardSendReport(0, KEY_NONE);
    GetHAL().display.setBrightness(0);
}

void AppHomeControl::wakeDisplay()
{
    if (!_screen_off) {
        markUserActivity();
        return;
    }

    exitPowerSave();
    _screen_off = false;
    markUserActivity();
    GetHAL().display.setBrightness(static_cast<uint8_t>(_display_brightness_before_sleep * 255 / 100));
    startConnections();
    render();
}

void AppHomeControl::ensureWifi()
{
    if (GetHAL().isWifiConnected()) {
        return;
    }

    auto ssid     = GetHAL().getSettings().GetString("wifi_ssid", "");
    auto password = GetHAL().getSettings().GetString("wifi_password", "");
    if (ssid.empty()) {
        setStatus("WiFi not configured");
        return;
    }

    GetHAL().wifiInit();
    if (GetHAL().wifiConnect(ssid, password)) {
        setStatus("WiFi connected");
    } else {
        setStatus("WiFi failed");
    }
}

void AppHomeControl::refreshHomePodState()
{
    _last_status_refresh = GetHAL().millis();
    if (!GetHAL().isWifiConnected()) {
        setStatus("WiFi offline");
        return;
    }
    if (!_ha.isConfigured()) {
        setStatus("HA not configured");
        return;
    }
    if (_ha.fetchHomePodState(_homepod_state)) {
        setStatus("HomePod updated");
    } else {
        setStatus("HA request failed");
    }
}

void AppHomeControl::render()
{
    if (_mode != Mode::Dashboard) {
        GetHAL().setFullscreenMode(false);
    }

    switch (_mode) {
        case Mode::Setup:
            renderSetup();
            break;
        case Mode::WifiSsid:
            renderWifiPrompt("WiFi SSID", false);
            break;
        case Mode::WifiPassword:
            renderWifiPrompt("WiFi Password", true);
            break;
        case Mode::Pointer:
            renderPointer();
            break;
        case Mode::Keyboard:
            renderKeyboard();
            break;
        case Mode::Volume:
            renderVolume();
            break;
        case Mode::Config:
            renderConfig();
            break;
        case Mode::AudioTest:
            renderAudioTest();
            break;
        case Mode::Dashboard:
        default:
            renderDashboard();
            break;
    }
}

void AppHomeControl::renderSetup()
{
    auto& canvas = GetHAL().canvas;
    canvas.fillScreen(THEME_COLOR_BG);
    canvas.setTextSize(1);
    canvas.setCursor(0, 0);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.println("ADVCtl");

    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.printf("BLE:  %s\n",
                  GetHAL().bleKeyboardIsConnected() ? "ready" : (_ble_start_requested ? "pair ADVCtl" : "stopped"));
    canvas.println("HA:   ADVCtl app");

    canvas.setTextColor(TFT_CYAN, THEME_COLOR_BG);
    canvas.println();
    canvas.println("Fn+W: WiFi settings");
    canvas.println("Fn+C: local settings");
    canvas.println("Fn+S: screen off");
    canvas.println("Fn+Enter: TV power");
    canvas.printf("Ptr:  %s  XY:%s X:%s Y:%s\n",
                  pointerSensitivityLabel().c_str(),
                  GetHAL().externalInput.getSwapAxes() ? "swap" : "normal",
                  GetHAL().externalInput.getFlipX() ? "inv" : "normal",
                  GetHAL().externalInput.getFlipY() ? "inv" : "normal");
    canvas.println("Home: launcher");

    renderStatusBar();
    GetHAL().pushCanvas();
}

void AppHomeControl::renderWifiPrompt(const char* title, bool maskInput)
{
    auto& canvas = GetHAL().canvas;
    canvas.fillScreen(THEME_COLOR_BG);
    canvas.setTextSize(1);
    canvas.setCursor(0, 0);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.println(title);

    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.println("Enter: next/save");
    canvas.println("Esc: setup");
    canvas.println();
    canvas.print("> ");
    if (maskInput) {
        for (size_t i = 0; i < _input_buffer.size(); ++i) {
            canvas.print("*");
        }
    } else {
        printClipped(_input_buffer, 30);
    }

    renderStatusBar();
    GetHAL().pushCanvas();
}

void AppHomeControl::renderDashboard()
{
    GetHAL().setFullscreenMode(false);
    auto& canvas = GetHAL().canvas;
    const auto& input = GetHAL().externalInput;
    const auto& nowPlaying = GetHAL().bleMacCtlNowPlaying();
    const char* knobMode = "SysVol";
    if (_knob_scroll_mode) {
        knobMode = "Wheel";
    } else if (_knob_mode == 1) {
        knobMode = "HomePod";
    } else if (_knob_mode == 2) {
        knobMode = "Off";
    }

    auto printShort = [&](const std::string& text, int maxChars) {
        if (static_cast<int>(text.size()) <= maxChars) {
            canvas.print(text.c_str());
            return;
        }
        if (maxChars <= 1) {
            canvas.print(".");
            return;
        }
        canvas.print((text.substr(0, maxChars - 1) + ".").c_str());
    };

    canvas.fillScreen(THEME_COLOR_BG);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.setCursor(0, 0);
    canvas.printf("ADVCtl B:%s A:%s",
                  GetHAL().bleKeyboardIsConnected() ? "ok" : "adv",
                  GetHAL().bleMacCtlIsConnected() ? "ok" : "--");

    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.setCursor(0, 12);
    canvas.printf("Wi:%s T:%s Mic:%s M:%s",
                  GetHAL().isWifiConnected() ? "ok" : "--",
                  GetHAL().bleMacCtlTimeSynced() ? "ok" : "--",
                  _audio_test_active ? "on" : "--",
                  input.isConnected() ? "ok" : "--");

    canvas.setCursor(0, 24);
    canvas.printf("Knob:%s %s",
                  input.isEncoderConnected() ? "ok" : "--",
                  knobMode);
    canvas.setCursor(98, 24);
    canvas.printf("Ptr:%s", pointerSensitivityLabel().c_str());

    canvas.setCursor(0, 36);
    canvas.printf("XY:%s X:%s Y:%s",
                  input.getSwapAxes() ? "sw" : "n",
                  input.getFlipX() ? "inv" : "n",
                  input.getFlipY() ? "inv" : "n");

    canvas.setTextColor(TFT_CYAN, THEME_COLOR_BG);
    canvas.setCursor(0, 50);
    canvas.print("TV Ent/T:Pwr Sp:In");
    canvas.setCursor(0, 62);
    canvas.print("Ptr E/S:Spd A:XY D:X R:Reset");
    canvas.setCursor(0, 74);
    canvas.print("Sys W:WiFi C:Cfg S:Sleep");
    canvas.setCursor(0, 86);
    canvas.print("Snd M:On [/]Vol  B:Pair");

    canvas.setTextColor(TFT_GREEN, THEME_COLOR_BG);
    canvas.setCursor(0, 98);
    if (nowPlaying.active) {
        canvas.printf("Now %s %u%% ", nowPlaying.playing ? ">" : "||", nowPlaying.volumePercent);
        printShort(nowPlaying.title[0] ? nowPlaying.title : "Apple TV", 18);
    } else {
        canvas.print(GetHAL().bleMacCtlIsConnected() ? "Now idle" : "Now app--");
    }

    renderStatusBar();
    GetHAL().pushCanvas();
}

void AppHomeControl::renderNowPlaying()
{
    GetHAL().setFullscreenMode(true);
    auto& display = GetHAL().display;
    const auto& state = GetHAL().bleMacCtlNowPlaying();
    const int width = display.width();
    const int height = display.height();

    uint32_t hash = 2166136261u;
    for (const char* p = state.title[0] ? state.title : "ADVCtl"; *p; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 16777619u;
    }
    const uint16_t coverA = display.color565(40 + (hash & 0x3F), 55 + ((hash >> 6) & 0x3F), 80 + ((hash >> 12) & 0x5F));
    const uint16_t coverB = display.color565(120 + ((hash >> 4) & 0x3F), 60 + ((hash >> 10) & 0x3F), 80 + ((hash >> 16) & 0x3F));

    display.fillScreen(display.color565(8, 10, 14));
    for (int y = 0; y < height; y += 3) {
        const uint8_t shade = static_cast<uint8_t>(12 + y / 5);
        display.fillRect(0, y, width, 3, display.color565(shade, shade + 2, shade + 8));
    }

    const int cover = std::min(84, height - 36);
    const int coverX = 12;
    const int coverY = 14;
    display.fillRoundRect(coverX, coverY, cover, cover, 10, coverA);
    display.fillRoundRect(coverX + 8, coverY + 8, cover - 16, cover - 16, 8, coverB);
    display.fillCircle(coverX + cover / 2, coverY + cover / 2, cover / 5, display.color565(250, 250, 250));
    display.fillCircle(coverX + cover / 2, coverY + cover / 2, cover / 12, coverA);

    const int textX = coverX + cover + 14;
    const int textW = width - textX - 12;
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE);
    display.setCursor(textX, 18);
    std::string title = state.title[0] ? state.title : "Playing";
    if (static_cast<int>(title.size()) > textW / 6) {
        title = title.substr(0, std::max(0, textW / 6 - 1)) + ".";
    }
    display.print(title.c_str());

    display.setTextColor(display.color565(175, 185, 195));
    display.setCursor(textX, 35);
    std::string artist = state.artist[0] ? state.artist : "HomePod";
    if (static_cast<int>(artist.size()) > textW / 6) {
        artist = artist.substr(0, std::max(0, textW / 6 - 1)) + ".";
    }
    display.print(artist.c_str());

    display.setTextColor(TFT_WHITE);
    display.setCursor(textX, 58);
    display.printf("%s  Vol %u%%", state.playing ? "Playing" : "Paused", state.volumePercent);
    if (state.muted) {
        display.print(" muted");
    }

    const int barX = textX;
    const int barY = 84;
    const int barW = textW;
    const int fillW = std::max(0, std::min(barW, barW * static_cast<int>(state.progressPercent) / 100));
    display.fillRoundRect(barX, barY, barW, 5, 3, display.color565(60, 65, 76));
    display.fillRoundRect(barX, barY, fillW, 5, 3, TFT_WHITE);
    display.setTextColor(display.color565(175, 185, 195));
    display.setCursor(barX, barY + 11);
    display.printf("%u%%", state.progressPercent);

    const int iconX = width - 36;
    const int iconY = height - 32;
    display.drawCircle(iconX, iconY, 13, display.color565(210, 215, 225));
    display.setCursor(iconX - 5, iconY - 4);
    display.setTextColor(TFT_WHITE);
    display.print(state.playing ? ">" : "||");
}

void AppHomeControl::renderPointer()
{
    GetHAL().setFullscreenMode(false);
    auto& canvas = GetHAL().canvas;
    canvas.fillScreen(THEME_COLOR_BG);
    canvas.setCursor(0, 0);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.println("Pointer Mode");
    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.println("Arrow: move");
    canvas.println("Enter/Space: click");
    canvas.println("Backspace: right click");
    canvas.println("[ ]: wheel");
    canvas.println("Fn+Esc: dashboard");
    renderStatusBar();
    GetHAL().pushCanvas();
}

void AppHomeControl::renderKeyboard()
{
    auto& canvas = GetHAL().canvas;
    canvas.fillScreen(THEME_COLOR_BG);
    canvas.setCursor(0, 0);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.println("Keyboard Mode");
    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.println("Typing forwards to macOS");
    canvas.println("Fn+Esc: dashboard");
    canvas.printf("BLE: %s\n", GetHAL().bleKeyboardIsConnected() ? "connected" : "advertising");
    renderStatusBar();
    GetHAL().pushCanvas();
}

void AppHomeControl::renderVolume()
{
    auto& canvas = GetHAL().canvas;
    canvas.fillScreen(THEME_COLOR_BG);
    canvas.setCursor(0, 0);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.println("Device Volume");

    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.println("Left/Right: +/- 5%");
    canvas.println("Up/Down: +/- 10%");
    canvas.println("Enter: save + test");
    canvas.println("Esc: dashboard");

    canvas.setTextColor(TFT_GREEN, THEME_COLOR_BG);
    canvas.setTextSize(2);
    canvas.setCursor(0, 56);
    canvas.printf("%d%%", _target_volume_percent);
    canvas.setTextSize(1);

    int bar_x  = 0;
    int bar_y  = 92;
    int bar_w  = canvas.width() - 2;
    int fill_w = std::max(0, std::min(bar_w, bar_w * _target_volume_percent / 100));
    canvas.drawRect(bar_x, bar_y, bar_w, 10, TFT_DARKGREY);
    canvas.fillRect(bar_x + 1, bar_y + 1, std::max(0, fill_w - 2), 8, TFT_GREEN);

    renderStatusBar();
    GetHAL().pushCanvas();
}

void AppHomeControl::renderConfig()
{
    auto& canvas = GetHAL().canvas;
    canvas.fillScreen(THEME_COLOR_BG);
    canvas.setCursor(0, 0);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.println("Config");
    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.println("url=http://ha:8123");
    canvas.println("token=LONG_TOKEN");
    canvas.println("entity=media_player.xxx");
    canvas.println("tv=0x3c,0xcc");
    canvas.println("input=0x86,0x01");
    canvas.println("vol=35");
    canvas.println("hpvol=35");
    canvas.println("Esc: dashboard");
    canvas.setTextColor(TFT_CYAN, THEME_COLOR_BG);
    canvas.print("> ");
    printClipped(_input_buffer, 28);
    renderStatusBar();
    GetHAL().pushCanvas();
}

void AppHomeControl::renderAudioTest()
{
    auto& canvas = GetHAL().canvas;
    canvas.fillScreen(THEME_COLOR_BG);
    canvas.setCursor(0, 0);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.println("ADV Recording");
    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.println("Mic active");
    canvas.println("Esc/Enter: stop");
    renderStatusBar();
    GetHAL().pushCanvas();
}

void AppHomeControl::renderAudioWaveform()
{
    if (!_audio_test_active || !GetHAL().mic.isEnabled()) {
        if (_audio_test_active && GetHAL().millis() - _last_audio_error_log > AUDIO_ERROR_LOG_MS) {
            _last_audio_error_log = GetHAL().millis();
            mclog::tagWarn(getAppInfo().name, "audio capture waiting: mic disabled");
        }
        return;
    }
    if (_audio_test_buffer.size() != AUDIO_TEST_LENGTH) {
        _audio_test_buffer.assign(AUDIO_TEST_LENGTH, 0);
    }
    if (!GetHAL().mic.record(_audio_test_buffer.data(), AUDIO_TEST_LENGTH, AUDIO_TEST_RATE)) {
        if (GetHAL().millis() - _last_audio_error_log > AUDIO_ERROR_LOG_MS) {
            _last_audio_error_log = GetHAL().millis();
            mclog::tagWarn(getAppInfo().name, "audio capture failed");
        }
        return;
    }
    streamAudioFrame();

    auto& canvas = GetHAL().canvas;
    const int32_t top = 28;
    const int32_t h   = canvas.height() - top - 12;
    const int32_t w   = std::min<int32_t>(canvas.width() - 4, AUDIO_TEST_LENGTH);
    const int32_t mid = top + h / 2;
    canvas.fillRect(0, top, canvas.width(), h, THEME_COLOR_BG);
    canvas.drawLine(0, mid, canvas.width(), mid, TFT_DARKGREY);

    for (int32_t x = 0; x < w; ++x) {
        int32_t sample = _audio_test_buffer[x] >> 8;
        int32_t y      = mid + sample;
        if (y < top) y = top;
        if (y >= top + h) y = top + h - 1;
        canvas.drawFastVLine(x + 2, std::min(mid, y), std::max<int32_t>(1, std::abs(y - mid)), TFT_CYAN);
    }

    canvas.setCursor(0, 0);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.println("ADV Recording");
    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.println("Mic active");
    renderStatusBar();
    GetHAL().pushCanvas();
}

void AppHomeControl::renderStatusBar()
{
    auto& canvas = GetHAL().canvas;
    canvas.fillRect(0, canvas.height() - 10, canvas.width(), 10, THEME_COLOR_BG);
    canvas.setCursor(0, canvas.height() - 9);
    canvas.setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
    printClipped(_status_line, 36);
}

void AppHomeControl::handleKeyEvent(const Keyboard::KeyEvent_t& keyEvent)
{
    if (_screen_off && _power_save_active) {
        if (keyEvent.state) {
            wakeDisplay();
        }
        return;
    }

    markUserActivity();
    if (!GetHAL().keyboard.isFnPressed() && keyEvent.keyCode == KEY_BACKSLASH) {
        if (keyEvent.state) {
            GetHAL().bleMouseClick(1);
            setStatus("Left click");
            if (_mode == Mode::Dashboard) {
                render();
            }
        }
        return;
    }

    if (_mode != Mode::Dashboard && GetHAL().keyboard.isFnPressed() && keyEvent.keyCode == KEY_S) {
        if (!keyEvent.state) {
            sleepDisplay();
        }
        return;
    }

    if (GetHAL().keyboard.isFnPressed() && !keyEvent.isModifier && keyEvent.keyCode == KEY_B) {
        if (keyEvent.state) {
            if (GetHAL().bleControlForgetBonds()) {
                _ble_start_requested = true;
                setStatus("BLE pairing reset");
            } else {
                setStatus("BLE reset failed");
            }
        }
        if (_mode == Mode::Dashboard) {
            render();
        }
        return;
    }

    if (GetHAL().keyboard.isFnPressed() && keyEvent.state && !keyEvent.isModifier &&
        (keyEvent.keyCode == KEY_LEFTBRACE || keyEvent.keyCode == KEY_RIGHTBRACE)) {
        const int volume = audio::get_keyboard_sfx_volume_percent();
        setStatus(volume > 0 ? "Key sound " + std::to_string(volume) + "%" : "Key sound off");
        if (_mode == Mode::Dashboard) {
            render();
        }
        return;
    }

    if (GetHAL().keyboard.isFnPressed() && keyEvent.state && !keyEvent.isModifier && keyEvent.keyCode == KEY_M) {
        setStatus(audio::is_keyboard_sfx_user_enabled() ? "Key sound on" : "Key sound off");
        if (_mode == Mode::Dashboard) {
            render();
        }
        return;
    }

    if (_mode == Mode::Dashboard) {
        handleDashboardKey(keyEvent);
        return;
    }

    if (_mode == Mode::Keyboard) {
        handleKeyboardKey(keyEvent);
        return;
    }

    if (_mode == Mode::Setup) {
        handleSetupKey(keyEvent);
        return;
    }

    if (_mode == Mode::WifiSsid || _mode == Mode::WifiPassword) {
        handleWifiKey(keyEvent);
        return;
    }

    if (_mode == Mode::AudioTest) {
        if (keyEvent.state && !keyEvent.isModifier && (keyEvent.keyCode == KEY_ESC || isEnterKey(keyEvent))) {
            stopAudioTest();
            _mode = Mode::Dashboard;
            render();
        } else {
            forwardKeyboardEvent(keyEvent);
        }
        return;
    }

    if (!keyEvent.state || keyEvent.isModifier) {
        return;
    }

    switch (_mode) {
        case Mode::Pointer:
            handlePointerKey(keyEvent);
            break;
        case Mode::Volume:
            handleVolumeKey(keyEvent);
            break;
        case Mode::Config:
            handleConfigKey(keyEvent);
            break;
        case Mode::Dashboard:
        default:
            handleDashboardKey(keyEvent);
            break;
    }
}

void AppHomeControl::handleSetupKey(const Keyboard::KeyEvent_t& keyEvent)
{
    if (!keyEvent.state || keyEvent.isModifier) {
        return;
    }

    if (GetHAL().keyboard.isFnPressed() && keyEvent.keyCode == KEY_W) {
        _mode         = Mode::WifiSsid;
        _input_buffer = _wifi_ssid;
        setStatus("Set WiFi SSID");
    } else if (GetHAL().keyboard.isFnPressed() && keyEvent.keyCode == KEY_C) {
        _mode = Mode::Config;
        _input_buffer.clear();
        setStatus("Set HA lines");
    } else if (isEnterKey(keyEvent) || keyEvent.keyCode == KEY_R) {
        startConnections();
        return;
    }
    enterControlIfReady();
    render();
}

void AppHomeControl::handleWifiKey(const Keyboard::KeyEvent_t& keyEvent)
{
    if (!keyEvent.state || keyEvent.isModifier) {
        return;
    }

    if (keyEvent.keyCode == KEY_ESC) {
        _mode = Mode::Setup;
        _input_buffer.clear();
        setStatus("Setup");
        render();
        return;
    }

    if (keyEvent.keyCode == KEY_ENTER) {
        if (_mode == Mode::WifiSsid) {
            if (!_input_buffer.empty()) {
                _wifi_ssid = _input_buffer;
            }
            _input_buffer = _wifi_password;
            _mode         = Mode::WifiPassword;
            setStatus("Set WiFi password");
        } else {
            _wifi_password = _input_buffer;
            saveWifiConfig();
            _input_buffer.clear();
            _auto_wifi_attempted = false;
            _mode                = Mode::Setup;
            setStatus("WiFi saved; Enter connects");
        }
        render();
        return;
    }

    if (keyEvent.keyCode == KEY_BACKSPACE) {
        if (!_input_buffer.empty()) {
            _input_buffer.pop_back();
        }
        render();
        return;
    }

    if (keyEvent.keyName && std::strlen(keyEvent.keyName) == 1 && _input_buffer.size() < 96) {
        _input_buffer += keyEvent.keyName;
        render();
    } else if (keyEvent.keyCode == KEY_SPACE && _input_buffer.size() < 96) {
        _input_buffer += ' ';
        render();
    }
}

void AppHomeControl::handleDashboardKey(const Keyboard::KeyEvent_t& keyEvent)
{
    if (handleDashboardFnControl(keyEvent)) {
        return;
    }

    forwardKeyboardEvent(keyEvent);
}

bool AppHomeControl::handleDashboardFnControl(const Keyboard::KeyEvent_t& keyEvent)
{
    const bool fnActive = GetHAL().keyboard.isFnPressed();
    if ((isLeftKey(keyEvent) && _hold_left) || (isRightKey(keyEvent) && _hold_right) ||
        (isUpKey(keyEvent) && _hold_up) || (isDownKey(keyEvent) && _hold_down)) {
        updatePointerHold(keyEvent);
        GetHAL().bleKeyboardSendReport(GetHAL().keyboard.getModifierMask(), KEY_NONE);
        return true;
    }

    if (!fnActive || !keyEvent.state || keyEvent.isModifier) {
        return false;
    }

    if (keyEvent.keyCode == KEY_E) {
        adjustPointerSensitivity(1);
    } else if (keyEvent.keyCode == KEY_S) {
        adjustPointerSensitivity(-1);
    } else if (keyEvent.keyCode == KEY_A) {
        GetHAL().externalInput.toggleSwapAxes();
        sendHardwareSettings();
        setStatus(GetHAL().externalInput.getSwapAxes() ? "Joystick XY swapped" : "Joystick XY normal");
    } else if (keyEvent.keyCode == KEY_D) {
        GetHAL().externalInput.toggleFlipX();
        sendHardwareSettings();
        setStatus(GetHAL().externalInput.getFlipX() ? "Joystick X inverted" : "Joystick X normal");
    } else if (keyEvent.keyCode == KEY_R) {
        resetHardwareSettings();
    } else if (keyEvent.keyCode == KEY_LEFTBRACE) {
        setStatus(audio::get_keyboard_sfx_volume_percent() > 0 ? "Key sound on" : "Key sound off");
    } else if (keyEvent.keyCode == KEY_RIGHTBRACE) {
        setStatus(audio::get_keyboard_sfx_volume_percent() > 0 ? "Key sound on" : "Key sound off");
    } else if (isEnterKey(keyEvent)) {
        sendTvPower();
    } else if (keyEvent.keyCode == KEY_SPACE) {
        sendTvInputSource();
    } else if (keyEvent.keyCode == KEY_W) {
        _mode         = Mode::WifiSsid;
        _input_buffer = _wifi_ssid;
        setStatus("Set WiFi SSID");
    } else if (keyEvent.keyCode == KEY_C) {
        _mode = Mode::Config;
        _input_buffer.clear();
        setStatus("Set HA lines");
    } else if (keyEvent.keyCode == KEY_T) {
        sendTvPower();
    } else if (keyEvent.keyCode == KEY_R) {
        setStatus("Karabiner owns HA");
    } else {
        return false;
    }

    GetHAL().bleKeyboardSendReport(GetHAL().keyboard.getModifierMask(), KEY_NONE);
    render();
    return true;
}

void AppHomeControl::forwardKeyboardEvent(const Keyboard::KeyEvent_t& keyEvent)
{
    if (GetHAL().bleMacSystemControlKey(keyEvent)) {
        return;
    }

    uint8_t modifier = GetHAL().keyboard.getModifierMask() | keyEvent.extraModifiers;
    KeScanCode_t key = keyEvent.isModifier ? KEY_NONE : keyEvent.keyCode;
    if (!keyEvent.state) {
        modifier = GetHAL().keyboard.getModifierMask();
        key      = KEY_NONE;
    }
    GetHAL().bleKeyboardSendReport(modifier, key);
}

void AppHomeControl::handlePointerKey(const Keyboard::KeyEvent_t& keyEvent)
{
    if (!keyEvent.state) {
        return;
    }

    if (keyEvent.keyCode == KEY_ESC) {
        _mode = Mode::Dashboard;
    } else if (isLeftKey(keyEvent)) {
        GetHAL().bleMouseMove(-pointerStep(), 0);
    } else if (isRightKey(keyEvent)) {
        GetHAL().bleMouseMove(pointerStep(), 0);
    } else if (isUpKey(keyEvent)) {
        GetHAL().bleMouseMove(0, -pointerStep());
    } else if (isDownKey(keyEvent)) {
        GetHAL().bleMouseMove(0, pointerStep());
    } else if (isEnterKey(keyEvent) || keyEvent.keyCode == KEY_SPACE) {
        GetHAL().bleMouseClick(1);
    } else if (keyEvent.keyCode == KEY_BACKSPACE) {
        GetHAL().bleMouseClick(2);
    } else if (keyEvent.keyCode == KEY_LEFTBRACE) {
        GetHAL().bleMouseMove(0, 0, -1);
    } else if (keyEvent.keyCode == KEY_RIGHTBRACE) {
        GetHAL().bleMouseMove(0, 0, 1);
    }
    render();
}

void AppHomeControl::handleKeyboardKey(const Keyboard::KeyEvent_t& keyEvent)
{
    if (GetHAL().keyboard.isFnPressed() && keyEvent.state &&
        (keyEvent.keyCode == KEY_LEFTBRACE || keyEvent.keyCode == KEY_RIGHTBRACE)) {
        return;
    }

    if (GetHAL().keyboard.isFnPressed() && keyEvent.state && keyEvent.keyCode == KEY_ESC) {
        GetHAL().bleKeyboardSendReport(0, KEY_NONE);
        _mode = Mode::Dashboard;
        render();
        return;
    }

    forwardKeyboardEvent(keyEvent);
}

void AppHomeControl::handleVolumeKey(const Keyboard::KeyEvent_t& keyEvent)
{
    if (keyEvent.keyCode == KEY_ESC) {
        _mode = Mode::Dashboard;
    } else if (isLeftKey(keyEvent)) {
        _target_volume_percent = std::max(0, _target_volume_percent - 5);
    } else if (isRightKey(keyEvent)) {
        _target_volume_percent = std::min(100, _target_volume_percent + 5);
    } else if (isDownKey(keyEvent)) {
        _target_volume_percent = std::max(0, _target_volume_percent - 10);
    } else if (isUpKey(keyEvent)) {
        _target_volume_percent = std::min(100, _target_volume_percent + 10);
    } else if (isEnterKey(keyEvent)) {
        applyVolume();
    }
    render();
}

void AppHomeControl::handleConfigKey(const Keyboard::KeyEvent_t& keyEvent)
{
    if (keyEvent.keyCode == KEY_ESC) {
        _mode = Mode::Setup;
        render();
        return;
    }

    if (keyEvent.keyCode == KEY_ENTER) {
        if (parseConfigLine(_input_buffer)) {
            saveHomeAssistantConfig();
            setStatus("Config saved");
        } else {
            setStatus("Config ignored");
        }
        _input_buffer.clear();
        render();
        return;
    }

    if (keyEvent.keyCode == KEY_BACKSPACE) {
        if (!_input_buffer.empty()) {
            _input_buffer.pop_back();
        }
        render();
        return;
    }

    if (keyEvent.keyName && std::strlen(keyEvent.keyName) == 1 && _input_buffer.size() < 96) {
        _input_buffer += keyEvent.keyName;
        render();
    } else if (keyEvent.keyCode == KEY_SPACE && _input_buffer.size() < 96) {
        _input_buffer += ' ';
        render();
    }
}

void AppHomeControl::activateSelectedAction()
{
    switch (_selected_action) {
        case 0:
            sendTvPower();
            break;
        case 1:
            GetHAL().bleControlInit();
            _mode = Mode::Pointer;
            setStatus("BLE pointer ready");
            break;
        case 2:
            GetHAL().bleControlInit();
            _mode = Mode::Keyboard;
            setStatus("BLE keyboard ready");
            break;
        case 3:
            sendTvInputSource();
            break;
        case 4:
            if (GetHAL().bleMacCtlPlayPause()) {
                setStatus("HomePod mute toggle");
            } else {
                setStatus("BLE not ready");
            }
            break;
        case 5:
            if (GetHAL().bleMacCtlVolumeDelta(1)) {
                setStatus("HomePod volume up");
            } else {
                setStatus("BLE not ready");
            }
            break;
        case 6:
            openVolumeSetter();
            break;
        case 7:
            _mode = Mode::Config;
            break;
        default:
            break;
    }
}

void AppHomeControl::openVolumeSetter()
{
    _target_volume_percent = GetHAL().getDeviceVolumePercent();
    _mode                  = Mode::Volume;
    setStatus("Set device volume");
}

void AppHomeControl::applyVolume()
{
    GetHAL().setDeviceVolumePercent(_target_volume_percent);
    audio::play_random_tone();
    setStatus("Device volume saved");
}

void AppHomeControl::sendTvPower()
{
    GetHAL().irInit();
    GetHAL().irSendXiaomi(_tv_power_addr, _tv_power_cmd, XIAOMI_TV_IR_REPEATS);
    GetHAL().delay(40);
    GetHAL().irSendRaw(MITV_RAW_CARRIER_HZ, MITV_RAW_POWER, sizeof(MITV_RAW_POWER) / sizeof(MITV_RAW_POWER[0]));
    setStatus("TV power x2");
}

void AppHomeControl::sendTvInputSource()
{
    GetHAL().irInit();
    GetHAL().irSendXiaomi(_tv_input_addr, _tv_input_cmd, XIAOMI_TV_IR_REPEATS);
    setStatus("TV input");
}

void AppHomeControl::setStatus(const std::string& status)
{
    _status_line = status;
}

bool AppHomeControl::parseConfigLine(const std::string& line)
{
    auto pos = line.find('=');
    if (pos == std::string::npos) {
        return false;
    }

    auto key   = line.substr(0, pos);
    auto value = line.substr(pos + 1);
    if (key == "url") {
        _ha_config.baseUrl = value;
    } else if (key == "token") {
        _ha_config.token = value;
    } else if (key == "entity") {
        _ha_config.homepodEntity = value;
    } else if (key == "tv") {
        return parseTvPowerConfig(value);
    } else if (key == "input" || key == "src") {
        return parseTvInputConfig(value);
    } else if (key == "vol") {
        int volume = std::strtol(value.c_str(), nullptr, 0);
        if (volume < 0 || volume > 100) {
            return false;
        }
        _target_volume_percent = volume;
        GetHAL().setDeviceVolumePercent(_target_volume_percent);
    } else if (key == "hpvol") {
        return false;
    } else {
        return false;
    }
    return true;
}

bool AppHomeControl::parseTvPowerConfig(const std::string& value)
{
    auto comma = value.find(',');
    if (comma == std::string::npos) {
        return false;
    }

    int addr = std::strtol(value.substr(0, comma).c_str(), nullptr, 0);
    int cmd  = std::strtol(value.substr(comma + 1).c_str(), nullptr, 0);
    if (addr < 0 || addr > 255 || cmd < 0 || cmd > 255) {
        return false;
    }

    _tv_power_addr = static_cast<uint8_t>(addr);
    _tv_power_cmd  = static_cast<uint8_t>(cmd);
    return true;
}

bool AppHomeControl::parseTvInputConfig(const std::string& value)
{
    auto comma = value.find(',');
    if (comma == std::string::npos) {
        return false;
    }

    int addr = std::strtol(value.substr(0, comma).c_str(), nullptr, 0);
    int cmd  = std::strtol(value.substr(comma + 1).c_str(), nullptr, 0);
    if (addr < 0 || addr > 255 || cmd < 0 || cmd > 255) {
        return false;
    }

    _tv_input_addr = static_cast<uint8_t>(addr);
    _tv_input_cmd  = static_cast<uint8_t>(cmd);
    return true;
}

bool AppHomeControl::isLeftKey(const Keyboard::KeyEvent_t& keyEvent) const
{
    auto raw = GetHAL().keyboard.getLatestKeyEventRaw();
    return keyEvent.keyCode == KEY_LEFT || (raw.row == 3 && raw.col == 10);
}

bool AppHomeControl::isRightKey(const Keyboard::KeyEvent_t& keyEvent) const
{
    auto raw = GetHAL().keyboard.getLatestKeyEventRaw();
    return keyEvent.keyCode == KEY_RIGHT || (raw.row == 3 && raw.col == 12);
}

bool AppHomeControl::isUpKey(const Keyboard::KeyEvent_t& keyEvent) const
{
    auto raw = GetHAL().keyboard.getLatestKeyEventRaw();
    return keyEvent.keyCode == KEY_UP || (raw.row == 2 && raw.col == 11);
}

bool AppHomeControl::isDownKey(const Keyboard::KeyEvent_t& keyEvent) const
{
    auto raw = GetHAL().keyboard.getLatestKeyEventRaw();
    return keyEvent.keyCode == KEY_DOWN || (raw.row == 3 && raw.col == 11);
}

bool AppHomeControl::isEnterKey(const Keyboard::KeyEvent_t& keyEvent) const
{
    auto raw = GetHAL().keyboard.getLatestKeyEventRaw();
    return keyEvent.keyCode == KEY_ENTER || (raw.row == 2 && raw.col == 13);
}

void AppHomeControl::printClipped(const std::string& text, int maxChars)
{
    if (static_cast<int>(text.size()) <= maxChars) {
        GetHAL().canvas.print(text.c_str());
        return;
    }

    auto clipped = text.substr(0, std::max(0, maxChars - 3)) + "...";
    GetHAL().canvas.print(clipped.c_str());
}
