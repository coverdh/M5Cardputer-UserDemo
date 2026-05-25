/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "hal_config.h"
#include <apps/utils/audio/audio.h>
#include <mooncake_log.h>
#include <M5Unified.hpp>
#include <esp_mac.h>
#include "utils/ble_hid_device/ble_hid_device_helper.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

static std::unique_ptr<Hal> _hal_instance;
static const std::string _tag = "HAL";
static constexpr uint32_t AIR_MOUSE_HOLD_MS = 450;
static constexpr uint32_t AIR_MOUSE_SAMPLE_MS = 8;
static constexpr uint16_t AIR_MOUSE_CALIBRATION_MIN_SAMPLES = 18;
static constexpr float AIR_MOUSE_CALIBRATION_MAX_GYRO_DPS = 25.0f;
static constexpr float AIR_MOUSE_CALIBRATION_ACCEL_TOLERANCE_G = 0.18f;
static constexpr float AIR_MOUSE_STILL_DEADZONE_DPS = 2.20f;
static constexpr float AIR_MOUSE_MOTION_DEADZONE_DPS = 0.85f;
static constexpr float AIR_MOUSE_STILL_FILTER_TAU_MS = 24.0f;
static constexpr float AIR_MOUSE_FAST_FILTER_TAU_MS = 6.0f;
static constexpr float AIR_MOUSE_FILTER_SETTLE_DPS = 0.20f;
static constexpr float AIR_MOUSE_PIXELS_PER_DEGREE = 11.0f;
static constexpr float AIR_MOUSE_ACCEL_START_DPS = 24.0f;
static constexpr float AIR_MOUSE_ACCEL_FULL_DPS = 180.0f;
static constexpr float AIR_MOUSE_ACCEL_MAX_BOOST = 1.80f;
static constexpr float AIR_MOUSE_ZERO_LOCK_DPS = 2.40f;
static constexpr float AIR_MOUSE_BIAS_UPDATE_MAX_GYRO_DPS = 3.20f;
static constexpr float AIR_MOUSE_BIAS_UPDATE_ALPHA = 0.012f;
static constexpr uint32_t AIR_MOUSE_AXIS_LOG_INTERVAL_MS = 500;
static constexpr int AIR_MOUSE_MAX_DELTA = 24;

Hal& GetHAL()
{
    if (!_hal_instance) {
        mclog::tagInfo(_tag, "creating hal instance");
        _hal_instance = std::make_unique<Hal>();
    }
    return *_hal_instance.get();
}

void Hal::init(bool gameboy_only_mode)
{
    mclog::tagInfo(_tag, "init{}", gameboy_only_mode ? " (gameboy only)" : "");

    M5.begin();
    M5.Display.setBrightness(0);
    M5.Speaker.begin();  // Codec takes some time to initialize

    display_init(!gameboy_only_mode);
    i2c_scan();
    keyboard_init();
    setting_init();
    externalInput.init();
    spi_init();
}

void Hal::update()
{
    M5.update();
    keyboard.update();
    update_air_mouse(millis());
    externalInput.update(millis());
    capLora868.update();
    if (_is_ble_keyboard_inited && !bleKeyboardIsConnected() &&
        millis() - _last_ble_advertising_ensure_ms > 3000) {
        _last_ble_advertising_ensure_ms = millis();
        ble_hid_device_helper_ensure_advertising();
    }
}

void Hal::feedTheDog()
{
    vTaskDelay(1);
}

std::vector<uint8_t> Hal::getDeviceMac()
{
    std::vector<uint8_t> mac(6);
    esp_read_mac(mac.data(), ESP_MAC_EFUSE_FACTORY);
    return mac;
}

std::string Hal::getDeviceMacString()
{
    auto mac = getDeviceMac();
    return fmt::format("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* -------------------------------------------------------------------------- */
/*                                  Dispplay                                  */
/* -------------------------------------------------------------------------- */
void Hal::display_init(bool create_ui_sprites)
{
    mclog::tagInfo(_tag, "display init");

    _ui_sprites_enabled = create_ui_sprites;
    if (!create_ui_sprites) {
        mclog::tagInfo(_tag, "skip ui sprites for dedicated GameBoy mode");
        return;
    }

    canvas.createSprite(204, 109);
    canvasKeyboardBar.createSprite(display.width() - canvas.width(), display.height());
    canvasSystemBar.createSprite(canvas.width(), display.height() - canvas.height());
}

void Hal::setFullscreenMode(bool fullscreen)
{
    if (_fullscreen_mode == fullscreen) {
        return;
    }
    _fullscreen_mode = fullscreen;
    mclog::tagInfo(_tag, "set fullscreen mode: {}", fullscreen ? "on" : "off");
}

void Hal::setDeviceBrightnessPercent(int percent)
{
    percent                     = std::max(1, std::min(100, percent));
    _display_brightness_percent = percent;
    display.setBrightness(static_cast<uint8_t>(percent * 255 / 100));
    if (_settings) {
        _settings->SetInt("bright", percent);
    }
}

/* -------------------------------------------------------------------------- */
/*                                     I2C                                    */
/* -------------------------------------------------------------------------- */
void Hal::i2c_scan()
{
    mclog::tagInfo(_tag, "i2c scan");

    bool ret[128] = {false};
    M5.In_I2C.scanID(ret);

    uint8_t address;
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
    for (int i = 0; i < 128; i += 16) {
        printf("%02x: ", i);
        for (int j = 0; j < 16; j++) {
            fflush(stdout);
            address = i + j;
            if (ret[address]) {
                printf("%02x ", address);
            } else {
                printf("-- ");
            }
        }
        printf("\r\n");
    }
}

/* -------------------------------------------------------------------------- */
/*                                  Settings                                  */
/* -------------------------------------------------------------------------- */
// https://github.com/78/xiaozhi-esp32/blob/main/main/main.cc

void Hal::setting_init()
{
    mclog::tagInfo(_tag, "setting init");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        mclog::tagWarn(_tag, "erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    _settings = new Settings("cardputer", true);
    setDeviceBrightnessPercent(_settings->GetInt("bright", 100));
    setDeviceVolumePercent(_settings->GetInt("device_volume", 35));
    externalInput.loadSettings(*_settings);
}

/* -------------------------------------------------------------------------- */
/*                                    Audio                                   */
/* -------------------------------------------------------------------------- */
void Hal::setDeviceVolumePercent(int percent)
{
    percent = std::max(0, std::min(100, percent));
    speaker.setVolume(static_cast<uint8_t>(percent * 255 / 100));
    if (_settings) {
        _settings->SetInt("device_volume", percent);
    }
}

int Hal::getDeviceVolumePercent() const
{
    return static_cast<int>(speaker.getVolume()) * 100 / 255;
}

/* -------------------------------------------------------------------------- */
/*                                  Keyboard                                  */
/* -------------------------------------------------------------------------- */
void Hal::keyboard_init()
{
    mclog::tagInfo(_tag, "keyboard init");

    if (!keyboard.init()) {
        mclog::tagError(_tag, "keyboard init failed");
        return;
    }
}

/* -------------------------------------------------------------------------- */
/*                                    WiFI                                    */
/* -------------------------------------------------------------------------- */
// https://github.com/espressif/esp-idf/blob/v5.3.3/examples/wifi/scan/main/scan.c
// https://github.com/espressif/esp-idf/blob/v5.4.2/examples/wifi/getting_started/station/main/station_example_main.c
// http://github.com/espressif/esp-idf/blob/v5.4.2/examples/protocols/sntp/main/sntp_example_main.c
#include <esp_wifi.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_event.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <vector>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>

#define DEFAULT_SCAN_LIST_SIZE 6
static wifi_ap_record_t _ap_info[DEFAULT_SCAN_LIST_SIZE];

void Hal::wifiScan(std::vector<ScanResult_t>& scanResult)
{
    mclog::tagInfo(_tag, "wifi scan");

    scanResult.clear();
    if (!_is_wifi_inited) {
        wifiInit();
    }
    if (!_is_wifi_inited) {
        mclog::tagError(_tag, "wifi scan skipped: wifi init failed");
        return;
    }

    uint16_t number   = DEFAULT_SCAN_LIST_SIZE;
    uint16_t ap_count = 0;
    memset(_ap_info, 0, sizeof(_ap_info));

    // Start WiFi scan
    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "failed to start wifi scan: {}", esp_err_to_name(ret));
        return;
    }

    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "failed to get AP number: {}", esp_err_to_name(ret));
        return;
    }

    ret = esp_wifi_scan_get_ap_records(&number, _ap_info);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "failed to get AP records: {}", esp_err_to_name(ret));
        return;
    }

    // Process scan results
    for (int i = 0; i < number; i++) {
        std::string ssid = (char*)_ap_info[i].ssid;
        int rssi         = _ap_info[i].rssi;

        // Skip empty SSID
        if (ssid.empty()) {
            continue;
        }

        // Add to ap_list
        scanResult.push_back(std::make_pair(rssi, ssid));
    }

    // Sort ap_list by RSSI (from strongest to weakest)
    std::sort(scanResult.begin(), scanResult.end(),
              [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) {
                  return a.first > b.first;  // Higher RSSI first
              });

    mclog::tagInfo(_tag, "wifi scan completed, found {} APs", scanResult.size());
}

