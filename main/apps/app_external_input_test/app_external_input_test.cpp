/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_external_input_test.h"
#include <apps/app_keyboard/assets/keyboard_big.h>
#include <apps/app_keyboard/assets/keyboard_small.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <assets.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <algorithm>
#include <cstdio>

using namespace mooncake;

namespace {
constexpr uint32_t I2C_FREQ = 100000;
constexpr uint32_t PROBE_INTERVAL_MS = 1000;
constexpr uint32_t READ_INTERVAL_MS = 80;
constexpr uint32_t RENDER_INTERVAL_MS = 120;
constexpr uint8_t JOYSTICK_UNIT_ADDR = 0x52;
constexpr uint8_t JOYSTICK2_ADDR = 0x63;
constexpr uint8_t JOYSTICK2_AXIS_REG = 0x60;
constexpr uint8_t JOYSTICK2_BUTTON_REG = 0x20;
constexpr uint8_t BYTE_BUTTON_ADDR = 0x47;
constexpr uint8_t BYTE_BUTTON_STATUS_REG = 0x60;
constexpr int JOYSTICK_UNIT_CENTER = 128;
constexpr int JOYSTICK_UNIT_DEAD_ZONE = 38;
constexpr int JOYSTICK2_DEAD_ZONE = 24;
constexpr uart_port_t CHAIN_UART = UART_NUM_1;
constexpr int CHAIN_RX_A = 1;
constexpr int CHAIN_TX_A = 2;
constexpr int CHAIN_RX_B = 2;
constexpr int CHAIN_TX_B = 1;
constexpr uint8_t CHAIN_DEVICE_JOYSTICK = 0x04;
constexpr gpio_num_t DUAL_BUTTON_RED_PIN = GPIO_NUM_1;
constexpr gpio_num_t DUAL_BUTTON_BLUE_PIN = GPIO_NUM_2;

void appendPad(std::string& text, const char* name)
{
    if (!text.empty()) {
        text += " ";
    }
    text += name;
}
}  // namespace

AppExternalInputTest::AppExternalInputTest()
{
    setAppInfo().name = "ExtIO";
    setAppInfo().userData = new AppIcon_t(image_data_keyboard_big, image_data_keyboard_small);
}

AppExternalInputTest::~AppExternalInputTest()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppExternalInputTest::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    GetHAL().externalInput.setPaused(true);
    M5.Ex_I2C.begin();
    _force_probe = true;
    _last_probe = 0;
    _last_read = 0;
    _last_render = 0;
    _i2c_resets = 0;
    _read_failures = 0;
    _chain = {};
    _key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& keyEvent) {
        handleKeyEvent(keyEvent);
    });
    probeChain();
    initDualButtonGpio();
    probe();
    readRaw();
    readChain();
    render();
}

void AppExternalInputTest::onRunning()
{
    const auto now = GetHAL().millis();
    const auto key = GetHAL().keyboard.getLatestKeyEventRaw();
    if (key.state && key.row == 2 && key.col == 13) {
        _force_probe = true;
    }

    if (_force_probe || now - _last_probe >= PROBE_INTERVAL_MS) {
        probe();
    }
    if (now - _last_read >= READ_INTERVAL_MS) {
        readRaw();
        readChain();
    }
    if (now - _last_render >= RENDER_INTERVAL_MS) {
        render();
    }

    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
    }
}

void AppExternalInputTest::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    if (_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_event_slot_id);
        _key_event_slot_id = -1;
    }
    GetHAL().externalInput.setPaused(false);
    GetHAL().externalInput.probe(GetHAL().millis());
    uart_driver_delete(CHAIN_UART);
}

void AppExternalInputTest::handleKeyEvent(const Keyboard::KeyEvent_t& keyEvent)
{
    if (!keyEvent.state) {
        return;
    }

    if (keyEvent.keyCode == KEY_U) {
        GetHAL().externalInput.toggleFlipY();
    } else if (keyEvent.keyCode == KEY_L) {
        GetHAL().externalInput.toggleFlipX();
    } else if (keyEvent.keyCode == KEY_S) {
        GetHAL().externalInput.toggleSwapAxes();
    } else if (keyEvent.keyCode == KEY_R || keyEvent.keyCode == KEY_ENTER) {
        _force_probe = true;
    } else {
        return;
    }
    render();
}

