/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <vector>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <random>

namespace audio {

static std::vector<int> c_major_scale = {60, 62, 64, 65, 67, 69, 71};  // C大调音阶（C D E F G A B）
static constexpr const char* KEYBOARD_SFX_VOLUME_KEY = "kbd_sfx_vol";
static constexpr const char* KEYBOARD_SFX_LAST_VOLUME_KEY = "kbd_sfx_last";
static constexpr const char* KEYBOARD_SFX_ENABLED_KEY = "kbd_sfx_en";
static int s_keyboard_sfx_volume_percent             = -1;
static int s_keyboard_sfx_user_enabled               = -1;

static int clamp_percent(int percent)
{
    return std::max(0, std::min(100, percent));
}

static int ensure_keyboard_sfx_volume_loaded()
{
    if (s_keyboard_sfx_volume_percent < 0) {
        s_keyboard_sfx_volume_percent = clamp_percent(GetHAL().getSettings().GetInt(KEYBOARD_SFX_VOLUME_KEY, 45));
    }
    return s_keyboard_sfx_volume_percent;
}

static bool ensure_keyboard_sfx_user_enabled_loaded()
{
    if (s_keyboard_sfx_user_enabled < 0) {
        s_keyboard_sfx_user_enabled = GetHAL().getSettings().GetInt(KEYBOARD_SFX_ENABLED_KEY, 1) != 0 ? 1 : 0;
    }
    return s_keyboard_sfx_user_enabled != 0;
}

static bool is_volume_shortcut(const Keyboard::KeyEvent_t& event)
{
    return GetHAL().keyboard.isFnPressed() &&
           (event.keyCode == KEY_LEFTBRACE || event.keyCode == KEY_RIGHTBRACE);
}

static bool is_brightness_shortcut(const Keyboard::KeyEvent_t& event)
{
    return GetHAL().keyboard.isFnPressed() &&
           (event.keyCode == KEY_MINUS || event.keyCode == KEY_EQUAL);
}

void play_tone(int frequency, double durationSec)
{
    if (GetHAL().speaker.getVolume() <= 0) {
        return;
    }

    const int sample_rate = GetHAL().speaker.config().sample_rate;
    const int samples     = static_cast<int>(sample_rate * durationSec);
    std::vector<int16_t> buffer(samples * 2);  // 双声道

    const int fade_len    = 200;  // 淡出长度（采样点）
    const float amplitude = 32767.0f / 5;

    for (int i = 0; i < samples; ++i) {
        float amp = amplitude;

        // 应用结尾淡出（fade-out）
        if (i >= samples - fade_len) {
            float fade_factor = static_cast<float>(samples - i) / fade_len;
            amp *= fade_factor;
        }

        int16_t value     = static_cast<int16_t>(amp * sin(2.0 * M_PI * frequency * i / sample_rate));
        buffer[i * 2]     = value;  // 左声道
        buffer[i * 2 + 1] = value;  // 右声道
    }

    GetHAL().speaker.playRaw(buffer.data(), buffer.size());
}

static void play_keyboard_tone(int frequency, double durationSec)
{
    const int sfx_volume = ensure_keyboard_sfx_volume_loaded();
    if (!ensure_keyboard_sfx_user_enabled_loaded() || sfx_volume <= 0 || GetHAL().speaker.getVolume() <= 0) {
        return;
    }

    const int sample_rate = GetHAL().speaker.config().sample_rate;
    const int samples     = static_cast<int>(sample_rate * durationSec);
    std::vector<int16_t> buffer(samples * 2);

    const int fade_len = 200;
    const float amplitude = (32767.0f / 5) * (static_cast<float>(sfx_volume) / 100.0f);

    for (int i = 0; i < samples; ++i) {
        float amp = amplitude;
        if (i >= samples - fade_len) {
            float fade_factor = static_cast<float>(samples - i) / fade_len;
            amp *= fade_factor;
        }

        int16_t value     = static_cast<int16_t>(amp * sin(2.0 * M_PI * frequency * i / sample_rate));
        buffer[i * 2]     = value;
        buffer[i * 2 + 1] = value;
    }

    GetHAL().speaker.playRaw(buffer.data(), buffer.size());
}

static void play_keyboard_tone_from_midi(int midi, double durationSec)
{
    double freq = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
    play_keyboard_tone(static_cast<int>(freq), durationSec);
}

static void play_random_keyboard_tone(int semitoneShift, double durationSec)
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, static_cast<int>(c_major_scale.size()) - 1);

    int index = dist(gen);
    int midi  = c_major_scale[index] + semitoneShift;
    play_keyboard_tone_from_midi(midi, durationSec);
}