static EventGroupHandle_t s_wifi_event_group = NULL;
static const int WIFI_CONNECTED_BIT          = BIT0;
static const int WIFI_DISCONNECTED_BIT       = BIT1;
static const int WIFI_FAIL_BIT               = BIT2;
static const int WIFI_STARTED_BIT            = BIT3;
static esp_netif_t* s_wifi_sta_netif         = nullptr;
static bool s_wifi_event_loop_ready          = false;
static bool s_wifi_handlers_registered       = false;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    const char* TAG = "wifi";

    // Wifi started
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_STARTED_BIT);
    }

    // Disconnected
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }

    // Connected
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void Hal::wifiInit()
{
    mclog::tagInfo(_tag, "wifi init");

    if (_is_wifi_inited) {
        return;
    }
    if (_wifi_init_failed) {
        mclog::tagWarn(_tag, "wifi init skipped: previous init failed");
        return;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NO_FREE_PAGES && ret != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        mclog::tagError(_tag, "nvs init failed: {}", esp_err_to_name(ret));
        return;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        mclog::tagError(_tag, "esp netif init failed: {}", esp_err_to_name(ret));
        return;
    }

    if (!s_wifi_event_loop_ready) {
        ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            mclog::tagError(_tag, "event loop init failed: {}", esp_err_to_name(ret));
            return;
        }
        s_wifi_event_loop_ready = true;
    }

    if (!s_wifi_sta_netif) {
        s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_wifi_sta_netif) {
            mclog::tagError(_tag, "failed to create wifi sta netif");
            return;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num  = 3;
    cfg.dynamic_rx_buf_num = 8;
    cfg.dynamic_tx_buf_num = 8;
    cfg.rx_mgmt_buf_num    = 3;
    cfg.cache_tx_buf_num   = 4;
    cfg.rx_ba_win          = 2;
    cfg.ampdu_rx_enable    = 0;
    cfg.ampdu_tx_enable    = 0;
    ret                    = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        mclog::tagError(_tag, "wifi driver init failed: {}", esp_err_to_name(ret));
        _wifi_init_failed = true;
        return;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "wifi set mode failed: {}", esp_err_to_name(ret));
        _wifi_init_failed = true;
        return;
    }

    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }

    if (!s_wifi_handlers_registered) {
        ESP_ERROR_CHECK(
            esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
        ESP_ERROR_CHECK(
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));
        s_wifi_handlers_registered = true;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        mclog::tagError(_tag, "wifi start failed: {}", esp_err_to_name(ret));
        _wifi_init_failed = true;
        return;
    }
    _is_wifi_inited   = true;
    _wifi_init_failed = false;
}

void Hal::wifiDeinit()
{
    mclog::tagInfo(_tag, "wifi deinit");

    if (!_is_wifi_inited) {
        return;
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    _is_wifi_inited = false;
}

bool Hal::wifiConnect(const std::string& ssid, const std::string& password)
{
    mclog::tagInfo(_tag, "wifi connect to ssid: {} password: {}", ssid, password);

    if (!_is_wifi_inited) {
        wifiInit();
    }

    wifiDisconnect();

    // Hold until wifi started
    xEventGroupWaitBits(s_wifi_event_group, WIFI_STARTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));

    // Reset event status
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_DISCONNECTED_BIT);

    // Set Wi-Fi config
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password));

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "wifi set config failed: {}", esp_err_to_name(ret));
        return false;
    }
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "wifi connect start failed: {}", esp_err_to_name(ret));
        return false;
    }

    // Wait for connection result
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(10000));

    if (bits & WIFI_CONNECTED_BIT) {
        mclog::tagInfo(_tag, "connected to SSID: {}", ssid);
        _is_wifi_connected = true;
        start_sntp();
        return true;
    } else if (bits & WIFI_FAIL_BIT) {
        mclog::tagError(_tag, "failed to connect to SSID: {}", ssid);
        return false;
    } else {
        mclog::tagError(_tag, "wifi connect timeout");
        return false;
    }
}

void Hal::wifiDisconnect()
{
    mclog::tagInfo(_tag, "wifi disconnect");

    if (!_is_wifi_inited) {
        return;
    }

    if (!_is_wifi_connected) {
        return;
    }

    // Hold until wifi started
    xEventGroupWaitBits(s_wifi_event_group, WIFI_STARTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));

    // Disconnect old connection
    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        mclog::tagError(_tag, "wifi disconnect failed: {}", esp_err_to_name(ret));
        return;
    }

    // Wait for disconnect result
    xEventGroupWaitBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));

    // Reset event status
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_DISCONNECTED_BIT);

    stop_sntp();

    _is_wifi_connected = false;
}

