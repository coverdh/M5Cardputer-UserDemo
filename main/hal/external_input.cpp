/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "external_input.h"
#include "utils/settings/settings.h"
#include <driver/gpio.h>
#include <driver/uart.h>
#include <mooncake_log.h>
#include <string>
#include <algorithm>

static constexpr uint32_t EXTERNAL_INPUT_I2C_FREQ             = 100000;
static constexpr uint32_t EXTERNAL_INPUT_POLL_INTERVAL_MS     = 12;
static constexpr uint32_t CHAIN_INPUT_POLL_INTERVAL_MS        = 50;
static constexpr uint32_t EXTERNAL_INPUT_PROBE_INTERVAL_MS    = 1000;
static constexpr uint32_t EXTERNAL_INPUT_SCAN_LOG_INTERVAL_MS = 5000;
static constexpr uint32_t EXTERNAL_INPUT_RETRY_INTERVAL_MS    = 5000;
static constexpr uint32_t CHAIN_COMMAND_TIMEOUT_MS            = 160;
static constexpr uint8_t CHAIN_INPUT_FAILURE_LIMIT            = 20;
static constexpr uint8_t JOYSTICK_UNIT_ADDR                   = 0x52;
static constexpr uint8_t JOYSTICK2_ADDR                       = 0x63;
static constexpr uint8_t JOYSTICK2_OFFSET_ADC_VALUE_8BITS_REG = 0x60;
static constexpr uint8_t JOYSTICK2_BUTTON_REG                 = 0x20;
static constexpr uint8_t UNIT_ENCODER_ADDR                    = 0x40;
static constexpr uint8_t UNIT_ENCODER_VALUE_REG               = 0x10;
static constexpr uint8_t UNIT_ENCODER_BUTTON_REG              = 0x20;
static constexpr uint8_t BYTE_BUTTON_ADDR                     = 0x47;
static constexpr uint8_t BYTE_BUTTON_STATUS_8BYTE_REG         = 0x60;
static constexpr int JOYSTICK_UNIT_CENTER                     = 128;
static constexpr int JOYSTICK_UNIT_DEAD_ZONE                  = 38;
static constexpr int JOYSTICK2_DEAD_ZONE                      = 24;
static constexpr uart_port_t CHAIN_UART                       = UART_NUM_1;
static constexpr int CHAIN_RX_A                               = 1;
static constexpr int CHAIN_TX_A                               = 2;
static constexpr int CHAIN_RX_B                               = 2;
static constexpr int CHAIN_TX_B                               = 1;
static constexpr uint8_t CHAIN_DEVICE_ENCODER                 = 0x01;
static constexpr uint8_t CHAIN_DEVICE_JOYSTICK                = 0x04;
static constexpr uint8_t CHAIN_DEVICE_UNIT_CHAINBUS           = 0x06;
static constexpr uint8_t CHAIN_ENCODER_READ_DELTA             = 0x11;
static constexpr uint8_t CHAIN_ENCODER_READ_BUTTON            = 0xE1;
static constexpr uint8_t CHAIN_GPIO_INPUT_INIT                = 0x40;
static constexpr uint8_t CHAIN_GPIO_READ_LEVEL                = 0x41;
static constexpr uint8_t CHAIN_GPIO_PIN_1                     = 0x01;
static constexpr uint8_t CHAIN_GPIO_PIN_2                     = 0x02;
static constexpr uint8_t CHAIN_GPIO_PULL_DOWN                 = 0x01;
static constexpr uint8_t CHAIN_OPERATION_SUCCESS              = 0x01;
static constexpr gpio_num_t DUAL_BUTTON_RED_PIN               = GPIO_NUM_1;
static constexpr gpio_num_t DUAL_BUTTON_BLUE_PIN              = GPIO_NUM_2;
static const std::string TAG                                  = "ExternalInput";

void ExternalInput::init()
{
    probeBus(0);
}

void ExternalInput::update(uint32_t now)
{
    _pressed          = 0;
    _released         = 0;
    _encoder_delta    = 0;
    _encoder_pressed  = false;
    _encoder_released = false;
    if (_paused) {
        _buttons        = 0;
        _encoder_button = false;
        _connected      = false;
        return;
    }
    const uint32_t pollInterval =
        _joystick_type == JoystickType::ChainJoystick || _encoder_type == EncoderType::ChainEncoder
            ? CHAIN_INPUT_POLL_INTERVAL_MS
            : EXTERNAL_INPUT_POLL_INTERVAL_MS;
    if (now - _last_poll < pollInterval) {
        return;
    }
    _last_poll = now;

    uint8_t buttons      = 0;
    const bool connected = read(buttons, now);
    if (!connected) {
        buttons = 0;
    }

    const uint8_t changed = buttons ^ _buttons;
    _pressed              = changed & buttons;
    _released             = changed & _buttons;
    _buttons              = buttons;
    _connected            = connected;
}

