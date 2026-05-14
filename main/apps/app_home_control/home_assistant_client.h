/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <string>

class HomeAssistantClient {
public:
    struct Config {
        std::string baseUrl;
        std::string token;
        std::string homepodEntity;
    };

    struct MediaState {
        bool ok = false;
        std::string state;
        std::string name;
        std::string title;
        std::string artist;
        int volumePercent = -1;
    };

    void setConfig(const Config& config);
    bool isConfigured() const;
    bool fetchHomePodState(MediaState& state);
    bool homePodVolumeUp();
    bool homePodVolumeDown();
    bool homePodSetVolume(int percent);
    bool homePodPlayPause();

private:
    Config _config;

    bool callService(const char* domain, const char* service);
    bool callService(const char* domain, const char* service, const std::string& body);
    bool request(const std::string& method, const std::string& path, const std::string& body, std::string& response);
    std::string buildUrl(const std::string& path) const;
};
