/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "ble_hid_device_helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#else
#include "esp_bt_defs.h"
#if CONFIG_BT_BLE_ENABLED
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#endif
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#if CONFIG_BT_SDP_COMMON_ENABLED
#include "esp_sdp_api.h"
#endif /* CONFIG_BT_SDP_COMMON_ENABLED */
#endif

#include "esp_hidd.h"
#include "ble_hid_gap.h"
#include "ble_hid_report_queue_policy.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "services/bas/ble_svc_bas.h"
#endif

static const char *TAG = "ble_hid";

#define BLE_HID_MACCTL_REPORT_LEN 63

static BleHidDeviceState_t s_ble_hid_keyboard_state = BLE_HID_DEVICE_STATE_IDLE;
static TickType_t s_ble_hid_ready_after_tick        = 0;
static bool s_ble_hid_notify_ready                  = false;
static bool s_ble_hid_stack_ready                   = false;
#if CONFIG_BT_NIMBLE_ENABLED
static uint16_t s_ble_hid_conn_handle               = BLE_HS_CONN_HANDLE_NONE;
#endif

typedef struct {
    uint8_t map_index;
    uint8_t report_id;
    uint8_t len;
    uint8_t data[BLE_HID_MACCTL_REPORT_LEN];
    uint16_t delay_after_ms;
} BleHidQueuedReport_t;

static QueueHandle_t s_ble_hid_control_report_queue     = NULL;
static QueueHandle_t s_ble_hid_best_effort_report_queue = NULL;
static TaskHandle_t s_ble_hid_report_task               = NULL;
static ble_hid_device_helper_output_callback_t s_ble_hid_output_callback = NULL;
#if CONFIG_BT_NIMBLE_ENABLED
static TickType_t s_ble_hid_last_drop_log    = 0;
#endif

#define BLE_HID_READY_DELAY_MS 2000

#define MACCTL_BLE_STORE_SCHEMA_VERSION 2

#if CONFIG_BT_NIMBLE_ENABLED
#define BLE_HID_MAP_INDEX_KEYBOARD 0
#define BLE_HID_MAP_INDEX_MOUSE    0
#define BLE_HID_MAP_INDEX_MEDIA    0
#define BLE_HID_MAP_INDEX_MACCTL   0
#else
#define BLE_HID_MAP_INDEX_KEYBOARD 0
#define BLE_HID_MAP_INDEX_MOUSE    1
#define BLE_HID_MAP_INDEX_MEDIA    2
#define BLE_HID_MAP_INDEX_MACCTL   3
#endif

#define BLE_HID_RPT_ID_KEYBOARD 1
#define BLE_HID_RPT_ID_MOUSE    2
#define BLE_HID_RPT_ID_MEDIA    3
#define BLE_HID_RPT_ID_MACCTL   4

#define MACCTL_CMD_VOLUME_DELTA 1
#define MACCTL_CMD_PLAY_PAUSE   2
#define MACCTL_LED_CMD_PREFIX      0x1F
#define MACCTL_LED_CMD_AUDIO_STOP  0x10
#define MACCTL_LED_CMD_AUDIO_START 0x11
#define MACCTL_LED_CMD_WINDOW_MS   1000

typedef struct {
    TaskHandle_t task_hdl;
    esp_hidd_dev_t *hid_dev;
    uint8_t protocol_mode;
    uint8_t *buffer;
} local_param_t;

#if CONFIG_BT_BLE_ENABLED || CONFIG_BT_NIMBLE_ENABLED
static local_param_t s_ble_hid_param = {0};

static bool ble_hid_device_helper_queue_report(uint8_t map_index, uint8_t report_id, const uint8_t *data, uint8_t len,
                                               uint16_t delay_after_ms)
{
    if (!ble_hid_device_helper_is_ready() || !s_ble_hid_control_report_queue ||
        !s_ble_hid_best_effort_report_queue || !data || len > sizeof(((BleHidQueuedReport_t *)0)->data)) {
        return false;
    }

    const bool is_audio_frame = ble_hid_report_is_audio_frame(map_index, report_id, BLE_HID_MAP_INDEX_MACCTL,
                                                              BLE_HID_RPT_ID_MACCTL, data, len);
    if (is_audio_frame &&
        ble_hid_report_should_drop_audio(uxQueueSpacesAvailable(s_ble_hid_best_effort_report_queue))) {
        TickType_t now = xTaskGetTickCount();
        if (now - s_ble_hid_last_drop_log > pdMS_TO_TICKS(2000)) {
            s_ble_hid_last_drop_log = now;
            ESP_LOGW(TAG, "hid audio queue congested; drop audio frame");
        }
        return false;
    }
    const bool is_mouse_movement = ble_hid_report_is_mouse_movement(map_index, report_id, BLE_HID_MAP_INDEX_MOUSE,
                                                                    BLE_HID_RPT_ID_MOUSE, data, len);
    if (is_mouse_movement &&
        ble_hid_report_should_drop_mouse_movement(uxQueueSpacesAvailable(s_ble_hid_best_effort_report_queue))) {
        return false;
    }

    BleHidQueuedReport_t report = {
        .map_index      = map_index,
        .report_id      = report_id,
        .len            = len,
        .delay_after_ms = delay_after_ms,
    };
    memcpy(report.data, data, len);
    const bool best_effort = ble_hid_report_is_best_effort(map_index, report_id, BLE_HID_MAP_INDEX_MACCTL,
                                                           BLE_HID_RPT_ID_MACCTL, BLE_HID_MAP_INDEX_MOUSE,
                                                           BLE_HID_RPT_ID_MOUSE, data, len);
    QueueHandle_t queue = best_effort ? s_ble_hid_best_effort_report_queue : s_ble_hid_control_report_queue;
    const BaseType_t sent = xQueueSend(queue, &report, 0);
    if (sent != pdTRUE) {
        ESP_LOGW(TAG, "hid %s report queue full; drop report=%u", best_effort ? "best-effort" : "control",
                 report_id);
        return false;
    }
    if (s_ble_hid_report_task) {
        xTaskNotifyGive(s_ble_hid_report_task);
    }
    return true;
}

