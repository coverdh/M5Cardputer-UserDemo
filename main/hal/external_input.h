/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <M5Unified.hpp>
#include <cstdint>

class Settings;

class ExternalInput {
public:
    static constexpr uint8_t PAD_UP     = 1 << 0;
    static constexpr uint8_t PAD_DOWN   = 1 << 1;
    static constexpr uint8_t PAD_LEFT   = 1 << 2;
    static constexpr uint8_t PAD_RIGHT  = 1 << 3;
    static constexpr uint8_t PAD_A      = 1 << 4;
    static constexpr uint8_t PAD_B      = 1 << 5;
    static constexpr uint8_t PAD_SELECT = 1 << 6;
    static constexpr uint8_t PAD_START  = 1 << 7;

    void init();
    void update(uint32_t now);
    void probe(uint32_t now);
    void setPaused(bool paused);
    void loadSettings(Settings& settings);
    void setDirectionTransform(bool flipX, bool flipY, bool swapAxes);
    void toggleFlipX();
    void toggleFlipY();
    void toggleSwapAxes();

    bool isConnected() const
    {
        return _connected;
    }

    uint8_t getButtons() const
    {
        return _buttons;
    }

    uint8_t getPressed() const
    {
        return _pressed;
    }

    uint8_t getReleased() const
    {
        return _released;
    }

    bool getFlipX() const
    {
        return _flip_x;
    }

    bool getFlipY() const
    {
        return _flip_y;
    }

    bool getSwapAxes() const
    {
        return _swap_axes;
    }

    bool isEncoderConnected() const
    {
        return _encoder_type != EncoderType::None;
    }

    int16_t getEncoderValue() const
    {
        return _encoder_value;
    }

    int16_t getEncoderDelta() const
    {
        return _encoder_delta;
    }

    bool getEncoderButton() const
    {
        return _encoder_button;
    }

    bool getEncoderPressed() const
    {
        return _encoder_pressed;
    }

    bool getEncoderReleased() const
    {
        return _encoder_released;
    }

private:
    enum class JoystickType : uint8_t {
        None,
        JoystickUnit,
        Joystick2,
        ChainJoystick,
    };

    enum class EncoderType : uint8_t {
        None,
        UnitEncoder,
        ChainEncoder,
    };

    m5::I2C_Class* _i2c               = nullptr;
    JoystickType _joystick_type       = JoystickType::None;
    EncoderType _encoder_type         = EncoderType::None;
    int _chain_rx_pin                 = -1;
    int _chain_tx_pin                 = -1;
    uint8_t _chain_count              = 0;
    uint8_t _chain_joystick_index     = 0;
    uint8_t _chain_encoder_index      = 0;
    uint8_t _chain_bus_index          = 0;
    uint8_t _buttons                  = 0;
    uint8_t _pressed                  = 0;
    uint8_t _released                 = 0;
    int16_t _encoder_value            = 0;
    int16_t _encoder_delta            = 0;
    uint32_t _last_poll               = 0;
    uint32_t _last_probe              = 0;
    uint32_t _last_scan_log           = 0;
    uint8_t _chain_read_failures      = 0;
    uint8_t _encoder_read_failures    = 0;
    uint8_t _chain_bus_read_failures  = 0;
    bool _connected                   = false;
    bool _byte_buttons_connected      = false;
    bool _dual_button_connected       = false;
    bool _encoder_button              = false;
    bool _encoder_pressed             = false;
    bool _encoder_released            = false;
    bool _unit_encoder_has_last_value = false;
    bool _scan_logged                 = false;
    bool _paused                      = false;
    bool _chain_uart_ready            = false;
    bool _flip_x                      = false;
    bool _flip_y                      = false;
    bool _swap_axes                   = false;
    Settings* _settings               = nullptr;

    bool read(uint8_t& buttons, uint32_t now);
    bool readJoystickUnit(uint8_t& buttons);
    bool readJoystick2(uint8_t& buttons);
    bool readChainJoystick(uint8_t& buttons);
    bool readEncoder();
    bool readUnitEncoder();
    bool readChainEncoder();
    bool readByteButtons(uint8_t& buttons);
    bool readDualButtons(uint8_t& buttons);
    void probeBus(uint32_t now);
    bool tryBus(m5::I2C_Class& bus);
    bool tryChainBus(int rxPin, int txPin);
    void initDualButtons();
    bool initChainBusButtons();
    bool chainBusReadInput(uint8_t gpio, uint8_t& level);
    uint8_t applyDirectionTransform(uint8_t buttons) const;
    void saveDirectionSettings();
    void initChainUart(int rxPin, int txPin);
    bool chainCommand(uint8_t index, uint8_t command, const uint8_t* data, size_t dataSize, uint8_t* response,
                      size_t& responseSize);
    bool chainReadPacket(uint8_t* response, size_t& responseSize, uint32_t timeoutMs);
    void chainWritePacket(uint8_t index, uint8_t command, const uint8_t* data, size_t dataSize);
    bool parseChainValue(uint8_t command, const uint8_t* response, size_t responseSize, const uint8_t*& data,
                         size_t& dataSize) const;
    uint8_t chainCrc(const uint8_t* frame, size_t frameSize) const;
    std::string scanBus(m5::I2C_Class& bus);
};