void ExternalInput::probe(uint32_t now)
{
    if (_paused) {
        return;
    }
    probeBus(now);
}

void ExternalInput::setPaused(bool paused)
{
    _paused = paused;
    if (!paused) {
        _last_probe  = 0;
        _scan_logged = false;
        return;
    }

    _buttons          = 0;
    _pressed          = 0;
    _released         = 0;
    _encoder_delta    = 0;
    _encoder_button   = false;
    _encoder_pressed  = false;
    _encoder_released = false;
    _connected        = false;
}

void ExternalInput::loadSettings(Settings& settings)
{
    _settings  = &settings;
    _flip_x    = settings.GetBool("ext_flip_x", false);
    _flip_y    = settings.GetBool("ext_flip_y", false);
    _swap_axes = settings.GetBool("ext_swap_axes", false);
    mclog::tagInfo(TAG, "direction transform: flip_x={} flip_y={} swap_axes={}", _flip_x ? "yes" : "no",
                   _flip_y ? "yes" : "no", _swap_axes ? "yes" : "no");
}

void ExternalInput::setDirectionTransform(bool flipX, bool flipY, bool swapAxes)
{
    if (_flip_x == flipX && _flip_y == flipY && _swap_axes == swapAxes) {
        return;
    }
    _flip_x    = flipX;
    _flip_y    = flipY;
    _swap_axes = swapAxes;
    saveDirectionSettings();
    mclog::tagInfo(TAG, "direction transform set: flip_x={} flip_y={} swap_axes={}", _flip_x ? "yes" : "no",
                   _flip_y ? "yes" : "no", _swap_axes ? "yes" : "no");
}

void ExternalInput::toggleFlipX()
{
    setDirectionTransform(!_flip_x, _flip_y, _swap_axes);
}

void ExternalInput::toggleFlipY()
{
    setDirectionTransform(_flip_x, !_flip_y, _swap_axes);
}

void ExternalInput::toggleSwapAxes()
{
    setDirectionTransform(_flip_x, _flip_y, !_swap_axes);
}

bool ExternalInput::read(uint8_t& buttons, uint32_t now)
{
    buttons = 0;
    if (_joystick_type == JoystickType::None && _encoder_type == EncoderType::None && !_byte_buttons_connected &&
        !_dual_button_connected && now - _last_probe >= EXTERNAL_INPUT_PROBE_INTERVAL_MS) {
        probeBus(now);
    }

    bool connected          = false;
    uint8_t joystickButtons = 0;
    if (_joystick_type == JoystickType::JoystickUnit) {
        if (readJoystickUnit(joystickButtons)) {
            buttons |= joystickButtons;
            connected = true;
        } else {
            mclog::tagWarn(TAG, "joystick-unit read failed");
            _joystick_type = JoystickType::None;
        }
    } else if (_joystick_type == JoystickType::Joystick2) {
        if (readJoystick2(joystickButtons)) {
            buttons |= joystickButtons;
            connected = true;
        } else {
            mclog::tagWarn(TAG, "joystick2 read failed");
            _joystick_type = JoystickType::None;
        }
    } else if (_joystick_type == JoystickType::ChainJoystick) {
        if (readChainJoystick(joystickButtons)) {
            buttons |= joystickButtons;
            connected = true;
        } else {
            mclog::tagWarn(TAG, "chain joystick read failed");
            _joystick_type        = JoystickType::None;
            _chain_joystick_index = 0;
            if (_encoder_type != EncoderType::ChainEncoder && !_dual_button_connected) {
                _chain_uart_ready = false;
            }
        }
    }

    if (_encoder_type != EncoderType::None) {
        if (readEncoder()) {
            connected = true;
        } else {
            mclog::tagWarn(TAG, "encoder read failed");
            _encoder_type                = EncoderType::None;
            _chain_encoder_index         = 0;
            _unit_encoder_has_last_value = false;
            if (_joystick_type != JoystickType::ChainJoystick && !_dual_button_connected) {
                _chain_uart_ready = false;
            }
        }
    }

    uint8_t byteButtons = 0;
    if (_byte_buttons_connected) {
        if (readByteButtons(byteButtons)) {
            buttons |= byteButtons;
            connected = true;
        } else {
            mclog::tagWarn(TAG, "byte buttons read failed");
            _byte_buttons_connected = false;
        }
    }

    uint8_t dualButtons = 0;
    if (_dual_button_connected) {
        if (readDualButtons(dualButtons)) {
            buttons |= dualButtons;
            connected = true;
        } else {
            mclog::tagWarn(TAG, "dual buttons read failed");
            _dual_button_connected = false;
            _chain_bus_index       = 0;
        }
    }

    buttons = applyDirectionTransform(buttons);

    if (!connected && now - _last_probe >= EXTERNAL_INPUT_PROBE_INTERVAL_MS) {
        probeBus(now);
    }
    return connected;
}