static void ble_hid_device_helper_report_task(void *pvParameters)
{
    (void)pvParameters;
    BleHidQueuedReport_t report;
    while (true) {
        if (!s_ble_hid_control_report_queue || !s_ble_hid_best_effort_report_queue) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (xQueueReceive(s_ble_hid_control_report_queue, &report, 0) != pdTRUE &&
            xQueueReceive(s_ble_hid_best_effort_report_queue, &report, 0) != pdTRUE) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (ble_hid_device_helper_is_ready()) {
#if CONFIG_BT_NIMBLE_ENABLED
            if (!esp_vhci_host_check_send_available()) {
                TickType_t now = xTaskGetTickCount();
                if (now - s_ble_hid_last_drop_log > pdMS_TO_TICKS(2000)) {
                    s_ble_hid_last_drop_log = now;
                    ESP_LOGW(TAG, "VHCI not ready; drop hid report=%u", report.report_id);
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
#endif
            esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, report.map_index, report.report_id, report.data, report.len);
        }
        vTaskDelay(pdMS_TO_TICKS(report.delay_after_ms > 0 ? report.delay_after_ms : 20));
    }
}

static void ble_hid_device_helper_ensure_report_task(void)
{
    if (!s_ble_hid_control_report_queue) {
        s_ble_hid_control_report_queue =
            xQueueCreate(BLE_HID_REPORT_CONTROL_QUEUE_LEN, sizeof(BleHidQueuedReport_t));
    }
    if (!s_ble_hid_best_effort_report_queue) {
        s_ble_hid_best_effort_report_queue =
            xQueueCreate(BLE_HID_REPORT_BEST_EFFORT_QUEUE_LEN, sizeof(BleHidQueuedReport_t));
    }
    if (s_ble_hid_control_report_queue && s_ble_hid_best_effort_report_queue && !s_ble_hid_report_task) {
        xTaskCreatePinnedToCore(ble_hid_device_helper_report_task, "ble_hid_tx", 4096, NULL, 3,
                                &s_ble_hid_report_task, CONFIG_BT_NIMBLE_PINNED_TO_CORE);
    }
}

static bool ble_hid_device_helper_queue_macctl_report(const uint8_t *buffer, uint8_t len)
{
    uint8_t report[BLE_HID_MACCTL_REPORT_LEN] = {0};
    if (buffer && len > 0) {
        memcpy(report, buffer, len > sizeof(report) ? sizeof(report) : len);
    }
    return ble_hid_device_helper_queue_report(BLE_HID_MAP_INDEX_MACCTL, BLE_HID_RPT_ID_MACCTL, report, sizeof(report), 4);
}

static bool ble_hid_device_helper_queue_macctl_command(uint8_t command, int8_t value)
{
    uint8_t buffer[4] = {command, (uint8_t)value, 0, 0};
    const bool queued = ble_hid_device_helper_queue_macctl_report(buffer, sizeof(buffer));
    if (queued) {
        ESP_LOGI(TAG, "macctl command queued: command=%u value=%d", command, value);
    } else {
        ESP_LOGW(TAG, "macctl command dropped: command=%u value=%d", command, value);
    }
    return queued;
}

static void ble_hid_device_helper_handle_keyboard_output(const uint8_t* data, uint8_t len)
{
    static TickType_t s_led_command_prefix_tick = 0;
    if (!data || len == 0) {
        return;
    }

    const uint8_t value = data[0] & 0x1F;
    const TickType_t now = xTaskGetTickCount();
    if (value == MACCTL_LED_CMD_PREFIX) {
        s_led_command_prefix_tick = now;
        return;
    }

    if (!s_ble_hid_output_callback || s_led_command_prefix_tick == 0 ||
        now - s_led_command_prefix_tick > pdMS_TO_TICKS(MACCTL_LED_CMD_WINDOW_MS)) {
        return;
    }

    if (value == MACCTL_LED_CMD_AUDIO_START || value == MACCTL_LED_CMD_AUDIO_STOP) {
        uint8_t command[4] = {0x82, value == MACCTL_LED_CMD_AUDIO_START ? 1 : 0, 0, 0};
        s_led_command_prefix_tick = 0;
        s_ble_hid_output_callback(command, sizeof(command));
    }
}

const unsigned char mediaReportMap[] = {
    0x05, 0x0C,                    // Usage Page (Consumer)
    0x09, 0x01,                    // Usage (Consumer Control)
    0xA1, 0x01,                    // Collection (Application)
    0x85, BLE_HID_RPT_ID_MEDIA,    //   Report ID (3)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x26, 0xFF, 0x03,              //   Logical Maximum (0x03FF)
    0x19, 0x00,                    //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,              //   Usage Maximum (0x03FF)
    0x75, 0x10,                    //   Report Size (16)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x00,                    //   Input (Data,Array,Abs)
    0xC0,                          // End Collection
};
const unsigned char bleMouseReportMap[] = {
    0x05, 0x01,                  // USAGE_PAGE (Generic Desktop)
    0x09, 0x02,                  // USAGE (Mouse)
    0xa1, 0x01,                  // COLLECTION (Application)
    0x85, BLE_HID_RPT_ID_MOUSE,  //   REPORT_ID (2)

    0x09, 0x01,  //   USAGE (Pointer)
    0xa1, 0x00,  //   COLLECTION (Physical)

    0x05, 0x09,  //     USAGE_PAGE (Button)
    0x19, 0x01,  //     USAGE_MINIMUM (Button 1)
    0x29, 0x03,  //     USAGE_MAXIMUM (Button 3)
    0x15, 0x00,  //     LOGICAL_MINIMUM (0)
    0x25, 0x01,  //     LOGICAL_MAXIMUM (1)
    0x95, 0x03,  //     REPORT_COUNT (3)
    0x75, 0x01,  //     REPORT_SIZE (1)
    0x81, 0x02,  //     INPUT (Data,Var,Abs)
    0x95, 0x01,  //     REPORT_COUNT (1)
    0x75, 0x05,  //     REPORT_SIZE (5)
    0x81, 0x03,  //     INPUT (Cnst,Var,Abs)

    0x05, 0x01,  //     USAGE_PAGE (Generic Desktop)
    0x09, 0x30,  //     USAGE (X)
    0x09, 0x31,  //     USAGE (Y)
    0x09, 0x38,  //     USAGE (Wheel)
    0x15, 0x81,  //     LOGICAL_MINIMUM (-127)
    0x25, 0x7f,  //     LOGICAL_MAXIMUM (127)
    0x75, 0x08,  //     REPORT_SIZE (8)
    0x95, 0x03,  //     REPORT_COUNT (3)
    0x81, 0x06,  //     INPUT (Data,Var,Rel)

    0xc0,  //   END_COLLECTION
    0xc0   // END_COLLECTION
};

const unsigned char macctlReportMap[] = {
    0x06, 0x00, 0xFF,              // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,                    // Usage (0x01)
    0xA1, 0x01,                    // Collection (Application)
    0x85, BLE_HID_RPT_ID_MACCTL,   //   Report ID (4)
    0x09, 0x02,                    //   Usage (0x02)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x26, 0xFF, 0x00,              //   Logical Maximum (255)
    0x75, 0x08,                    //   Report Size (8)
    0x95, BLE_HID_MACCTL_REPORT_LEN, //   Report Count (63)
    0x81, 0x02,                    //   Input (Data,Var,Abs)
    0x09, 0x03,                    //   Usage (0x03)
    0x95, BLE_HID_MACCTL_REPORT_LEN, //   Report Count (63)
    0x91, 0x02,                    //   Output (Data,Var,Abs)
    0x09, 0x04,                    //   Usage (0x04)
    0x95, BLE_HID_MACCTL_REPORT_LEN, //   Report Count (63)
    0xB1, 0x02,                    //   Feature (Data,Var,Abs)
    0xC0,                          // End Collection
};

const unsigned char compositeReportMap[] = {
    // Keyboard
    0x05, 0x01,  // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x85, BLE_HID_RPT_ID_KEYBOARD,
    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x03,
    0x95, 0x05,
    0x75, 0x01,
    0x05, 0x08,
    0x19, 0x01,
    0x29, 0x05,
    0x91, 0x02,
    0x95, 0x01,
    0x75, 0x03,
    0x91, 0x03,
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,
    0xC0,

    // Mouse
    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x85, BLE_HID_RPT_ID_MOUSE,
    0x09, 0x01,
    0xA1, 0x00,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x03,
    0x75, 0x01,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x05,
    0x81, 0x03,
    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x09, 0x38,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x03,
    0x81, 0x06,
    0xC0,
    0xC0,

    // Consumer Control
    0x05, 0x0C,
    0x09, 0x01,
    0xA1, 0x01,
    0x85, BLE_HID_RPT_ID_MEDIA,
    0x15, 0x00,
    0x26, 0xFF, 0x03,
    0x19, 0x00,
    0x2A, 0xFF, 0x03,
    0x75, 0x10,
    0x95, 0x01,
    0x81, 0x00,
    0xC0,

    // ADVCtl vendor control
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,
    0x85, BLE_HID_RPT_ID_MACCTL,
    0x09, 0x02,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, BLE_HID_MACCTL_REPORT_LEN,
    0x81, 0x02,
    0x09, 0x03,
    0x95, BLE_HID_MACCTL_REPORT_LEN,
    0x91, 0x02,
    0x09, 0x04,
    0x95, BLE_HID_MACCTL_REPORT_LEN,
    0xB1, 0x02,
    0xC0,
};

#if CONFIG_EXAMPLE_HID_DEVICE_ROLE && CONFIG_EXAMPLE_HID_DEVICE_ROLE == 3
// send the buttons, change in x, and change in y
void send_mouse(uint8_t buttons, char dx, char dy, char wheel)
{
    static uint8_t buffer[4] = {0};
    buffer[0]                = buttons;
    buffer[1]                = dx;
    buffer[2]                = dy;
    buffer[3]                = wheel;
    esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, BLE_HID_MAP_INDEX_MOUSE, BLE_HID_RPT_ID_MOUSE, buffer, 4);
}