void AppExternalInputTest::probe()
{
    M5.Ex_I2C.begin();
    _probe.addresses = scanExternalBus();
    _probe.joystick_unit = M5.Ex_I2C.scanID(JOYSTICK_UNIT_ADDR, I2C_FREQ);
    _probe.joystick2 = M5.Ex_I2C.scanID(JOYSTICK2_ADDR, I2C_FREQ);
    _probe.byte_button = M5.Ex_I2C.scanID(BYTE_BUTTON_ADDR, I2C_FREQ);
    _last_probe = GetHAL().millis();
    _force_probe = false;

    mclog::tagInfo(getAppInfo().name,
                   "probe ex=[{}] joy52={} joy63={} btn47={}",
                   _probe.addresses,
                   _probe.joystick_unit ? "yes" : "no",
                   _probe.joystick2 ? "yes" : "no",
                   _probe.byte_button ? "yes" : "no");
}

void AppExternalInputTest::readRaw()
{
    _raw = {};
    if (_probe.joystick_unit) {
        if (M5.Ex_I2C.start(JOYSTICK_UNIT_ADDR, true, I2C_FREQ)) {
            _raw.joystick_unit_ok = M5.Ex_I2C.read(_raw.joystick_unit, sizeof(_raw.joystick_unit), true);
            M5.Ex_I2C.stop();
        }
    }

    if (_probe.joystick2) {
        _raw.joystick2_axis_ok = M5.Ex_I2C.readRegister(JOYSTICK2_ADDR,
                                                        JOYSTICK2_AXIS_REG,
                                                        _raw.joystick2_axis,
                                                        sizeof(_raw.joystick2_axis),
                                                        I2C_FREQ);
        _raw.joystick2_button_ok = M5.Ex_I2C.readRegister(JOYSTICK2_ADDR,
                                                          JOYSTICK2_BUTTON_REG,
                                                          &_raw.joystick2_button,
                                                          1,
                                                          I2C_FREQ);
    }

    if (_probe.byte_button) {
        _raw.byte_button_ok = M5.Ex_I2C.readRegister(BYTE_BUTTON_ADDR,
                                                     BYTE_BUTTON_STATUS_REG,
                                                     _raw.byte_button,
                                                     sizeof(_raw.byte_button),
                                                     I2C_FREQ);
    }
    if (!_chain.device_found) {
        _raw.dual_button_ok = true;
        _raw.dual_red = gpio_get_level(DUAL_BUTTON_RED_PIN) != 0;
        _raw.dual_blue = gpio_get_level(DUAL_BUTTON_BLUE_PIN) != 0;
    }

    const bool expectedDevice = _probe.joystick_unit || _probe.joystick2 || _probe.byte_button;
    const bool anyReadOk = _raw.joystick_unit_ok ||
                           _raw.joystick2_axis_ok ||
                           _raw.joystick2_button_ok ||
                           _raw.byte_button_ok;
    if (expectedDevice && !anyReadOk) {
        ++_read_failures;
        if ((_read_failures % 3) == 0) {
            resetExternalBus();
            probe();
        }
    }
    _last_read = GetHAL().millis();
}

