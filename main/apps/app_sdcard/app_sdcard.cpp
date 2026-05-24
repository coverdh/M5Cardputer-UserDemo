/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_sdcard.h"
#include "assets/tf_big.h"
#include "assets/tf_small.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <assets.h>
#include <hal.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <lwip/inet.h>
#include <sys/stat.h>

using namespace mooncake;

namespace {
constexpr const char* ROM_DIR = "/sdcard/roms";
constexpr size_t UPLOAD_BUFFER_SIZE = 4096;

bool is_hex(char value)
{
    return std::isxdigit(static_cast<unsigned char>(value)) != 0;
}

int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return 0;
}

void send_text(httpd_req_t* req, const char* type, const std::string& text)
{
    httpd_resp_set_type(req, type);
    httpd_resp_send(req, text.c_str(), text.size());
}
}  // namespace

AppSdcard::AppSdcard()
{
    setAppInfo().name     = "SDCard";
    setAppInfo().userData = new AppIcon_t(image_data_tf_big, image_data_tf_small);
}

AppSdcard::~AppSdcard()
{
    // delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppSdcard::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    // Clear screen
    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setFont(FONT_REPL);
    GetHAL().canvas.setTextScroll(true);
    GetHAL().canvas.setTextSize(1);
    GetHAL().canvas.setCursor(0, 0);

    probe_sd_card();
    start_upload_service();
    _time_count = GetHAL().millis();
}

void AppSdcard::onRunning()
{
    if (GetHAL().millis() - _time_count > 3000) {
        render_upload_state();
        _time_count = GetHAL().millis();
    }

    // Close app when home button clicked
    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
    }
}

void AppSdcard::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    stop_upload_service();
}

void AppSdcard::probe_sd_card()
{
    mclog::tagInfo(getAppInfo().name, "probe sd card");

    auto result = GetHAL().sdCardProbe();
    // mclog::tagDebug(getAppInfo().name, "sd card probe result:\n  is_mounted: {}\n  name: {}\n  size: {}\n  type: {}",
    //                 result.is_mounted, result.name, result.size, result.type);

    GetHAL().canvas.fillScreen(THEME_COLOR_BG);
    GetHAL().canvas.setCursor(0, 0);
    GetHAL().canvas.setTextColor(TFT_ORANGE);
    GetHAL().canvas.println("SD Card Info:");

    GetHAL().canvas.setCursor(0, 24);
    if (result.is_mounted) {
        _sd_mounted = true;
        GetHAL().canvas.setTextColor(TFT_CYAN);
        GetHAL().canvas.println(result.name.c_str());
        GetHAL().canvas.println(result.size.c_str());
        GetHAL().canvas.println(result.type.c_str());
    } else {
        _sd_mounted = false;
        GetHAL().canvas.setTextColor(TFT_RED);
        GetHAL().canvas.println("SD Card Not Found");
    }
    GetHAL().pushCanvas();
}

void AppSdcard::start_upload_service()
{
    if (!_sd_mounted) {
        _status = "Insert SD card";
        render_upload_state();
        return;
    }

    mkdir(ROM_DIR, 0775);

    _status = "Connecting WiFi";
    render_upload_state();
    if (!ensure_wifi()) {
        _upload_ready = false;
        render_upload_state();
        return;
    }

    _status = "Starting upload";
    render_upload_state();
    _upload_ready = start_server();
    _status = _upload_ready ? "Upload ready" : "Upload failed";
    render_upload_state();
}

void AppSdcard::stop_upload_service()
{
    if (!_server) {
        return;
    }
    httpd_stop(_server);
    _server = nullptr;
    _upload_ready = false;
}

bool AppSdcard::ensure_wifi()
{
    _ssid = GetHAL().getSettings().GetString("wifi_ssid", "");
    const auto password = GetHAL().getSettings().GetString("wifi_password", "");
    if (_ssid.empty()) {
        _status = "Run SetWiFi first";
        return false;
    }

    GetHAL().wifiInit();
    if (!GetHAL().isWifiConnected() && !GetHAL().wifiConnect(_ssid, password)) {
        _status = "WiFi failed";
        return false;
    }

    _ip = get_wifi_ip();
    if (_ip.empty() || _ip == "0.0.0.0") {
        _status = "No IP";
        return false;
    }
    return true;
}

