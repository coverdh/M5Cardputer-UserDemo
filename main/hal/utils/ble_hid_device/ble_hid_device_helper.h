/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
// https://github.com/espressif/esp-idf/blob/v5.4.2/examples/bluetooth/esp_hid_device
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_HID_DEVICE_STATE_IDLE = 0,
    BLE_HID_DEVICE_STATE_CONNECTED,
} BleHidDeviceState_t;

typedef void (*ble_hid_device_helper_output_callback_t)(const uint8_t* data, uint8_t len);

bool ble_hid_device_helper_init(void);
void ble_hid_device_helper_stop(void);
bool ble_hid_device_helper_forget_bonds(void);
bool ble_hid_device_helper_ensure_advertising(void);
void ble_hid_device_helper_send(uint8_t* buffer);
void ble_hid_device_helper_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
void ble_hid_device_helper_send_consumer(uint16_t usage_id);
bool ble_hid_device_helper_send_macctl_volume_delta(int8_t delta);
bool ble_hid_device_helper_send_macctl_play_pause(void);
bool ble_hid_device_helper_send_macctl_config(uint8_t flags, uint8_t sensitivity, uint8_t knob_mode);
bool ble_hid_device_helper_send_macctl_power_config(uint8_t screen_timeout_s, uint8_t power_save_timeout_min);
bool ble_hid_device_helper_send_macctl_audio_state(bool active);
bool ble_hid_device_helper_send_macctl_audio(uint8_t sequence, const uint8_t* data, uint8_t len);
void ble_hid_device_helper_set_output_callback(ble_hid_device_helper_output_callback_t callback);
void ble_hid_device_helper_poll_output_reports(void);
BleHidDeviceState_t ble_hid_device_helper_get_state(void);
bool ble_hid_device_helper_is_ready(void);
void ble_hid_device_helper_gap_connected(uint16_t conn_handle);
void ble_hid_device_helper_gap_disconnected(uint16_t conn_handle);
void ble_hid_device_helper_gap_encrypted(uint16_t conn_handle);
void ble_hid_device_helper_gap_subscribe(uint16_t conn_handle, uint16_t attr_handle, bool notify_enabled);

enum {
    BLE_HID_CONSUMER_POWER               = 48,
    BLE_HID_CONSUMER_PLAY                = 176,
    BLE_HID_CONSUMER_PAUSE               = 177,
    BLE_HID_CONSUMER_SCAN_NEXT_TRACK     = 181,
    BLE_HID_CONSUMER_SCAN_PREVIOUS_TRACK = 182,
    BLE_HID_CONSUMER_STOP                = 183,
    BLE_HID_CONSUMER_PLAY_PAUSE          = 205,
    BLE_HID_CONSUMER_MUTE                = 226,
    BLE_HID_CONSUMER_VOLUME_UP           = 233,
    BLE_HID_CONSUMER_VOLUME_DOWN         = 234,
};

#ifdef __cplusplus
}
#endif