void Hal::start_sntp()
{
    mclog::tagInfo(_tag, "start sntp");

    if (!_is_wifi_connected) {
        mclog::tagError(_tag, "wifi not connected");
        return;
    }

    // Set timezone to UTC+8
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void Hal::stop_sntp()
{
    mclog::tagInfo(_tag, "stop sntp");

    if (!_is_wifi_connected) {
        mclog::tagError(_tag, "wifi not connected");
        return;
    }

    esp_sntp_stop();
}

/* -------------------------------------------------------------------------- */
/*                                   EspNow                                   */
/* -------------------------------------------------------------------------- */
// https://github.com/espressif/esp-now/blob/master/examples/get-started/main/app_main.c
#include <esp_mac.h>
#include <espnow.h>
#include <espnow_storage.h>
#include <espnow_utils.h>
#include <esp_check.h>

static std::string _espnow_received_data;
static esp_err_t _handle_espnow_received(uint8_t* src_addr, void* data, size_t size, wifi_pkt_rx_ctrl_t* rx_ctrl)
{
    const char* TAG = "espnow";

    ESP_PARAM_CHECK(src_addr);
    ESP_PARAM_CHECK(data);
    ESP_PARAM_CHECK(size);
    ESP_PARAM_CHECK(rx_ctrl);

    static uint32_t count = 0;

    ESP_LOGI(TAG, "espnow_recv, <%" PRIu32 "> [" MACSTR "][%d][%d][%u]: %.*s", count++, MAC2STR(src_addr),
             rx_ctrl->channel, rx_ctrl->rssi, size, size, (char*)data);

    _espnow_received_data = std::string((char*)data, size);

    return ESP_OK;
}

void Hal::espNowInit()
{
    mclog::tagInfo(_tag, "esp now init");

    if (!_is_wifi_inited) {
        wifiInit();
    }

    if (_is_wifi_connected) {
        wifiDisconnect();
        espNowDeinit();
    }

    if (_is_esp_now_inited) {
        mclog::tagInfo(_tag, "esp now already inited");
        return;
    }

    // espnow_storage_init();

    espnow_config_t espnow_config = ESPNOW_INIT_CONFIG_DEFAULT();
    espnow_init(&espnow_config);
    espnow_set_config_for_data_type(ESPNOW_DATA_TYPE_DATA, true, _handle_espnow_received);

    _is_esp_now_inited = true;
}

void Hal::espNowDeinit()
{
    mclog::tagInfo(_tag, "esp now deinit");

    if (!_is_esp_now_inited) {
        mclog::tagInfo(_tag, "esp now not inited");
        return;
    }

    espnow_deinit();

    _is_esp_now_inited = false;
}

void Hal::espNowSend(const std::string& data)
{
    mclog::tagInfo(_tag, "esp now send: {}", data);

    if (!_is_esp_now_inited) {
        mclog::tagError(_tag, "esp now not inited");
        return;
    }

    espnow_frame_head_t frame_head = ESPNOW_FRAME_CONFIG_DEFAULT();
    auto ret = espnow_send(ESPNOW_DATA_TYPE_DATA, ESPNOW_ADDR_BROADCAST, data.c_str(), data.size(), &frame_head,
                           portMAX_DELAY);

    if (ret != ESP_OK) {
        mclog::tagError(_tag, "failed to send esp now: {}", esp_err_to_name(ret));
    }
}

bool Hal::espNowAvailable()
{
    return _espnow_received_data.size() > 0;
}

const std::string& Hal::espNowGetReceivedData()
{
    return _espnow_received_data;
}

void Hal::espNowClearReceivedData()
{
    _espnow_received_data.clear();
}

/* -------------------------------------------------------------------------- */
/*                                     IR                                     */
/* -------------------------------------------------------------------------- */
#include "utils/ir_nec/ir_helper.h"

void Hal::irInit()
{
    mclog::tagInfo(_tag, "ir init");

    if (_is_ir_inited) {
        mclog::tagInfo(_tag, "ir already inited");
        return;
    }

    ir_helper_init((gpio_num_t)HAL_PIN_IR_TX);

    _is_ir_inited = true;
}

void Hal::irSend(uint8_t addr, uint8_t cmd)
{
    mclog::tagInfo(_tag, "ir send: addr: {:02X}, cmd: {:02X}", addr, cmd);

    if (!_is_ir_inited) {
        mclog::tagError(_tag, "ir not inited");
        return;
    }

    ir_helper_send(addr, cmd);
}

void Hal::irSendXiaomi(uint8_t device, uint8_t function, uint8_t repeats)
{
    mclog::tagInfo(_tag, "xiaomi ir send: device: {:02X}, function: {:02X}", device, function);

    if (!_is_ir_inited) {
        mclog::tagError(_tag, "ir not inited");
        return;
    }

    ir_helper_send_xiaomi(device, function, repeats);
}

void Hal::irSendRaw(uint32_t carrier_hz, const uint32_t* durations_us, size_t duration_count)
{
    mclog::tagInfo(_tag, "raw ir send: carrier: {}, durations: {}", carrier_hz, duration_count);

    if (!_is_ir_inited) {
        mclog::tagError(_tag, "ir not inited");
        return;
    }

    ir_helper_send_raw(carrier_hz, durations_us, duration_count);
}

/* -------------------------------------------------------------------------- */
/*                                     BLE                                    */
/* -------------------------------------------------------------------------- */
static bool s_advctl_control_ready = false;
static bool s_advctl_audio_test_pending = false;
static bool s_advctl_audio_test_active = false;
static bool s_advctl_time_synced = false;
static Hal::MacCtlNowPlayingState s_advctl_now_playing;
static constexpr time_t ADVCTL_TIME_SYNC_EPOCH = 1704067200;  // 2024-01-01 00:00:00 UTC

static void write_advctl_text_chunk(char* target, size_t target_len, uint8_t offset, uint8_t a, uint8_t b)
{
    if (!target || target_len == 0) {
        return;
    }
    if (offset == 0) {
        std::memset(target, 0, target_len);
    }
    const size_t pos = static_cast<size_t>(offset) * 2;
    if (pos < target_len - 1) {
        target[pos] = static_cast<char>(a);
    }
    if (pos + 1 < target_len - 1) {
        target[pos + 1] = static_cast<char>(b);
    }
    target[target_len - 1] = '\0';
}

static uint8_t advctl_config_flags()
{
    uint8_t flags = 0;
    if (GetHAL().externalInput.getSwapAxes()) flags |= 0x01;
    if (GetHAL().externalInput.getFlipX()) flags |= 0x02;
    if (GetHAL().externalInput.getFlipY()) flags |= 0x04;
    return flags;
}

static void send_advctl_config_report()
{
    const uint8_t knob_mode = static_cast<uint8_t>(
        std::max<int32_t>(0, std::min<int32_t>(2, GetHAL().getSettings().GetInt("adv_knob_mode", 0))));
    GetHAL().bleMacCtlConfig(advctl_config_flags(), GetHAL().externalInput.getJoystickSensitivity(), knob_mode);
}

static void send_advctl_power_report()
{
    const uint8_t screen_timeout_s = static_cast<uint8_t>(
        std::max<int32_t>(5, std::min<int32_t>(255, GetHAL().getSettings().GetInt("adv_scr_s", 30))));
    const uint8_t power_save_timeout_min = static_cast<uint8_t>(
        std::max<int32_t>(1, std::min<int32_t>(255, GetHAL().getSettings().GetInt("adv_pwr_m", 3))));
    GetHAL().bleMacCtlPowerConfig(screen_timeout_s, power_save_timeout_min);
}

static void handle_advctl_output_report(const uint8_t* data, uint8_t len)
{
    if (!data || len < 2) {
        return;
    }
    if (data[0] == 0x04 && len > 2) {
        data += 1;
        len -= 1;
    }

    s_advctl_control_ready = true;
    if (data[0] == 0x80) {
        send_advctl_config_report();
        send_advctl_power_report();
        return;
    }

    if (data[0] == 0x82) {
        s_advctl_audio_test_active = data[1] != 0;
        s_advctl_audio_test_pending = true;
        mclog::tagInfo("hal", "advctl audio test request: {}", s_advctl_audio_test_active ? "start" : "stop");
        return;
    }

    if (data[0] == 0x83) {
        GetHAL().externalInput.setDirectionTransform(false, false, false);
        GetHAL().externalInput.setJoystickSensitivity(ExternalInput::DEFAULT_JOYSTICK_SENSITIVITY);
        GetHAL().getSettings().SetInt("ptr_sens2", 2);
        GetHAL().getSettings().SetInt("adv_knob_mode", 0);
        send_advctl_config_report();
        return;
    }

    if (data[0] == 0x84 && len >= 4) {
        const uint32_t minutes_since_epoch = static_cast<uint32_t>(data[1]) |
                                            (static_cast<uint32_t>(data[2]) << 8) |
                                            (static_cast<uint32_t>(data[3]) << 16);
        const time_t now = ADVCTL_TIME_SYNC_EPOCH + static_cast<time_t>(minutes_since_epoch) * 60;
        timeval tv {};
        tv.tv_sec  = now;
        tv.tv_usec = 0;
        if (settimeofday(&tv, nullptr) == 0) {
            s_advctl_time_synced = true;
            mclog::tagInfo("hal", "advctl time synced: {}", static_cast<long long>(now));
        } else {
            mclog::tagWarn("hal", "advctl time sync failed");
        }
        return;
    }

    if (data[0] == 0x86 && len >= 3) {
        const int screen_timeout_s = std::max(5, std::min(255, static_cast<int>(data[1])));
        const int power_save_min   = std::max(1, std::min(255, static_cast<int>(data[2])));
        GetHAL().getSettings().SetInt("adv_scr_s", screen_timeout_s);
        GetHAL().getSettings().SetInt("adv_pwr_m", power_save_min);
        send_advctl_power_report();
        mclog::tagInfo("hal", "advctl power settings: screen={}s power={}m", screen_timeout_s, power_save_min);
        return;
    }

    if (data[0] == 0x87 && len >= 4) {
        s_advctl_now_playing.active = (data[1] & 0x01) != 0;
        s_advctl_now_playing.playing = (data[1] & 0x02) != 0;
        s_advctl_now_playing.muted = (data[1] & 0x04) != 0;
        s_advctl_now_playing.volumePercent = std::min<uint8_t>(100, data[2]);
        s_advctl_now_playing.progressPercent = std::min<uint8_t>(100, data[3]);
        s_advctl_now_playing.updatedMs = GetHAL().millis();
        if (!s_advctl_now_playing.active) {
            std::memset(s_advctl_now_playing.title, 0, sizeof(s_advctl_now_playing.title));
            std::memset(s_advctl_now_playing.artist, 0, sizeof(s_advctl_now_playing.artist));
        }
        return;
    }

    if (data[0] == 0x88 && len >= 4) {
        write_advctl_text_chunk(s_advctl_now_playing.title, sizeof(s_advctl_now_playing.title), data[1], data[2], data[3]);
        s_advctl_now_playing.updatedMs = GetHAL().millis();
        return;
    }

    if (data[0] == 0x89 && len >= 4) {
        write_advctl_text_chunk(s_advctl_now_playing.artist, sizeof(s_advctl_now_playing.artist), data[1], data[2], data[3]);
        s_advctl_now_playing.updatedMs = GetHAL().millis();
        return;
    }

    if (len < 3 || data[0] != 0x81) {
        return;
    }

    const uint8_t flags = data[1];
    GetHAL().externalInput.setDirectionTransform((flags & 0x02) != 0, (flags & 0x04) != 0, (flags & 0x01) != 0);
    if (len >= 3) {
        GetHAL().externalInput.setJoystickSensitivity(data[2]);
    }
    if (len >= 4) {
        GetHAL().getSettings().SetInt("adv_knob_mode", std::max(0, std::min(2, static_cast<int>(data[3]))));
    }
    send_advctl_config_report();
}

bool Hal::bleControlInit()
{
    if (_is_ble_keyboard_inited) {
        mclog::tagWarn(_tag, "ble hid already initialized");
        ble_hid_device_helper_set_output_callback(handle_advctl_output_report);
        ble_hid_device_helper_ensure_advertising();
        return true;
    }

    mclog::tagInfo(_tag, "ble control hid init");
    ble_hid_device_helper_set_output_callback(handle_advctl_output_report);
    if (!ble_hid_device_helper_init()) {
        mclog::tagWarn(_tag, "ble control hid init failed");
        return false;
    }
    _is_ble_keyboard_inited = true;
    return true;
}

void Hal::bleControlStop()
{
    if (!_is_ble_keyboard_inited) {
        return;
    }

    mclog::tagInfo(_tag, "ble control hid stop");
    ble_hid_device_helper_stop();
    _is_ble_keyboard_inited = false;
    s_advctl_control_ready  = false;
    s_advctl_time_synced    = false;
    s_advctl_now_playing    = {};
}

bool Hal::bleControlForgetBonds()
{
    mclog::tagWarn(_tag, "ble control forget bonds");
    if (!_is_ble_keyboard_inited && !bleControlInit()) {
        return false;
    }
    const bool ok = ble_hid_device_helper_forget_bonds();
    s_advctl_control_ready = false;
    s_advctl_time_synced   = false;
    return ok;
}

void Hal::bleKeyboardInit()
{
    mclog::tagInfo(_tag, "ble keyboard init");

    bleControlInit();

    if (_ble_keyboard_event_slot_id >= 0) {
        mclog::tagWarn(_tag, "ble keyboard forwarding already initialized");
        return;
    }

    // Register keyboard event callback to automatically forward keys
    _ble_keyboard_event_slot_id = keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& keyEvent) { handle_ble_keyboard_event(keyEvent); });

    mclog::tagInfo(_tag, "ble keyboard init done, auto-forwarding enabled");
}