bool AppSdcard::start_server()
{
    if (_server) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.stack_size = 4096;
    config.max_open_sockets = 3;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&_server, &config);
    if (err == ESP_ERR_HTTPD_TASK) {
        config.stack_size = 3072;
        mclog::tagWarn(getAppInfo().name,
                       "httpd_start task allocation failed, retry stack={}",
                       static_cast<unsigned>(config.stack_size));
        err = httpd_start(&_server, &config);
    }
    if (err != ESP_OK) {
        mclog::tagError(getAppInfo().name, "httpd_start failed: {}", esp_err_to_name(err));
        _server = nullptr;
        return false;
    }

    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = &AppSdcard::handle_root;
    root.user_ctx = this;
    httpd_register_uri_handler(_server, &root);

    httpd_uri_t list = {};
    list.uri = "/list";
    list.method = HTTP_GET;
    list.handler = &AppSdcard::handle_list;
    list.user_ctx = this;
    httpd_register_uri_handler(_server, &list);

    httpd_uri_t upload = {};
    upload.uri = "/upload";
    upload.method = HTTP_POST;
    upload.handler = &AppSdcard::handle_upload;
    upload.user_ctx = this;
    httpd_register_uri_handler(_server, &upload);

    mclog::tagInfo(getAppInfo().name, "upload server started: http://{}/", _ip);
    return true;
}

void AppSdcard::render_upload_state()
{
    GetHAL().canvas.fillScreen(THEME_COLOR_BG);
    GetHAL().canvas.setCursor(0, 0);
    GetHAL().canvas.setTextSize(1);
    GetHAL().canvas.setTextScroll(false);
    GetHAL().canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    GetHAL().canvas.println("SD Card");

    GetHAL().canvas.setCursor(0, 18);
    GetHAL().canvas.setTextColor(_upload_ready ? TFT_GREEN : TFT_CYAN, THEME_COLOR_BG);
    GetHAL().canvas.println(_status.c_str());

    GetHAL().canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    GetHAL().canvas.setCursor(0, 38);
    GetHAL().canvas.print("WiFi: ");
    GetHAL().canvas.println(_ssid.empty() ? "not set" : _ssid.c_str());

    GetHAL().canvas.setCursor(0, 54);
    if (!_ip.empty()) {
        GetHAL().canvas.print("http://");
        GetHAL().canvas.print(_ip.c_str());
        GetHAL().canvas.println("/");
    } else {
        GetHAL().canvas.println("Upload: offline");
    }

    GetHAL().canvas.setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
    GetHAL().canvas.setCursor(0, 78);
    GetHAL().canvas.println("Upload .gb .gbc .gba");
    GetHAL().canvas.println("Saved to /sdcard/roms");

    if (!_last_upload.empty()) {
        auto name = _last_upload;
        if (name.size() > 22) {
            name = name.substr(0, 19) + "...";
        }
        GetHAL().canvas.setTextColor(TFT_GREEN, THEME_COLOR_BG);
        GetHAL().canvas.setCursor(0, 104);
        GetHAL().canvas.print("Last: ");
        GetHAL().canvas.println(name.c_str());
    }

    GetHAL().pushCanvas();
}

std::string AppSdcard::get_wifi_ip() const
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return {};
    }

    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return {};
    }

    char ip[IP4ADDR_STRLEN_MAX] = {};
    esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
    return ip;
}

std::string AppSdcard::build_rom_list_text() const
{
    DIR* dir = opendir(ROM_DIR);
    if (!dir) {
        return "Cannot open /sdcard/roms\n";
    }

    std::string list;
    while (auto* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (!is_supported_rom_name(name)) {
            continue;
        }
        list += name;
        list += "\n";
    }
    closedir(dir);
    return list.empty() ? "No ROM files\n" : list;
}

std::string AppSdcard::safe_upload_path(const std::string& filename) const
{
    if (filename.empty() || filename.size() > 180 || filename[0] == '.' || filename[0] == '_') {
        return {};
    }
    for (char ch : filename) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '\0') {
            return {};
        }
    }
    if (!is_supported_rom_name(filename)) {
        return {};
    }
    return std::string(ROM_DIR) + "/" + filename;
}

bool AppSdcard::is_supported_rom_name(const std::string& name) const
{
    if (name.size() < 4 || name[0] == '.' || name[0] == '_') {
        return false;
    }

    auto lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    auto has_suffix = [&lower](const char* suffix) {
        const auto suffix_len = std::strlen(suffix);
        return lower.size() >= suffix_len && lower.compare(lower.size() - suffix_len, suffix_len, suffix) == 0;
    };
    return has_suffix(".gb") || has_suffix(".gbc") || has_suffix(".gba");
}

