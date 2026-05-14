/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "home_assistant_client.h"

#include <cJSON.h>
#include <esp_http_client.h>
#include <mooncake_log.h>
#include <algorithm>
#include <cstdlib>

static const char* TAG = "HomeAssistant";

static esp_err_t http_event_handler(esp_http_client_event_t* event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data && event->data && event->data_len > 0) {
        auto* response = static_cast<std::string*>(event->user_data);
        response->append(static_cast<const char*>(event->data), event->data_len);
    }
    return ESP_OK;
}

void HomeAssistantClient::setConfig(const Config& config)
{
    _config = config;
}

bool HomeAssistantClient::isConfigured() const
{
    return !_config.baseUrl.empty() && !_config.token.empty() && !_config.homepodEntity.empty();
}

bool HomeAssistantClient::fetchHomePodState(MediaState& state)
{
    state = {};
    if (!isConfigured()) {
        return false;
    }

    std::string response;
    if (!request("GET", "/api/states/" + _config.homepodEntity, "", response)) {
        return false;
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        mclog::tagError(TAG, "failed to parse HA state json");
        return false;
    }

    cJSON* state_item = cJSON_GetObjectItem(root, "state");
    cJSON* attrs      = cJSON_GetObjectItem(root, "attributes");
    if (cJSON_IsString(state_item)) {
        state.state = state_item->valuestring;
    }
    if (cJSON_IsObject(attrs)) {
        cJSON* name = cJSON_GetObjectItem(attrs, "friendly_name");
        cJSON* title = cJSON_GetObjectItem(attrs, "media_title");
        cJSON* artist = cJSON_GetObjectItem(attrs, "media_artist");
        cJSON* volume = cJSON_GetObjectItem(attrs, "volume_level");

        if (cJSON_IsString(name)) {
            state.name = name->valuestring;
        }
        if (cJSON_IsString(title)) {
            state.title = title->valuestring;
        }
        if (cJSON_IsString(artist)) {
            state.artist = artist->valuestring;
        }
        if (cJSON_IsNumber(volume)) {
            state.volumePercent = static_cast<int>(volume->valuedouble * 100.0);
        }
    }

    cJSON_Delete(root);
    state.ok = true;
    return true;
}

bool HomeAssistantClient::homePodVolumeUp()
{
    return callService("media_player", "volume_up");
}

bool HomeAssistantClient::homePodVolumeDown()
{
    return callService("media_player", "volume_down");
}

bool HomeAssistantClient::homePodSetVolume(int percent)
{
    if (!isConfigured()) {
        return false;
    }

    percent = std::max(0, std::min(100, percent));
    float level = static_cast<float>(percent) / 100.0f;
    std::string body = "{\"entity_id\":\"" + _config.homepodEntity + "\",\"volume_level\":" + std::to_string(level) + "}";
    return callService("media_player", "volume_set", body);
}

bool HomeAssistantClient::homePodPlayPause()
{
    return callService("media_player", "media_play_pause");
}

bool HomeAssistantClient::callService(const char* domain, const char* service)
{
    if (!isConfigured()) {
        return false;
    }

    std::string body = "{\"entity_id\":\"" + _config.homepodEntity + "\"}";
    return callService(domain, service, body);
}

bool HomeAssistantClient::callService(const char* domain, const char* service, const std::string& body)
{
    std::string response;
    return request("POST", std::string("/api/services/") + domain + "/" + service, body, response);
}

bool HomeAssistantClient::request(const std::string& method, const std::string& path, const std::string& body,
                                  std::string& response)
{
    response.clear();

    auto url = buildUrl(path);
    esp_http_client_config_t config = {};
    config.url                     = url.c_str();
    config.event_handler           = http_event_handler;
    config.user_data               = &response;
    config.timeout_ms              = 5000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }

    std::string auth = "Bearer " + _config.token;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (method == "POST") {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_post_field(client, body.c_str(), body.size());
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        mclog::tagError(TAG, "request failed: {} status {}", esp_err_to_name(err), status);
        return false;
    }

    return true;
}

std::string HomeAssistantClient::buildUrl(const std::string& path) const
{
    if (_config.baseUrl.empty()) {
        return path;
    }

    if (_config.baseUrl.back() == '/') {
        return _config.baseUrl.substr(0, _config.baseUrl.size() - 1) + path;
    }
    return _config.baseUrl + path;
}