void play_melody(const std::vector<int>& midiList, double durationSec = 0.1)
{
    if (GetHAL().speaker.getVolume() <= 0) {
        return;
    }

    const int sample_rate      = GetHAL().speaker.config().sample_rate;
    const int samples_per_note = static_cast<int>(sample_rate * durationSec);
    const int fade_len         = 200;  // 每个音符结尾的淡出长度
    const float amplitude      = 32767.0f / 5;

    std::vector<int16_t> buffer;                             // 大 buffer 存放整首旋律
    buffer.reserve(midiList.size() * samples_per_note * 2);  // 双声道预留空间

    for (int midiNote : midiList) {
        for (int i = 0; i < samples_per_note; ++i) {
            float amp = amplitude;

            // 应用淡出（仅每个音符的结尾）
            if (i >= samples_per_note - fade_len) {
                float fade_factor = static_cast<float>(samples_per_note - i) / fade_len;
                amp *= fade_factor;
            }

            int16_t sample = 0;
            if (midiNote >= 0) {
                double freq = 440.0 * pow(2.0, (midiNote - 69) / 12.0);
                sample      = static_cast<int16_t>(amp * sin(2.0 * M_PI * freq * i / sample_rate));
            }

            buffer.push_back(sample);  // 左声道
            buffer.push_back(sample);  // 右声道
        }
    }

    GetHAL().speaker.playRaw(buffer.data(), buffer.size());
}

void play_tone_from_midi(int midi, double durationSec)
{
    if (GetHAL().speaker.getVolume() <= 0) {
        return;
    }

    double freq = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
    play_tone(static_cast<int>(freq), durationSec);
}

void play_random_tone(int semitoneShift = 0, double durationSec = 0.15)
{
    if (GetHAL().speaker.getVolume() <= 0) {
        return;
    }

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, static_cast<int>(c_major_scale.size()) - 1);

    int index = dist(gen);
    int midi  = c_major_scale[index] + semitoneShift;

    play_tone_from_midi(midi, durationSec);
}

/* -------------------------------------------------------------------------- */
/*                                  Keyboard                                  */
/* -------------------------------------------------------------------------- */
static void _keyboard_sfx_on_key_event(const Keyboard::KeyEvent_t& event)
{
    if (!event.state) {
        return;
    }
    if (is_volume_shortcut(event)) {
        return;
    }
    if (is_brightness_shortcut(event)) {
        return;
    }

    int semitoneShift = 48;
    switch (event.keyCode) {
        case KEY_1:
            play_keyboard_tone_from_midi(c_major_scale[0] + semitoneShift, 0.02);
            return;
        case KEY_2:
            play_keyboard_tone_from_midi(c_major_scale[1] + semitoneShift, 0.02);
            return;
        case KEY_3:
            play_keyboard_tone_from_midi(c_major_scale[2] + semitoneShift, 0.02);
            return;
        case KEY_4:
            play_keyboard_tone_from_midi(c_major_scale[3] + semitoneShift, 0.02);
            return;
        case KEY_5:
            play_keyboard_tone_from_midi(c_major_scale[4] + semitoneShift, 0.02);
            return;
        case KEY_6:
            play_keyboard_tone_from_midi(c_major_scale[5] + semitoneShift, 0.02);
            return;
        case KEY_7:
            play_keyboard_tone_from_midi(c_major_scale[6] + semitoneShift, 0.02);
            return;
        default:
            play_random_keyboard_tone(semitoneShift, 0.02);
    }
}

void set_keyboard_sfx_enable(bool enable)
{
    static size_t slot_id  = 0;
    static bool is_enabled = false;

    mclog::tagInfo("audio", "set keyboard sfx enable: {}", enable);
    if (enable) {
        if (is_enabled) {
            return;
        }
        is_enabled = true;
        slot_id    = GetHAL().keyboard.onKeyEvent.connect(_keyboard_sfx_on_key_event);
    } else {
        if (!is_enabled) {
            return;
        }
        is_enabled = false;
        GetHAL().keyboard.onKeyEvent.disconnect(slot_id);
    }
}

int get_keyboard_sfx_volume_percent()
{
    return ensure_keyboard_sfx_volume_loaded();
}

