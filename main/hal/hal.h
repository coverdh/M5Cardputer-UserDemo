/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "keyboard/keyboard.h"
#include "external_input.h"
#include "cap_lora868/cap_lora868.h"
#include "utils/settings/settings.h"
#include <M5Unified.hpp>
#include <M5GFX.h>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

class Hal {
public:
    void init(bool gameboy_only_mode = false);
    void update();

    /* --------------------------------- System --------------------------------- */
    void delay(std::uint32_t ms)
    {
        m5gfx::delay(ms);
    }
    std::uint32_t millis()
    {
        return m5gfx::millis();
    }
    void feedTheDog();
    std::vector<uint8_t> getDeviceMac();
    std::string getDeviceMacString();

    /* --------------------------------- Display -------------------------------- */
    M5GFX& display                = M5.Display;
    LGFX_Sprite canvas            = LGFX_Sprite(&M5.Display);
    LGFX_Sprite canvasSystemBar   = LGFX_Sprite(&M5.Display);
    LGFX_Sprite canvasKeyboardBar = LGFX_Sprite(&M5.Display);

    inline void pushCanvasSystemBar()
    {
        if (_fullscreen_mode || !_ui_sprites_enabled) {
            return;
        }
        canvasSystemBar.pushSprite(canvasKeyboardBar.width(), 0);
    }
    inline void pushCanvasKeyboardBar()
    {
        if (_fullscreen_mode || !_ui_sprites_enabled) {
            return;
        }
        canvasKeyboardBar.pushSprite(0, 0);
    }
    inline void pushCanvas()
    {
        if (!_ui_sprites_enabled) {
            return;
        }
        canvas.pushSprite(canvasKeyboardBar.width(), canvasSystemBar.height());
    }
    void setFullscreenMode(bool fullscreen);
    bool isFullscreenMode() const
    {
        return _fullscreen_mode;
    }
    void setDeviceBrightnessPercent(int percent);
    int getDeviceBrightnessPercent() const
    {
        return _display_brightness_percent;
    }

    /* ---------------------------------- Audio --------------------------------- */
    m5::Speaker_Class& speaker = M5.Speaker;
    m5::Mic_Class& mic         = M5.Mic;
    void setDeviceVolumePercent(int percent);
    int getDeviceVolumePercent() const;

    /* ---------------------------------- Input --------------------------------- */
    m5::Button_Class& homeButton = M5.BtnA;
    Keyboard keyboard;
    ExternalInput externalInput;

    /* ---------------------------------- Power --------------------------------- */
    inline uint8_t getBatLevel()
    {
        return M5.Power.getBatteryLevel();
    }

    /* ---------------------------------- WiFi ---------------------------------- */
    using ScanResult_t = std::pair<int, std::string>;
    void wifiInit();
    void wifiDeinit();
    void wifiScan(std::vector<ScanResult_t>& scanResult);
    bool wifiConnect(const std::string& ssid, const std::string& password);
    bool isWifiConnected() const
    {
        return _is_wifi_connected;
    }
    void wifiDisconnect();

    /* --------------------------------- EspNow --------------------------------- */
    void espNowInit();
    void espNowDeinit();
    void espNowSend(const std::string& data);
    bool espNowAvailable();
    const std::string& espNowGetReceivedData();
    void espNowClearReceivedData();

    /* ----------------------------------- IR ----------------------------------- */
    void irInit();
    void irSend(uint8_t addr, uint8_t cmd);
    void irSendXiaomi(uint8_t device, uint8_t function, uint8_t repeats = 3);
    void irSendRaw(uint32_t carrier_hz, const uint32_t* durations_us, size_t duration_count);

    /* ----------------------------------- BLE ---------------------------------- */
    bool bleControlInit();
    void bleControlStop();
    bool bleControlForgetBonds();
    void bleKeyboardInit();
    bool bleKeyboardIsConnected() const;
    void bleKeyboardSendReport(uint8_t modifier, KeScanCode_t keyCode);
    void bleKeyboardTap(uint8_t modifier, KeScanCode_t keyCode);
    void bleMouseReport(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel = 0);
    void bleMouseMove(int8_t dx, int8_t dy, int8_t wheel = 0);
    void bleMouseClick(uint8_t buttons);
    void bleConsumerSend(uint16_t usageId);
    bool bleMacSystemControlKey(const Keyboard::KeyEvent_t& keyEvent);
    bool bleMacCtlSystemKey(uint8_t key);
    struct MacCtlNowPlayingState {
        bool active = false;
        bool playing = false;
        bool muted = false;
        uint8_t volumePercent = 0;
        uint8_t progressPercent = 0;
        uint32_t updatedMs = 0;
        char title[33] = {};
        char artist[33] = {};
    };
    bool bleMacCtlVolumeDelta(int8_t delta);
    bool bleMacCtlPlayPause();
    bool bleMacCtlConfig(uint8_t flags, uint8_t sensitivity, uint8_t knobMode);
    bool bleMacCtlPowerConfig(uint8_t screenTimeoutSeconds, uint8_t powerSaveTimeoutMinutes);
    bool bleMacCtlIsConnected() const;
    bool bleMacCtlTimeSynced() const;
    const MacCtlNowPlayingState& bleMacCtlNowPlaying() const;
    bool bleMacCtlAudioState(bool active);
    bool bleMacCtlAudioFrame(uint8_t sequence, const uint8_t* data, uint8_t len);
    bool bleConsumeAudioTestRequest(bool& active);

    /* ----------------------------------- USB ---------------------------------- */
    void usbKeyboardInit();
    bool usbKeyboardIsConnected() const;

    /* -------------------------------- Settings -------------------------------- */
    Settings& getSettings()
    {
        return *_settings;
    }

    /* ----------------------------------- IMU ---------------------------------- */
    m5::IMU_Class& imu = M5.Imu;

    /* --------------------------------- SD Card -------------------------------- */
    struct SdCardProbeResult_t {
        bool is_mounted = false;
        std::string size;
        std::string type;
        std::string name;

        bool operator==(const SdCardProbeResult_t& other) const
        {
            return is_mounted == other.is_mounted && size == other.size && type == other.type && name == other.name;
        }
    };

    SdCardProbeResult_t sdCardProbe();
    void sdCardUnmount();

    /* ----------------------------------- Cap ---------------------------------- */
    CapLoRa868 capLora868;

private:
    Settings* _settings             = nullptr;
    bool _is_wifi_inited            = false;
    bool _wifi_init_failed          = false;
    bool _is_wifi_connected         = false;
    bool _is_esp_now_inited         = false;
    bool _is_ir_inited              = false;
    bool _is_ble_keyboard_inited    = false;
    uint32_t _last_ble_advertising_ensure_ms = 0;
    bool _is_usb_keyboard_inited    = false;
    bool _is_sd_card_mounted        = false;
    bool _fullscreen_mode           = false;
    bool _ui_sprites_enabled        = true;
    int _display_brightness_percent = 100;
    int _ble_keyboard_event_slot_id = -1;
    int _usb_keyboard_event_slot_id = -1;
    std::unique_ptr<CapLoRa868> _cap_lora868;

    void display_init(bool create_ui_sprites);
    void i2c_scan();
    void keyboard_init();
    void start_sntp();
    void stop_sntp();
    void setting_init();
    void spi_init();
    void sd_card_init();
    void handle_ble_keyboard_event(const Keyboard::KeyEvent_t& keyEvent);
    void handle_usb_keyboard_event(const Keyboard::KeyEvent_t& keyEvent);
};

Hal& GetHAL();