bool Hal::bleKeyboardIsConnected() const
{
    if (!_is_ble_keyboard_inited) {
        return false;
    }

    return ble_hid_device_helper_get_state() == BLE_HID_DEVICE_STATE_CONNECTED;
}

bool Hal::bleAirMouseHandleKeyEvent(const Keyboard::KeyEvent_t& keyEvent)
{
    if (!ble_hid_device_helper_is_ready()) {
        return false;
    }

    return handle_air_mouse_space_event(keyEvent);
}

void Hal::bleKeyboardSendReport(uint8_t modifier, KeScanCode_t keyCode)
{
    if (!ble_hid_device_helper_is_ready()) {
        return;
    }

    uint8_t buffer[8] = {0};
    buffer[0]         = modifier;
    buffer[2]         = keyCode;
    ble_hid_device_helper_send(buffer);
}

void Hal::bleKeyboardTap(uint8_t modifier, KeScanCode_t keyCode)
{
    bleKeyboardSendReport(modifier, keyCode);
    delay(30);
    bleKeyboardSendReport(0, KEY_NONE);
}

void Hal::bleMouseReport(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (!ble_hid_device_helper_is_ready()) {
        return;
    }

    ble_hid_device_helper_send_mouse(buttons, dx, dy, wheel);
}

void Hal::bleMouseMove(int8_t dx, int8_t dy, int8_t wheel)
{
    bleMouseReport(0, dx, dy, wheel);
}

void Hal::bleMouseClick(uint8_t buttons)
{
    if (!ble_hid_device_helper_is_ready()) {
        return;
    }

    ble_hid_device_helper_send_mouse(buttons, 0, 0, 0);
    delay(30);
    ble_hid_device_helper_send_mouse(0, 0, 0, 0);
}

void Hal::bleConsumerSend(uint16_t usageId)
{
    if (!ble_hid_device_helper_is_ready()) {
        return;
    }

    ble_hid_device_helper_send_consumer(usageId);
}