void ble_hid_demo_task_mouse(void *pvParameters)
{
    static const char *help_string =
        "########################################################################\n"
        "BT hid mouse demo usage:\n"
        "You can input these value to simulate mouse: 'q', 'w', 'e', 'a', 's', 'd', 'h'\n"
        "q -- click the left key\n"
        "w -- move up\n"
        "e -- click the right key\n"
        "a -- move left\n"
        "s -- move down\n"
        "d -- move right\n"
        "h -- show the help\n"
        "########################################################################\n";
    printf("%s\n", help_string);
    char c;
    while (1) {
        c = fgetc(stdin);
        switch (c) {
            case 'q':
                send_mouse(1, 0, 0, 0);
                break;
            case 'w':
                send_mouse(0, 0, -10, 0);
                break;
            case 'e':
                send_mouse(2, 0, 0, 0);
                break;
            case 'a':
                send_mouse(0, -10, 0, 0);
                break;
            case 's':
                send_mouse(0, 0, 10, 0);
                break;
            case 'd':
                send_mouse(0, 10, 0, 0);
                break;
            case 'h':
                printf("%s\n", help_string);
                break;
            default:
                break;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
#endif

#if CONFIG_EXAMPLE_HID_DEVICE_ROLE && CONFIG_EXAMPLE_HID_DEVICE_ROLE == 2
#define CASE(a, b, c)  \
    case a:            \
        buffer[0] = b; \
        buffer[2] = c; \
        break;

// USB keyboard codes
#define USB_HID_MODIFIER_LEFT_CTRL   0x01
#define USB_HID_MODIFIER_LEFT_SHIFT  0x02
#define USB_HID_MODIFIER_LEFT_ALT    0x04
#define USB_HID_MODIFIER_RIGHT_CTRL  0x10
#define USB_HID_MODIFIER_RIGHT_SHIFT 0x20
#define USB_HID_MODIFIER_RIGHT_ALT   0x40

#define USB_HID_SPACE   0x2C
#define USB_HID_DOT     0x37
#define USB_HID_NEWLINE 0x28
#define USB_HID_FSLASH  0x38
#define USB_HID_BSLASH  0x31
#define USB_HID_COMMA   0x36
#define USB_HID_DOT     0x37

const unsigned char keyboardReportMap[] = {
    // 8 bytes input (modifiers, reserved, keys*6), 1 byte output
    0x05, 0x01,  // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report ID (1)
    0x05, 0x07,  //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,  //   Usage Minimum (0xE0)
    0x29, 0xE7,  //   Usage Maximum (0xE7)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x03,  //   Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x05,  //   Report Count (5)
    0x75, 0x01,  //   Report Size (1)
    0x05, 0x08,  //   Usage Page (LEDs)
    0x19, 0x01,  //   Usage Minimum (Num Lock)
    0x29, 0x05,  //   Usage Maximum (Kana)
    0x91, 0x02,  //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x03,  //   Report Size (3)
    0x91, 0x03,  //   Output (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x65,  //   Logical Maximum (101)
    0x05, 0x07,  //   Usage Page (Kbrd/Keypad)
    0x19, 0x00,  //   Usage Minimum (0x00)
    0x29, 0x65,  //   Usage Maximum (0x65)
    0x81, 0x00,  //   Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,        // End Collection
};

static void char_to_code(uint8_t *buffer, char ch)
{
    // Check if lower or upper case
    if (ch >= 'a' && ch <= 'z') {
        buffer[0] = 0;
        // convert ch to HID letter, starting at a = 4
        buffer[2] = (uint8_t)(4 + (ch - 'a'));
    } else if (ch >= 'A' && ch <= 'Z') {
        // Add left shift
        buffer[0] = USB_HID_MODIFIER_LEFT_SHIFT;
        // convert ch to lower case
        ch = ch - ('A' - 'a');
        // convert ch to HID letter, starting at a = 4
        buffer[2] = (uint8_t)(4 + (ch - 'a'));
    } else if (ch >= '0' && ch <= '9')  // Check if number
    {
        buffer[0] = 0;
        // convert ch to HID number, starting at 1 = 30, 0 = 39
        if (ch == '0') {
            buffer[2] = 39;
        } else {
            buffer[2] = (uint8_t)(30 + (ch - '1'));
        }
    } else  // not a letter nor a number
    {
        switch (ch) {
            CASE(' ', 0, USB_HID_SPACE);
            CASE('.', 0, USB_HID_DOT);
            CASE('\n', 0, USB_HID_NEWLINE);
            CASE('?', USB_HID_MODIFIER_LEFT_SHIFT, USB_HID_FSLASH);
            CASE('/', 0, USB_HID_FSLASH);
            CASE('\\', 0, USB_HID_BSLASH);
            CASE('|', USB_HID_MODIFIER_LEFT_SHIFT, USB_HID_BSLASH);
            CASE(',', 0, USB_HID_COMMA);
            CASE('<', USB_HID_MODIFIER_LEFT_SHIFT, USB_HID_COMMA);
            CASE('>', USB_HID_MODIFIER_LEFT_SHIFT, USB_HID_COMMA);
            CASE('@', USB_HID_MODIFIER_LEFT_SHIFT, 31);
            CASE('!', USB_HID_MODIFIER_LEFT_SHIFT, 30);
            CASE('#', USB_HID_MODIFIER_LEFT_SHIFT, 32);
            CASE('$', USB_HID_MODIFIER_LEFT_SHIFT, 33);
            CASE('%', USB_HID_MODIFIER_LEFT_SHIFT, 34);
            CASE('^', USB_HID_MODIFIER_LEFT_SHIFT, 35);
            CASE('&', USB_HID_MODIFIER_LEFT_SHIFT, 36);
            CASE('*', USB_HID_MODIFIER_LEFT_SHIFT, 37);
            CASE('(', USB_HID_MODIFIER_LEFT_SHIFT, 38);
            CASE(')', USB_HID_MODIFIER_LEFT_SHIFT, 39);
            CASE('-', 0, 0x2D);
            CASE('_', USB_HID_MODIFIER_LEFT_SHIFT, 0x2D);
            CASE('=', 0, 0x2E);
            CASE('+', USB_HID_MODIFIER_LEFT_SHIFT, 39);
            CASE(8, 0, 0x2A);  // backspace
            CASE('\t', 0, 0x2B);
            default:
                buffer[0] = 0;
                buffer[2] = 0;
        }
    }
}

void send_keyboard(char c)
{
    static uint8_t buffer[8] = {0};
    char_to_code(buffer, c);
    esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 1, buffer, 8);
    /* send the keyrelease event with sufficient delay */
    vTaskDelay(50 / portTICK_PERIOD_MS);
    memset(buffer, 0, sizeof(uint8_t) * 8);
    esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 1, buffer, 8);
}

void ble_hid_demo_task_kbd(void *pvParameters)
{
    static const char *help_string =
        "########################################################################\n"
        "BT hid keyboard demo usage:\n"
        "########################################################################\n";
    /* TODO : Add support for function keys and ctrl, alt, esc, etc. */
    printf("%s\n", help_string);
    // char c;
    while (1) {
        // c = fgetc(stdin);

        // if (c != 255) {
        //     send_keyboard(c);
        // }
        // vTaskDelay(10 / portTICK_PERIOD_MS);

        vTaskDelay(2000 / portTICK_PERIOD_MS);
        send_keyboard('f');
    }
}
#endif
static esp_hid_raw_report_map_t ble_report_maps[] = {
#if CONFIG_EXAMPLE_HID_DEVICE_ROLE == 2 && CONFIG_BT_NIMBLE_ENABLED
    {.data = compositeReportMap, .len = sizeof(compositeReportMap)},
#elif CONFIG_EXAMPLE_HID_DEVICE_ROLE == 2
    {.data = keyboardReportMap, .len = sizeof(keyboardReportMap)},
    {.data = bleMouseReportMap, .len = sizeof(bleMouseReportMap)},
    {.data = mediaReportMap, .len = sizeof(mediaReportMap)},
    {.data = macctlReportMap, .len = sizeof(macctlReportMap)},
#elif !CONFIG_BT_NIMBLE_ENABLED || CONFIG_EXAMPLE_HID_DEVICE_ROLE == 1
    /* This block is compiled for bluedroid as well */
    {.data = mediaReportMap, .len = sizeof(mediaReportMap)}
#elif CONFIG_EXAMPLE_HID_DEVICE_ROLE && CONFIG_EXAMPLE_HID_DEVICE_ROLE == 3
    {.data = bleMouseReportMap, .len = sizeof(bleMouseReportMap)},
#endif
};

static esp_hid_device_config_t ble_hid_config = {
    .vendor_id  = 0x16C0,
    .product_id = 0x05DF,
    .version    = 0x0105,
#if CONFIG_EXAMPLE_HID_DEVICE_ROLE == 2
    .device_name = "ADVCtl",
#elif CONFIG_EXAMPLE_HID_DEVICE_ROLE == 3
    .device_name = "ESP Mouse",
#else
    .device_name = "ESP BLE HID2",
#endif
    .manufacturer_name = "M5Stack",
    .serial_number     = "1234567890",
    .report_maps       = ble_report_maps,
    .report_maps_len   = sizeof(ble_report_maps) / sizeof(ble_report_maps[0])};

#define HID_CC_RPT_MUTE          1
#define HID_CC_RPT_POWER         2
#define HID_CC_RPT_LAST          3
#define HID_CC_RPT_ASSIGN_SEL    4
#define HID_CC_RPT_PLAY          5
#define HID_CC_RPT_PAUSE         6
#define HID_CC_RPT_RECORD        7
#define HID_CC_RPT_FAST_FWD      8
#define HID_CC_RPT_REWIND        9
#define HID_CC_RPT_SCAN_NEXT_TRK 10
#define HID_CC_RPT_SCAN_PREV_TRK 11
#define HID_CC_RPT_STOP          12

#define HID_CC_RPT_CHANNEL_UP   0x10
#define HID_CC_RPT_CHANNEL_DOWN 0x30
#define HID_CC_RPT_VOLUME_UP    0x40
#define HID_CC_RPT_VOLUME_DOWN  0x80

// HID Consumer Control report bitmasks
#define HID_CC_RPT_NUMERIC_BITS   0xF0
#define HID_CC_RPT_CHANNEL_BITS   0xCF
#define HID_CC_RPT_VOLUME_BITS    0x3F
#define HID_CC_RPT_BUTTON_BITS    0xF0
#define HID_CC_RPT_SELECTION_BITS 0xCF

// Macros for the HID Consumer Control 2-byte report
#define HID_CC_RPT_SET_NUMERIC(s, x)   \
    (s)[0] &= HID_CC_RPT_NUMERIC_BITS; \
    (s)[0] = (x)
#define HID_CC_RPT_SET_CHANNEL(s, x)   \
    (s)[0] &= HID_CC_RPT_CHANNEL_BITS; \
    (s)[0] |= ((x) & 0x03) << 4
#define HID_CC_RPT_SET_VOLUME_UP(s)   \
    (s)[0] &= HID_CC_RPT_VOLUME_BITS; \
    (s)[0] |= 0x40
#define HID_CC_RPT_SET_VOLUME_DOWN(s) \
    (s)[0] &= HID_CC_RPT_VOLUME_BITS; \
    (s)[0] |= 0x80
#define HID_CC_RPT_SET_BUTTON(s, x)   \
    (s)[1] &= HID_CC_RPT_BUTTON_BITS; \
    (s)[1] |= (x)
#define HID_CC_RPT_SET_SELECTION(s, x)   \
    (s)[1] &= HID_CC_RPT_SELECTION_BITS; \
    (s)[1] |= ((x) & 0x03) << 4

// HID Consumer Usage IDs (subset of the codes available in the USB HID Usage Tables spec)
#define HID_CONSUMER_POWER 48  // Power
#define HID_CONSUMER_RESET 49  // Reset
#define HID_CONSUMER_SLEEP 50  // Sleep

#define HID_CONSUMER_MENU         64   // Menu
#define HID_CONSUMER_SELECTION    128  // Selection
#define HID_CONSUMER_ASSIGN_SEL   129  // Assign Selection
#define HID_CONSUMER_MODE_STEP    130  // Mode Step
#define HID_CONSUMER_RECALL_LAST  131  // Recall Last
#define HID_CONSUMER_QUIT         148  // Quit
#define HID_CONSUMER_HELP         149  // Help
#define HID_CONSUMER_CHANNEL_UP   156  // Channel Increment
#define HID_CONSUMER_CHANNEL_DOWN 157  // Channel Decrement

#define HID_CONSUMER_PLAY          176  // Play
#define HID_CONSUMER_PAUSE         177  // Pause
#define HID_CONSUMER_RECORD        178  // Record
#define HID_CONSUMER_FAST_FORWARD  179  // Fast Forward
#define HID_CONSUMER_REWIND        180  // Rewind
#define HID_CONSUMER_SCAN_NEXT_TRK 181  // Scan Next Track
#define HID_CONSUMER_SCAN_PREV_TRK 182  // Scan Previous Track
#define HID_CONSUMER_STOP          183  // Stop
#define HID_CONSUMER_EJECT         184  // Eject
#define HID_CONSUMER_RANDOM_PLAY   185  // Random Play
#define HID_CONSUMER_SELECT_DISC   186  // Select Disk
#define HID_CONSUMER_ENTER_DISC    187  // Enter Disc
#define HID_CONSUMER_REPEAT        188  // Repeat
#define HID_CONSUMER_STOP_EJECT    204  // Stop/Eject
#define HID_CONSUMER_PLAY_PAUSE    205  // Play/Pause
#define HID_CONSUMER_PLAY_SKIP     206  // Play/Skip

#define HID_CONSUMER_VOLUME      224  // Volume
#define HID_CONSUMER_BALANCE     225  // Balance
#define HID_CONSUMER_MUTE        226  // Mute
#define HID_CONSUMER_BASS        227  // Bass
#define HID_CONSUMER_VOLUME_UP   233  // Volume Increment
#define HID_CONSUMER_VOLUME_DOWN 234  // Volume Decrement

#define HID_RPT_ID_CC_IN  3  // Consumer Control input report ID
#define HID_CC_IN_RPT_LEN 2  // Consumer Control input report Len
void esp_hidd_send_consumer_value(uint16_t key_cmd, bool key_pressed)
{
    uint8_t buffer[HID_CC_IN_RPT_LEN] = {0, 0};
    if (key_pressed) {
        buffer[0] = (uint8_t)(key_cmd & 0xFF);
        buffer[1] = (uint8_t)((key_cmd >> 8) & 0xFF);
    }
    ble_hid_device_helper_queue_report(BLE_HID_MAP_INDEX_MEDIA, HID_RPT_ID_CC_IN, buffer, HID_CC_IN_RPT_LEN,
                                       key_pressed ? 40 : 8);
    return;
}

#if !CONFIG_BT_NIMBLE_ENABLED || CONFIG_EXAMPLE_HID_DEVICE_ROLE == 1
void ble_hid_demo_task(void *pvParameters)
{
    static bool send_volum_up = false;
    while (1) {
        ESP_LOGI(TAG, "Send the volume");
        if (send_volum_up) {
            esp_hidd_send_consumer_value(HID_CONSUMER_VOLUME_UP, true);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            esp_hidd_send_consumer_value(HID_CONSUMER_VOLUME_UP, false);
        } else {
            esp_hidd_send_consumer_value(HID_CONSUMER_VOLUME_DOWN, true);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            esp_hidd_send_consumer_value(HID_CONSUMER_VOLUME_DOWN, false);
        }
        send_volum_up = !send_volum_up;
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}
#endif

void ble_hid_task_start_up(void)
{
    // ADVCtl sends reports from the application layer only.  The ESP-IDF demo
    // task emits periodic test reports after bonding, which interferes with
    // normal keyboard use and can overflow its small stack on this build.
    s_ble_hid_param.task_hdl = NULL;
}

void ble_hid_task_shut_down(void)
{
    if (s_ble_hid_param.task_hdl) {
        vTaskDelete(s_ble_hid_param.task_hdl);
        s_ble_hid_param.task_hdl = NULL;
    }
}

static void ble_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event       = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;
    static const char *TAG       = "HID_DEV_BLE";

    switch (event) {
        case ESP_HIDD_START_EVENT: {
            ESP_LOGI(TAG, "START");
            esp_hid_ble_gap_adv_start();
            break;
        }
        case ESP_HIDD_CONNECT_EVENT: {
            ESP_LOGI(TAG, "CONNECT");
            s_ble_hid_keyboard_state = BLE_HID_DEVICE_STATE_CONNECTED;
#if !CONFIG_BT_NIMBLE_ENABLED
            s_ble_hid_notify_ready     = true;
            s_ble_hid_ready_after_tick = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
#endif
            break;
        }
        case ESP_HIDD_PROTOCOL_MODE_EVENT: {
            ESP_LOGI(TAG, "PROTOCOL MODE[%u]: %s", param->protocol_mode.map_index,
                     param->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
            break;
        }
        case ESP_HIDD_CONTROL_EVENT: {
            ESP_LOGI(TAG, "CONTROL[%u]: %sSUSPEND", param->control.map_index, param->control.control ? "EXIT_" : "");
            if (param->control.control) {
                // exit suspend
                // ble_hid_task_start_up();  // No task needed, send it myself
            } else {
                // suspend
                // ble_hid_task_shut_down();  // No task needed, send it myself
            }
            break;
        }
        case ESP_HIDD_OUTPUT_EVENT: {
            ESP_LOGI(TAG, "OUTPUT[%u]: %8s ID: %2u, Len: %d, Data:", param->output.map_index,
                     esp_hid_usage_str(param->output.usage), param->output.report_id, param->output.length);
            ESP_LOG_BUFFER_HEX(TAG, param->output.data, param->output.length);
            if (param->output.report_id == BLE_HID_RPT_ID_KEYBOARD) {
                ble_hid_device_helper_handle_keyboard_output(param->output.data, param->output.length);
            }
            if (param->output.report_id == BLE_HID_RPT_ID_MACCTL && s_ble_hid_output_callback) {
                s_ble_hid_output_callback(param->output.data, param->output.length);
            }
            break;
        }
        case ESP_HIDD_FEATURE_EVENT: {
            ESP_LOGI(TAG, "FEATURE[%u]: %8s ID: %2u, Len: %d, Data:", param->feature.map_index,
                     esp_hid_usage_str(param->feature.usage), param->feature.report_id, param->feature.length);
            ESP_LOG_BUFFER_HEX(TAG, param->feature.data, param->feature.length);
            if (param->feature.report_id == BLE_HID_RPT_ID_MACCTL && s_ble_hid_output_callback) {
                s_ble_hid_output_callback(param->feature.data, param->feature.length);
            }
            break;
        }
        case ESP_HIDD_DISCONNECT_EVENT: {
            ESP_LOGI(TAG, "DISCONNECT: %s",
                     esp_hid_disconnect_reason_str(esp_hidd_dev_transport_get(param->disconnect.dev),
                                                   param->disconnect.reason));
            ble_hid_task_shut_down();
            esp_hid_ble_gap_adv_start();
            s_ble_hid_notify_ready     = false;
            s_ble_hid_ready_after_tick = 0;
            s_ble_hid_keyboard_state = BLE_HID_DEVICE_STATE_IDLE;
            break;
        }
        case ESP_HIDD_STOP_EVENT: {
            ESP_LOGI(TAG, "STOP");
            break;
        }
        default:
            break;
    }
    return;
}
#endif

#if CONFIG_BT_HID_DEVICE_ENABLED
static local_param_t s_bt_hid_param  = {0};
const unsigned char mouseReportMap[] = {
    0x05, 0x01,  // USAGE_PAGE (Generic Desktop)
    0x09, 0x02,  // USAGE (Mouse)
    0xa1, 0x01,  // COLLECTION (Application)

    0x09, 0x01,  //   USAGE (Pointer)
    0xa1, 0x00,  //   COLLECTION (Physical)

    0x05, 0x09,  //     USAGE_PAGE (Button)
    0x19, 0x01,  //     USAGE_MINIMUM (Button 1)
    0x29, 0x03,  //     USAGE_MAXIMUM (Button 3)
    0x15, 0x00,  //     LOGICAL_MINIMUM (0)
    0x25, 0x01,  //     LOGICAL_MAXIMUM (1)
    0x95, 0x03,  //     REPORT_COUNT (3)
    0x75, 0x01,  //     REPORT_SIZE (1)
    0x81, 0x02,  //     INPUT (Data,Var,Abs)
    0x95, 0x01,  //     REPORT_COUNT (1)
    0x75, 0x05,  //     REPORT_SIZE (5)
    0x81, 0x03,  //     INPUT (Cnst,Var,Abs)

    0x05, 0x01,  //     USAGE_PAGE (Generic Desktop)
    0x09, 0x30,  //     USAGE (X)
    0x09, 0x31,  //     USAGE (Y)
    0x09, 0x38,  //     USAGE (Wheel)
    0x15, 0x81,  //     LOGICAL_MINIMUM (-127)
    0x25, 0x7f,  //     LOGICAL_MAXIMUM (127)
    0x75, 0x08,  //     REPORT_SIZE (8)
    0x95, 0x03,  //     REPORT_COUNT (3)
    0x81, 0x06,  //     INPUT (Data,Var,Rel)

    0xc0,  //   END_COLLECTION
    0xc0   // END_COLLECTION
};

static esp_hid_raw_report_map_t bt_report_maps[] = {
    {.data = mouseReportMap, .len = sizeof(mouseReportMap)},
};

static esp_hid_device_config_t bt_hid_config = {.vendor_id         = 0x16C0,
                                                .product_id        = 0x05DF,
                                                .version           = 0x0100,
                                                .device_name       = "ESP BT HID1",
                                                .manufacturer_name = "Espressif",
                                                .serial_number     = "1234567890",
                                                .report_maps       = bt_report_maps,
                                                .report_maps_len   = 1};

// send the buttons, change in x, and change in y
void send_mouse(uint8_t buttons, char dx, char dy, char wheel)
{
    static uint8_t buffer[4] = {0};
    buffer[0]                = buttons;
    buffer[1]                = dx;
    buffer[2]                = dy;
    buffer[3]                = wheel;
    esp_hidd_dev_input_set(s_bt_hid_param.hid_dev, 0, 0, buffer, 4);
}

void bt_hid_demo_task(void *pvParameters)
{
    static const char *help_string =
        "########################################################################\n"
        "BT hid mouse demo usage:\n"
        "You can input these value to simulate mouse: 'q', 'w', 'e', 'a', 's', 'd', 'h'\n"
        "q -- click the left key\n"
        "w -- move up\n"
        "e -- click the right key\n"
        "a -- move left\n"
        "s -- move down\n"
        "d -- move right\n"
        "h -- show the help\n"
        "########################################################################\n";
    printf("%s\n", help_string);
    char c;
    while (1) {
        c = fgetc(stdin);
        switch (c) {
            case 'q':
                send_mouse(1, 0, 0, 0);
                break;
            case 'w':
                send_mouse(0, 0, -10, 0);
                break;
            case 'e':
                send_mouse(2, 0, 0, 0);
                break;
            case 'a':
                send_mouse(0, -10, 0, 0);
                break;
            case 's':
                send_mouse(0, 0, 10, 0);
                break;
            case 'd':
                send_mouse(0, 10, 0, 0);
                break;
            case 'h':
                printf("%s\n", help_string);
                break;
            default:
                break;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void bt_hid_task_start_up(void)
{
    xTaskCreate(bt_hid_demo_task, "bt_hid_demo_task", 2 * 1024, NULL, configMAX_PRIORITIES - 3,
                &s_bt_hid_param.task_hdl);
    return;
}

void bt_hid_task_shut_down(void)
{
    if (s_bt_hid_param.task_hdl) {
        vTaskDelete(s_bt_hid_param.task_hdl);
        s_bt_hid_param.task_hdl = NULL;
    }
}

static void bt_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event       = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;
    static const char *TAG       = "HID_DEV_BT";

    switch (event) {
        case ESP_HIDD_START_EVENT: {
            if (param->start.status == ESP_OK) {
                ESP_LOGI(TAG, "START OK");
                ESP_LOGI(TAG, "Setting to connectable, discoverable");
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            } else {
                ESP_LOGE(TAG, "START failed!");
            }
            break;
        }
        case ESP_HIDD_CONNECT_EVENT: {
            if (param->connect.status == ESP_OK) {
                ESP_LOGI(TAG, "CONNECT OK");
                ESP_LOGI(TAG, "Setting to non-connectable, non-discoverable");
                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
                bt_hid_task_start_up();
            } else {
                ESP_LOGE(TAG, "CONNECT failed!");
            }
            break;
        }
        case ESP_HIDD_PROTOCOL_MODE_EVENT: {
            ESP_LOGI(TAG, "PROTOCOL MODE[%u]: %s", param->protocol_mode.map_index,
                     param->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
            break;
        }
        case ESP_HIDD_OUTPUT_EVENT: {
            ESP_LOGI(TAG, "OUTPUT[%u]: %8s ID: %2u, Len: %d, Data:", param->output.map_index,
                     esp_hid_usage_str(param->output.usage), param->output.report_id, param->output.length);
            ESP_LOG_BUFFER_HEX(TAG, param->output.data, param->output.length);
            break;
        }
        case ESP_HIDD_FEATURE_EVENT: {
            ESP_LOGI(TAG, "FEATURE[%u]: %8s ID: %2u, Len: %d, Data:", param->feature.map_index,
                     esp_hid_usage_str(param->feature.usage), param->feature.report_id, param->feature.length);
            ESP_LOG_BUFFER_HEX(TAG, param->feature.data, param->feature.length);
            break;
        }
        case ESP_HIDD_DISCONNECT_EVENT: {
            if (param->disconnect.status == ESP_OK) {
                ESP_LOGI(TAG, "DISCONNECT OK");
                bt_hid_task_shut_down();
                ESP_LOGI(TAG, "Setting to connectable, discoverable again");
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            } else {
                ESP_LOGE(TAG, "DISCONNECT failed!");
            }
            break;
        }
        case ESP_HIDD_STOP_EVENT: {
            ESP_LOGI(TAG, "STOP");
            break;
        }
        default:
            break;
    }
    return;
}

#if CONFIG_BT_SDP_COMMON_ENABLED
static void esp_sdp_cb(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param)
{
    switch (event) {
        case ESP_SDP_INIT_EVT:
            ESP_LOGI(TAG, "ESP_SDP_INIT_EVT: status:%d", param->init.status);
            if (param->init.status == ESP_SDP_SUCCESS) {
                esp_bluetooth_sdp_dip_record_t dip_record = {
                    .hdr =
                        {
                            .type = ESP_SDP_TYPE_DIP_SERVER,
                        },
                    .vendor           = bt_hid_config.vendor_id,
                    .vendor_id_source = ESP_SDP_VENDOR_ID_SRC_BT,
                    .product          = bt_hid_config.product_id,
                    .version          = bt_hid_config.version,
                    .primary_record   = true,
                };
                esp_sdp_create_record((esp_bluetooth_sdp_record_t *)&dip_record);
            }
            break;
        case ESP_SDP_DEINIT_EVT:
            ESP_LOGI(TAG, "ESP_SDP_DEINIT_EVT: status:%d", param->deinit.status);
            break;
        case ESP_SDP_SEARCH_COMP_EVT:
            ESP_LOGI(TAG, "ESP_SDP_SEARCH_COMP_EVT: status:%d", param->search.status);
            break;
        case ESP_SDP_CREATE_RECORD_COMP_EVT:
            ESP_LOGI(TAG, "ESP_SDP_CREATE_RECORD_COMP_EVT: status:%d, handle:0x%x", param->create_record.status,
                     param->create_record.record_handle);
            break;
        case ESP_SDP_REMOVE_RECORD_COMP_EVT:
            ESP_LOGI(TAG, "ESP_SDP_REMOVE_RECORD_COMP_EVT: status:%d", param->remove_record.status);
            break;
        default:
            break;
    }
}
#endif /* CONFIG_BT_SDP_COMMON_ENABLED */

#endif

#if CONFIG_BT_NIMBLE_ENABLED
void ble_hid_device_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}
void ble_store_config_init(void);
#endif

#if CONFIG_BT_NIMBLE_ENABLED
static void ble_hid_device_helper_migrate_store(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("macctl_ble", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open BLE migration NVS failed: %s", esp_err_to_name(err));
        return;
    }

    uint32_t version = 0;
    err = nvs_get_u32(handle, "store_ver", &version);
    if (err == ESP_OK && version == MACCTL_BLE_STORE_SCHEMA_VERSION) {
        nvs_close(handle);
        return;
    }

    int rc = ble_store_clear();
    if (rc == 0) {
        ESP_LOGW(TAG, "cleared stale BLE bond store for schema v%" PRIu32 " -> v%d", version,
                 MACCTL_BLE_STORE_SCHEMA_VERSION);
        ESP_ERROR_CHECK(nvs_set_u32(handle, "store_ver", MACCTL_BLE_STORE_SCHEMA_VERSION));
        ESP_ERROR_CHECK(nvs_commit(handle));
    } else {
        ESP_LOGW(TAG, "BLE bond store clear failed: %d", rc);
    }

    nvs_close(handle);
}

#endif

bool _demo_app_main(void)
{
    esp_err_t ret;
#if HID_DEV_MODE == HIDD_IDLE_MODE
    ESP_LOGE(TAG, "Please turn on BT HID device or BLE!");
    return false;
#endif
    if (!s_ble_hid_stack_ready) {
        ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ret = nvs_flash_erase();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "nvs erase failed: %d", ret);
                return false;
            }
            ret = nvs_flash_init();
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs init failed: %d", ret);
            return false;
        }

        ESP_LOGI(TAG, "setting hid gap, mode:%d", HID_DEV_MODE);
        ret = esp_hid_gap_init(HID_DEV_MODE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "hid gap init failed: %d", ret);
            return false;
        }

#if CONFIG_BT_BLE_ENABLED || CONFIG_BT_NIMBLE_ENABLED
#if CONFIG_EXAMPLE_HID_DEVICE_ROLE == 2
        ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, ble_hid_config.device_name);
#elif CONFIG_EXAMPLE_HID_DEVICE_ROLE == 3
        ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_MOUSE, ble_hid_config.device_name);
