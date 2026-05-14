/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <esp_http_server.h>
#include <cstdint>
#include <string>

/**
 * @brief
 *
 */
class AppSdcard : public mooncake::AppAbility {
public:
    AppSdcard();
    ~AppSdcard();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    uint32_t _time_count;
    httpd_handle_t _server = nullptr;
    std::string _ssid;
    std::string _ip;
    std::string _status;
    std::string _last_upload;
    size_t _last_upload_size = 0;
    bool _sd_mounted = false;
    bool _upload_ready = false;

    void probe_sd_card();
    void start_upload_service();
    void stop_upload_service();
    bool ensure_wifi();
    bool start_server();
    void render_upload_state();
    std::string get_wifi_ip() const;
    std::string build_rom_list_text() const;
    std::string safe_upload_path(const std::string& filename) const;
    bool is_supported_rom_name(const std::string& name) const;
    static std::string url_decode(const std::string& input);
    static std::string html_escape(const std::string& input);
    static esp_err_t handle_root(httpd_req_t* req);
    static esp_err_t handle_list(httpd_req_t* req);
    static esp_err_t handle_upload(httpd_req_t* req);
};