bool Hal::bleMacSystemControlKey(const Keyboard::KeyEvent_t& keyEvent)
{
    if (!keyboard.isFnPressed()) {
        return false;
    }

    uint16_t usageId = 0;
    switch (keyEvent.keyCode) {
        case KEY_F1:
            usageId = BLE_HID_CONSUMER_BRIGHTNESS_DOWN;
            break;
        case KEY_F2:
            usageId = BLE_HID_CONSUMER_BRIGHTNESS_UP;
            break;
        case KEY_F3:
            if (keyEvent.state) {
                bleMacCtlSystemKey(BLE_HID_MACCTL_SYSTEM_CONTROL_CENTER);
            }
            return true;
        case KEY_F4:
            if (keyEvent.state) {
                bleMacCtlSystemKey(BLE_HID_MACCTL_SYSTEM_SPOTLIGHT);
            }
            return true;
        case KEY_F7:
            usageId = BLE_HID_CONSUMER_SCAN_PREVIOUS_TRACK;
            break;
        case KEY_F8:
            usageId = BLE_HID_CONSUMER_PLAY_PAUSE;
            break;
        case KEY_F9:
            usageId = BLE_HID_CONSUMER_SCAN_NEXT_TRACK;
            break;
        case KEY_F10:
            usageId = BLE_HID_CONSUMER_MUTE;
            break;
        default:
            return false;
    }

    if (keyEvent.state) {
        bleConsumerSend(usageId);
    }
    return true;
}

bool Hal::bleMacCtlVolumeDelta(int8_t delta)
{
    if (!ble_hid_device_helper_is_ready()) {
        return false;
    }
    if (!s_advctl_control_ready) {
        return false;
    }

    return ble_hid_device_helper_send_macctl_volume_delta(delta);
}

bool Hal::bleMacCtlPlayPause()
{
    if (!ble_hid_device_helper_is_ready()) {
        return false;
    }
    if (!s_advctl_control_ready) {
        return false;
    }

    return ble_hid_device_helper_send_macctl_play_pause();
}

bool Hal::bleMacCtlSystemKey(uint8_t key)
{
    if (!ble_hid_device_helper_is_ready()) {
        return false;
    }
    if (!s_advctl_control_ready) {
        return false;
    }

    return ble_hid_device_helper_send_macctl_system_key(key);
}

bool Hal::bleMacCtlConfig(uint8_t flags, uint8_t sensitivity, uint8_t knobMode)
{
    if (!ble_hid_device_helper_is_ready()) {
        return false;
    }

    return ble_hid_device_helper_send_macctl_config(flags, sensitivity, knobMode);
}

bool Hal::bleMacCtlPowerConfig(uint8_t screenTimeoutSeconds, uint8_t powerSaveTimeoutMinutes)
{
    if (!ble_hid_device_helper_is_ready()) {
        return false;
    }

    return ble_hid_device_helper_send_macctl_power_config(screenTimeoutSeconds, powerSaveTimeoutMinutes);
}

bool Hal::bleMacCtlIsConnected() const
{
    return ble_hid_device_helper_is_ready() && s_advctl_control_ready;
}

bool Hal::bleMacCtlTimeSynced() const
{
    return s_advctl_time_synced;
}

const Hal::MacCtlNowPlayingState& Hal::bleMacCtlNowPlaying() const
{
    return s_advctl_now_playing;
}

bool Hal::bleMacCtlAudioState(bool active)
{
    if (!ble_hid_device_helper_is_ready() || !s_advctl_control_ready) {
        return false;
    }
    return ble_hid_device_helper_send_macctl_audio_state(active);
}

bool Hal::bleMacCtlAudioFrame(uint8_t sequence, const uint8_t* data, uint8_t len)
{
    if (!ble_hid_device_helper_is_ready() || !s_advctl_control_ready) {
        return false;
    }
    return ble_hid_device_helper_send_macctl_audio(sequence, data, len);
}

bool Hal::bleConsumeAudioTestRequest(bool& active)
{
    ble_hid_device_helper_poll_output_reports();
    if (!s_advctl_audio_test_pending) {
        return false;
    }

    active = s_advctl_audio_test_active;
    s_advctl_audio_test_pending = false;
    return true;
}

static float apply_air_mouse_deadzone(float value, float deadzone)
{
    if (std::fabs(value) <= deadzone) {
        return 0.0f;
    }
    return value > 0.0f ? value - deadzone : value + deadzone;
}

static float air_mouse_deadzone_for_speed(float speed)
{
    const float t = std::clamp((speed - 3.0f) / 12.0f, 0.0f, 1.0f);
    return AIR_MOUSE_STILL_DEADZONE_DPS +
           t * (AIR_MOUSE_MOTION_DEADZONE_DPS - AIR_MOUSE_STILL_DEADZONE_DPS);
}

static float air_mouse_remote_axis_weight(const m5::imu_data_t& data)
{
    const float accel_mag = std::sqrt(data.accel.x * data.accel.x + data.accel.y * data.accel.y +
                                      data.accel.z * data.accel.z);
    if (accel_mag < 0.2f) {
        return 0.0f;
    }

    const float y = std::fabs(data.accel.y) / accel_mag;
    const float z = std::fabs(data.accel.z) / accel_mag;
    return std::clamp((y - z + 0.30f) / 0.60f, 0.0f, 1.0f);
}

static float air_mouse_smoothing_alpha(float dt, float speed)
{
    const float t = std::clamp((speed - 6.0f) / 120.0f, 0.0f, 1.0f);
    const float tau_ms = AIR_MOUSE_STILL_FILTER_TAU_MS +
                         t * (AIR_MOUSE_FAST_FILTER_TAU_MS - AIR_MOUSE_STILL_FILTER_TAU_MS);
    const float tau = tau_ms / 1000.0f;
    return std::clamp(dt / (tau + dt), 0.0f, 1.0f);
}

static float air_mouse_response_boost(float rate)
{
    const float speed = std::fabs(rate);
    const float range = AIR_MOUSE_ACCEL_FULL_DPS - AIR_MOUSE_ACCEL_START_DPS;
    const float t = std::clamp((speed - AIR_MOUSE_ACCEL_START_DPS) / range, 0.0f, 1.0f);
    return 1.0f + t * t * (AIR_MOUSE_ACCEL_MAX_BOOST - 1.0f);
}

static bool air_mouse_is_still_for_bias(const m5::imu_data_t& data)
{
    const float gyro_mag = std::sqrt(data.gyro.x * data.gyro.x + data.gyro.y * data.gyro.y +
                                     data.gyro.z * data.gyro.z);
    const float accel_mag = std::sqrt(data.accel.x * data.accel.x + data.accel.y * data.accel.y +
                                      data.accel.z * data.accel.z);
    return gyro_mag <= AIR_MOUSE_BIAS_UPDATE_MAX_GYRO_DPS &&
           std::fabs(accel_mag - 1.0f) <= AIR_MOUSE_CALIBRATION_ACCEL_TOLERANCE_G;
}

static char air_mouse_dominant_accel_axis(float ax, float ay, float az)
{
    ax = std::fabs(ax);
    ay = std::fabs(ay);
    az = std::fabs(az);
    if (ax >= ay && ax >= az) {
        return 'X';
    }
    if (ay >= ax && ay >= az) {
        return 'Y';
    }
    return 'Z';
}

static void air_mouse_update_bias(float& bias, float value, float alpha)
{
    bias += (value - bias) * alpha;
}

void Hal::reset_air_mouse_calibration()
{
    _air_mouse_pending_gyro_samples = 0;
    _air_mouse_pending_gyro_sum_x = 0.0f;
    _air_mouse_pending_gyro_sum_y = 0.0f;
    _air_mouse_pending_gyro_sum_z = 0.0f;
}