#else
        ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_GENERIC, ble_hid_config.device_name);
#endif
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ble gap adv init failed: %d", ret);
            return false;
        }
#if CONFIG_BT_BLE_ENABLED
        if ((ret = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler)) != ESP_OK) {
            ESP_LOGE(TAG, "GATTS register callback failed: %d", ret);
            return false;
        }
#endif
#endif
        s_ble_hid_stack_ready = true;
    }

#if CONFIG_BT_BLE_ENABLED || CONFIG_BT_NIMBLE_ENABLED
    if (s_ble_hid_param.hid_dev) {
        ESP_LOGI(TAG, "ble hid device already initialized");
        return true;
    }
    ESP_LOGI(TAG, "setting ble device");
    ret = esp_hidd_dev_init(&ble_hid_config, ESP_HID_TRANSPORT_BLE, ble_hidd_event_callback, &s_ble_hid_param.hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ble hid device init failed: %d", ret);
        return false;
    }
#endif

#if CONFIG_BT_HID_DEVICE_ENABLED
    ESP_LOGI(TAG, "setting device name");
    esp_bt_gap_set_device_name(bt_hid_config.device_name);
    ESP_LOGI(TAG, "setting cod major, peripheral");
    esp_bt_cod_t cod = {0};
    cod.major        = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    cod.minor        = ESP_BT_COD_MINOR_PERIPHERAL_POINTING;
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "setting bt device");
    ret = esp_hidd_dev_init(&bt_hid_config, ESP_HID_TRANSPORT_BT, bt_hidd_event_callback, &s_bt_hid_param.hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bt hid device init failed: %d", ret);
        return false;
    }
