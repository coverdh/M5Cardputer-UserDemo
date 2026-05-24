/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <M5Unified.hpp>
#include <cstdint>
#include <hal/keyboard/keyboard.h>
#include <mooncake.h>
#include <string>

class AppExternalInputTest : public mooncake::AppAbility {
public:
    AppExternalInputTest();
    ~AppExternalInputTest();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct ProbeState {
        std::string addresses = "none";
        bool joystick_unit = false;
        bool joystick2 = false;
        bool byte_button = false;
    };

    struct RawState {
        bool joystick_unit_ok = false;
        uint8_t joystick_unit[3] = {};
        bool joystick2_axis_ok = false;
        uint8_t joystick2_axis[2] = {};
        bool joystick2_button_ok = false;
        uint8_t joystick2_button = 0xFF;
        bool byte_button_ok = false;
        uint8_t byte_button[8] = {};
        bool dual_button_ok = false;
        bool dual_red = false;
        bool dual_blue = false;
    };

    struct ChainState {
        bool active = false;
        bool device_found = false;
        bool type_ok = false;
        bool axis_ok = false;
        bool button_ok = false;
        int rx_pin = -1;
        int tx_pin = -1;
        uint8_t count = 0;
        uint16_t device_type = 0;
        int8_t x = 0;
        int8_t y = 0;
        uint8_t button = 0xFF;
        uint32_t packets = 0;
        uint32_t failures = 0;
    };

    ProbeState _probe;
    RawState _raw;
    ChainState _chain;
    uint32_t _last_probe = 0;
    uint32_t _last_read = 0;
    uint32_t _last_render = 0;
    uint32_t _i2c_resets = 0;
    uint32_t _read_failures = 0;
    int _key_event_slot_id = -1;
    bool _force_probe = false;

    void handleKeyEvent(const Keyboard::KeyEvent_t& keyEvent);
    void probe();
    void readRaw();
    void render();
    void resetExternalBus();
    void initDualButtonGpio();
    void initChainUart(int rxPin, int txPin);
    void probeChain();
    void readChain();
    bool chainCommand(uint8_t index, uint8_t command, const uint8_t* data, size_t dataSize, uint8_t* response, size_t& responseSize);
    bool chainReadPacket(uint8_t* response, size_t& responseSize, uint32_t timeoutMs);
    void chainWritePacket(uint8_t index, uint8_t command, const uint8_t* data, size_t dataSize);
    bool parseChainValue(uint8_t command, const uint8_t* response, size_t responseSize, const uint8_t*& data, size_t& dataSize) const;
    uint8_t chainCrc(const uint8_t* frame, size_t frameSize) const;
    std::string scanExternalBus();
    std::string formatBytes(const uint8_t* data, size_t size) const;
    std::string formatPad(uint8_t buttons) const;
    uint8_t decodeRawButtons() const;
};