void ExternalInput::probeBus(uint32_t now)
{
    const auto previousJoystick    = _joystick_type;
    const auto previousEncoder     = _encoder_type;
    const bool previousButtons     = _byte_buttons_connected;
    const bool previousDualButtons = _dual_button_connected;
    auto* previousBus              = _i2c;
    const int previousChainRx      = _chain_rx_pin;
    const int previousChainTx      = _chain_tx_pin;

    _i2c                         = nullptr;
    _joystick_type               = JoystickType::None;
    _encoder_type                = EncoderType::None;
    _byte_buttons_connected      = false;
    _dual_button_connected       = false;
    _chain_rx_pin                = -1;
    _chain_tx_pin                = -1;
    _chain_count                 = 0;
    _chain_joystick_index        = 0;
    _chain_encoder_index         = 0;
    _chain_bus_index             = 0;
    _chain_uart_ready            = false;
    _chain_read_failures         = 0;
    _encoder_read_failures       = 0;
    _chain_bus_read_failures     = 0;
    _unit_encoder_has_last_value = false;
    uart_driver_delete(CHAIN_UART);

    M5.Ex_I2C.begin();
    const bool i2cFound = tryBus(M5.Ex_I2C);
    if (!i2cFound) {
        M5.Ex_I2C.release();
        if (!tryChainBus(CHAIN_RX_A, CHAIN_TX_A)) {
            if (!tryChainBus(CHAIN_RX_B, CHAIN_TX_B)) {
                initDualButtons();
            }
        }
    }
    _last_probe = now;

    const bool stateChanged = previousJoystick != _joystick_type || previousEncoder != _encoder_type ||
                              previousButtons != _byte_buttons_connected ||
                              previousDualButtons != _dual_button_connected || previousBus != _i2c ||
                              previousChainRx != _chain_rx_pin || previousChainTx != _chain_tx_pin;
    const bool shouldLogScan =
        stateChanged || !_scan_logged || now - _last_scan_log >= EXTERNAL_INPUT_SCAN_LOG_INTERVAL_MS;
    if (!shouldLogScan) {
        return;
    }

    _scan_logged   = true;
    _last_scan_log = now;

    const char* joystickName = "none";
    if (_joystick_type == JoystickType::JoystickUnit) {
        joystickName = "joystick-unit";
    } else if (_joystick_type == JoystickType::Joystick2) {
        joystickName = "joystick2";
    } else if (_joystick_type == JoystickType::ChainJoystick) {
        joystickName = "chain-joystick";
    }
    const char* encoderName = "none";
    if (_encoder_type == EncoderType::UnitEncoder) {
        encoderName = "unit-encoder";
    } else if (_encoder_type == EncoderType::ChainEncoder) {
        encoderName = "chain-encoder";
    }

    mclog::tagInfo(TAG, "external i2c scan: ex=[{}] in=[{}]",
                   (_joystick_type == JoystickType::ChainJoystick || _encoder_type == EncoderType::ChainEncoder)
                       ? "skip-uart"
                       : scanBus(M5.Ex_I2C),
                   "skip");
    mclog::tagInfo(
        TAG, "external input: bus={} joystick={} encoder={} byte_buttons={} chain_count={}",
        _i2c == &M5.Ex_I2C
            ? "ex-i2c"
            : ((_joystick_type == JoystickType::ChainJoystick || _encoder_type == EncoderType::ChainEncoder) ? "ex-uart"
                                                                                                             : "none"),
        joystickName, encoderName,
        _byte_buttons_connected ? "yes"
                                : (_dual_button_connected ? (_chain_bus_index ? "chainbus-dual" : "dual-gpio") : "no"),
        _chain_count);
}