#if CONFIG_BT_SDP_COMMON_ENABLED
    ret = esp_sdp_register_callback(esp_sdp_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdp callback init failed: %d", ret);
        return false;
    }
    ret = esp_sdp_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdp init failed: %d", ret);
        return false;
    }
#endif /* CONFIG_BT_SDP_COMMON_ENABLED */
#endif /* CONFIG_BT_HID_DEVICE_ENABLED */
#if CONFIG_BT_NIMBLE_ENABLED
    /* XXX Need to have template for store */
    ble_store_config_init();
    ble_hid_device_helper_migrate_store();

    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Starting nimble task after gatts is initialized*/
    ret = esp_nimble_enable(ble_hid_device_host_task);
    if (ret) {
        ESP_LOGE(TAG, "esp_nimble_enable failed: %d", ret);
        return false;
    }

    ESP_LOGI(TAG, "Setting battery level to 100%%");
    ble_svc_bas_battery_level_set(100);
#endif
    return true;
}

bool ble_hid_device_helper_init(void)
{
    ble_hid_device_helper_ensure_report_task();
    return _demo_app_main();
}

void ble_hid_device_helper_stop(void)
{
#if CONFIG_BT_BLE_ENABLED || CONFIG_BT_NIMBLE_ENABLED
    if (!s_ble_hid_param.hid_dev) {
        return;
    }
    ESP_LOGI(TAG, "stopping ble hid device");
    ble_hid_task_shut_down();
    esp_err_t ret = esp_hidd_dev_deinit(s_ble_hid_param.hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ble hid deinit failed: %d", ret);
        return;
    }
    s_ble_hid_param.hid_dev   = NULL;
    s_ble_hid_keyboard_state  = BLE_HID_DEVICE_STATE_IDLE;
    s_ble_hid_notify_ready    = false;
    s_ble_hid_ready_after_tick = 0;
#if CONFIG_BT_NIMBLE_ENABLED
    s_ble_hid_conn_handle      = BLE_HS_CONN_HANDLE_NONE;
#endif
    if (s_ble_hid_control_report_queue) {
        xQueueReset(s_ble_hid_control_report_queue);
    }
    if (s_ble_hid_best_effort_report_queue) {
        xQueueReset(s_ble_hid_best_effort_report_queue);
    }
#endif
}

