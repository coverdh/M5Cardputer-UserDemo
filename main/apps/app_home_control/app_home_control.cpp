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

const AppHomeControl::Action AppHomeControl::ACTIONS[AppHomeControl::ACTION_COUNT] = {
    {"TV Power", "IR toggle"},
    {"Pointer", "BLE mouse"},
    {"Keyboard", "BLE keys"},
    {"HP Vol-", "HA volume"},
    {"HP Play", "HA media"},
    {"HP Vol+", "HA volume"},
    {"Volume", "device"},
    {"Config", "type setup"},
};

AppHomeControl::AppHomeControl()
{
    setAppInfo().name     = "MacCtl";
    setAppInfo().userData = new AppIcon_t(image_data_macctl_big, image_data_macctl_small);
}

AppHomeControl::~AppHomeControl()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppHomeControl::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    _mode = Mode::Setup;
    _selected_action = 0;
    _input_buffer.clear();
    _auto_wifi_attempted = false;
    _ble_start_requested = false;
    resetPointerHolds();
    setStatus("Enter starts BLE/WiFi");

    loadConfig();
    loadWifiConfig();

    _key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& keyEvent) { handleKeyEvent(keyEvent); });
    render();
}

void AppHomeControl::onRunning()
{
    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
        return;
    }

    if (_mode == Mode::Setup) {
        enterControlIfReady();
        if (GetHAL().millis() - _last_setup_render > SETUP_RENDER_MS) {
            _last_setup_render = GetHAL().millis();
            render();
        }
        return;
    }

    if (_mode == Mode::Dashboard) {
        repeatPointerMove();
    }

    if (_mode == Mode::Dashboard && _ha.isConfigured() && GetHAL().isWifiConnected() &&
        GetHAL().millis() - _last_status_refresh > 15000) {
        refreshHomePodState();
        render();
    }
}

void AppHomeControl::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    if (_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_event_slot_id);
        _key_event_slot_id = -1;
    }
    resetPointerHolds();
    GetHAL().bleKeyboardSendReport(0, KEY_NONE);
}

void AppHomeControl::loadConfig()
{
    auto& settings = GetHAL().getSettings();
    _ha_config.baseUrl = settings.GetString("ha_url", "");
    _ha_config.token = settings.GetString("ha_token", "");
    _ha_config.homepodEntity = settings.GetString("ha_homepod", "");
    _tv_power_addr = static_cast<uint8_t>(settings.GetInt("tv_power_addr", 0x04));
    _tv_power_cmd = static_cast<uint8_t>(settings.GetInt("tv_power_cmd", 0x08));
    _ha.setConfig(_ha_config);
}

void AppHomeControl::loadWifiConfig()
{
    auto& settings = GetHAL().getSettings();
    _wifi_ssid = settings.GetString("wifi_ssid", "");
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
        _ble_start_requested = true;
        setStatus("Starting BLE");
        render();
        GetHAL().bleControlInit();
    }
    tryAutoWifiConnect();
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
    return GetHAL().bleKeyboardIsConnected() && GetHAL().isWifiConnected() && _ha.isConfigured();
}

void AppHomeControl::enterControlIfReady()
{
    if (_mode != Mode::Setup || !isConnectionReady()) {
        return;
    }

    _mode = Mode::Dashboard;
    resetPointerHolds();
    setStatus("Control ready");
    if (_ha.isConfigured()) {
        refreshHomePodState();
    }
    render();
}

void AppHomeControl::resetPointerHolds()
{
    _hold_left = false;
    _hold_right = false;
    _hold_up = false;
    _hold_down = false;
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
        dx -= POINTER_STEP;
    }
    if (_hold_right) {
        dx += POINTER_STEP;
    }
    if (_hold_up) {
        dy -= POINTER_STEP;
    }
    if (_hold_down) {
        dy += POINTER_STEP;
    }

    _last_pointer_repeat = GetHAL().millis();
    GetHAL().bleMouseMove(dx, dy);
}