void set_keyboard_sfx_volume_percent(int percent)
{
    const int previous = ensure_keyboard_sfx_volume_loaded();
    s_keyboard_sfx_volume_percent = clamp_percent(percent);
    if (s_keyboard_sfx_volume_percent > 0) {
        s_keyboard_sfx_user_enabled = 1;
        GetHAL().getSettings().SetInt(KEYBOARD_SFX_ENABLED_KEY, 1);
        GetHAL().getSettings().SetInt(KEYBOARD_SFX_LAST_VOLUME_KEY, s_keyboard_sfx_volume_percent);
    } else {
        s_keyboard_sfx_user_enabled = 0;
        GetHAL().getSettings().SetInt(KEYBOARD_SFX_ENABLED_KEY, 0);
        if (previous > 0) {
            GetHAL().getSettings().SetInt(KEYBOARD_SFX_LAST_VOLUME_KEY, previous);
        }
    }
    GetHAL().getSettings().SetInt(KEYBOARD_SFX_VOLUME_KEY, s_keyboard_sfx_volume_percent);
    mclog::tagInfo("audio", "keyboard sfx volume: {}%", s_keyboard_sfx_volume_percent);
}

int adjust_keyboard_sfx_volume_percent(int delta)
{
    const int volume = clamp_percent(ensure_keyboard_sfx_volume_loaded() + delta);
    set_keyboard_sfx_volume_percent(volume);
    if (volume > 0) {
        play_keyboard_tone_from_midi(108, 0.03);
    }
    return volume;
}

bool is_keyboard_sfx_user_enabled()
{
    return ensure_keyboard_sfx_user_enabled_loaded() && ensure_keyboard_sfx_volume_loaded() > 0;
}

bool toggle_keyboard_sfx_user_enabled()
{
    const bool enabled = !is_keyboard_sfx_user_enabled();
    if (enabled) {
        int restoreVolume = clamp_percent(GetHAL().getSettings().GetInt(KEYBOARD_SFX_LAST_VOLUME_KEY, 45));
        if (restoreVolume <= 0) {
            restoreVolume = 45;
        }
        s_keyboard_sfx_volume_percent = restoreVolume;
        s_keyboard_sfx_user_enabled = 1;
        GetHAL().getSettings().SetInt(KEYBOARD_SFX_VOLUME_KEY, restoreVolume);
        GetHAL().getSettings().SetInt(KEYBOARD_SFX_LAST_VOLUME_KEY, restoreVolume);
        GetHAL().getSettings().SetInt(KEYBOARD_SFX_ENABLED_KEY, 1);
        play_keyboard_tone_from_midi(108, 0.03);
    } else {
        const int currentVolume = ensure_keyboard_sfx_volume_loaded();
        if (currentVolume > 0) {
            GetHAL().getSettings().SetInt(KEYBOARD_SFX_LAST_VOLUME_KEY, currentVolume);
        }
        s_keyboard_sfx_user_enabled = 0;
        GetHAL().getSettings().SetInt(KEYBOARD_SFX_ENABLED_KEY, 0);
    }
    mclog::tagInfo("audio", "keyboard sfx user enabled: {}", enabled);
    return enabled;
}

static void _global_shortcuts_on_key_event(const Keyboard::KeyEvent_t& event)
{
    if (!event.state || event.isModifier || !GetHAL().keyboard.isFnPressed()) {
        return;
    }

    if (event.keyCode == KEY_LEFTBRACE) {
        adjust_keyboard_sfx_volume_percent(-10);
    } else if (event.keyCode == KEY_RIGHTBRACE) {
        adjust_keyboard_sfx_volume_percent(10);
    } else if (event.keyCode == KEY_M) {
        toggle_keyboard_sfx_user_enabled();
    } else if (event.keyCode == KEY_MINUS) {
        int brightness = GetHAL().getDeviceBrightnessPercent();
        brightness = std::max(1, brightness - 10);
        GetHAL().setDeviceBrightnessPercent(brightness);
        mclog::tagInfo("audio", "display brightness shortcut: {}%", brightness);
    } else if (event.keyCode == KEY_EQUAL) {
        int brightness = GetHAL().getDeviceBrightnessPercent();
        brightness = std::min(100, brightness + 10);
        GetHAL().setDeviceBrightnessPercent(brightness);
        mclog::tagInfo("audio", "display brightness shortcut: {}%", brightness);
    } else {
        return;
    }
}

void set_global_shortcuts_enable(bool enable)
{
    static size_t slot_id  = 0;
    static bool is_enabled = false;

    mclog::tagInfo("audio", "set global shortcuts enable: {}", enable);
    if (enable) {
        if (is_enabled) {
            return;
        }
        is_enabled = true;
        slot_id    = GetHAL().keyboard.onKeyEvent.connect(_global_shortcuts_on_key_event);
    } else {
        if (!is_enabled) {
            return;
        }
        is_enabled = false;
        GetHAL().keyboard.onKeyEvent.disconnect(slot_id);
    }
}

}  // namespace audio