bool ble_hid_device_helper_forget_bonds(void)
{
#if CONFIG_BT_NIMBLE_ENABLED
    if (!s_ble_hid_stack_ready) {
        ESP_LOGW(TAG, "cannot forget BLE bonds before stack init");
        return false;
    }

    ESP_LOGW(TAG, "forget BLE bonds and restart pairing");
    if (s_ble_hid_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        int term_rc = ble_gap_terminate(s_ble_hid_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (term_rc != 0) {
            ESP_LOGW(TAG, "terminate BLE connection failed: %d", term_rc);
        }
    }

    s_ble_hid_keyboard_state   = BLE_HID_DEVICE_STATE_IDLE;
    s_ble_hid_notify_ready     = false;
    s_ble_hid_ready_after_tick = 0;
    s_ble_hid_conn_handle      = BLE_HS_CONN_HANDLE_NONE;
    if (s_ble_hid_control_report_queue) {
        xQueueReset(s_ble_hid_control_report_queue);
    }
    if (s_ble_hid_best_effort_report_queue) {
        xQueueReset(s_ble_hid_best_effort_report_queue);
    }

    int rc = ble_store_clear();
    if (rc != 0) {
        ESP_LOGW(TAG, "clear BLE bond store failed: %d", rc);
        return false;
    }
    if (s_ble_hid_param.hid_dev && !ble_gap_adv_active()) {
        rc = esp_hid_ble_gap_adv_start();
        if (rc != 0) {
            ESP_LOGW(TAG, "restart BLE advertising failed: %d", rc);
            return false;
        }
    }
    return true;
#else
    ESP_LOGW(TAG, "forget BLE bonds is only implemented for NimBLE");
    return false;
#endif
}

void ble_hid_device_helper_send(uint8_t *buffer)
{
    ble_hid_device_helper_queue_report(BLE_HID_MAP_INDEX_KEYBOARD, BLE_HID_RPT_ID_KEYBOARD, buffer, 8, 8);
}

void ble_hid_device_helper_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    uint8_t buffer[4] = {buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};
    const bool movement_only = buttons == 0 && (dx != 0 || dy != 0 || wheel != 0);
    ble_hid_device_helper_queue_report(BLE_HID_MAP_INDEX_MOUSE, BLE_HID_RPT_ID_MOUSE, buffer, sizeof(buffer),
                                       movement_only ? 20 : 8);
}