bool ExternalInput::tryBus(m5::I2C_Class& bus)
{
    JoystickType joystick = JoystickType::None;
    if (bus.scanID(JOYSTICK2_ADDR, EXTERNAL_INPUT_I2C_FREQ)) {
        joystick = JoystickType::Joystick2;
    } else if (bus.scanID(JOYSTICK_UNIT_ADDR, EXTERNAL_INPUT_I2C_FREQ)) {
        joystick = JoystickType::JoystickUnit;
    }

    const bool unitEncoder = bus.scanID(UNIT_ENCODER_ADDR, EXTERNAL_INPUT_I2C_FREQ);
    const bool byteButtons = bus.scanID(BYTE_BUTTON_ADDR, EXTERNAL_INPUT_I2C_FREQ);
    if (joystick == JoystickType::None && !unitEncoder && !byteButtons) {
        return false;
    }

    _i2c                    = &bus;
    _joystick_type          = joystick;
    _encoder_type           = unitEncoder ? EncoderType::UnitEncoder : EncoderType::None;
    _byte_buttons_connected = byteButtons;
    return true;
}

bool ExternalInput::tryChainBus(int rxPin, int txPin)
{
    initChainUart(rxPin, txPin);

    uint8_t sendNum      = 0;
    uint8_t response[64] = {};
    size_t responseSize  = sizeof(response);
    if (!chainCommand(0xFF, 0xFE, &sendNum, 1, response, responseSize)) {
        return false;
    }
    const uint8_t* data = nullptr;
    size_t dataSize     = 0;
    if (!parseChainValue(0xFE, response, responseSize, data, dataSize) || dataSize < 1 || data[0] == 0) {
        return false;
    }

    const uint8_t chainCount = data[0];
    bool found               = false;
    _chain_count             = chainCount;
    _chain_rx_pin            = rxPin;
    _chain_tx_pin            = txPin;
    _chain_uart_ready        = true;

    for (uint8_t id = 1; id <= chainCount; ++id) {
        responseSize = sizeof(response);
        if (!chainCommand(id, 0xFB, nullptr, 0, response, responseSize) ||
            !parseChainValue(0xFB, response, responseSize, data, dataSize) || dataSize < 2) {
            mclog::tagWarn(TAG, "chain device {} type read failed", id);
            continue;
        }

        const uint16_t deviceType = static_cast<uint16_t>(data[0] | (data[1] << 8));
        mclog::tagInfo(TAG, "chain device {} type=0x{:04X}", id, deviceType);
        if (deviceType == CHAIN_DEVICE_ENCODER && _chain_encoder_index == 0) {
            _encoder_type        = EncoderType::ChainEncoder;
            _chain_encoder_index = id;
            found                = true;
        } else if (deviceType == CHAIN_DEVICE_JOYSTICK && _chain_joystick_index == 0) {
            _joystick_type        = JoystickType::ChainJoystick;
            _chain_joystick_index = id;
            found                 = true;
        } else if (deviceType == CHAIN_DEVICE_UNIT_CHAINBUS && _chain_bus_index == 0) {
            _chain_bus_index = id;
            if (initChainBusButtons()) {
                _dual_button_connected = true;
                found                  = true;
            } else {
                _chain_bus_index = 0;
            }
        }
    }

    if (!found) {
        _chain_count         = 0;
        _chain_rx_pin        = -1;
        _chain_tx_pin        = -1;
        _encoder_type        = EncoderType::None;
        _chain_encoder_index = 0;
        _chain_uart_ready    = false;
        return false;
    }

    mclog::tagInfo(TAG, "chain input detected: count={} joystick_id={} encoder_id={} chainbus_id={} rx={} tx={}",
                   chainCount, _chain_joystick_index, _chain_encoder_index, _chain_bus_index, rxPin, txPin);
    return true;
}

void ExternalInput::initChainUart(int rxPin, int txPin)
{
    uart_driver_delete(CHAIN_UART);
    uart_config_t config = {
        .baud_rate           = 115200,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk          = UART_SCLK_DEFAULT,
    };
    uart_driver_install(CHAIN_UART, 1024, 0, 0, nullptr, 0);
    uart_param_config(CHAIN_UART, &config);
    uart_set_pin(CHAIN_UART, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_flush(CHAIN_UART);
}

void ExternalInput::initDualButtons()
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << DUAL_BUTTON_RED_PIN) | (1ULL << DUAL_BUTTON_BLUE_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
    _dual_button_connected = true;
    mclog::tagInfo(TAG, "dual button gpio enabled: red=GPIO{}->B blue=GPIO{}->A", static_cast<int>(DUAL_BUTTON_RED_PIN),
                   static_cast<int>(DUAL_BUTTON_BLUE_PIN));
}

