#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BLE_HID_REPORT_AUDIO_QUEUE_RESERVE 8
#define BLE_HID_REPORT_MOUSE_QUEUE_RESERVE 4
#define BLE_HID_REPORT_CONTROL_QUEUE_LEN 16
#define BLE_HID_REPORT_BEST_EFFORT_QUEUE_LEN 32
#define BLE_HID_REPORT_MACCTL_AUDIO_ULAW_FRAME 0xA0
#define BLE_HID_REPORT_MACCTL_AUDIO_ADPCM_FRAME 0xA2

static inline bool ble_hid_report_is_audio_frame(uint8_t map_index, uint8_t report_id, uint8_t macctl_map_index,
                                                 uint8_t macctl_report_id, const uint8_t* data, uint8_t len)
{
    return map_index == macctl_map_index && report_id == macctl_report_id && data && len > 0 &&
           (data[0] == BLE_HID_REPORT_MACCTL_AUDIO_ULAW_FRAME ||
            data[0] == BLE_HID_REPORT_MACCTL_AUDIO_ADPCM_FRAME);
}

static inline bool ble_hid_report_is_mouse_movement(uint8_t map_index, uint8_t report_id, uint8_t mouse_map_index,
                                                    uint8_t mouse_report_id, const uint8_t* data, uint8_t len)
{
    return map_index == mouse_map_index && report_id == mouse_report_id && data && len >= 4 && data[0] == 0 &&
           (data[1] != 0 || data[2] != 0 || data[3] != 0);
}

static inline bool ble_hid_report_should_drop_audio(uint32_t spaces_available)
{
    return spaces_available <= BLE_HID_REPORT_AUDIO_QUEUE_RESERVE;
}

static inline bool ble_hid_report_should_drop_mouse_movement(uint32_t spaces_available)
{
    return spaces_available <= BLE_HID_REPORT_MOUSE_QUEUE_RESERVE;
}

static inline bool ble_hid_report_should_prioritize(uint8_t map_index, uint8_t report_id, uint8_t macctl_map_index,
                                                    uint8_t macctl_report_id, uint8_t mouse_map_index,
                                                    uint8_t mouse_report_id, const uint8_t* data, uint8_t len)
{
    return !ble_hid_report_is_audio_frame(map_index, report_id, macctl_map_index, macctl_report_id, data, len) &&
           !ble_hid_report_is_mouse_movement(map_index, report_id, mouse_map_index, mouse_report_id, data, len);
}

static inline bool ble_hid_report_is_best_effort(uint8_t map_index, uint8_t report_id, uint8_t macctl_map_index,
                                                 uint8_t macctl_report_id, uint8_t mouse_map_index,
                                                 uint8_t mouse_report_id, const uint8_t* data, uint8_t len)
{
    return ble_hid_report_is_audio_frame(map_index, report_id, macctl_map_index, macctl_report_id, data, len) ||
           ble_hid_report_is_mouse_movement(map_index, report_id, mouse_map_index, mouse_report_id, data, len);
}
