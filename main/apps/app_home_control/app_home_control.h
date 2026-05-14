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
    };

    struct Action {
        const char* label;
        const char* hint;
    };

    Mode _mode = Mode::Dashboard;
    HomeAssistantClient _ha;
    HomeAssistantClient::Config _ha_config;
    HomeAssistantClient::MediaState _homepod_state;
    int _key_event_slot_id = -1;
    int _selected_action = 0;
    int _target_volume_percent = 30;
    uint8_t _tv_power_addr = 0x04;
    uint8_t _tv_power_cmd = 0x08;
    uint32_t _last_status_refresh = 0;
    uint32_t _last_pointer_repeat = 0;
    uint32_t _last_setup_render = 0;
    bool _auto_wifi_attempted = false;
    bool _ble_start_requested = false;
    bool _hold_left = false;
    bool _hold_right = false;
    bool _hold_up = false;
    bool _hold_down = false;
    std::string _wifi_ssid;
    std::string _wifi_password;
    std::string _input_buffer;
    std::string _status_line;

    static constexpr int POINTER_STEP = 12;
    static constexpr uint32_t POINTER_REPEAT_MS = 55;
    static constexpr uint32_t SETUP_RENDER_MS = 1000;
    static constexpr int ACTION_COUNT = 8;
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
    void refreshHomePodState();
    void render();
    void renderSetup();
    void renderWifiPrompt(const char* title, bool maskInput);
    void renderDashboard();
    void renderPointer();
    void renderKeyboard();
    void renderVolume();
    void renderConfig();
    void renderStatusBar();
    void handleKeyEvent(const Keyboard::KeyEvent_t& keyEvent);
    void handleSetupKey(const Keyboard::KeyEvent_t& keyEvent);
    void handleWifiKey(const Keyboard::KeyEvent_t& keyEvent);
    void handleDashboardKey(const Keyboard::KeyEvent_t& keyEvent);
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