void AppHomeControl::ensureWifi()
{
    if (GetHAL().isWifiConnected()) {
        return;
    }

    auto ssid = GetHAL().getSettings().GetString("wifi_ssid", "");
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
    canvas.println("MacCtl Setup");

    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.printf("BLE:  %s\n", GetHAL().bleKeyboardIsConnected() ? "paired" :
                                                     (_ble_start_requested ? "pair MacCtl" : "stopped"));
    canvas.printf("WiFi: %s\n", GetHAL().isWifiConnected() ? "connected" :
                                                     (_wifi_ssid.empty() ? "not set" : _wifi_ssid.c_str()));
    canvas.printf("HA:   %s\n", _ha.isConfigured() ? "configured" : "not set");

    canvas.setTextColor(TFT_CYAN, THEME_COLOR_BG);
    canvas.println();
    canvas.println("W: WiFi");
    canvas.println("C: Home Assistant");
    canvas.println("Enter: start/connect");

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
    auto& canvas = GetHAL().canvas;
    canvas.fillScreen(THEME_COLOR_BG);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.setCursor(0, 0);
    canvas.println("HomePod");

    canvas.setTextColor(TFT_CYAN, THEME_COLOR_BG);
    if (_homepod_state.ok) {
        canvas.print("State: ");
        printClipped(_homepod_state.state.empty() ? "unknown" : _homepod_state.state, 18);
        if (_homepod_state.volumePercent >= 0) {
            canvas.printf(" %d%%", _homepod_state.volumePercent);
        }
        canvas.println();
        if (!_homepod_state.name.empty()) {
            canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
            printClipped(_homepod_state.name, 28);
            canvas.println();
        }
        if (!_homepod_state.title.empty()) {
            canvas.setTextColor(TFT_GREEN, THEME_COLOR_BG);
            printClipped(_homepod_state.title, 30);
            canvas.println();
        }
        if (!_homepod_state.artist.empty()) {
            canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
            printClipped(_homepod_state.artist, 30);
            canvas.println();
        }
    } else {
        canvas.println(_ha.isConfigured() ? "No HomePod state" : "HA not configured");
    }

    renderStatusBar();
    GetHAL().pushCanvas();
}