void Hal::reset_air_mouse_axis_log(uint32_t now)
{
    _air_mouse_last_axis_log_ms = now;
    _air_mouse_axis_log_samples = 0;
    _air_mouse_axis_accel_sum_x = 0.0f;
    _air_mouse_axis_accel_sum_y = 0.0f;
    _air_mouse_axis_accel_sum_z = 0.0f;
    _air_mouse_axis_gyro_sum_x = 0.0f;
    _air_mouse_axis_gyro_sum_y = 0.0f;
    _air_mouse_axis_gyro_sum_z = 0.0f;
    _air_mouse_axis_gyro_sq_sum_x = 0.0f;
    _air_mouse_axis_gyro_sq_sum_y = 0.0f;
    _air_mouse_axis_gyro_sq_sum_z = 0.0f;
}

void Hal::sample_air_mouse_axis_log(const m5::imu_data_t& data, uint32_t now)
{
    _air_mouse_axis_accel_sum_x += data.accel.x;
    _air_mouse_axis_accel_sum_y += data.accel.y;
    _air_mouse_axis_accel_sum_z += data.accel.z;
    _air_mouse_axis_gyro_sum_x += data.gyro.x;
    _air_mouse_axis_gyro_sum_y += data.gyro.y;
    _air_mouse_axis_gyro_sum_z += data.gyro.z;
    _air_mouse_axis_gyro_sq_sum_x += data.gyro.x * data.gyro.x;
    _air_mouse_axis_gyro_sq_sum_y += data.gyro.y * data.gyro.y;
    _air_mouse_axis_gyro_sq_sum_z += data.gyro.z * data.gyro.z;
    ++_air_mouse_axis_log_samples;

    if (now - _air_mouse_last_axis_log_ms < AIR_MOUSE_AXIS_LOG_INTERVAL_MS ||
        _air_mouse_axis_log_samples == 0) {
        return;
    }

    const float samples = static_cast<float>(_air_mouse_axis_log_samples);
    const float ax = _air_mouse_axis_accel_sum_x / samples;
    const float ay = _air_mouse_axis_accel_sum_y / samples;
    const float az = _air_mouse_axis_accel_sum_z / samples;
    const float gx = _air_mouse_axis_gyro_sum_x / samples;
    const float gy = _air_mouse_axis_gyro_sum_y / samples;
    const float gz = _air_mouse_axis_gyro_sum_z / samples;
    const float grx = std::sqrt(_air_mouse_axis_gyro_sq_sum_x / samples);
    const float gry = std::sqrt(_air_mouse_axis_gyro_sq_sum_y / samples);
    const float grz = std::sqrt(_air_mouse_axis_gyro_sq_sum_z / samples);
    const float accel_mag = std::sqrt(ax * ax + ay * ay + az * az);
    const float remote_weight = air_mouse_remote_axis_weight(data);
    mclog::tagInfo(_tag,
                   "air mouse imu axes: accel avg x={:.2f} y={:.2f} z={:.2f} |g|={:.2f} dom={} gyro avg x={:.2f} y={:.2f} z={:.2f} rms x={:.2f} y={:.2f} z={:.2f} x-axis mix y={:.0f}% z={:.0f}%",
                   ax, ay, az, accel_mag, air_mouse_dominant_accel_axis(ax, ay, az), gx, gy, gz, grx, gry, grz,
                   remote_weight * 100.0f, (1.0f - remote_weight) * 100.0f);
    reset_air_mouse_axis_log(now);
}

void Hal::sample_air_mouse_calibration(const m5::imu_data_t& data)
{
    const float gyro_mag = std::sqrt(data.gyro.x * data.gyro.x + data.gyro.y * data.gyro.y +
                                     data.gyro.z * data.gyro.z);
    const float accel_mag = std::sqrt(data.accel.x * data.accel.x + data.accel.y * data.accel.y +
                                      data.accel.z * data.accel.z);
    if (gyro_mag > AIR_MOUSE_CALIBRATION_MAX_GYRO_DPS ||
        std::fabs(accel_mag - 1.0f) > AIR_MOUSE_CALIBRATION_ACCEL_TOLERANCE_G) {
        return;
    }

    _air_mouse_pending_gyro_sum_x += data.gyro.x;
    _air_mouse_pending_gyro_sum_y += data.gyro.y;
    _air_mouse_pending_gyro_sum_z += data.gyro.z;
    ++_air_mouse_pending_gyro_samples;
}

bool Hal::handle_air_mouse_space_event(const Keyboard::KeyEvent_t& keyEvent)
{
    if (keyEvent.keyCode != KEY_SPACE) {
        return false;
    }

    const bool is_plain_space =
        keyEvent.extraModifiers == 0 && keyboard.getModifierMask() == 0 && !keyboard.isFnPressed();
    if (keyEvent.state) {
        if (!is_plain_space) {
            return false;
        }
        if (!_air_mouse_space_pending && !_air_mouse_active) {
            _air_mouse_space_pending = true;
            _air_mouse_space_pressed_ms = millis();
            _air_mouse_last_sample_ms = 0;
            reset_air_mouse_calibration();
            reset_air_mouse_axis_log(_air_mouse_space_pressed_ms);
            if (!_air_mouse_imu_ready) {
                imu.begin();
                _air_mouse_imu_ready = true;
            }
        }
        return true;
    }

    if (_air_mouse_active) {
        stop_air_mouse();
        return true;
    }
    if (_air_mouse_space_pending) {
        _air_mouse_space_pending = false;
        _air_mouse_space_pressed_ms = 0;
        bleKeyboardTap(0, KEY_SPACE);
        return true;
    }
    return false;
}

void Hal::start_air_mouse(uint32_t now)
{
    _air_mouse_space_pending = false;
    _air_mouse_active = true;
    _air_mouse_last_sample_ms = now;
    _air_mouse_accum_x = 0.0f;
    _air_mouse_accum_y = 0.0f;
    _air_mouse_filtered_x = 0.0f;
    _air_mouse_filtered_y = 0.0f;
    reset_air_mouse_axis_log(now);
    if (!_air_mouse_imu_ready) {
        imu.begin();
        _air_mouse_imu_ready = true;
    }
    if (_air_mouse_pending_gyro_samples >= AIR_MOUSE_CALIBRATION_MIN_SAMPLES) {
        const float samples = static_cast<float>(_air_mouse_pending_gyro_samples);
        _air_mouse_gyro_bias_x = _air_mouse_pending_gyro_sum_x / samples;
        _air_mouse_gyro_bias_y = _air_mouse_pending_gyro_sum_y / samples;
        _air_mouse_gyro_bias_z = _air_mouse_pending_gyro_sum_z / samples;
        _air_mouse_gyro_bias_ready = true;
        mclog::tagInfo(_tag, "air mouse gyro bias: x={:.2f} y={:.2f} z={:.2f}", _air_mouse_gyro_bias_x,
                       _air_mouse_gyro_bias_y, _air_mouse_gyro_bias_z);
    }
    reset_air_mouse_calibration();
    bleMouseMove(0, 0);
    mclog::tagInfo(_tag, "air mouse active");
}

void Hal::stop_air_mouse()
{
    _air_mouse_active = false;
    _air_mouse_space_pending = false;
    _air_mouse_space_pressed_ms = 0;
    _air_mouse_last_sample_ms = 0;
    _air_mouse_accum_x = 0.0f;
    _air_mouse_accum_y = 0.0f;
    _air_mouse_filtered_x = 0.0f;
    _air_mouse_filtered_y = 0.0f;
    reset_air_mouse_calibration();
    reset_air_mouse_axis_log(millis());
    bleMouseMove(0, 0);
    mclog::tagInfo(_tag, "air mouse inactive");
}