bool ExternalInput::initChainBusButtons()
{
    if (_chain_bus_index == 0) {
        return false;
    }

    uint8_t response[16] = {};
    size_t responseSize  = sizeof(response);
    const uint8_t* data  = nullptr;
    size_t dataSize      = 0;

    uint8_t gpio1Config[2] = {CHAIN_GPIO_PIN_1, CHAIN_GPIO_PULL_DOWN};
    if (!chainCommand(_chain_bus_index, CHAIN_GPIO_INPUT_INIT, gpio1Config, sizeof(gpio1Config), response,
                      responseSize) ||
        !parseChainValue(CHAIN_GPIO_INPUT_INIT, response, responseSize, data, dataSize) || dataSize < 1 ||
        data[0] != CHAIN_OPERATION_SUCCESS) {
        mclog::tagWarn(TAG, "chainbus {} gpio1 input init failed", _chain_bus_index);
        return false;
    }

    responseSize           = sizeof(response);
    uint8_t gpio2Config[2] = {CHAIN_GPIO_PIN_2, CHAIN_GPIO_PULL_DOWN};
    if (!chainCommand(_chain_bus_index, CHAIN_GPIO_INPUT_INIT, gpio2Config, sizeof(gpio2Config), response,
                      responseSize) ||
        !parseChainValue(CHAIN_GPIO_INPUT_INIT, response, responseSize, data, dataSize) || dataSize < 1 ||
        data[0] != CHAIN_OPERATION_SUCCESS) {
        mclog::tagWarn(TAG, "chainbus {} gpio2 input init failed", _chain_bus_index);
        return false;
    }

    mclog::tagInfo(TAG, "chainbus dual button enabled: id={} gpio1->B gpio2->A", _chain_bus_index);
    return true;
}

bool ExternalInput::chainBusReadInput(uint8_t gpio, uint8_t& level)
{
    level = 0;
    if (_chain_bus_index == 0) {
        return false;
    }

    uint8_t response[16] = {};
    size_t responseSize  = sizeof(response);
    const uint8_t* data  = nullptr;
    size_t dataSize      = 0;
    if (!chainCommand(_chain_bus_index, CHAIN_GPIO_READ_LEVEL, &gpio, 1, response, responseSize) ||
        !parseChainValue(CHAIN_GPIO_READ_LEVEL, response, responseSize, data, dataSize) || dataSize < 2 ||
        data[0] != CHAIN_OPERATION_SUCCESS) {
        return false;
    }

    level = data[1];
    return true;
}