void AppExternalInputTest::render()
{
    auto& canvas = GetHAL().canvas;
    canvas.setBaseColor(THEME_COLOR_BG);
    canvas.setTextScroll(false);
    canvas.setFont(FONT_REPL);
    canvas.setTextSize(1);
    canvas.fillScreen(THEME_COLOR_BG);
    canvas.setCursor(0, 0);

    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.println("ExtIO 4-pin test");

    canvas.setTextColor(_chain.device_found ? TFT_GREEN : TFT_RED, THEME_COLOR_BG);
    canvas.printf("Chain UART: %s RX%d TX%d\n",
                  _chain.device_found ? "yes" : "no",
                  _chain.rx_pin,
                  _chain.tx_pin);
    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.printf("  cnt:%u type:%04X pkt:%u\n",
                  _chain.count,
                  _chain.device_type,
                  static_cast<unsigned>(_chain.packets));
    canvas.printf("  x:%d y:%d btn:%u fail:%u\n",
                  static_cast<int>(_chain.x),
                  static_cast<int>(_chain.y),
                  _chain.button,
                  static_cast<unsigned>(_chain.failures));

    canvas.setTextColor(TFT_CYAN, THEME_COLOR_BG);
    canvas.printf("Ex I2C: %s\n", _probe.addresses.c_str());

    canvas.setTextColor(_probe.joystick_unit ? TFT_GREEN : TFT_RED, THEME_COLOR_BG);
    canvas.printf("0x52 JoyUnit: %s\n", _probe.joystick_unit ? "yes" : "no");
    canvas.setTextColor(_raw.joystick_unit_ok ? TFT_WHITE : TFT_DARKGREY, THEME_COLOR_BG);
    canvas.printf("  raw: %s\n", _raw.joystick_unit_ok ? formatBytes(_raw.joystick_unit, 3).c_str() : "--");

    canvas.setTextColor(_probe.joystick2 ? TFT_GREEN : TFT_RED, THEME_COLOR_BG);
    canvas.printf("0x63 Joy2: %s\n", _probe.joystick2 ? "yes" : "no");
    canvas.setTextColor((_raw.joystick2_axis_ok || _raw.joystick2_button_ok) ? TFT_WHITE : TFT_DARKGREY, THEME_COLOR_BG);
    canvas.printf("  axis:%s btn:%02X\n",
                  _raw.joystick2_axis_ok ? formatBytes(_raw.joystick2_axis, 2).c_str() : "--",
                  _raw.joystick2_button_ok ? _raw.joystick2_button : 0xFF);

    canvas.setTextColor(_probe.byte_button ? TFT_GREEN : TFT_RED, THEME_COLOR_BG);
    canvas.printf("0x47 Btn: %s\n", _probe.byte_button ? "yes" : "no");
    canvas.setTextColor(_raw.byte_button_ok ? TFT_WHITE : TFT_DARKGREY, THEME_COLOR_BG);
    canvas.printf("  raw: %s\n", _raw.byte_button_ok ? formatBytes(_raw.byte_button, 8).c_str() : "--");

    canvas.setTextColor(_raw.dual_button_ok ? TFT_GREEN : TFT_DARKGREY, THEME_COLOR_BG);
    canvas.printf("Dual GPIO: R:%u->B B:%u->A\n", _raw.dual_red ? 1 : 0, _raw.dual_blue ? 1 : 0);

    const uint8_t rawButtons = decodeRawButtons();
    const uint8_t serviceButtons = GetHAL().externalInput.getButtons();
    canvas.setTextColor(TFT_YELLOW, THEME_COLOR_BG);
    canvas.printf("raw pad: %s\n", formatPad(rawButtons).c_str());
    canvas.printf("svc pad: %s\n", formatPad(serviceButtons).c_str());
    canvas.printf("flip L/R:%s U/D:%s swap:%s\n",
                  GetHAL().externalInput.getFlipX() ? "on" : "off",
                  GetHAL().externalInput.getFlipY() ? "on" : "off",
                  GetHAL().externalInput.getSwapAxes() ? "on" : "off");

    canvas.setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
    canvas.printf("fail:%u reset:%u svc:paused\n",
                  static_cast<unsigned>(_read_failures),
                  static_cast<unsigned>(_i2c_resets));
    canvas.println("U/L/S: flip/swap  R:scan");
    GetHAL().pushCanvas();
    _last_render = GetHAL().millis();
}

void AppExternalInputTest::resetExternalBus()
{
    mclog::tagWarn(getAppInfo().name, "external i2c reset after read failures={}", static_cast<unsigned>(_read_failures));
    M5.Ex_I2C.release();
    GetHAL().delay(5);
    M5.Ex_I2C.begin();
    GetHAL().delay(2);
    ++_i2c_resets;
    _force_probe = true;
}

void AppExternalInputTest::initDualButtonGpio()
{
    if (_chain.device_found) {
        return;
    }
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << DUAL_BUTTON_RED_PIN) | (1ULL << DUAL_BUTTON_BLUE_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
}