void Hal::update_air_mouse(uint32_t now)
{
    if (_air_mouse_space_pending && !_air_mouse_active && _air_mouse_imu_ready &&
        now - _air_mouse_last_sample_ms >= AIR_MOUSE_SAMPLE_MS) {
        _air_mouse_last_sample_ms = now;
        if (imu.update()) {
            const auto data = imu.getImuData();
            sample_air_mouse_calibration(data);
            sample_air_mouse_axis_log(data, now);
        }
    }

    if (_air_mouse_space_pending && !_air_mouse_active && now - _air_mouse_space_pressed_ms >= AIR_MOUSE_HOLD_MS) {
        start_air_mouse(now);
    }

    if (!_air_mouse_active || !_air_mouse_imu_ready || now - _air_mouse_last_sample_ms < AIR_MOUSE_SAMPLE_MS) {
        return;
    }

    const uint32_t elapsed_ms = now - _air_mouse_last_sample_ms;
    _air_mouse_last_sample_ms = now;
    const float dt = std::min(0.050f, std::max(0.001f, elapsed_ms / 1000.0f));
    if (!imu.update()) {
        return;
    }

    const auto data = imu.getImuData();
    sample_air_mouse_axis_log(data, now);
    if (!_air_mouse_gyro_bias_ready) {
        _air_mouse_gyro_bias_x = data.gyro.x;
        _air_mouse_gyro_bias_y = data.gyro.y;
        _air_mouse_gyro_bias_z = data.gyro.z;
        _air_mouse_gyro_bias_ready = true;
        _air_mouse_filtered_x = 0.0f;
        _air_mouse_filtered_y = 0.0f;
        _air_mouse_accum_x = 0.0f;
        _air_mouse_accum_y = 0.0f;
        return;
    }
    if (air_mouse_is_still_for_bias(data)) {
        air_mouse_update_bias(_air_mouse_gyro_bias_x, data.gyro.x, AIR_MOUSE_BIAS_UPDATE_ALPHA);
        air_mouse_update_bias(_air_mouse_gyro_bias_y, data.gyro.y, AIR_MOUSE_BIAS_UPDATE_ALPHA);
        air_mouse_update_bias(_air_mouse_gyro_bias_z, data.gyro.z, AIR_MOUSE_BIAS_UPDATE_ALPHA);
    }

    const float bias_x = _air_mouse_gyro_bias_ready ? _air_mouse_gyro_bias_x : 0.0f;
    const float bias_y = _air_mouse_gyro_bias_ready ? _air_mouse_gyro_bias_y : 0.0f;
    const float bias_z = _air_mouse_gyro_bias_ready ? _air_mouse_gyro_bias_z : 0.0f;
    const float remote_weight = air_mouse_remote_axis_weight(data);
    const float flat_yaw_rate = data.gyro.z - bias_z;
    const float remote_yaw_rate = data.gyro.y - bias_y;
    const float raw_yaw_rate = flat_yaw_rate * (1.0f - remote_weight) + remote_yaw_rate * remote_weight;
    const float raw_pitch_rate = data.gyro.x - bias_x;
    const float raw_speed = std::sqrt(raw_yaw_rate * raw_yaw_rate + raw_pitch_rate * raw_pitch_rate);
    if (raw_speed <= AIR_MOUSE_ZERO_LOCK_DPS) {
        air_mouse_update_bias(_air_mouse_gyro_bias_x, data.gyro.x, AIR_MOUSE_BIAS_UPDATE_ALPHA);
        air_mouse_update_bias(_air_mouse_gyro_bias_y, data.gyro.y, AIR_MOUSE_BIAS_UPDATE_ALPHA);
        air_mouse_update_bias(_air_mouse_gyro_bias_z, data.gyro.z, AIR_MOUSE_BIAS_UPDATE_ALPHA);
        _air_mouse_filtered_x = 0.0f;
        _air_mouse_filtered_y = 0.0f;
        _air_mouse_accum_x = 0.0f;
        _air_mouse_accum_y = 0.0f;
        return;
    }

    const float deadzone = air_mouse_deadzone_for_speed(raw_speed);
    const float yaw_rate = apply_air_mouse_deadzone(raw_yaw_rate, deadzone);
    const float pitch_rate = apply_air_mouse_deadzone(raw_pitch_rate, deadzone);
    const float motion_speed = std::sqrt(yaw_rate * yaw_rate + pitch_rate * pitch_rate);
    if (motion_speed == 0.0f) {
        _air_mouse_filtered_x = 0.0f;
        _air_mouse_filtered_y = 0.0f;
        _air_mouse_accum_x = 0.0f;
        _air_mouse_accum_y = 0.0f;
        return;
    }

    const float alpha = air_mouse_smoothing_alpha(dt, motion_speed);

    _air_mouse_filtered_x += (yaw_rate - _air_mouse_filtered_x) * alpha;
    _air_mouse_filtered_y += (pitch_rate - _air_mouse_filtered_y) * alpha;
    if (yaw_rate == 0.0f && std::fabs(_air_mouse_filtered_x) < AIR_MOUSE_FILTER_SETTLE_DPS) {
        _air_mouse_filtered_x = 0.0f;
    }
    if (pitch_rate == 0.0f && std::fabs(_air_mouse_filtered_y) < AIR_MOUSE_FILTER_SETTLE_DPS) {
        _air_mouse_filtered_y = 0.0f;
    }

    const float mouse_x_rate = _air_mouse_filtered_x * air_mouse_response_boost(_air_mouse_filtered_x);
    const float mouse_y_rate = _air_mouse_filtered_y * air_mouse_response_boost(_air_mouse_filtered_y);
    _air_mouse_accum_x += -mouse_x_rate * dt * AIR_MOUSE_PIXELS_PER_DEGREE;
    _air_mouse_accum_y += -mouse_y_rate * dt * AIR_MOUSE_PIXELS_PER_DEGREE;

    int dx = static_cast<int>(std::round(_air_mouse_accum_x));
    int dy = static_cast<int>(std::round(_air_mouse_accum_y));
    dx = std::clamp(dx, -AIR_MOUSE_MAX_DELTA, AIR_MOUSE_MAX_DELTA);
    dy = std::clamp(dy, -AIR_MOUSE_MAX_DELTA, AIR_MOUSE_MAX_DELTA);
    if (dx == 0 && dy == 0) {
        return;
    }

    _air_mouse_accum_x -= dx;
    _air_mouse_accum_y -= dy;
    bleMouseMove(static_cast<int8_t>(dx), static_cast<int8_t>(dy));
}

void Hal::handle_ble_keyboard_event(const Keyboard::KeyEvent_t& keyEvent)
{
    if (keyboard.isFnPressed() && keyEvent.state &&
        (keyEvent.keyCode == KEY_LEFTBRACE || keyEvent.keyCode == KEY_RIGHTBRACE)) {
        return;
    }

    // Only forward once the host subscribed to HID input notifications.
    if (!ble_hid_device_helper_is_ready()) {
        return;
    }

    if (bleMacSystemControlKey(keyEvent)) {
        return;
    }

    if (handle_air_mouse_space_event(keyEvent)) {
        return;
    }

    // Create HID buffer (8 bytes: modifier, reserved, keycode1-6)
    uint8_t buffer[8] = {0};

    // Handle key press/release
    if (keyEvent.state) {
        // Get current modifier state from keyboard
        uint8_t modifierMask = keyboard.getModifierMask();

        // Set modifier byte (physical modifiers + any firmware-injected ones, e.g. Fn+alpha -> LSHIFT)
        buffer[0] = modifierMask | keyEvent.extraModifiers;

        // For modifier keys themselves, don't set keycode
        if (keyEvent.isModifier) {
            buffer[2] = 0;  // No keycode for pure modifier keys
        } else {
            buffer[2] = keyEvent.keyCode;
        }

        // Send key press
        ble_hid_device_helper_send(buffer);
        mclog::tagDebug(_tag, "ble keyboard sent key: {} (code: {}, modifier: {:08b})",
                        keyEvent.keyName ? keyEvent.keyName : "special", (int)keyEvent.keyCode, modifierMask);
    } else {
        // Key released - always preserve current modifier state so held modifiers (e.g. ALT) stay active
        buffer[0] = keyboard.getModifierMask();
        buffer[2] = 0;
        ble_hid_device_helper_send(buffer);
        mclog::tagDebug(_tag, "ble keyboard key released (modifier: {:08b})", buffer[0]);
    }
}