void ble_hid_device_helper_send_consumer(uint16_t usage_id)
{
    esp_hidd_send_consumer_value(usage_id, true);
    vTaskDelay(40 / portTICK_PERIOD_MS);
    esp_hidd_send_consumer_value(0, false);
}

bool ble_hid_device_helper_send_macctl_volume_delta(int8_t delta)
{
    if (delta == 0) {
        return true;
    }

    return ble_hid_device_helper_queue_macctl_command(MACCTL_CMD_VOLUME_DELTA, delta);
}

bool ble_hid_device_helper_send_macctl_play_pause(void)
{
    return ble_hid_device_helper_queue_macctl_command(MACCTL_CMD_PLAY_PAUSE, 0);
}

bool ble_hid_device_helper_send_macctl_config(uint8_t flags, uint8_t sensitivity, uint8_t knob_mode)
{
    const uint8_t buffer[4] = {0x90, flags, sensitivity, knob_mode};
    return ble_hid_device_helper_queue_macctl_report(buffer, sizeof(buffer));
}

bool ble_hid_device_helper_send_macctl_power_config(uint8_t screen_timeout_s, uint8_t power_save_timeout_min)
{
    const uint8_t buffer[4] = {0x91, screen_timeout_s, power_save_timeout_min, 0};
    return ble_hid_device_helper_queue_macctl_report(buffer, sizeof(buffer));
}

