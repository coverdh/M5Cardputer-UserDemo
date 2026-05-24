/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "home_assistant_client.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <cstdint>
#include <string>
#include <vector>

class AppHomeControl : public mooncake::AppAbility {
public:
    AppHomeControl();
    ~AppHomeControl();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class Mode {
        Setup,
        Dashboard,
        Pointer,
        Keyboard,
        Volume,
        Config,
        WifiSsid,
        WifiPassword,
        AudioTest,
    };

    struct Action {
        const char* label;
        const char* hint;
    };

    Mode _mode = Mode::Dashboard;
    HomeAssistantClient _ha;
    HomeAssistantClient::Config _ha_config;
    HomeAssistantClient::MediaState _homepod_state;
    int _key_event_slot_id         = -1;
    int _selected_action           = 0;
    int _target_volume_percent     = 30;
    int _pointer_sensitivity       = 2;
    uint32_t _screen_timeout_ms    = 30000;
    uint32_t _power_save_timeout_ms = 180000;
    uint8_t _tv_power_addr         = 0x04;
    uint8_t _tv_power_cmd          = 0x08;
    uint32_t _last_status_refresh  = 0;
    uint32_t _last_pointer_repeat  = 0;
    uint32_t _last_external_render = 0;
    uint32_t _last_volume_apply    = 0;
    uint32_t _last_setup_render    = 0;
    uint32_t _last_ble_retry       = 0;
    uint32_t _last_user_activity   = 0;
    int16_t _pending_volume_delta  = 0;
    int _display_brightness_before_sleep = 100;
    bool _auto_wifi_attempted      = false;
    bool _ble_start_requested      = false;
    bool _screen_off               = false;
    bool _power_save_active        = false;
    bool _knob_scroll_mode         = false;
    bool _hold_left                = false;
    bool _hold_right               = false;
    bool _hold_up                  = false;
    bool _hold_down                = false;
    bool _audio_test_active        = false;
    uint8_t _audio_frame_sequence  = 0;
    uint32_t _last_audio_error_log = 0;
    uint8_t _knob_mode             = 0;
    std::string _wifi_ssid;
    std::string _wifi_password;
    std::string _input_buffer;
    std::string _status_line;
    std::string _knob_status       = "idle";
    std::string _pointer_status    = "idle";
    std::vector<int16_t> _audio_test_buffer;
    std::vector<uint8_t> _audio_stream_buffer;

    static constexpr int POINTER_STEP            = 12;
    static constexpr uint32_t POINTER_REPEAT_MS  = 55;
    static constexpr uint32_t EXTERNAL_RENDER_MS = 250;
    static constexpr uint32_t SETUP_RENDER_MS    = 1000;
    static constexpr uint32_t DASHBOARD_RENDER_MS = 1000;
    static constexpr uint32_t DEFAULT_SCREEN_TIMEOUT_MS = 30000;
    static constexpr uint32_t DEFAULT_POWER_SAVE_TIMEOUT_MS = 180000;
    static constexpr int ACTION_COUNT            = 8;
    static constexpr size_t AUDIO_TEST_LENGTH    = 120;
    static constexpr size_t AUDIO_TEST_RATE      = 16000;
    static constexpr size_t AUDIO_STREAM_PAYLOAD = 60;
    static constexpr uint32_t AUDIO_ERROR_LOG_MS = 2000;
    static const Action ACTIONS[ACTION_COUNT];

    void loadConfig();
    void saveHomeAssistantConfig();
    void loadWifiConfig();
    void saveWifiConfig();
    void startConnections();
    void tryAutoWifiConnect();
    bool ensureHomeAssistantReady();
    void ensureWifi();
    bool isConnectionReady() const;
    void enterControlIfReady();
    void resetPointerHolds();
    void updatePointerHold(const Keyboard::KeyEvent_t& keyEvent);
    void repeatPointerMove();
    void handleExternalInput();
    void handleExternalPointer(uint8_t buttons, uint8_t pressed, uint8_t released);
    void handleExternalEncoder();
    void handleBleControlRequests();
    void applyHardwareSettings(uint8_t flags, uint8_t sensitivity, uint8_t knobMode);
    void resetHardwareSettings();
    void sendHardwareSettings();
    void startAudioTest();
    void stopAudioTest();
    uint8_t encodeULaw(int16_t sample) const;
    void streamAudioFrame();
    void sendKnobWheelStep(int8_t step);
    void sendKnobControlStep(int8_t step);
    int pointerStep();
    std::string pointerSensitivityLabel() const;
    void adjustPointerSensitivity(int delta);
    void adjustKeyboardSfxVolume(int delta);
    void toggleKeyboardSfx();
    void loadPowerSettings();
    void markUserActivity();
    void updatePowerSave();
    void sleepDisplay();
    void wakeDisplay();
    void enterPowerSave();
    void exitPowerSave();
    void refreshHomePodState();
    void render();
    void renderSetup();
    void renderWifiPrompt(const char* title, bool maskInput);
    void renderDashboard();
    void renderNowPlaying();
    void renderPointer();
    void renderKeyboard();
    void renderVolume();
    void renderConfig();
    void renderAudioTest();
    void renderAudioWaveform();
    void renderStatusBar();
    void handleKeyEvent(const Keyboard::KeyEvent_t& keyEvent);
    void handleSetupKey(const Keyboard::KeyEvent_t& keyEvent);
    void handleWifiKey(const Keyboard::KeyEvent_t& keyEvent);
    void handleDashboardKey(const Keyboard::KeyEvent_t& keyEvent);
    bool handleDashboardFnControl(const Keyboard::KeyEvent_t& keyEvent);
    void forwardKeyboardEvent(const Keyboard::KeyEvent_t& keyEvent);
    void handlePointerKey(const Keyboard::KeyEvent_t& keyEvent);
    void handleKeyboardKey(const Keyboard::KeyEvent_t& keyEvent);
    void handleVolumeKey(const Keyboard::KeyEvent_t& keyEvent);
    void handleConfigKey(const Keyboard::KeyEvent_t& keyEvent);
    void activateSelectedAction();
    void openVolumeSetter();
    void applyVolume();
    void sendTvPower();
    void setStatus(const std::string& status);
    bool parseConfigLine(const std::string& line);
    bool parseTvPowerConfig(const std::string& value);
    bool isLeftKey(const Keyboard::KeyEvent_t& keyEvent) const;
    bool isRightKey(const Keyboard::KeyEvent_t& keyEvent) const;
    bool isUpKey(const Keyboard::KeyEvent_t& keyEvent) const;
    bool isDownKey(const Keyboard::KeyEvent_t& keyEvent) const;
    bool isEnterKey(const Keyboard::KeyEvent_t& keyEvent) const;
    void printClipped(const std::string& text, int maxChars);
};