bool ExternalInput::chainCommand(uint8_t index, uint8_t command, const uint8_t* data, size_t dataSize,
                                 uint8_t* response, size_t& responseSize)
{
    const size_t capacity = responseSize;
    for (int attempt = 0; attempt < 2; ++attempt) {
        responseSize = capacity;
        uart_flush(CHAIN_UART);
        chainWritePacket(index, command, data, dataSize);
        if (chainReadPacket(response, responseSize, CHAIN_COMMAND_TIMEOUT_MS)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    return false;
}

bool ExternalInput::chainReadPacket(uint8_t* response, size_t& responseSize, uint32_t timeoutMs)
{
    if (!response || responseSize < 9) {
        return false;
    }

    const auto start = M5.millis();
    size_t pos       = 0;
    while (M5.millis() - start < timeoutMs) {
        uint8_t byte  = 0;
        const int got = uart_read_bytes(CHAIN_UART, &byte, 1, pdMS_TO_TICKS(4));
        if (got <= 0) {
            continue;
        }

        if (pos == 0 && byte != 0xAA) {
            continue;
        }
        if (pos == 1 && byte != 0x55) {
            pos = byte == 0xAA ? 1 : 0;
            continue;
        }
        if (pos >= responseSize) {
            pos = 0;
            continue;
        }
        response[pos++] = byte;

        if (pos >= 4) {
            const size_t length = response[2] | (response[3] << 8);
            const size_t total  = 2 + 2 + length + 2;
            if (total > responseSize) {
                pos = 0;
                continue;
            }
            if (pos == total) {
                if (response[total - 2] != 0x55 || response[total - 1] != 0xAA) {
                    return false;
                }
                if (chainCrc(response, total) != response[total - 3]) {
                    return false;
                }
                responseSize = total;
                return true;
            }
        }
    }
    return false;
}

void ExternalInput::chainWritePacket(uint8_t index, uint8_t command, const uint8_t* data, size_t dataSize)
{
    uint8_t frame[64]   = {};
    const size_t length = 3 + dataSize;
    const size_t total  = 2 + 2 + length + 2;
    if (total > sizeof(frame)) {
        return;
    }

    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[2] = static_cast<uint8_t>(length & 0xFF);
    frame[3] = static_cast<uint8_t>((length >> 8) & 0xFF);
    frame[4] = index;
    frame[5] = command;
    for (size_t i = 0; i < dataSize; ++i) {
        frame[6 + i] = data[i];
    }
    frame[6 + dataSize] = chainCrc(frame, total);
    frame[7 + dataSize] = 0x55;
    frame[8 + dataSize] = 0xAA;
    uart_write_bytes(CHAIN_UART, frame, total);
    uart_wait_tx_done(CHAIN_UART, pdMS_TO_TICKS(20));
}

bool ExternalInput::parseChainValue(uint8_t command, const uint8_t* response, size_t responseSize, const uint8_t*& data,
                                    size_t& dataSize) const
{
    data     = nullptr;
    dataSize = 0;
    if (!response || responseSize < 9 || response[5] != command) {
        return false;
    }
    const size_t length = response[2] | (response[3] << 8);
    if (length < 3 || 2 + 2 + length + 2 != responseSize) {
        return false;
    }
    data     = &response[6];
    dataSize = length - 3;
    return true;
}

uint8_t ExternalInput::chainCrc(const uint8_t* frame, size_t frameSize) const
{
    uint8_t crc = 0;
    if (!frame || frameSize < 7) {
        return 0;
    }
    for (size_t i = 4; i < frameSize - 3; ++i) {
        crc = static_cast<uint8_t>(crc + frame[i]);
    }
    return crc;
}

std::string ExternalInput::scanBus(m5::I2C_Class& bus)
{
    std::string addresses;
    for (uint8_t address = 0x08; address < 0x78; ++address) {
        if (!bus.scanID(address, EXTERNAL_INPUT_I2C_FREQ)) {
            continue;
        }
        if (!addresses.empty()) {
            addresses += ",";
        }
        addresses += fmt::format("0x{:02X}", address);
    }
    return addresses.empty() ? "none" : addresses;
}

bool ExternalInput::readJoystickUnit(uint8_t& buttons)
{
    buttons = 0;
    if (!_i2c) {
        return false;
    }

    uint8_t data[3] = {};
    if (!_i2c->start(JOYSTICK_UNIT_ADDR, true, EXTERNAL_INPUT_I2C_FREQ)) {
        return false;
    }
    const bool ok = _i2c->read(data, sizeof(data), true);
    _i2c->stop();
    if (!ok) {
        return false;
    }

    const int x = data[0];
    const int y = data[1];
    if (x < JOYSTICK_UNIT_CENTER - JOYSTICK_UNIT_DEAD_ZONE) {
        buttons |= PAD_LEFT;
    } else if (x > JOYSTICK_UNIT_CENTER + JOYSTICK_UNIT_DEAD_ZONE) {
        buttons |= PAD_RIGHT;
    }
    if (y < JOYSTICK_UNIT_CENTER - JOYSTICK_UNIT_DEAD_ZONE) {
        buttons |= PAD_UP;
    } else if (y > JOYSTICK_UNIT_CENTER + JOYSTICK_UNIT_DEAD_ZONE) {
        buttons |= PAD_DOWN;
    }
    if (data[2] != 0) {
        buttons |= PAD_A;
    }
    return true;
}

bool ExternalInput::readJoystick2(uint8_t& buttons)
{
    buttons = 0;
    if (!_i2c) {
        return false;
    }

    uint8_t offsets[2] = {};
    if (!_i2c->readRegister(JOYSTICK2_ADDR, JOYSTICK2_OFFSET_ADC_VALUE_8BITS_REG, offsets, sizeof(offsets),
                            EXTERNAL_INPUT_I2C_FREQ)) {
        return false;
    }

    const int x = static_cast<int8_t>(offsets[0]);
    const int y = static_cast<int8_t>(offsets[1]);
    if (x < -JOYSTICK2_DEAD_ZONE) {
        buttons |= PAD_LEFT;
    } else if (x > JOYSTICK2_DEAD_ZONE) {
        buttons |= PAD_RIGHT;
    }
    if (y < -JOYSTICK2_DEAD_ZONE) {
        buttons |= PAD_UP;
    } else if (y > JOYSTICK2_DEAD_ZONE) {
        buttons |= PAD_DOWN;
    }

    uint8_t button = 1;
    if (_i2c->readRegister(JOYSTICK2_ADDR, JOYSTICK2_BUTTON_REG, &button, 1, EXTERNAL_INPUT_I2C_FREQ) && button == 0) {
        buttons |= PAD_A;
    }
    return true;
}

bool ExternalInput::readChainJoystick(uint8_t& buttons)
{
    buttons = 0;
    if (!_chain_uart_ready) {
        return false;
    }

    uint8_t response[64] = {};
    size_t responseSize  = sizeof(response);
    const uint8_t* data  = nullptr;
    size_t dataSize      = 0;
    if (!chainCommand(_chain_joystick_index, 0x35, nullptr, 0, response, responseSize) ||
        !parseChainValue(0x35, response, responseSize, data, dataSize) || dataSize < 2) {
        ++_chain_read_failures;
        if (_chain_read_failures < CHAIN_INPUT_FAILURE_LIMIT) {
            mclog::tagWarn(TAG, "chain joystick axis read miss {}/{}", _chain_read_failures,
                           CHAIN_INPUT_FAILURE_LIMIT);
            return true;
        }
        return false;
    }
    _chain_read_failures = 0;

    const int x = static_cast<int8_t>(data[0]);
    const int y = static_cast<int8_t>(data[1]);
    if (x < -JOYSTICK2_DEAD_ZONE) {
        buttons |= PAD_LEFT;
    } else if (x > JOYSTICK2_DEAD_ZONE) {
        buttons |= PAD_RIGHT;
    }
    if (y < -JOYSTICK2_DEAD_ZONE) {
        buttons |= PAD_UP;
    } else if (y > JOYSTICK2_DEAD_ZONE) {
        buttons |= PAD_DOWN;
    }

    responseSize = sizeof(response);
    if (chainCommand(_chain_joystick_index, 0xE1, nullptr, 0, response, responseSize) &&
        parseChainValue(0xE1, response, responseSize, data, dataSize) && dataSize >= 1 && data[0] != 0) {
        buttons |= PAD_A;
    }
    return true;
}

bool ExternalInput::readEncoder()
{
    if (_encoder_type == EncoderType::UnitEncoder) {
        return readUnitEncoder();
    }
    if (_encoder_type == EncoderType::ChainEncoder) {
        return readChainEncoder();
    }
    return false;
}

bool ExternalInput::readUnitEncoder()
{
    if (!_i2c) {
        return false;
    }

    uint8_t valueBytes[2] = {};
    if (!_i2c->readRegister(UNIT_ENCODER_ADDR, UNIT_ENCODER_VALUE_REG, valueBytes, sizeof(valueBytes),
                            EXTERNAL_INPUT_I2C_FREQ)) {
        return false;
    }

    const int16_t value = static_cast<int16_t>(valueBytes[0] | (valueBytes[1] << 8));
    if (_unit_encoder_has_last_value) {
        const int32_t delta = static_cast<int32_t>(value) - static_cast<int32_t>(_encoder_value);
        _encoder_delta      = static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, delta)));
    } else {
        _unit_encoder_has_last_value = true;
        _encoder_delta               = 0;
    }
    _encoder_value = value;

    uint8_t button = 0;
    if (!_i2c->readRegister(UNIT_ENCODER_ADDR, UNIT_ENCODER_BUTTON_REG, &button, 1, EXTERNAL_INPUT_I2C_FREQ)) {
        return false;
    }

    const bool pressed = button != 0;
    _encoder_pressed   = pressed && !_encoder_button;
    _encoder_released  = !pressed && _encoder_button;
    _encoder_button    = pressed;
    return true;
}