void AppExternalInputTest::initChainUart(int rxPin, int txPin)
{
    uart_driver_delete(CHAIN_UART);
    uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(CHAIN_UART, 1024, 0, 0, nullptr, 0);
    uart_param_config(CHAIN_UART, &config);
    uart_set_pin(CHAIN_UART, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_flush(CHAIN_UART);
    _chain.rx_pin = rxPin;
    _chain.tx_pin = txPin;
    _chain.active = true;
}

void AppExternalInputTest::probeChain()
{
    auto tryPins = [this](int rxPin, int txPin) {
        initChainUart(rxPin, txPin);
        uint8_t sendNum = 0;
        uint8_t response[64] = {};
        size_t responseSize = sizeof(response);
        if (!chainCommand(0xFF, 0xFE, &sendNum, 1, response, responseSize)) {
            return false;
        }
        const uint8_t* data = nullptr;
        size_t dataSize = 0;
        if (!parseChainValue(0xFE, response, responseSize, data, dataSize) || dataSize < 1) {
            return false;
        }

        _chain.count = data[0];
        _chain.device_found = _chain.count > 0;
        if (!_chain.device_found) {
            return false;
        }

        responseSize = sizeof(response);
        if (chainCommand(1, 0xFB, nullptr, 0, response, responseSize) &&
            parseChainValue(0xFB, response, responseSize, data, dataSize) &&
            dataSize >= 2) {
            _chain.device_type = static_cast<uint16_t>(data[0] | (data[1] << 8));
            _chain.type_ok = _chain.device_type == CHAIN_DEVICE_JOYSTICK;
        }
        return true;
    };

    _chain = {};
    if (!tryPins(CHAIN_RX_A, CHAIN_TX_A)) {
        _chain = {};
        tryPins(CHAIN_RX_B, CHAIN_TX_B);
    }
    mclog::tagInfo(getAppInfo().name,
                   "chain probe found={} count={} type=0x{:04X} rx={} tx={}",
                   _chain.device_found ? "yes" : "no",
                   _chain.count,
                   _chain.device_type,
                   _chain.rx_pin,
                   _chain.tx_pin);
}

void AppExternalInputTest::readChain()
{
    if (!_chain.active || !_chain.device_found) {
        static uint32_t lastProbe = 0;
        const auto now = GetHAL().millis();
        if (now - lastProbe > 1000) {
            lastProbe = now;
            probeChain();
        }
        return;
    }

    uint8_t response[64] = {};
    size_t responseSize = sizeof(response);
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
    if (chainCommand(1, 0x35, nullptr, 0, response, responseSize) &&
        parseChainValue(0x35, response, responseSize, data, dataSize) &&
        dataSize >= 2) {
        _chain.x = static_cast<int8_t>(data[0]);
        _chain.y = static_cast<int8_t>(data[1]);
        _chain.axis_ok = true;
        ++_chain.packets;
    } else {
        _chain.axis_ok = false;
        ++_chain.failures;
    }

    responseSize = sizeof(response);
    if (chainCommand(1, 0xE1, nullptr, 0, response, responseSize) &&
        parseChainValue(0xE1, response, responseSize, data, dataSize) &&
        dataSize >= 1) {
        _chain.button = data[0];
        _chain.button_ok = true;
    } else {
        _chain.button_ok = false;
        ++_chain.failures;
    }
}

bool AppExternalInputTest::chainCommand(uint8_t index,
                                        uint8_t command,
                                        const uint8_t* data,
                                        size_t dataSize,
                                        uint8_t* response,
                                        size_t& responseSize)
{
    if (!_chain.active) {
        return false;
    }
    uart_flush(CHAIN_UART);
    chainWritePacket(index, command, data, dataSize);
    return chainReadPacket(response, responseSize, 80);
}

bool AppExternalInputTest::chainReadPacket(uint8_t* response, size_t& responseSize, uint32_t timeoutMs)
{
    if (!response || responseSize < 9) {
        return false;
    }

    const auto start = GetHAL().millis();
    size_t pos = 0;
    while (GetHAL().millis() - start < timeoutMs) {
        uint8_t byte = 0;
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
            const size_t total = 2 + 2 + length + 2;
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

void AppExternalInputTest::chainWritePacket(uint8_t index, uint8_t command, const uint8_t* data, size_t dataSize)
{
    uint8_t frame[64] = {};
    const size_t length = 3 + dataSize;
    const size_t total = 2 + 2 + length + 2;
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

bool AppExternalInputTest::parseChainValue(uint8_t command,
                                           const uint8_t* response,
                                           size_t responseSize,
                                           const uint8_t*& data,
                                           size_t& dataSize) const
{
    data = nullptr;
    dataSize = 0;
    if (!response || responseSize < 9 || response[5] != command) {
        return false;
    }
    const size_t length = response[2] | (response[3] << 8);
    if (length < 3 || 2 + 2 + length + 2 != responseSize) {
        return false;
    }
    data = &response[6];
    dataSize = length - 3;
    return true;
}

uint8_t AppExternalInputTest::chainCrc(const uint8_t* frame, size_t frameSize) const
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

std::string AppExternalInputTest::scanExternalBus()
{
    std::string addresses;
    for (uint8_t address = 0x08; address < 0x78; ++address) {
        if (!M5.Ex_I2C.scanID(address, I2C_FREQ)) {
            continue;
        }
        if (!addresses.empty()) {
            addresses += ",";
        }
        char item[8] = {};
        std::snprintf(item, sizeof(item), "0x%02X", address);
        addresses += item;
    }
    return addresses.empty() ? "none" : addresses;
}

std::string AppExternalInputTest::formatBytes(const uint8_t* data, size_t size) const
{
    std::string text;
    for (size_t i = 0; i < size; ++i) {
        if (!text.empty()) {
            text += " ";
        }
        char item[4] = {};
        std::snprintf(item, sizeof(item), "%02X", data[i]);
        text += item;
    }
    return text;
}

std::string AppExternalInputTest::formatPad(uint8_t buttons) const
{
    std::string text;
    if (buttons & ExternalInput::PAD_UP) {
        appendPad(text, "U");
    }
    if (buttons & ExternalInput::PAD_DOWN) {
        appendPad(text, "D");
    }
    if (buttons & ExternalInput::PAD_LEFT) {
        appendPad(text, "L");
    }
    if (buttons & ExternalInput::PAD_RIGHT) {
        appendPad(text, "R");
    }
    if (buttons & ExternalInput::PAD_A) {
        appendPad(text, "A");
    }
    if (buttons & ExternalInput::PAD_B) {
        appendPad(text, "B");
    }
    if (buttons & ExternalInput::PAD_SELECT) {
        appendPad(text, "Sel");
    }
    if (buttons & ExternalInput::PAD_START) {
        appendPad(text, "Start");
    }
    return text.empty() ? "none" : text;
}

uint8_t AppExternalInputTest::decodeRawButtons() const
{
    uint8_t buttons = 0;
    if (_raw.joystick_unit_ok) {
        const int x = _raw.joystick_unit[0];
        const int y = _raw.joystick_unit[1];
        if (x < JOYSTICK_UNIT_CENTER - JOYSTICK_UNIT_DEAD_ZONE) {
            buttons |= ExternalInput::PAD_LEFT;
        } else if (x > JOYSTICK_UNIT_CENTER + JOYSTICK_UNIT_DEAD_ZONE) {
            buttons |= ExternalInput::PAD_RIGHT;
        }
        if (y < JOYSTICK_UNIT_CENTER - JOYSTICK_UNIT_DEAD_ZONE) {
            buttons |= ExternalInput::PAD_UP;
        } else if (y > JOYSTICK_UNIT_CENTER + JOYSTICK_UNIT_DEAD_ZONE) {
            buttons |= ExternalInput::PAD_DOWN;
        }
        if (_raw.joystick_unit[2] != 0) {
            buttons |= ExternalInput::PAD_A;
        }
    }

    if (_raw.joystick2_axis_ok) {
        const int x = static_cast<int8_t>(_raw.joystick2_axis[0]);
        const int y = static_cast<int8_t>(_raw.joystick2_axis[1]);
        if (x < -JOYSTICK2_DEAD_ZONE) {
            buttons |= ExternalInput::PAD_LEFT;
        } else if (x > JOYSTICK2_DEAD_ZONE) {
            buttons |= ExternalInput::PAD_RIGHT;
        }
        if (y < -JOYSTICK2_DEAD_ZONE) {
            buttons |= ExternalInput::PAD_UP;
        } else if (y > JOYSTICK2_DEAD_ZONE) {
            buttons |= ExternalInput::PAD_DOWN;
        }
    }
    if (_raw.joystick2_button_ok && _raw.joystick2_button == 0) {
        buttons |= ExternalInput::PAD_A;
    }

    if (_raw.byte_button_ok) {
        if (_raw.byte_button[0]) {
            buttons |= ExternalInput::PAD_B;
        }
        if (_raw.byte_button[1]) {
            buttons |= ExternalInput::PAD_A;
        }
        if (_raw.byte_button[2]) {
            buttons |= ExternalInput::PAD_SELECT;
        }
        if (_raw.byte_button[3]) {
            buttons |= ExternalInput::PAD_START;
        }
        if (_raw.byte_button[4]) {
            buttons |= ExternalInput::PAD_UP;
        }
        if (_raw.byte_button[5]) {
            buttons |= ExternalInput::PAD_DOWN;
        }
        if (_raw.byte_button[6]) {
            buttons |= ExternalInput::PAD_LEFT;
        }
        if (_raw.byte_button[7]) {
            buttons |= ExternalInput::PAD_RIGHT;
        }
    }
    if (_raw.dual_button_ok) {
        if (_raw.dual_red) {
            buttons |= ExternalInput::PAD_B;
        }
        if (_raw.dual_blue) {
            buttons |= ExternalInput::PAD_A;
        }
    }
    return buttons;
}