/* -------------------------------------------------------------------------- */
/*                                     USB                                    */
/* -------------------------------------------------------------------------- */
// https://github.com/espressif/esp-idf/blob/v5.4.2/examples/peripherals/usb/device/tusb_hid
#include "utils/tusb_hid_device/tusb_hid_device_helper.h"

void Hal::usbKeyboardInit()
{
    if (_is_usb_keyboard_inited) {
        mclog::tagWarn(_tag, "usb keyboard already initialized");
        return;
    }

    mclog::tagInfo(_tag, "usb keyboard init");

    delay(200);

    tusb_hid_device_helper_init();

    _usb_keyboard_event_slot_id = keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& keyEvent) { handle_usb_keyboard_event(keyEvent); });

    _is_usb_keyboard_inited = true;
}

bool Hal::usbKeyboardIsConnected() const
{
    if (!_is_usb_keyboard_inited) {
        return false;
    }

    return tusb_hid_device_helper_is_mounted();
}

void Hal::handle_usb_keyboard_event(const Keyboard::KeyEvent_t& keyEvent)
{
    if (keyboard.isFnPressed() && keyEvent.state &&
        (keyEvent.keyCode == KEY_LEFTBRACE || keyEvent.keyCode == KEY_RIGHTBRACE)) {
        return;
    }

    // Only forward if USB keyboard is connected
    if (!usbKeyboardIsConnected()) {
        return;
    }

    // Handle key press/release
    if (keyEvent.state) {
        uint8_t mod = GetHAL().keyboard.getModifierMask() | keyEvent.extraModifiers;
        if (keyEvent.isModifier) {
            tusb_hid_device_helper_report(mod, NULL);
        } else {
            uint8_t keycode[6] = {keyEvent.keyCode};
            tusb_hid_device_helper_report(mod, keycode);
        }
        mclog::tagDebug(_tag, "usb keyboard sent key: {} (code: {}, modifier: {:08b})",
                        keyEvent.keyName ? keyEvent.keyName : "special", (int)keyEvent.keyCode, mod);
    } else {
        tusb_hid_device_helper_report(GetHAL().keyboard.getModifierMask(), NULL);
        mclog::tagDebug(_tag, "usb keyboard key released");
    }
}

/* -------------------------------------------------------------------------- */
/*                                     SPI                                    */
/* -------------------------------------------------------------------------- */
#include <driver/spi_master.h>
#include <driver/sdspi_host.h>
#include <driver/sdmmc_host.h>

static bool _spi_bus_initialized = false;

void Hal::spi_init()
{
    mclog::tagInfo(_tag, "spi init");

    esp_err_t ret;

    // spi_host_device_t host_id = SPI2_HOST;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = HAL_PIN_SPI_MOSI,
        .miso_io_num     = HAL_PIN_SPI_MISO,
        .sclk_io_num     = HAL_PIN_SPI_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };

    // Initialize SPI bus only if not already initialized
    if (!_spi_bus_initialized) {
        ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK) {
            mclog::tagError(_tag, "failed to initialize SPI bus");
            return;
        }
        _spi_bus_initialized = true;
        mclog::tagInfo(_tag, "spi bus initialized");
    } else {
        mclog::tagWarn(_tag, "spi bus already initialized, reusing");
    }
}

/* -------------------------------------------------------------------------- */
/*                                   SD Card                                  */
/* -------------------------------------------------------------------------- */
// https://github.com/espressif/esp-idf/blob/v5.3.3/examples/storage/sd_card/sdspi
// https://github.com/m5stack/M5PaperS3-UserDemo/blob/main/main/hal/hal.h
#include <driver/spi_master.h>
#include <driver/sdspi_host.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include <esp_vfs_fat.h>

#define MOUNT_POINT "/sdcard"

static sdmmc_card_t* _sd_card = nullptr;

void Hal::sd_card_init()
{
    mclog::tagInfo(_tag, "sd card init");

    if (!_spi_bus_initialized) {
        spi_init();
    }

    // If already mounted successfully, return
    if (_is_sd_card_mounted) {
        mclog::tagInfo(_tag, "sd card already mounted");
        return;
    }

    esp_err_t ret;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 40000;

    // Options for mounting the filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 16 * 1024};

    const char mount_point[] = MOUNT_POINT;
    mclog::tagInfo(_tag, "initializing SD card");

    // Initialize SD card slot
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs               = HAL_PIN_SD_CARD_CS;
    slot_config.host_id               = (spi_host_device_t)host.slot;

    mclog::tagInfo(_tag, "mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &_sd_card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            mclog::tagError(_tag, "failed to mount filesystem");
        } else {
            mclog::tagError(_tag, "failed to initialize the card, make sure SD card lines have pull-up resistors");
        }

        // Don't clean up SPI bus on failure - leave it for retry
        mclog::tagInfo(_tag, "sd card init failed, but spi bus remains initialized for retry");
        return;
    }

    mclog::tagInfo(_tag, "filesystem mounted successfully");

    sdmmc_card_print_info(stdout, _sd_card);

    _is_sd_card_mounted = true;
}

void Hal::sdCardUnmount()
{
    if (!_is_sd_card_mounted || !_sd_card) {
        return;
    }

    mclog::tagInfo(_tag, "sd card unmount");
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, _sd_card);
    _sd_card            = nullptr;
    _is_sd_card_mounted = false;
}

Hal::SdCardProbeResult_t Hal::sdCardProbe()
{
    SdCardProbeResult_t result;

    if (!_is_sd_card_mounted) {
        for (int attempt = 0; attempt < 2 && !_is_sd_card_mounted; ++attempt) {
            sd_card_init();
            if (!_is_sd_card_mounted) {
                mclog::tagWarn(_tag, "sd card probe retry {}", attempt + 1);
                delay(80);
            }
        }
        if (!_is_sd_card_mounted) {
            result.is_mounted = false;
            result.size       = "Not Found";
            return result;
        }
    }

    result.is_mounted = true;

    // Try write to sd card
    FILE* fp = fopen(MOUNT_POINT "/test.txt", "w");
    if (fp) {
        fwrite("Hello, World!", 1, 13, fp);
        fclose(fp);

        result.size =
            fmt::format("Size: {:.1f} GB",
                        ((float)((uint64_t)_sd_card->csd.capacity) * _sd_card->csd.sector_size) / (1024 * 1024 * 1024));
    } else {
        result.size = "Write Failed";
    }

    result.type = "Type: ";
    if (_sd_card->is_sdio) {
        result.type += "SDIO";
    } else if (_sd_card->is_mmc) {
        result.type += "MMC";
    } else {
        result.type += (_sd_card->ocr & (1 << 30)) ? "SDHC/SDXC" : "SDSC";
    }

    result.name = fmt::format("Name: {}", std::string(_sd_card->cid.name));

    return result;
}