bool ExternalInput::readChainEncoder()
{
    if (!_chain_uart_ready || _chain_encoder_index == 0) {
        return false;
    }

    uint8_t response[64] = {};
    size_t responseSize  = sizeof(response);
    const uint8_t* data  = nullptr;
    size_t dataSize      = 0;
    if (!chainCommand(_chain_encoder_index, CHAIN_ENCODER_READ_DELTA, nullptr, 0, response, responseSize) ||
        !parseChainValue(CHAIN_ENCODER_READ_DELTA, response, responseSize, data, dataSize) || dataSize < 2) {
        ++_encoder_read_failures;
        if (_encoder_read_failures < CHAIN_INPUT_FAILURE_LIMIT) {
            mclog::tagWarn(TAG, "chain encoder delta read miss {}/{}", _encoder_read_failures,
                           CHAIN_INPUT_FAILURE_LIMIT);
            return true;
        }
        return false;
    }
    _encoder_read_failures = 0;

    _encoder_delta = static_cast<int16_t>(data[0] | (data[1] << 8));
    _encoder_value = static_cast<int16_t>(_encoder_value + _encoder_delta);

    responseSize = sizeof(response);
    if (!chainCommand(_chain_encoder_index, CHAIN_ENCODER_READ_BUTTON, nullptr, 0, response, responseSize) ||
        !parseChainValue(CHAIN_ENCODER_READ_BUTTON, response, responseSize, data, dataSize) || dataSize < 1) {
        return true;
    }

    const bool pressed = data[0] != 0;
    _encoder_pressed   = pressed && !_encoder_button;
    _encoder_released  = !pressed && _encoder_button;
    _encoder_button    = pressed;
    return true;
}