void AppHomeControl::renderPointer()
{
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
    canvas.println("Esc: dashboard");
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
    canvas.println("Esc: dashboard");
    canvas.printf("BLE: %s\n", GetHAL().bleKeyboardIsConnected() ? "paired" : "advertising");
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

    int bar_x = 0;
    int bar_y = 92;
    int bar_w = canvas.width() - 2;
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
    canvas.println("tv=0x04,0x08");
    canvas.println("vol=35");
    canvas.println("hpvol=35");
    canvas.println("Esc: dashboard");
    canvas.setTextColor(TFT_CYAN, THEME_COLOR_BG);
    canvas.print("> ");
    printClipped(_input_buffer, 28);
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

    if (keyEvent.keyCode == KEY_W) {
        _mode = Mode::WifiSsid;
        _input_buffer = _wifi_ssid;
        setStatus("Set WiFi SSID");
    } else if (keyEvent.keyCode == KEY_C) {
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
            _mode = Mode::WifiPassword;
            setStatus("Set WiFi password");
        } else {
            _wifi_password = _input_buffer;
            saveWifiConfig();
            _input_buffer.clear();
            _auto_wifi_attempted = false;
            _mode = Mode::Setup;
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
    if (GetHAL().keyboard.isFnPressed() && keyEvent.state) {
        if (keyEvent.keyCode == KEY_MINUS) {
            if (ensureHomeAssistantReady() && _ha.homePodVolumeDown()) {
                refreshHomePodState();
            } else {
                setStatus("HA request failed");
            }
            render();
            return;
        }
        if (keyEvent.keyCode == KEY_EQUAL) {
            if (ensureHomeAssistantReady() && _ha.homePodVolumeUp()) {
                refreshHomePodState();
            } else {
                setStatus("HA request failed");
            }
            render();
            return;
        }
        if (keyEvent.keyCode == KEY_SPACE) {
            if (ensureHomeAssistantReady() && _ha.homePodPlayPause()) {
                refreshHomePodState();
            } else {
                setStatus("HA request failed");
            }
            render();
            return;
        }
        if (keyEvent.keyCode == KEY_T) {
            sendTvPower();
            render();
            return;
        }
        if (keyEvent.keyCode == KEY_R) {
            if (ensureHomeAssistantReady()) {
                refreshHomePodState();
            }
            render();
            return;
        }
    }

    if (isLeftKey(keyEvent) || isRightKey(keyEvent) || isUpKey(keyEvent) || isDownKey(keyEvent)) {
        updatePointerHold(keyEvent);
        if (keyEvent.state) {
            repeatPointerMove();
        }
        return;
    }

    if (!keyEvent.state) {
        GetHAL().bleKeyboardSendReport(GetHAL().keyboard.getModifierMask(), KEY_NONE);
        return;
    }

    if (keyEvent.keyCode == KEY_LEFTBRACE) {
        GetHAL().bleMouseClick(1);
        setStatus("Left click");
        return;
    }

    if (keyEvent.keyCode == KEY_RIGHTBRACE) {
        GetHAL().bleMouseClick(2);
        setStatus("Right click");
        return;
    }

    uint8_t modifier = GetHAL().keyboard.getModifierMask() | keyEvent.extraModifiers;
    KeScanCode_t key = keyEvent.isModifier ? KEY_NONE : keyEvent.keyCode;
    GetHAL().bleKeyboardSendReport(modifier, key);
}

void AppHomeControl::handlePointerKey(const Keyboard::KeyEvent_t& keyEvent)
{
    if (keyEvent.keyCode == KEY_ESC) {
        _mode = Mode::Dashboard;
    } else if (isLeftKey(keyEvent)) {
        GetHAL().bleMouseMove(-POINTER_STEP, 0);
    } else if (isRightKey(keyEvent)) {
        GetHAL().bleMouseMove(POINTER_STEP, 0);
    } else if (isUpKey(keyEvent)) {
        GetHAL().bleMouseMove(0, -POINTER_STEP);
    } else if (isDownKey(keyEvent)) {
        GetHAL().bleMouseMove(0, POINTER_STEP);
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

    if (keyEvent.state && keyEvent.keyCode == KEY_ESC) {
        GetHAL().bleKeyboardSendReport(0, KEY_NONE);
        _mode = Mode::Dashboard;
        render();
        return;
    }

    uint8_t modifier = GetHAL().keyboard.getModifierMask() | keyEvent.extraModifiers;
    KeScanCode_t key = keyEvent.isModifier ? KEY_NONE : keyEvent.keyCode;
    if (!keyEvent.state) {
        key = KEY_NONE;
        modifier = GetHAL().keyboard.getModifierMask();
    }
    GetHAL().bleKeyboardSendReport(modifier, key);
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
            if (ensureHomeAssistantReady()) {
                if (_ha.homePodVolumeDown()) {
                    refreshHomePodState();
                } else {
                    setStatus("HA request failed");
                }
            }
            break;
        case 4:
            if (ensureHomeAssistantReady()) {
                if (_ha.homePodPlayPause()) {
                    refreshHomePodState();
                } else {
                    setStatus("HA request failed");
                }
            }
            break;
        case 5:
            if (ensureHomeAssistantReady()) {
                if (_ha.homePodVolumeUp()) {
                    refreshHomePodState();
                } else {
                    setStatus("HA request failed");
                }
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
    _mode = Mode::Volume;
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
    GetHAL().irSend(_tv_power_addr, _tv_power_cmd);
    setStatus("TV power sent");
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

    auto key = line.substr(0, pos);
    auto value = line.substr(pos + 1);
    if (key == "url") {
        _ha_config.baseUrl = value;
    } else if (key == "token") {
        _ha_config.token = value;
    } else if (key == "entity") {
        _ha_config.homepodEntity = value;
    } else if (key == "tv") {
        return parseTvPowerConfig(value);
    } else if (key == "vol") {
        int volume = std::strtol(value.c_str(), nullptr, 0);
        if (volume < 0 || volume > 100) {
            return false;
        }
        _target_volume_percent = volume;
        GetHAL().setDeviceVolumePercent(_target_volume_percent);
    } else if (key == "hpvol") {
        int volume = std::strtol(value.c_str(), nullptr, 0);
        if (volume < 0 || volume > 100) {
            return false;
        }
        return _ha.homePodSetVolume(volume);
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
    int cmd = std::strtol(value.substr(comma + 1).c_str(), nullptr, 0);
    if (addr < 0 || addr > 255 || cmd < 0 || cmd > 255) {
        return false;
    }

    _tv_power_addr = static_cast<uint8_t>(addr);
    _tv_power_cmd = static_cast<uint8_t>(cmd);
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