bool ble_hid_device_helper_send_macctl_audio(uint8_t sequence, const uint8_t* data, uint8_t len)
{
    uint8_t buffer[BLE_HID_MACCTL_REPORT_LEN] = {0};
    buffer[0] = 0xA0;
    buffer[1] = sequence;
    if (data && len > 0) {
        const uint8_t payload_len = len > (BLE_HID_MACCTL_REPORT_LEN - 3) ? (BLE_HID_MACCTL_REPORT_LEN - 3) : len;
        buffer[2] = payload_len;
        memcpy(&buffer[3], data, payload_len);
    }
    return ble_hid_device_helper_queue_macctl_report(buffer, sizeof(buffer));
}

void ble_hid_device_helper_set_output_callback(ble_hid_device_helper_output_callback_t callback)
{
    s_ble_hid_output_callback = callback;
}

BleHidDeviceState_t ble_hid_device_helper_get_state(void)
{
    return s_ble_hid_keyboard_state;
}

bool ble_hid_device_helper_is_ready(void)
{
    return s_ble_hid_param.hid_dev && s_ble_hid_keyboard_state == BLE_HID_DEVICE_STATE_CONNECTED &&
           s_ble_hid_notify_ready && xTaskGetTickCount() >= s_ble_hid_ready_after_tick;
}

void ble_hid_device_helper_gap_connected(uint16_t conn_handle)
{
#if CONFIG_BT_NIMBLE_ENABLED
    s_ble_hid_conn_handle = conn_handle;
#else
    (void)conn_handle;
#endif
    if (s_ble_hid_notify_ready) {
        s_ble_hid_ready_after_tick = xTaskGetTickCount();
    } else {
        s_ble_hid_ready_after_tick = 0;
    }
}

void ble_hid_device_helper_gap_disconnected(uint16_t conn_handle)
{
#if CONFIG_BT_NIMBLE_ENABLED
    if (s_ble_hid_conn_handle == conn_handle) {
        s_ble_hid_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
#else
    (void)conn_handle;
#endif
    s_ble_hid_notify_ready     = false;
    s_ble_hid_ready_after_tick = 0;
}

void ble_hid_device_helper_gap_subscribe(uint16_t conn_handle, uint16_t attr_handle, bool notify_enabled)
{
    (void)conn_handle;
    (void)attr_handle;
    if (notify_enabled) {
        s_ble_hid_notify_ready     = true;
        s_ble_hid_ready_after_tick = xTaskGetTickCount() + pdMS_TO_TICKS(BLE_HID_READY_DELAY_MS);
        if (s_ble_hid_control_report_queue) {
            xQueueReset(s_ble_hid_control_report_queue);
        }
        if (s_ble_hid_best_effort_report_queue) {
            xQueueReset(s_ble_hid_best_effort_report_queue);
        }
        ESP_LOGI(TAG, "hid input notify ready");
    }
}