bool ExternalInput::readDualButtons(uint8_t& buttons)
{
    buttons = 0;
    if (_chain_bus_index != 0) {
        uint8_t red  = 0;
        uint8_t blue = 0;
        if (!chainBusReadInput(CHAIN_GPIO_PIN_1, red) || !chainBusReadInput(CHAIN_GPIO_PIN_2, blue)) {
            ++_chain_bus_read_failures;
            if (_chain_bus_read_failures < CHAIN_INPUT_FAILURE_LIMIT) {
                mclog::tagWarn(TAG, "chainbus dual button read miss {}/{}", _chain_bus_read_failures,
                               CHAIN_INPUT_FAILURE_LIMIT);
                return true;
            }
            return false;
        }
        _chain_bus_read_failures = 0;
        if (red != 0) {
            buttons |= PAD_B;
        }
        if (blue != 0) {
            buttons |= PAD_A;
        }
        return true;
    }

    if (gpio_get_level(DUAL_BUTTON_RED_PIN) != 0) {
        buttons |= PAD_B;
    }
    if (gpio_get_level(DUAL_BUTTON_BLUE_PIN) != 0) {
        buttons |= PAD_A;
    }
    return true;
}

uint8_t ExternalInput::applyDirectionTransform(uint8_t buttons) const
{
    int x = 0;
    int y = 0;
    if (buttons & PAD_LEFT) {
        x = -1;
    } else if (buttons & PAD_RIGHT) {
        x = 1;
    }
    if (buttons & PAD_UP) {
        y = -1;
    } else if (buttons & PAD_DOWN) {
        y = 1;
    }

    if (_swap_axes) {
        std::swap(x, y);
    }
    if (_flip_x) {
        x = -x;
    }
    if (_flip_y) {
        y = -y;
    }

    uint8_t mapped = buttons & ~(PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
    if (x < 0) {
        mapped |= PAD_LEFT;
    } else if (x > 0) {
        mapped |= PAD_RIGHT;
    }
    if (y < 0) {
        mapped |= PAD_UP;
    } else if (y > 0) {
        mapped |= PAD_DOWN;
    }
    return mapped;
}

void ExternalInput::saveDirectionSettings()
{
    if (!_settings) {
        return;
    }
    _settings->SetBool("ext_flip_x", _flip_x);
    _settings->SetBool("ext_flip_y", _flip_y);
    _settings->SetBool("ext_swap_axes", _swap_axes);
}

bool ExternalInput::readByteButtons(uint8_t& buttons)
{
    buttons = 0;
    if (!_i2c) {
        return false;
    }

    uint8_t status[8] = {};
    if (!_i2c->readRegister(BYTE_BUTTON_ADDR, BYTE_BUTTON_STATUS_8BYTE_REG, status, sizeof(status),
                            EXTERNAL_INPUT_I2C_FREQ)) {
        return false;
    }

    if (status[0]) {
        buttons |= PAD_B;
    }
    if (status[1]) {
        buttons |= PAD_A;
    }
    if (status[2]) {
        buttons |= PAD_SELECT;
    }
    if (status[3]) {
        buttons |= PAD_START;
    }
    if (status[4]) {
        buttons |= PAD_UP;
    }
    if (status[5]) {
        buttons |= PAD_DOWN;
    }
    if (status[6]) {
        buttons |= PAD_LEFT;
    }
    if (status[7]) {
        buttons |= PAD_RIGHT;
    }
    return true;
}
