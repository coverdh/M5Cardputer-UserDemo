/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
// https://github.com/espressif/esp-idf/blob/v5.4.2/examples/bluetooth/esp_hid_device
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_HID_DEVICE_STATE_IDLE = 0,
    BLE_HID_DEVICE_STATE_CONNECTED,
} BleHidDeviceState_t;

void ble_hid_device_helper_init(void);
void ble_hid_device_helper_send(uint8_t* buffer);
void ble_hid_device_helper_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
void ble_hid_device_helper_send_consumer(uint16_t usage_id);
BleHidDeviceState_t ble_hid_device_helper_get_state(void);

enum {
    BLE_HID_CONSUMER_POWER = 48,
    BLE_HID_CONSUMER_PLAY = 176,
    BLE_HID_CONSUMER_PAUSE = 177,
    BLE_HID_CONSUMER_SCAN_NEXT_TRACK = 181,
    BLE_HID_CONSUMER_SCAN_PREVIOUS_TRACK = 182,
    BLE_HID_CONSUMER_STOP = 183,
    BLE_HID_CONSUMER_PLAY_PAUSE = 205,
    BLE_HID_CONSUMER_MUTE = 226,
    BLE_HID_CONSUMER_VOLUME_UP = 233,
    BLE_HID_CONSUMER_VOLUME_DOWN = 234,
};

#ifdef __cplusplus
}
#endif