std::string AppSdcard::url_decode(const std::string& input)
{
    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size() && is_hex(input[i + 1]) && is_hex(input[i + 2])) {
            output.push_back(static_cast<char>((hex_value(input[i + 1]) << 4) | hex_value(input[i + 2])));
            i += 2;
        } else if (input[i] == '+') {
            output.push_back(' ');
        } else {
            output.push_back(input[i]);
        }
    }
    return output;
}

std::string AppSdcard::html_escape(const std::string& input)
{
    std::string output;
    output.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
            case '&':
                output += "&amp;";
                break;
            case '<':
                output += "&lt;";
                break;
            case '>':
                output += "&gt;";
                break;
            case '"':
                output += "&quot;";
                break;
            default:
                output.push_back(ch);
                break;
        }
    }
    return output;
}

esp_err_t AppSdcard::handle_root(httpd_req_t* req)
{
    auto* app = static_cast<AppSdcard*>(req->user_ctx);
    const auto roms = html_escape(app->build_rom_list_text());
    const std::string html =
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Cardputer SDCard</title>"
        "<style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:24px;max-width:720px}"
        "button,input{font:inherit}button{padding:10px 14px}pre{background:#f3f3f3;padding:12px;white-space:pre-wrap}"
        "#bar{height:8px;background:#ddd;margin:12px 0}#fill{height:100%;width:0;background:#0a7}</style>"
        "<h1>Cardputer SDCard</h1>"
        "<p>Upload .gb, .gbc or .gba to /sdcard/roms.</p>"
        "<input id=file type=file accept='.gb,.gbc,.gba'><button onclick='up()'>Upload</button>"
        "<div id=bar><div id=fill></div></div><p id=msg></p><h2>ROMs</h2><pre id=list>" +
        roms +
        "</pre><script>"
        "async function up(){const f=document.getElementById('file').files[0];if(!f){msg.textContent='Choose a file first';return;}"
        "const x=new XMLHttpRequest();x.open('POST','/upload?name='+encodeURIComponent(f.name));"
        "x.upload.onprogress=e=>{if(e.lengthComputable)fill.style.width=(e.loaded*100/e.total)+'%'};"
        "x.onload=async()=>{msg.textContent=x.responseText;fill.style.width='0';list.textContent=await (await fetch('/list')).text()};"
        "x.onerror=()=>msg.textContent='Upload failed';x.send(f);}"
        "</script>";
    send_text(req, "text/html; charset=utf-8", html);
    return ESP_OK;
}

esp_err_t AppSdcard::handle_list(httpd_req_t* req)
{
    auto* app = static_cast<AppSdcard*>(req->user_ctx);
    send_text(req, "text/plain; charset=utf-8", app->build_rom_list_text());
    return ESP_OK;
}

esp_err_t AppSdcard::handle_upload(httpd_req_t* req)
{
    auto* app = static_cast<AppSdcard*>(req->user_ctx);

    char query[256] = {};
    char encoded_name[192] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", encoded_name, sizeof(encoded_name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file name");
        return ESP_FAIL;
    }

    const auto filename = url_decode(encoded_name);
    const auto path = app->safe_upload_path(filename);
    if (path.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unsupported or unsafe file name");
        return ESP_FAIL;
    }

    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        mclog::tagError(app->getAppInfo().name, "open upload target failed: {} errno={}", path, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open target file");
        return ESP_FAIL;
    }

    auto* buffer = static_cast<char*>(heap_caps_malloc(UPLOAD_BUFFER_SIZE, MALLOC_CAP_8BIT));
    if (!buffer) {
        std::fclose(fp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No upload buffer");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    size_t written = 0;
    while (remaining > 0) {
        int received = httpd_req_recv(req, buffer, std::min<int>(remaining, UPLOAD_BUFFER_SIZE));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            heap_caps_free(buffer);
            std::fclose(fp);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        if (std::fwrite(buffer, 1, received, fp) != static_cast<size_t>(received)) {
            heap_caps_free(buffer);
            std::fclose(fp);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }

        written += received;
        remaining -= received;
    }

    std::fflush(fp);
    std::fclose(fp);
    heap_caps_free(buffer);

    app->_last_upload = filename;
    app->_last_upload_size = written;
    app->_status = "Uploaded";
    app->render_upload_state();

    mclog::tagInfo(app->getAppInfo().name, "uploaded: {} ({} bytes)", path, static_cast<unsigned>(written));
    send_text(req, "text/plain; charset=utf-8", "Uploaded " + filename + " (" + std::to_string(written) + " bytes)");
    return ESP_OK;
}
