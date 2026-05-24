/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma GCC optimize("O3")

#include "app_pokemon_yellow.h"
#include "assets/gameboy_big.h"
#include "assets/gameboy_small.h"
#include "emu/gearboy/GearboyCore.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <hal/hal.h>
#include <system/gameboy_boot_mode.h>
#include <mooncake_log.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <sys/stat.h>
#include <cstring>
#include <strings.h>
#include <cstdio>
#include <ctime>
#include <new>

static uint8_t audio_read(uint_fast16_t addr);
static void audio_write(uint_fast16_t addr, uint8_t value);
static std::string rom_miss_summary_and_reset();
static uint32_t fnv1a_update(uint32_t hash, const uint8_t* data, size_t size);
static void cgb_ram_changed_callback();

#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#define AUDIO_SAMPLE_RATE 16384
extern "C" {
#include "emu/minigb_apu/minigb_apu.h"
}

#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#include "emu/peanut_gb.h"

extern "C" {
#include "emu/gnuboy/gnuboy.h"
}

using namespace mooncake;

static constexpr size_t DEFAULT_SAVE_SIZE = 32 * 1024;
static constexpr size_t GB_ROM_BANK_SIZE = 16 * 1024;
static constexpr int GB_SCREEN_WIDTH = 160;
static constexpr int GB_SCREEN_HEIGHT = 144;
static constexpr int GB_RENDER_MAX_WIDTH = 240;
static constexpr int GB_RENDER_MAX_HEIGHT = 135;
static constexpr size_t GB_FRAMEBUFFER_PIXELS = GB_RENDER_MAX_WIDTH * GB_RENDER_MAX_HEIGHT;
static constexpr uint8_t GB_PALETTE_GROUP_BG = 0;
static constexpr uint8_t GB_PALETTE_GROUP_OBJ0 = 1;
static constexpr uint8_t GB_PALETTE_GROUP_OBJ1 = 2;
static constexpr int GB_PUSH_CHUNK_HEIGHT = 8;
static constexpr uint32_t SAVE_FLUSH_INTERVAL_MS = 30000;
static constexpr uint32_t SAVE_FLUSH_QUIET_MS = 2500;
static constexpr uint32_t HOME_EXIT_GUARD_MS = 700;
static constexpr uint32_t EMULATOR_TASK_STACK_SIZE = 8192;
static constexpr UBaseType_t EMULATOR_TASK_PRIORITY = 6;
static constexpr BaseType_t EMULATOR_TASK_CORE = 1;
static constexpr uint32_t AUDIO_TASK_STACK_SIZE = 4096;
static constexpr UBaseType_t AUDIO_TASK_PRIORITY = 7;
static constexpr BaseType_t AUDIO_TASK_CORE = 0;
static constexpr uint32_t AUDIO_FRAME_PERIOD_US = 1000000 / 60;
static constexpr UBaseType_t AUDIO_WRITE_QUEUE_DEPTH = 1024;
static constexpr int AUDIO_OUTPUT_GAIN_NUM = 3;
static constexpr int AUDIO_OUTPUT_GAIN_DEN = 4;
static constexpr int GNUBOY_AUDIO_GAIN_NUM = 1;
static constexpr int GNUBOY_AUDIO_GAIN_DEN = 2;
static constexpr size_t GNUBOY_AUDIO_STEREO_SAMPLES = 1024;
static constexpr size_t GNUBOY_AUDIO_MONO_FRAMES = GNUBOY_AUDIO_STEREO_SAMPLES / 2;
static constexpr size_t GNUBOY_AUDIO_PLAY_BUFFERS = 3;
static constexpr int64_t GB_EMULATION_FRAME_PERIOD_US = 16743;
static constexpr uint32_t GB_VIDEO_FAST_INTERVAL_MS = 50;
static constexpr uint32_t GB_VIDEO_NORMAL_INTERVAL_MS = 66;
static constexpr uint32_t GB_VIDEO_SLOW_INTERVAL_MS = 100;
static constexpr uint32_t GB_VIDEO_STRESS_INTERVAL_MS = 160;
static constexpr size_t GB_ROM_BANK_MISS_HISTOGRAM = 128;
static constexpr uint32_t ROM_FLASH_CACHE_MAGIC = 0x47425243;  // CRBG
static constexpr uint32_t ROM_FLASH_CACHE_VERSION = 2;
static constexpr size_t ROM_FLASH_CACHE_DATA_OFFSET = 4096;
static constexpr const char* ROM_FLASH_CACHE_LABEL = "romcache";
static constexpr uint32_t EXTERNAL_INPUT_I2C_FREQ = 400000;
static constexpr uint32_t EXTERNAL_INPUT_PROBE_INTERVAL_MS = 1000;
static constexpr uint32_t EXTERNAL_INPUT_POLL_INTERVAL_MS = 12;
static constexpr uint32_t EXTERNAL_INPUT_SCAN_LOG_INTERVAL_MS = 5000;
static constexpr uint8_t JOYSTICK_UNIT_ADDR = 0x52;
static constexpr uint8_t JOYSTICK2_ADDR = 0x63;
static constexpr uint8_t JOYSTICK2_OFFSET_ADC_VALUE_8BITS_REG = 0x60;
static constexpr uint8_t JOYSTICK2_BUTTON_REG = 0x20;
static constexpr uint8_t BYTE_BUTTON_ADDR = 0x47;
static constexpr uint8_t BYTE_BUTTON_STATUS_8BYTE_REG = 0x60;
static constexpr int JOYSTICK_UNIT_CENTER = 128;
static constexpr int JOYSTICK_UNIT_DEAD_ZONE = 38;
static constexpr int JOYSTICK2_DEAD_ZONE = 24;
static constexpr uint8_t EXT_PAD_UP = 1 << 0;
static constexpr uint8_t EXT_PAD_DOWN = 1 << 1;
static constexpr uint8_t EXT_PAD_LEFT = 1 << 2;
static constexpr uint8_t EXT_PAD_RIGHT = 1 << 3;
static constexpr uint8_t EXT_PAD_A = 1 << 4;
static constexpr uint8_t EXT_PAD_B = 1 << 5;
static constexpr uint8_t EXT_PAD_SELECT = 1 << 6;
static constexpr uint8_t EXT_PAD_START = 1 << 7;

struct RomFlashCacheHeader {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t rom_size = 0;
    uint32_t rom_mtime = 0;
    uint32_t rom_hash = 0;
    char rom_path[180] = {};
};

static minigb_apu_ctx s_audio_context;
static std::array<uint16_t, GB_ROM_BANK_MISS_HISTOGRAM> s_rom_bank_miss_counts = {};
struct GameBoyRuntimeBuffers {
    gb_s gb_context {};
    std::array<uint8_t, GB_ROM_BANK_SIZE> rom_bank0 {};
    std::array<uint8_t, DEFAULT_SAVE_SIZE> save_ram {};
    std::array<uint8_t, 240> render_x_map {};
    std::array<uint16_t, GB_RENDER_MAX_WIDTH * GB_PUSH_CHUNK_HEIGHT> push_buffer {};
    std::array<char, 4096> rom_file_buffer {};
    std::array<audio_sample_t, AUDIO_SAMPLES_TOTAL> audio_stereo_buffer {};
    std::array<int16_t, AUDIO_SAMPLES> audio_mono_buffer {};
    std::array<uint16_t, GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT> cgb_framebuffer_storage {};
    std::array<int16_t, GNUBOY_AUDIO_STEREO_SAMPLES> cgb_audio_stereo_buffer {};
    std::array<std::array<int16_t, GNUBOY_AUDIO_MONO_FRAMES>, GNUBOY_AUDIO_PLAY_BUFFERS> gnuboy_audio_mono_buffers {};
    std::array<int16_t, AUDIO_BUFFER_SIZE / 2> gearboy_cgb_audio_mono_buffer {};
};

static GameBoyRuntimeBuffers* s_gb_buffers = nullptr;
static AppPokemonYellow* s_cgb_active_app = nullptr;
static AppPokemonYellow* s_gnuboy_active_app = nullptr;
static volatile bool s_cgb_save_dirty_flag = false;
static volatile bool s_gnuboy_sound_enabled = false;
static size_t s_gnuboy_audio_buffer_index = 0;
static int32_t s_gnuboy_audio_filter = 0;
static uint32_t s_gnuboy_audio_frames = 0;
static uint32_t s_gnuboy_audio_play_failures = 0;
static volatile bool s_audio_output_enabled = false;
static bool s_audio_context_ready = false;
static portMUX_TYPE s_audio_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_audio_task_stop = false;
static volatile bool s_audio_task_running = false;
static TaskHandle_t s_audio_task_handle = nullptr;
struct AudioRegisterWrite {
    uint16_t addr;
    uint8_t value;
};
static QueueHandle_t s_audio_write_queue = nullptr;
static uint32_t s_audio_frames = 0;
static uint32_t s_audio_play_failures = 0;
static uint32_t s_audio_write_drops = 0;
static UBaseType_t s_audio_max_queue_depth = 0;
static uint32_t s_audio_last_log_ms = 0;

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static constexpr uint16_t rgb555ToRgb565(uint16_t color)
{
    const uint8_t r5 = color & 0x1F;
    const uint8_t g5 = (color >> 5) & 0x1F;
    const uint8_t b5 = (color >> 10) & 0x1F;
    return rgb565(static_cast<uint8_t>((r5 << 3) | (r5 >> 2)),
                  static_cast<uint8_t>((g5 << 3) | (g5 >> 2)),
                  static_cast<uint8_t>((b5 << 3) | (b5 >> 2)));
}

static constexpr std::array<uint16_t, 12> s_dmg_lcd_palette = {
    0xE7F7, 0x9D6B, 0x52A5, 0x1082,
    0xE7F7, 0x9D6B, 0x52A5, 0x1082,
    0xE7F7, 0x9D6B, 0x52A5, 0x1082,
};

static constexpr std::array<uint16_t, 12> s_dmg_gray_palette = {
    0xFFFF, 0xAD55, 0x52AA, 0x0000,
    0xFFFF, 0xAD55, 0x52AA, 0x0000,
    0xFFFF, 0xAD55, 0x52AA, 0x0000,
};

static std::array<uint16_t, 12> s_gameboy_palette = s_dmg_lcd_palette;

static bool ensureGameBoyBuffers()
{
    if (s_gb_buffers) {
        return true;
    }

    void* memory = heap_caps_malloc(sizeof(GameBoyRuntimeBuffers), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) {
        mclog::tagError("GameBoy",
                        "runtime buffer allocation failed: need={} internal_free={} largest={}",
                        static_cast<unsigned>(sizeof(GameBoyRuntimeBuffers)),
                        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return false;
    }

    s_gb_buffers = new (memory) GameBoyRuntimeBuffers();
    mclog::tagInfo("GameBoy",
                   "runtime buffers allocated: {} bytes internal_free={} largest={}",
                   static_cast<unsigned>(sizeof(GameBoyRuntimeBuffers)),
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    return true;
}

static void releaseGameBoyBuffers()
{
    if (!s_gb_buffers) {
        return;
    }

    s_gb_buffers->~GameBoyRuntimeBuffers();
    heap_caps_free(s_gb_buffers);
    s_gb_buffers = nullptr;
    mclog::tagInfo("GameBoy",
                   "runtime buffers released: internal_free={} largest={}",
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

static uint32_t fnv1a_update(uint32_t hash, const uint8_t* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

static void cgb_ram_changed_callback()
{
    s_cgb_save_dirty_flag = true;
}

static void gnuboyAudioCallback(void* buffer, size_t length)
{
    if (!s_gnuboy_sound_enabled || !s_gb_buffers || !buffer || length < 2) {
        return;
    }

    const auto* stereo = static_cast<const int16_t*>(buffer);
    auto& mono = s_gb_buffers->gnuboy_audio_mono_buffers[s_gnuboy_audio_buffer_index++ % s_gb_buffers->gnuboy_audio_mono_buffers.size()];
    const size_t frames = std::min(length / 2, mono.size());
    for (size_t i = 0; i < frames; ++i) {
        const int32_t left = stereo[i * 2];
        const int32_t right = stereo[i * 2 + 1];
        int32_t mixed = ((left + right) / 2) * GNUBOY_AUDIO_GAIN_NUM / GNUBOY_AUDIO_GAIN_DEN;
        s_gnuboy_audio_filter += (mixed - s_gnuboy_audio_filter) / 4;
        mixed = s_gnuboy_audio_filter;
        mixed = std::clamp<int32_t>(mixed, INT16_MIN, INT16_MAX);
        mono[i] = static_cast<int16_t>(mixed);
    }
    const bool queued = GetHAL().speaker.playRaw(mono.data(), frames, AUDIO_SAMPLE_RATE, false, 1, 0, false);
    ++s_gnuboy_audio_frames;
    if (!queued) {
        ++s_gnuboy_audio_play_failures;
    }
}

static const char* gnuboyColorModeName(int mode)
{
    switch (mode & 7) {
        case 0:
            return "RGB raw";
        case 1:
            return "RGB LCD";
        case 2:
            return "BGR raw";
        case 3:
            return "BGR LCD";
        case 4:
            return "RGB raw swap";
        case 5:
            return "RGB LCD swap";
        case 6:
            return "BGR raw swap";
        case 7:
            return "BGR LCD swap";
        default:
            return "unknown";
    }
}

static constexpr std::array<uint8_t, 94> s_cgb_boot_title_checksums = {
    0x00, 0x88, 0x16, 0x36, 0xD1, 0xDB, 0xF2, 0x3C, 0x8C, 0x92, 0x3D, 0x5C,
    0x58, 0xC9, 0x3E, 0x70, 0x1D, 0x59, 0x69, 0x19, 0x35, 0xA8, 0x14, 0xAA,
    0x75, 0x95, 0x99, 0x34, 0x6F, 0x15, 0xFF, 0x97, 0x4B, 0x90, 0x17, 0x10,
    0x39, 0xF7, 0xF6, 0xA2, 0x49, 0x4E, 0x43, 0x68, 0xE0, 0x8B, 0xF0, 0xCE,
    0x0C, 0x29, 0xE8, 0xB7, 0x86, 0x9A, 0x52, 0x01, 0x9D, 0x71, 0x9C, 0xBD,
    0x5D, 0x6D, 0x67, 0x3F, 0x6B, 0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27,
    0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4, 0xB3, 0x46, 0x28, 0xA5, 0xC6,
    0xD3, 0x27, 0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4, 0xB3,
};

static constexpr size_t CGB_BOOT_FIRST_DUPLICATE_CHECKSUM = 65;
static constexpr std::array<char, 29> s_cgb_boot_duplicate_title_letters = {
    'B', 'E', 'F', 'A', 'A', 'R', 'B', 'E', 'K', 'E',
    'K', ' ', 'R', '-', 'U', 'R', 'A', 'R', ' ', 'I',
    'N', 'A', 'I', 'L', 'I', 'C', 'E', ' ', 'R',
};

static constexpr std::array<uint8_t, 94> s_cgb_boot_palette_per_checksum = {
    0, 4, 5, 35, 34, 3, 31, 15, 10, 5, 19, 36, 135, 37, 30, 44, 21, 32, 31,
    20, 5, 33, 13, 14, 5, 29, 5, 18, 9, 3, 2, 26, 25, 25, 41, 42, 26, 45,
    42, 45, 36, 38, 154, 42, 30, 41, 34, 34, 5, 42, 6, 5, 33, 25, 42, 42,
    40, 2, 16, 25, 42, 42, 5, 0, 39, 36, 22, 25, 6, 32, 12, 36, 11, 39,
    18, 39, 24, 31, 50, 17, 46, 6, 27, 0, 47, 41, 41, 0, 0, 19, 34, 23,
    18, 29,
};

static constexpr std::array<std::array<uint8_t, 3>, 55> s_cgb_boot_palette_combinations = {{
    {{ 16,  16, 116}}, {{ 72,  72,  72}}, {{ 80,  80,  80}}, {{ 96,  96,  96}},
    {{ 36,  36,  36}}, {{  0,   0,   0}}, {{108, 108, 108}}, {{ 20,  20,  20}},
    {{ 48,  48,  48}}, {{104, 104, 104}}, {{ 64,  32,  32}}, {{ 16, 112, 112}},
    {{ 16,   8,   8}}, {{ 12,  16,  16}}, {{ 16, 116, 116}}, {{112,  16, 112}},
    {{  8,  68,   8}}, {{ 64,  64,  32}}, {{ 16,  16,  28}}, {{ 16,  16,  72}},
    {{ 16,  16,  80}}, {{ 76,  76,  36}}, {{ 15,  15,  44}}, {{ 68,  68,   8}},
    {{ 16,  16,   8}}, {{ 16,  16,  12}}, {{112, 112,   0}}, {{ 12,  12,   0}},
    {{  0,   0,   4}}, {{ 72,  88,  72}}, {{ 80,  88,  80}}, {{ 96,  88,  96}},
    {{ 64,  88,  32}}, {{ 68,  16,  52}}, {{111,   0,  56}}, {{111,  16,  60}},
    {{ 76,  91,  36}}, {{ 64, 112,  40}}, {{ 16,  92, 112}}, {{ 68,  88,   8}},
    {{ 16,   0,   8}}, {{ 16, 112,  12}}, {{112,  12,   0}}, {{ 12, 112,  16}},
    {{ 84, 112,  16}}, {{ 12, 112,   0}}, {{100,  12, 112}}, {{  0, 112,  32}},
    {{ 16,  12, 112}}, {{112,  12,  24}}, {{ 16, 112, 116}}, {{120, 120, 120}},
    {{124, 124, 124}}, {{112,  16,   4}}, {{  0,   0,   8}},
}};

static constexpr std::array<uint16_t, 128> s_cgb_boot_palette_words = {
    0x7FFF, 0x32BF, 0x00D0, 0x0000, 0x639F, 0x4279, 0x15B0, 0x04CB,
    0x7FFF, 0x6E31, 0x454A, 0x0000, 0x7FFF, 0x1BEF, 0x0200, 0x0000,
    0x7FFF, 0x421F, 0x1CF2, 0x0000, 0x7FFF, 0x5294, 0x294A, 0x0000,
    0x7FFF, 0x03FF, 0x012F, 0x0000, 0x7FFF, 0x03EF, 0x01D6, 0x0000,
    0x7FFF, 0x42B5, 0x3DC8, 0x0000, 0x7E74, 0x03FF, 0x0180, 0x0000,
    0x67FF, 0x77AC, 0x1A13, 0x2D6B, 0x7ED6, 0x4BFF, 0x2175, 0x0000,
    0x53FF, 0x4A5F, 0x7E52, 0x0000, 0x4FFF, 0x7ED2, 0x3A4C, 0x1CE0,
    0x03ED, 0x7FFF, 0x255F, 0x0000, 0x036A, 0x021F, 0x03FF, 0x7FFF,
    0x7FFF, 0x01DF, 0x0112, 0x0000, 0x231F, 0x035F, 0x00F2, 0x0009,
    0x7FFF, 0x03EA, 0x011F, 0x0000, 0x299F, 0x001A, 0x000C, 0x0000,
    0x7FFF, 0x027F, 0x001F, 0x0000, 0x7FFF, 0x03E0, 0x0206, 0x0120,
    0x7FFF, 0x7EEB, 0x001F, 0x7C00, 0x7FFF, 0x3FFF, 0x7E00, 0x001F,
    0x7FFF, 0x03FF, 0x001F, 0x0000, 0x03FF, 0x001F, 0x000C, 0x0000,
    0x7FFF, 0x033F, 0x0193, 0x0000, 0x0000, 0x4200, 0x037F, 0x7FFF,
    0x7FFF, 0x7E8C, 0x7C00, 0x0000, 0x7FFF, 0x1BEF, 0x6180, 0x0000,
    0x7FFF, 0x7FEA, 0x7D5F, 0x0000, 0x4778, 0x3290, 0x1D87, 0x0861,
};

struct BootPaletteSelection {
    uint8_t checksum = 0;
    uint8_t combination = 0;
    bool cgb_capable = false;
    bool matched = false;
};

static bool isNintendoLicensedRom(const uint8_t* rom)
{
    if (rom[0x014B] == 0x01) {
        return true;
    }
    return rom[0x014B] == 0x33 && rom[0x0144] == '0' && rom[0x0145] == '1';
}

static uint8_t cgbBootTitleChecksum(const uint8_t* rom)
{
    uint8_t checksum = 0;
    for (size_t i = 0x0134; i <= 0x0143; ++i) {
        checksum = static_cast<uint8_t>(checksum + rom[i]);
    }
    return checksum;
}

static std::string romHeaderTitle(const uint8_t* rom)
{
    std::string title;
    title.reserve(16);
    for (size_t i = 0x0134; i <= 0x0143; ++i) {
        const uint8_t value = rom[i];
        if (value == 0) {
            break;
        }
        title.push_back(value >= 0x20 && value <= 0x7E ? static_cast<char>(value) : '.');
    }
    return title;
}

static uint8_t calculateRomHeaderChecksum(const uint8_t* rom)
{
    uint8_t checksum = 0;
    for (size_t i = 0x0134; i <= 0x014C; ++i) {
        checksum = static_cast<uint8_t>(checksum - rom[i] - 1);
    }
    return checksum;
}

static bool shouldForcePokemonGoldRtcMapper(const uint8_t* rom, const std::string& path)
{
    auto lowerPath = path;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    const bool looksLikeGoldOrSilver =
        lowerPath.find("gold") != std::string::npos ||
        lowerPath.find("silver") != std::string::npos ||
        lowerPath.find("crystal") != std::string::npos;
    const bool cgbCapable = (rom[0x0143] & 0x80) != 0;
    const bool mbc5WithoutRtc = rom[0x0147] == 0x19 || rom[0x0147] == 0x1A || rom[0x0147] == 0x1B;
    const bool hasSaveRam = rom[0x0149] != 0;
    return looksLikeGoldOrSilver && cgbCapable && mbc5WithoutRtc && hasSaveRam;
}

static bool forcePokemonGoldRtcMapper(uint8_t* rom, const std::string& path)
{
    if (!shouldForcePokemonGoldRtcMapper(rom, path)) {
        return false;
    }

    const uint8_t oldType = rom[0x0147];
    rom[0x0147] = 0x10;  // MBC3 + timer + RAM + battery, required by Pokemon Gold/Silver RTC.
    rom[0x014D] = calculateRomHeaderChecksum(rom);
    mclog::tagWarn("GameBoy",
                   "forcing Pokemon Gold RTC mapper: cart_type 0x{:02X}->0x10 checksum=0x{:02X}",
                   oldType,
                   rom[0x014D]);
    return true;
}

static BootPaletteSelection applyStandardDmgPalette(const uint8_t* rom)
{
    s_gameboy_palette = s_dmg_lcd_palette;

    BootPaletteSelection selection;
    selection.checksum = cgbBootTitleChecksum(rom);
    selection.cgb_capable = (rom[0x0143] & 0x80) != 0;
    if (selection.cgb_capable) {
        s_gameboy_palette = s_dmg_gray_palette;
        return selection;
    }

    uint8_t combination = 0;
    if (isNintendoLicensedRom(rom)) {
        for (size_t i = 0; i < s_cgb_boot_title_checksums.size(); ++i) {
            if (s_cgb_boot_title_checksums[i] != selection.checksum) {
                continue;
            }
            if (i >= CGB_BOOT_FIRST_DUPLICATE_CHECKSUM) {
                const size_t duplicateIndex = i - CGB_BOOT_FIRST_DUPLICATE_CHECKSUM;
                if (duplicateIndex >= s_cgb_boot_duplicate_title_letters.size() ||
                    rom[0x0137] != static_cast<uint8_t>(s_cgb_boot_duplicate_title_letters[duplicateIndex])) {
                    continue;
                }
            }
            combination = static_cast<uint8_t>(s_cgb_boot_palette_per_checksum[i] & 0x7F);
            selection.matched = true;
            break;
        }
    }
    selection.combination = combination;

    if (combination >= s_cgb_boot_palette_combinations.size()) {
        combination = 0;
        selection.combination = 0;
    }

    const auto& offsets = s_cgb_boot_palette_combinations[combination];
    const uint8_t groupOffsets[3] = {offsets[2], offsets[0], offsets[1]};
    for (size_t group = 0; group < 3; ++group) {
        for (size_t shade = 0; shade < 4; ++shade) {
            const size_t wordIndex = static_cast<size_t>(groupOffsets[group]) + shade;
            s_gameboy_palette[group * 4 + shade] =
                wordIndex < s_cgb_boot_palette_words.size()
                    ? rgb555ToRgb565(s_cgb_boot_palette_words[wordIndex])
                    : s_dmg_lcd_palette[group * 4 + shade];
        }
    }

    return selection;
}

static void audio_reset()
{
    portENTER_CRITICAL(&s_audio_lock);
    minigb_apu_audio_init(&s_audio_context);
    s_audio_context_ready = true;
    portEXIT_CRITICAL(&s_audio_lock);
}

static void audioTaskEntry(void*)
{
    s_audio_task_running = true;
    TickType_t lastWake = xTaskGetTickCount();
    while (!s_audio_task_stop) {
        if (!s_audio_output_enabled || !s_audio_context_ready || !s_gb_buffers) {
            vTaskDelay(pdMS_TO_TICKS(8));
            lastWake = xTaskGetTickCount();
            continue;
        }

        AudioRegisterWrite write{};
        UBaseType_t queuedBeforeDrain = 0;
        if (s_audio_write_queue) {
            queuedBeforeDrain = uxQueueMessagesWaiting(s_audio_write_queue);
            if (queuedBeforeDrain > s_audio_max_queue_depth) {
                s_audio_max_queue_depth = queuedBeforeDrain;
            }
        }
        while (s_audio_write_queue && xQueueReceive(s_audio_write_queue, &write, 0) == pdTRUE) {
            portENTER_CRITICAL(&s_audio_lock);
            minigb_apu_audio_write(&s_audio_context, write.addr, write.value);
            portEXIT_CRITICAL(&s_audio_lock);
        }
        portENTER_CRITICAL(&s_audio_lock);
        minigb_apu_audio_callback(&s_audio_context, s_gb_buffers->audio_stereo_buffer.data());
        portEXIT_CRITICAL(&s_audio_lock);

        for (size_t i = 0; i < s_gb_buffers->audio_mono_buffer.size(); ++i) {
            const int32_t left = s_gb_buffers->audio_stereo_buffer[i * 2];
            const int32_t right = s_gb_buffers->audio_stereo_buffer[i * 2 + 1];
            int32_t mixed = ((left + right) / 2) * AUDIO_OUTPUT_GAIN_NUM / AUDIO_OUTPUT_GAIN_DEN;
            mixed = std::clamp<int32_t>(mixed, INT16_MIN, INT16_MAX);
            s_gb_buffers->audio_mono_buffer[i] = static_cast<int16_t>(mixed);
        }

        const bool queued = GetHAL().speaker.playRaw(s_gb_buffers->audio_mono_buffer.data(),
                                                     s_gb_buffers->audio_mono_buffer.size(),
                                                     AUDIO_SAMPLE_RATE,
                                                     false,
                                                     1,
                                                     0,
                                                     false);
        ++s_audio_frames;
        if (!queued) {
            ++s_audio_play_failures;
        }

        const uint32_t now = GetHAL().millis();
        if (s_audio_last_log_ms == 0) {
            s_audio_last_log_ms = now;
        } else if (now - s_audio_last_log_ms >= 3000) {
            mclog::tagInfo("GameBoy",
                           "audio: frames={} play_fail={} write_drop={} queue_max={} sample_rate={} samples={}",
                           static_cast<unsigned>(s_audio_frames),
                           static_cast<unsigned>(s_audio_play_failures),
                           static_cast<unsigned>(s_audio_write_drops),
                           static_cast<unsigned>(s_audio_max_queue_depth),
                           static_cast<unsigned>(AUDIO_SAMPLE_RATE),
                           static_cast<unsigned>(AUDIO_SAMPLES));
            s_audio_frames = 0;
            s_audio_play_failures = 0;
            s_audio_write_drops = 0;
            s_audio_max_queue_depth = 0;
            s_audio_last_log_ms = now;
        }

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(17));
    }
    s_audio_task_running = false;
    s_audio_task_handle = nullptr;
    vTaskDelete(nullptr);
}

static void audio_start_task()
{
    if (s_audio_task_handle) {
        return;
    }

    s_audio_task_stop = false;
    s_audio_task_running = false;
    s_audio_frames = 0;
    s_audio_play_failures = 0;
    s_audio_write_drops = 0;
    s_audio_max_queue_depth = 0;
    s_audio_last_log_ms = 0;
    if (!s_audio_write_queue) {
        s_audio_write_queue = xQueueCreate(AUDIO_WRITE_QUEUE_DEPTH, sizeof(AudioRegisterWrite));
        if (!s_audio_write_queue) {
            s_audio_output_enabled = false;
            mclog::tagWarn("GameBoy", "failed to create audio register queue");
            return;
        }
    } else {
        xQueueReset(s_audio_write_queue);
    }
    const auto ok = xTaskCreatePinnedToCore(audioTaskEntry,
                                           "gb_audio",
                                           AUDIO_TASK_STACK_SIZE,
                                           nullptr,
                                           AUDIO_TASK_PRIORITY,
                                           &s_audio_task_handle,
                                           AUDIO_TASK_CORE);
    if (ok != pdPASS) {
        s_audio_task_handle = nullptr;
        s_audio_output_enabled = false;
        mclog::tagWarn("GameBoy", "failed to create audio task");
    } else {
        mclog::tagInfo("GameBoy", "audio task started on core {}", AUDIO_TASK_CORE);
    }
}

static void audio_stop_task()
{
    if (!s_audio_task_handle) {
        s_audio_task_stop = false;
        s_audio_task_running = false;
        return;
    }

    s_audio_task_stop = true;
    const uint32_t start = GetHAL().millis();
    while (s_audio_task_running && GetHAL().millis() - start < 500) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_audio_task_handle = nullptr;
    s_audio_task_running = false;
    s_audio_task_stop = false;
    GetHAL().speaker.stop(0);
    if (s_audio_write_queue) {
        xQueueReset(s_audio_write_queue);
    }
}

static void audio_set_output_enabled(bool enabled)
{
    s_audio_output_enabled = enabled;
    if (enabled) {
        if (!s_audio_context_ready) {
            audio_reset();
        }
        audio_start_task();
        return;
    }

    audio_stop_task();
    GetHAL().speaker.stop(0);
}

static std::string rom_miss_summary_and_reset()
{
    std::array<int, 4> topBank = {-1, -1, -1, -1};
    std::array<uint16_t, 4> topCount = {0, 0, 0, 0};
    for (size_t bank = 0; bank < s_rom_bank_miss_counts.size(); ++bank) {
        const uint16_t count = s_rom_bank_miss_counts[bank];
        if (count == 0) {
            continue;
        }
        for (size_t slot = 0; slot < topCount.size(); ++slot) {
            if (count > topCount[slot]) {
                for (size_t move = topCount.size() - 1; move > slot; --move) {
                    topCount[move] = topCount[move - 1];
                    topBank[move] = topBank[move - 1];
                }
                topCount[slot] = count;
                topBank[slot] = static_cast<int>(bank);
                break;
            }
        }
    }

    std::string summary;
    for (size_t i = 0; i < topBank.size(); ++i) {
        if (topBank[i] < 0) {
            continue;
        }
        if (!summary.empty()) {
            summary += ",";
        }
        summary += fmt::format("{}:{}", topBank[i], topCount[i]);
    }
    if (summary.empty()) {
        summary = "none";
    }
    std::fill(s_rom_bank_miss_counts.begin(), s_rom_bank_miss_counts.end(), 0);
    return summary;
}

static uint8_t audio_read(const uint_fast16_t addr)
{
    if (addr < 0xFF10 || addr > 0xFF3F) {
        return 0xFF;
    }
    if (!s_audio_context_ready) {
        audio_reset();
    }
    portENTER_CRITICAL(&s_audio_lock);
    const uint8_t value = minigb_apu_audio_read(&s_audio_context, static_cast<uint16_t>(addr));
    portEXIT_CRITICAL(&s_audio_lock);
    return value;
}

static void audio_write(const uint_fast16_t addr, const uint8_t value)
{
    if (addr < 0xFF10 || addr > 0xFF3F) {
        return;
    }
    if (!s_audio_context_ready) {
        audio_reset();
    }
    if (!s_audio_output_enabled || !s_audio_write_queue) {
        return;
    }

    portENTER_CRITICAL(&s_audio_lock);
    minigb_apu_audio_write(&s_audio_context, static_cast<uint16_t>(addr), value);
    portEXIT_CRITICAL(&s_audio_lock);
    return;

    const AudioRegisterWrite write{static_cast<uint16_t>(addr), value};
    if (xQueueSend(s_audio_write_queue, &write, 0) != pdTRUE) {
        ++s_audio_write_drops;
    } else {
        const UBaseType_t queued = uxQueueMessagesWaiting(s_audio_write_queue);
        if (queued > s_audio_max_queue_depth) {
            s_audio_max_queue_depth = queued;
        }
    }
}

static uint8_t gbRomReadCallback(gb_s* gb, const uint_fast32_t addr)
{
    return static_cast<AppPokemonYellow*>(gb->direct.priv)->emulatorRomRead(addr);
}

static uint8_t gbCartRamReadCallback(gb_s* gb, const uint_fast32_t addr)
{
    return static_cast<AppPokemonYellow*>(gb->direct.priv)->emulatorSaveRead(addr);
}

static void gbCartRamWriteCallback(gb_s* gb, const uint_fast32_t addr, const uint8_t value)
{
    static_cast<AppPokemonYellow*>(gb->direct.priv)->emulatorSaveWrite(addr, value);
}

static void gbErrorCallback(gb_s* gb, const enum gb_error_e error, const uint16_t addr)
{
    auto* app = static_cast<AppPokemonYellow*>(gb->direct.priv);
    mclog::tagError(app->getAppInfo().name, "emulator error={} addr=0x{:04X}", static_cast<int>(error), addr);
}

static void gbLcdDrawLineCallback(gb_s* gb, const uint8_t* pixels, const uint_fast8_t line)
{
    static_cast<AppPokemonYellow*>(gb->direct.priv)->emulatorDrawLine(pixels, line);
}

static void logGameBoyHeap(const char* phase)
{
    mclog::tagInfo("GameBoyHeap",
                   "{} internal_free={} internal_largest={} internal_min={} dma_free={} dma_largest={}",
                   phase,
                   heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   heap_caps_get_free_size(MALLOC_CAP_DMA),
                   heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

AppPokemonYellow::AppPokemonYellow()
{
    setAppInfo().name     = "GameBoy";
    setAppInfo().userData = new AppIcon_t(image_data_gameboy_big, image_data_gameboy_small);
}

AppPokemonYellow::~AppPokemonYellow()
{
    stopEmulator();
    releaseGameBoyBuffers();
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppPokemonYellow::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    logGameBoyHeap("onOpen");
    if (!gameboy_boot_mode_is_active()) {
        mclog::tagInfo(getAppInfo().name, "entering dedicated GameBoy mode");
        gameboy_boot_mode_enter();
        esp_restart();
        return;
    }
    if (!ensureGameBoyBuffers()) {
        setStatus("No GameBoy RAM");
        render();
        close();
        return;
    }

    _opened_at = GetHAL().millis();
    _mode = Mode::Browser;
    _sound_enabled = GetHAL().getSettings().GetInt("gb_sound", 0) != 0;
    _gnuboy_color_mode = GetHAL().getSettings().GetInt("gb_color_mode", 4) & 7;
    gnuboy_set_color_mode(_gnuboy_color_mode);
    mclog::tagInfo(getAppInfo().name,
                   "gnuboy color mode: {} ({})",
                   _gnuboy_color_mode,
                   gnuboyColorModeName(_gnuboy_color_mode));
    audio_set_output_enabled(false);
    _pending_launch = false;
    _pending_probe = false;
    _key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& keyEvent) { handleKeyEvent(keyEvent); });
    setStatus("Checking SD");
    probe();
    GetHAL().externalInput.probe(GetHAL().millis());
    logGameBoyHeap("afterProbe");
    render();
}

void AppPokemonYellow::onRunning()
{
    const bool homeExitAllowed = GetHAL().millis() - _opened_at > HOME_EXIT_GUARD_MS;
    if (homeExitAllowed && GetHAL().homeButton.wasClicked()) {
        mclog::tagInfo(getAppInfo().name, "home button clicked, closing");
        if (gameboy_boot_mode_is_active()) {
            stopEmulator();
            gameboy_boot_mode_exit();
            esp_restart();
            return;
        }
        if (_mode == Mode::Emulator) {
            stopEmulator();
            close();
            return;
        }
        audio::play_random_tone();
        close();
        return;
    }

    if (_pending_launch) {
        _pending_launch = false;
        startEmulator();
    }

    updateExternalInput();

    if (_mode == Mode::Emulator) {
        runEmulatorFrame();
        return;
    }

    if (_pending_probe) {
        _pending_probe = false;
        probe();
        render();
    }
}

void AppPokemonYellow::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    logGameBoyHeap("onCloseBeforeStop");
    stopEmulator();
    logGameBoyHeap("onCloseAfterStop");
    releaseGameBoyBuffers();
    if (_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_event_slot_id);
        _key_event_slot_id = -1;
    }
}

void AppPokemonYellow::probe()
{
    mclog::tagInfo(getAppInfo().name, "probe start");
    auto probeStart = GetHAL().millis();
    std::string previousPath;
    if (_selected_rom >= 0 && _selected_rom < static_cast<int>(_roms.size())) {
        previousPath = _roms[_selected_rom].path;
    }
    const auto savedPath = GetHAL().getSettings().GetString("gb_last_rom", "");
    if (!savedPath.empty()) {
        previousPath = savedPath;
    }

    _last_probe_time = GetHAL().millis();
    _state           = {};
    _roms.clear();

    auto sd = GetHAL().sdCardProbe();
    _state.sdMounted = sd.is_mounted;
    mclog::tagInfo(getAppInfo().name, "sd mounted: {}", _state.sdMounted ? "yes" : "no");
    if (!_state.sdMounted) {
        setStatus("Insert SD card");
        mclog::tagWarn(getAppInfo().name, "probe stopped: no SD card elapsed={}ms", GetHAL().millis() - probeStart);
        return;
    }

    createDirectories();

    scanRomDirectory("/sdcard/roms");
    scanRomDirectory("/sdcard/ROMS");

    std::sort(_roms.begin(), _roms.end(), [](const RomEntry& a, const RomEntry& b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    if (!_roms.empty()) {
        _selected_rom = std::max(0, std::min(_selected_rom, static_cast<int>(_roms.size()) - 1));
        if (!previousPath.empty()) {
            for (int i = 0; i < static_cast<int>(_roms.size()); ++i) {
                if (_roms[i].path == previousPath) {
                    _selected_rom = i;
                    break;
                }
            }
        }
        activateSelectedRom();
    } else {
        _selected_rom = 0;
    }

    if (_roms.empty()) {
        setStatus("ROM missing");
    } else if (!_state.saveFound) {
        setStatus("Press Enter: create save");
    } else {
        setStatus("Ready with save");
    }
    mclog::tagInfo(getAppInfo().name,
                   "probe done: rom_count={} selected={} rom='{}' save='{}' save_found={}",
                   static_cast<unsigned>(_roms.size()),
                   _selected_rom,
                   _state.romPath,
                   _state.savePath,
                   _state.saveFound ? "yes" : "no");
    mclog::tagInfo(getAppInfo().name, "probe elapsed: {}ms", GetHAL().millis() - probeStart);
}

void AppPokemonYellow::render()
{
    if (_mode == Mode::Emulator) {
        renderEmulatorFrame();
        return;
    }
    renderBrowser();
}

void AppPokemonYellow::renderBrowser()
{
    auto drawBrowser = [this](auto& target) {
        auto printClippedTo = [&target](const std::string& text, int maxChars) {
            if (static_cast<int>(text.size()) <= maxChars) {
                target.print(text.c_str());
                return;
            }

            auto clipped = text.substr(0, std::max(0, maxChars - 3)) + "...";
            target.print(clipped.c_str());
        };

        target.fillScreen(THEME_COLOR_BG);
        target.setTextScroll(false);
        target.setTextSize(1);
        target.setCursor(0, 0);

        if (_roms.empty()) {
            target.setTextColor(TFT_CYAN, THEME_COLOR_BG);
            target.setCursor(0, 0);
            target.println(_state.sdMounted ? "No ROM found" : "Insert SD card");
            target.setCursor(0, 14);
            target.println("/sdcard/roms");
            target.setCursor(0, 28);
            target.println("*.gb *.gbc *.gba");
        } else {
            int visibleCount = 8;
            int start = std::max(0, std::min(_selected_rom - 4, static_cast<int>(_roms.size()) - visibleCount));
            for (int row = 0; row < visibleCount && start + row < static_cast<int>(_roms.size()); ++row) {
                int index = start + row;
                const auto& rom = _roms[index];
                target.setCursor(0, row * 12);
                target.setTextColor(index == _selected_rom ? TFT_GREEN : TFT_WHITE, THEME_COLOR_BG);
                target.print(index == _selected_rom ? "> " : "  ");
                printClippedTo(rom.name, 31);
            }
        }

        if (_state.romFound) {
            target.setTextColor(TFT_CYAN, THEME_COLOR_BG);
            target.setCursor(0, 104);
            target.print("Save:");
            target.print(_state.saveFound ? "ok" : "new");
        }

        target.setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
        target.setCursor(0, 120);
        target.printf("E/S Sel Enter Run M Sound:%s", _sound_enabled ? "on" : "off");
    };

    if (gameboy_boot_mode_is_active()) {
        auto& display = GetHAL().display;
        display.startWrite();
        drawBrowser(display);
        display.endWrite();
        return;
    }

    auto& canvas = GetHAL().canvas;
    drawBrowser(canvas);
    GetHAL().pushCanvas();
}

void AppPokemonYellow::handleKeyEvent(const Keyboard::KeyEvent_t& keyEvent)
{
    if (keyEvent.isModifier) {
        return;
    }

    if (handleSystemShortcut(keyEvent)) {
        return;
    }

    const auto& raw = GetHAL().keyboard.getLatestKeyEventRaw();

    if (_mode == Mode::Emulator) {
        handleEmulatorKey(keyEvent, raw);
        return;
    }

    if (!keyEvent.state) {
        return;
    }

    handleBrowserKey(keyEvent, raw);
}

void AppPokemonYellow::handleBrowserKey(const Keyboard::KeyEvent_t& keyEvent, const Keyboard::KeyEventRaw_t& raw)
{
    mclog::tagInfo(getAppInfo().name,
                   "browser key: name='{}' code={} raw=({}, {}) selected={} roms={}",
                   keyEvent.keyName ? keyEvent.keyName : "",
                   static_cast<unsigned>(keyEvent.keyCode),
                   static_cast<unsigned>(raw.row),
                   static_cast<unsigned>(raw.col),
                   _selected_rom,
                   _roms.size());

    if (keyEvent.keyCode == KEY_UP || (raw.row == 2 && raw.col == 11)) {
        selectRom(-1);
        setStatus("Selected");
        render();
    } else if (keyEvent.keyCode == KEY_E) {
        selectRom(-1);
        setStatus("Selected");
        render();
    } else if (keyEvent.keyCode == KEY_DOWN || (raw.row == 3 && raw.col == 11)) {
        selectRom(1);
        setStatus("Selected");
        render();
    } else if (keyEvent.keyCode == KEY_S) {
        selectRom(1);
        setStatus("Selected");
        render();
    } else if (isEnterKey(keyEvent, raw)) {
        activateSelectedRom();
        createSaveIfNeeded();
        if (_state.romFound && !_state.savePath.empty()) {
            _state.saveFound = fileExists(_state.savePath.c_str(), &_state.saveSize);
        }
        setStatus("Launching");
        _pending_launch = true;
        render();
    } else if (keyEvent.keyCode == KEY_R || (raw.row == 1 && raw.col == 4)) {
        setStatus("Refresh queued");
        _pending_probe = true;
        render();
    } else if (keyEvent.keyCode == KEY_M) {
        toggleSound();
        render();
    } else {
        setStatus(fmt::format("Ignored {}", keyEvent.keyName));
    }
}

void AppPokemonYellow::handleEmulatorKey(const Keyboard::KeyEvent_t& keyEvent, const Keyboard::KeyEventRaw_t& raw)
{
    const bool pressed = keyEvent.state;
    if (pressed) {
        mclog::tagInfo(getAppInfo().name,
                       "emu key: name='{}' code={} raw=({}, {}) joypad=0x{:02X}",
                       keyEvent.keyName ? keyEvent.keyName : "",
                       static_cast<unsigned>(keyEvent.keyCode),
                       static_cast<unsigned>(raw.row),
                       static_cast<unsigned>(raw.col),
                       static_cast<unsigned>(_gb ? _gb->direct.joypad : 0));
    }

    if (pressed && keyEvent.keyCode == KEY_M) {
        toggleSound();
        return;
    }

    const bool physicalE = raw.row == 1 && raw.col == 3;
    const bool physicalA = raw.row == 2 && raw.col == 2;
    const bool physicalS = raw.row == 2 && raw.col == 3;
    const bool physicalD = raw.row == 2 && raw.col == 4;

    if (physicalE || keyEvent.keyCode == KEY_E || keyEvent.keyCode == KEY_UP || (raw.row == 2 && raw.col == 11)) {
        setJoypadButton(JOYPAD_UP, pressed);
    } else if (physicalS || keyEvent.keyCode == KEY_S || keyEvent.keyCode == KEY_DOWN || (raw.row == 3 && raw.col == 11)) {
        setJoypadButton(JOYPAD_DOWN, pressed);
    } else if (physicalA || keyEvent.keyCode == KEY_A || keyEvent.keyCode == KEY_LEFT || (raw.row == 3 && raw.col == 10)) {
        setJoypadButton(JOYPAD_LEFT, pressed);
    } else if (physicalD || keyEvent.keyCode == KEY_D || keyEvent.keyCode == KEY_RIGHT || (raw.row == 3 && raw.col == 12)) {
        setJoypadButton(JOYPAD_RIGHT, pressed);
    } else if (isEnterKey(keyEvent, raw)) {
        setJoypadButton(JOYPAD_START, pressed);
    } else if (keyEvent.keyCode == KEY_SPACE) {
        setJoypadButton(JOYPAD_SELECT, pressed);
    } else if (keyEvent.keyCode == KEY_K) {
        setJoypadButton(JOYPAD_A, pressed);
    } else if (keyEvent.keyCode == KEY_J) {
        setJoypadButton(JOYPAD_B, pressed);
    }
}

bool AppPokemonYellow::isEnterKey(const Keyboard::KeyEvent_t& keyEvent, const Keyboard::KeyEventRaw_t& raw) const
{
    return keyEvent.keyCode == KEY_ENTER ||
           keyEvent.keyCode == KEY_KPENTER ||
           (keyEvent.keyName && std::strcmp(keyEvent.keyName, "enter") == 0) ||
           (raw.row == 2 && raw.col == 13);
}

void AppPokemonYellow::startEmulator()
{
    if (!ensureGameBoyBuffers()) {
        setStatus("No GameBoy RAM");
        renderBrowser();
        return;
    }
    if (!_state.romFound || _state.romPath.empty()) {
        setStatus("Select ROM first");
        render();
        return;
    }

    if (_gb || _rom_file || _rom_flash_data || _emulator_task_handle) {
        stopEmulator();
    }
    GetHAL().getSettings().SetString("gb_last_rom", _state.romPath);
    logGameBoyHeap("startAfterStop");

    setStatus("Preparing ROM");
    renderBrowser();
    const bool flashBackend = prepareFlashRomCache();

    if (!flashBackend) {
        if (_rom_cache_slot_count == 0 && !allocateRomCache()) {
            setStatus("No ROM cache RAM");
            logGameBoyHeap("romCacheAllocFailed");
            renderBrowser();
            return;
        }
        _rom_file = std::fopen(_state.romPath.c_str(), "rb");
        if (!_rom_file) {
            setStatus(fmt::format("ROM open failed {}", errno));
            releaseFramebuffer();
            renderBrowser();
            return;
        }
        std::setvbuf(_rom_file, s_gb_buffers->rom_file_buffer.data(), _IOFBF, s_gb_buffers->rom_file_buffer.size());
        mclog::tagInfo(getAppInfo().name, "using SD ROM bank cache backend");
    }

    setStatus("Loading emulator");
    renderBrowser();

    std::fill(s_gb_buffers->rom_bank0.begin(), s_gb_buffers->rom_bank0.end(), 0xFF);
    for (size_t i = 0; i < _rom_cache_slot_count; ++i) {
        if (_rom_bank_cache[i]) {
            std::fill(_rom_bank_cache[i], _rom_bank_cache[i] + GB_ROM_BANK_SIZE, 0xFF);
        }
    }
    std::fill(_rom_bank_cache_index.begin(), _rom_bank_cache_index.end(), -1);
    std::fill(_rom_bank_cache_stamp.begin(), _rom_bank_cache_stamp.end(), 0);
    _rom_bank_access_clock = 0;
    _save_ram_size = 0;
    _save_dirty = false;
    _last_save_flush = GetHAL().millis();
    _last_save_write = 0;

    if (!loadRomBank(0, s_gb_buffers->rom_bank0.data(), s_gb_buffers->rom_bank0.size())) {
        setStatus("ROM read failed");
        stopEmulator();
        renderBrowser();
        return;
    }
    const bool forcedRtcMapper = forcePokemonGoldRtcMapper(s_gb_buffers->rom_bank0.data(), _state.romPath);
    const auto paletteSelection = applyStandardDmgPalette(s_gb_buffers->rom_bank0.data());
    mclog::tagInfo(getAppInfo().name,
                   "rom title='{}' cgb flag=0x{:02X} type=0x{:02X} ram=0x{:02X} checksum=0x{:02X} palette={} matched={} licensed={} rtc_mapper={} (Peanut-GB runs DMG display path)",
                   romHeaderTitle(s_gb_buffers->rom_bank0.data()),
                   s_gb_buffers->rom_bank0[0x0143],
                   s_gb_buffers->rom_bank0[0x0147],
                   s_gb_buffers->rom_bank0[0x0149],
                   paletteSelection.checksum,
                   static_cast<unsigned>(paletteSelection.combination),
                   paletteSelection.matched ? "yes" : "no",
                   isNintendoLicensedRom(s_gb_buffers->rom_bank0.data()) ? "yes" : "no",
                   forcedRtcMapper ? "forced" : "header");
    if (paletteSelection.cgb_capable) {
        if (flashBackend) {
            if (startGnuboyCgbEmulator()) {
                return;
            }
            const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (largestBlock >= 65536) {
                startCgbEmulator();
                return;
            }
            mclog::tagWarn(getAppInfo().name,
                           "CGB cores skipped: largest contiguous internal block={} bytes, using DMG fallback",
                           static_cast<unsigned>(largestBlock));
            setStatus("CGB RAM low, DMG fallback");
            renderBrowser();
        } else {
            mclog::tagWarn(getAppInfo().name,
                           "CGB ROM detected but flash backend is unavailable; using DMG LCD fallback");
        }
    }

    if (!_framebuffer && !allocateFramebuffer()) {
        setStatus("No framebuffer RAM");
        logGameBoyHeap("framebufferAllocFailed");
        stopEmulator();
        renderBrowser();
        return;
    }

    std::memset(&s_gb_buffers->gb_context, 0, sizeof(s_gb_buffers->gb_context));
    _gb = &s_gb_buffers->gb_context;

    auto init = gb_init(_gb,
                        gbRomReadCallback,
                        gbCartRamReadCallback,
                        gbCartRamWriteCallback,
                        gbErrorCallback,
                        this);
    if (init != GB_INIT_NO_ERROR) {
        mclog::tagError(getAppInfo().name, "gb_init failed: {}", static_cast<int>(init));
        setStatus(fmt::format("GB init failed {}", static_cast<int>(init)));
        stopEmulator();
        renderBrowser();
        return;
    }
    std::time_t now = std::time(nullptr);
    if (now > 24 * 60 * 60) {
        std::tm localTime {};
        localtime_r(&now, &localTime);
        gb_set_rtc(_gb, &localTime);
        mclog::tagInfo(getAppInfo().name, "rtc initialized from system time");
    }

    size_t saveSize = 0;
    if (gb_get_save_size_s(_gb, &saveSize) != 0 || saveSize == 0) {
        saveSize = DEFAULT_SAVE_SIZE;
    }
    loadSaveRam(saveSize);
    logGameBoyHeap("afterSaveLoad");
    gb_init_lcd(_gb, gbLcdDrawLineCallback);
    _gb->direct.frame_skip = true;
    _gb->direct.interlace = false;
    _gb->direct.joypad = 0xFF;
    _gb->gb_frame = false;

    _mode = Mode::Emulator;
    _emulator_frame_ready = false;
    _render_geometry_valid = false;
    _perf_window_start = GetHAL().millis();
    _perf_emulated_frames = 0;
    _perf_drawn_frames = 0;
    _perf_skipped_video_frames = 0;
    _perf_frame_ms_total = 0;
    _perf_frame_ms_max = 0;
    _perf_draw_ms_total = 0;
    _perf_rom_loads = 0;
    _perf_rom_load_ms = 0;
    _video_frame_interval_ms = _sound_enabled ? GB_VIDEO_NORMAL_INTERVAL_MS : GB_VIDEO_FAST_INTERVAL_MS;
    _next_video_frame_due = 0;
    _next_emulation_frame_due_us = 0;
    audio::set_keyboard_sfx_enable(false);
    audio::set_global_shortcuts_enable(false);
    GetHAL().setFullscreenMode(true);
    mclog::tagInfo(getAppInfo().name,
                   "emulator started: rom='{}' save='{}' save_size={}",
                   _state.romPath,
                   _state.savePath,
                   static_cast<unsigned>(_save_ram_size));
    GetHAL().display.fillScreen(TFT_BLACK);
    GetHAL().display.startWrite();
    _display_write_active = true;
    logGameBoyHeap("beforeEmuTask");
    startEmulatorTask();
    audio_reset();
    audio_set_output_enabled(_sound_enabled);
}

bool AppPokemonYellow::startGnuboyCgbEmulator()
{
    if (!_rom_flash_data || _rom_flash_size == 0) {
        mclog::tagWarn(getAppInfo().name, "gnuboy CGB needs flash cache");
        return false;
    }

    releaseFramebuffer();
    if (!allocateCgbFramebuffer()) {
        mclog::tagWarn(getAppInfo().name, "gnuboy CGB framebuffer allocation failed");
        return false;
    }

    s_gnuboy_active_app = this;
    s_gnuboy_sound_enabled = _sound_enabled;
    s_gnuboy_audio_buffer_index = 0;
    s_gnuboy_audio_filter = 0;
    s_gnuboy_audio_frames = 0;
    s_gnuboy_audio_play_failures = 0;
    _gnuboy_active = false;
    _gnuboy_pad = 0;

    gnuboy_set_sram_buffer(s_gb_buffers->save_ram.data(), s_gb_buffers->save_ram.size());
    gnuboy_set_color_mode(_gnuboy_color_mode);
    if (gnuboy_init(AUDIO_SAMPLE_RATE, GB_AUDIO_STEREO_S16, GB_PIXEL_565_LE, nullptr, gnuboyAudioCallback) < 0) {
        mclog::tagWarn(getAppInfo().name, "gnuboy init failed");
        gnuboy_clear_sram_buffer();
        s_gnuboy_active_app = nullptr;
        releaseCgbFramebuffer();
        return false;
    }

    gnuboy_set_framebuffer(_cgb_framebuffer);
    gnuboy_set_soundbuffer(s_gb_buffers->cgb_audio_stereo_buffer.data(), s_gb_buffers->cgb_audio_stereo_buffer.size());

    if (gnuboy_load_rom(_rom_flash_data, _rom_flash_size) < 0) {
        mclog::tagWarn(getAppInfo().name, "gnuboy ROM load failed");
        gnuboy_free_rom();
        gnuboy_clear_sram_buffer();
        s_gnuboy_active_app = nullptr;
        releaseCgbFramebuffer();
        return false;
    }

    gnuboy_set_palette(GB_PALETTE_CGB);
    gnuboy_reset(true);
    _save_ram_size = DEFAULT_SAVE_SIZE;
    if (!_state.savePath.empty()) {
        std::fill(s_gb_buffers->save_ram.begin(), s_gb_buffers->save_ram.end(), 0xFF);
        FILE* fp = std::fopen(_state.savePath.c_str(), "rb");
        size_t bytesRead = 0;
        if (fp) {
            bytesRead = std::fread(s_gb_buffers->save_ram.data(), 1, DEFAULT_SAVE_SIZE, fp);
            std::fclose(fp);
        }
        printf("gnuboy SRAM load: path=%s bytes=%u\n",
               _state.savePath.c_str(),
               static_cast<unsigned>(bytesRead));
    }
    std::time_t now = std::time(nullptr);
    if (now > 24 * 60 * 60) {
        std::tm localTime {};
        localtime_r(&now, &localTime);
        gnuboy_set_time(localTime.tm_yday, localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
    }

    _gnuboy_active = true;
    _save_dirty = false;
    _last_save_flush = GetHAL().millis();
    _last_save_write = 0;
    _mode = Mode::Emulator;
    _emulator_frame_ready = false;
    _render_geometry_valid = false;
    _perf_window_start = GetHAL().millis();
    _perf_emulated_frames = 0;
    _perf_drawn_frames = 0;
    _perf_skipped_video_frames = 0;
    _perf_frame_ms_total = 0;
    _perf_frame_ms_max = 0;
    _perf_draw_ms_total = 0;
    _perf_rom_loads = 0;
    _perf_rom_load_ms = 0;
    _video_frame_interval_ms = _sound_enabled ? GB_VIDEO_NORMAL_INTERVAL_MS : GB_VIDEO_FAST_INTERVAL_MS;
    _next_video_frame_due = 0;
    _next_emulation_frame_due_us = 0;
    audio_set_output_enabled(false);
    audio::set_keyboard_sfx_enable(false);
    audio::set_global_shortcuts_enable(false);
    GetHAL().setFullscreenMode(true);
    GetHAL().display.fillScreen(TFT_BLACK);
    GetHAL().display.startWrite();
    _display_write_active = true;
    logGameBoyHeap("beforeGnuboyTask");
    mclog::tagInfo(getAppInfo().name,
                   "gnuboy CGB core started: rom='{}' mapped={} save='{}' sound={} color={} ({})",
                   _state.romPath,
                   static_cast<unsigned>(_rom_flash_size),
                   _state.savePath,
                   _sound_enabled ? "on" : "off",
                   _gnuboy_color_mode,
                   gnuboyColorModeName(_gnuboy_color_mode));
    startEmulatorTask();
    return true;
}

void AppPokemonYellow::startCgbEmulator()
{
    if (!_rom_flash_data || _rom_flash_size == 0) {
        setStatus("CGB needs flash cache");
        stopEmulator();
        renderBrowser();
        return;
    }

    releaseFramebuffer();
    if (!allocateCgbFramebuffer()) {
        setStatus("No CGB framebuffer");
        stopEmulator();
        renderBrowser();
        return;
    }

    _cgb_core = new (std::nothrow) GearboyCore();
    if (!_cgb_core) {
        setStatus("No CGB core RAM");
        stopEmulator();
        renderBrowser();
        return;
    }

    s_cgb_active_app = this;
    s_cgb_save_dirty_flag = false;

    GetHAL().sdCardUnmount();
    logGameBoyHeap("afterSdUnmountBeforeCgbCore");

    _cgb_core->Init(GB_PIXEL_RGB565);
    _cgb_core->SetSGBEnabled(false);
    _cgb_core->SetSGBBorder(false);
    _cgb_core->SetSoundSampleRate(AUDIO_SAMPLE_RATE);
    _cgb_core->SetSoundMute(!_sound_enabled);
    _cgb_core->SetSoundVolume(0.9f);
    _cgb_core->SetRamModificationCallback(cgb_ram_changed_callback);

    if (!_cgb_core->LoadROMFromExternalBuffer(_rom_flash_data,
                                              static_cast<int>(_rom_flash_size),
                                              _state.romPath.c_str(),
                                              false)) {
        mclog::tagError(getAppInfo().name, "Gearboy CGB core failed to load ROM");
        setStatus("CGB load failed");
        stopEmulator();
        renderBrowser();
        return;
    }

    GetHAL().sdCardProbe();
    logGameBoyHeap("afterSdRemountBeforeCgbSave");
    _cgb_core->LoadRam(_state.savePath.c_str(), true);

    _save_ram_size = DEFAULT_SAVE_SIZE;
    _save_dirty = false;
    _last_save_flush = GetHAL().millis();
    _last_save_write = 0;
    _mode = Mode::Emulator;
    _emulator_frame_ready = false;
    _render_geometry_valid = false;
    _perf_window_start = GetHAL().millis();
    _perf_emulated_frames = 0;
    _perf_drawn_frames = 0;
    _perf_skipped_video_frames = 0;
    _perf_frame_ms_total = 0;
    _perf_frame_ms_max = 0;
    _perf_draw_ms_total = 0;
    _perf_rom_loads = 0;
    _perf_rom_load_ms = 0;
    _video_frame_interval_ms = _sound_enabled ? GB_VIDEO_NORMAL_INTERVAL_MS : GB_VIDEO_FAST_INTERVAL_MS;
    _next_video_frame_due = 0;
    _next_emulation_frame_due_us = 0;
    audio_set_output_enabled(false);
    audio::set_keyboard_sfx_enable(false);
    audio::set_global_shortcuts_enable(false);
    GetHAL().setFullscreenMode(true);
    GetHAL().display.fillScreen(TFT_BLACK);
    GetHAL().display.startWrite();
    _display_write_active = true;
    logGameBoyHeap("beforeCgbTask");
    mclog::tagInfo(getAppInfo().name,
                   "Gearboy CGB core started: rom='{}' mapped={} save='{}' sound={}",
                   _state.romPath,
                   static_cast<unsigned>(_rom_flash_size),
                   _state.savePath,
                   _sound_enabled ? "on" : "off");
    startEmulatorTask();
}

void AppPokemonYellow::stopEmulator()
{
    if (!_gb && !_cgb_core && !_gnuboy_active && !_rom_file && !_rom_flash_data && !_emulator_task_handle && !_framebuffer && !_cgb_framebuffer) {
        return;
    }

    stopEmulatorTask();
    applyExternalPadButtons(0);
    _external_pad_buttons = 0;
    _external_pad_last_buttons = 0;
    if (_display_write_active) {
        GetHAL().display.endWrite();
        _display_write_active = false;
    }
    flushSaveRam(true);
    if (_cgb_core) {
        delete _cgb_core;
        _cgb_core = nullptr;
    }
    if (_gnuboy_active) {
        gnuboy_free_rom();
        gnuboy_clear_sram_buffer();
        _gnuboy_active = false;
    }
    s_cgb_active_app = nullptr;
    s_gnuboy_active_app = nullptr;
    s_gnuboy_sound_enabled = false;
    s_cgb_save_dirty_flag = false;
    if (_rom_file) {
        std::fclose(_rom_file);
        _rom_file = nullptr;
    }
    releaseFlashRomCache();
    _gb = nullptr;
    if (s_gb_buffers) {
        std::fill(s_gb_buffers->rom_bank0.begin(), s_gb_buffers->rom_bank0.end(), 0xFF);
    }
    for (size_t i = 0; i < _rom_cache_slot_count; ++i) {
        if (_rom_bank_cache[i]) {
            std::fill(_rom_bank_cache[i], _rom_bank_cache[i] + GB_ROM_BANK_SIZE, 0xFF);
        }
    }
    _save_ram_size = 0;
    std::fill(_rom_bank_cache_index.begin(), _rom_bank_cache_index.end(), -1);
    std::fill(_rom_bank_cache_stamp.begin(), _rom_bank_cache_stamp.end(), 0);
    _rom_bank_access_clock = 0;
    _save_dirty = false;
    _last_save_write = 0;
    _render_geometry_valid = false;
    audio_set_output_enabled(false);
    releaseFramebuffer();
    releaseCgbFramebuffer();
    releaseRomCache();
    GetHAL().setFullscreenMode(false);
    GetHAL().pushCanvasSystemBar();
    GetHAL().pushCanvasKeyboardBar();
    if (!gameboy_boot_mode_is_active()) {
        audio::set_global_shortcuts_enable(true);
        audio::set_keyboard_sfx_enable(true);
    }
    logGameBoyHeap("afterStop");
    mclog::tagInfo(getAppInfo().name, "emulator stopped");
}

void AppPokemonYellow::runEmulatorFrame()
{
    if (_gnuboy_active) {
        if (_emulator_task_handle) {
            if (_emulator_frame_ready) {
                _emulator_frame_ready = false;
                flushSaveRam(false);
            }
            GetHAL().feedTheDog();
            return;
        }
        runGnuboyEmulatorFrame();
        return;
    }

    if (_cgb_core) {
        if (_emulator_task_handle) {
            if (_emulator_frame_ready) {
                _emulator_frame_ready = false;
                flushSaveRam(false);
            }
            GetHAL().feedTheDog();
            return;
        }
        runCgbEmulatorFrame();
        return;
    }

    if (!_gb) {
        _mode = Mode::Browser;
        render();
        return;
    }

    if (_emulator_task_handle) {
        if (_emulator_frame_ready) {
            _emulator_frame_ready = false;
            flushSaveRam(false);
        }
        GetHAL().feedTheDog();
        return;
    }

    int64_t nowUs = esp_timer_get_time();
    if (_next_emulation_frame_due_us == 0) {
        _next_emulation_frame_due_us = nowUs;
    } else if (nowUs < _next_emulation_frame_due_us) {
        const int64_t waitUs = _next_emulation_frame_due_us - nowUs;
        if (waitUs >= 1000) {
            vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(waitUs / 1000)));
        }
    }

    const uint32_t frameStart = GetHAL().millis();
    bool drawThisFrame = false;
    if (_next_video_frame_due == 0 || frameStart >= _next_video_frame_due) {
        drawThisFrame = true;
        _next_video_frame_due = frameStart + _video_frame_interval_ms;
    }

    _gb->direct.frame_skip = !drawThisFrame;
    _gb->display.frame_skip_count = drawThisFrame;
    _gb->gb_frame = false;
    while (_gb && !_gb->gb_frame) {
        __gb_step_cpu(_gb);
    }
    ++_perf_emulated_frames;
    const uint32_t frameElapsed = GetHAL().millis() - frameStart;
    _perf_frame_ms_total += frameElapsed;
    if (frameElapsed > _perf_frame_ms_max) {
        _perf_frame_ms_max = frameElapsed;
    }
    if (!drawThisFrame) {
        ++_perf_skipped_video_frames;
    }
    _next_emulation_frame_due_us += GB_EMULATION_FRAME_PERIOD_US;
    nowUs = esp_timer_get_time();
    if (nowUs - _next_emulation_frame_due_us > GB_EMULATION_FRAME_PERIOD_US * 2) {
        _next_emulation_frame_due_us = nowUs;
    }

    const uint32_t now = GetHAL().millis();
    if (_perf_window_start == 0) {
        _perf_window_start = now;
    } else if (now - _perf_window_start >= 3000) {
        const uint32_t elapsed = now - _perf_window_start;
        const uint32_t drawn = _perf_drawn_frames;
        const uint32_t emuFps = _perf_emulated_frames * 1000 / elapsed;
        if (_sound_enabled) {
            if (emuFps < 35) {
                _video_frame_interval_ms = GB_VIDEO_STRESS_INTERVAL_MS;
            } else if (emuFps < 52) {
                _video_frame_interval_ms = GB_VIDEO_SLOW_INTERVAL_MS;
            } else {
                _video_frame_interval_ms = GB_VIDEO_NORMAL_INTERVAL_MS;
            }
        } else {
            if (emuFps < 45) {
                _video_frame_interval_ms = GB_VIDEO_SLOW_INTERVAL_MS;
            } else if (emuFps < 58) {
                _video_frame_interval_ms = GB_VIDEO_NORMAL_INTERVAL_MS;
            } else {
                _video_frame_interval_ms = GB_VIDEO_FAST_INTERVAL_MS;
            }
        }
        const auto romMisses = rom_miss_summary_and_reset();
        mclog::tagInfo(getAppInfo().name,
                       "perf: emu_fps={} draw_fps={} skip={} video_ms={} frame_avg={} frame_max={} draw_ms={} rom_loads={} rom_ms={} miss={} sound={} mode=fallback",
                       static_cast<unsigned>(emuFps),
                       static_cast<unsigned>(drawn * 1000 / elapsed),
                       static_cast<unsigned>(_perf_skipped_video_frames),
                       static_cast<unsigned>(_video_frame_interval_ms),
                       static_cast<unsigned>(_perf_emulated_frames ? _perf_frame_ms_total / _perf_emulated_frames : 0),
                       static_cast<unsigned>(_perf_frame_ms_max),
                       static_cast<unsigned>(_perf_draw_ms_total),
                       static_cast<unsigned>(_perf_rom_loads),
                       static_cast<unsigned>(_perf_rom_load_ms),
                       romMisses,
                       _sound_enabled ? "on" : "off");
        _perf_window_start = now;
        _perf_emulated_frames = 0;
        _perf_drawn_frames = 0;
        _perf_skipped_video_frames = 0;
        _perf_frame_ms_total = 0;
        _perf_frame_ms_max = 0;
        _perf_draw_ms_total = 0;
        _perf_rom_loads = 0;
        _perf_rom_load_ms = 0;
    }

    _emulator_frame_ready = true;
    if (_emulator_frame_ready) {
        _emulator_frame_ready = false;
        flushSaveRam(false);
    }
    GetHAL().feedTheDog();
}

void AppPokemonYellow::renderEmulatorFrame()
{
}

void AppPokemonYellow::runGnuboyEmulatorFrame()
{
    if (!_gnuboy_active || !_cgb_framebuffer) {
        return;
    }

    int64_t nowUs = esp_timer_get_time();
    if (_next_emulation_frame_due_us == 0) {
        _next_emulation_frame_due_us = nowUs;
    } else if (nowUs < _next_emulation_frame_due_us) {
        const int64_t waitUs = _next_emulation_frame_due_us - nowUs;
        if (waitUs >= 1000) {
            vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(waitUs / 1000)));
        }
    }

    const uint32_t frameStart = GetHAL().millis();
    bool drawThisFrame = false;
    if (_next_video_frame_due == 0 || frameStart >= _next_video_frame_due) {
        drawThisFrame = true;
        _next_video_frame_due = frameStart + _video_frame_interval_ms;
    }

    s_gnuboy_sound_enabled = _sound_enabled;
    gnuboy_run(drawThisFrame);

    ++_perf_emulated_frames;
    const uint32_t frameElapsed = GetHAL().millis() - frameStart;
    _perf_frame_ms_total += frameElapsed;
    if (frameElapsed > _perf_frame_ms_max) {
        _perf_frame_ms_max = frameElapsed;
    }
    if (drawThisFrame) {
        renderCgbEmulatorFrame();
    } else {
        ++_perf_skipped_video_frames;
    }

    _next_emulation_frame_due_us += GB_EMULATION_FRAME_PERIOD_US;
    nowUs = esp_timer_get_time();
    if (nowUs - _next_emulation_frame_due_us > GB_EMULATION_FRAME_PERIOD_US * 2) {
        _next_emulation_frame_due_us = nowUs;
    }

    const uint32_t now = GetHAL().millis();
    if (_perf_window_start == 0) {
        _perf_window_start = now;
    } else if (now - _perf_window_start >= 3000) {
        const uint32_t elapsed = now - _perf_window_start;
        const uint32_t emuFps = _perf_emulated_frames * 1000 / elapsed;
        _video_frame_interval_ms = emuFps < 45 ? GB_VIDEO_STRESS_INTERVAL_MS :
                                   (_sound_enabled ? GB_VIDEO_NORMAL_INTERVAL_MS : GB_VIDEO_FAST_INTERVAL_MS);
        mclog::tagInfo(getAppInfo().name,
                       "perf: emu_fps={} draw_fps={} skip={} video_ms={} frame_avg={} frame_max={} draw_ms={} sound={} audio_frames={} audio_fail={} mode=gnuboy-cgb heap={}",
                       static_cast<unsigned>(emuFps),
                       static_cast<unsigned>(_perf_drawn_frames * 1000 / elapsed),
                       static_cast<unsigned>(_perf_skipped_video_frames),
                       static_cast<unsigned>(_video_frame_interval_ms),
                       static_cast<unsigned>(_perf_emulated_frames ? _perf_frame_ms_total / _perf_emulated_frames : 0),
                       static_cast<unsigned>(_perf_frame_ms_max),
                       static_cast<unsigned>(_perf_draw_ms_total),
                       _sound_enabled ? "on" : "off",
                       static_cast<unsigned>(s_gnuboy_audio_frames),
                       static_cast<unsigned>(s_gnuboy_audio_play_failures),
                       static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        s_gnuboy_audio_frames = 0;
        s_gnuboy_audio_play_failures = 0;
        _perf_window_start = now;
        _perf_emulated_frames = 0;
        _perf_drawn_frames = 0;
        _perf_skipped_video_frames = 0;
        _perf_frame_ms_total = 0;
        _perf_frame_ms_max = 0;
        _perf_draw_ms_total = 0;
    }

    _emulator_frame_ready = true;
    flushSaveRam(false);
    GetHAL().feedTheDog();
}

void AppPokemonYellow::renderCgbEmulatorFrame()
{
    if (!_cgb_framebuffer) {
        return;
    }

    if (!_render_geometry_valid) {
        const int displayW = GetHAL().display.width();
        const int displayH = GetHAL().display.height();
        _render_w = std::min(displayW, GB_RENDER_MAX_WIDTH);
        _render_h = std::min(displayH, GB_RENDER_MAX_HEIGHT);
        _render_x = (displayW - _render_w) / 2;
        _render_y = (displayH - _render_h) / 2;
        if (_render_w > 0 && _render_w <= static_cast<int>(s_gb_buffers->render_x_map.size())) {
            for (int x = 0; x < _render_w; ++x) {
                s_gb_buffers->render_x_map[x] = static_cast<uint8_t>(x * GB_SCREEN_WIDTH / _render_w);
            }
        }
        _render_geometry_valid = true;
    }

    if (_render_w <= 0 || _render_h <= 0 || _render_w > GB_RENDER_MAX_WIDTH || _render_h > GB_RENDER_MAX_HEIGHT) {
        return;
    }

    const uint32_t drawStart = GetHAL().millis();
    GetHAL().display.waitDMA();
    for (int y = 0; y < _render_h; y += GB_PUSH_CHUNK_HEIGHT) {
        const int rows = std::min(GB_PUSH_CHUNK_HEIGHT, _render_h - y);
        for (int rowIndex = 0; rowIndex < rows; ++rowIndex) {
            const int srcY = (y + rowIndex) * GB_SCREEN_HEIGHT / _render_h;
            const uint16_t* src = _cgb_framebuffer + static_cast<size_t>(srcY) * GB_SCREEN_WIDTH;
            uint16_t* dst = s_gb_buffers->push_buffer.data() + static_cast<size_t>(rowIndex) * GB_RENDER_MAX_WIDTH;
            for (int x = 0; x < _render_w; ++x) {
                dst[x] = src[s_gb_buffers->render_x_map[x]];
            }
        }
        GetHAL().display.pushImage(_render_x, _render_y + y, _render_w, rows, s_gb_buffers->push_buffer.data());
    }
    _perf_draw_ms_total += GetHAL().millis() - drawStart;
    ++_perf_drawn_frames;
}

void AppPokemonYellow::runCgbEmulatorFrame()
{
    if (!_cgb_core || !_cgb_framebuffer) {
        return;
    }

    int64_t nowUs = esp_timer_get_time();
    if (_next_emulation_frame_due_us == 0) {
        _next_emulation_frame_due_us = nowUs;
    } else if (nowUs < _next_emulation_frame_due_us) {
        const int64_t waitUs = _next_emulation_frame_due_us - nowUs;
        if (waitUs >= 1000) {
            vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(waitUs / 1000)));
        }
    }

    const uint32_t frameStart = GetHAL().millis();
    bool drawThisFrame = false;
    if (_next_video_frame_due == 0 || frameStart >= _next_video_frame_due) {
        drawThisFrame = true;
        _next_video_frame_due = frameStart + _video_frame_interval_ms;
    }

    int sampleCount = 0;
    _cgb_core->SetSoundMute(!_sound_enabled);
    _cgb_core->RunToVBlank(_cgb_framebuffer, s_gb_buffers->cgb_audio_stereo_buffer.data(), &sampleCount, false, nullptr);
    if (_sound_enabled && sampleCount > 1) {
        const size_t frames = std::min(static_cast<size_t>(sampleCount / 2), s_gb_buffers->gearboy_cgb_audio_mono_buffer.size());
        for (size_t i = 0; i < frames; ++i) {
            const int32_t left = s_gb_buffers->cgb_audio_stereo_buffer[i * 2];
            const int32_t right = s_gb_buffers->cgb_audio_stereo_buffer[i * 2 + 1];
            int32_t mixed = ((left + right) / 2) * AUDIO_OUTPUT_GAIN_NUM / AUDIO_OUTPUT_GAIN_DEN;
            mixed = std::clamp<int32_t>(mixed, INT16_MIN, INT16_MAX);
            s_gb_buffers->gearboy_cgb_audio_mono_buffer[i] = static_cast<int16_t>(mixed);
        }
        GetHAL().speaker.playRaw(s_gb_buffers->gearboy_cgb_audio_mono_buffer.data(), frames, AUDIO_SAMPLE_RATE, false, 1, 0, false);
    }

    ++_perf_emulated_frames;
    const uint32_t frameElapsed = GetHAL().millis() - frameStart;
    _perf_frame_ms_total += frameElapsed;
    if (frameElapsed > _perf_frame_ms_max) {
        _perf_frame_ms_max = frameElapsed;
    }
    if (drawThisFrame) {
        renderCgbEmulatorFrame();
    } else {
        ++_perf_skipped_video_frames;
    }

    _next_emulation_frame_due_us += GB_EMULATION_FRAME_PERIOD_US;
    nowUs = esp_timer_get_time();
    if (nowUs - _next_emulation_frame_due_us > GB_EMULATION_FRAME_PERIOD_US * 2) {
        _next_emulation_frame_due_us = nowUs;
    }

    const uint32_t now = GetHAL().millis();
    if (_perf_window_start == 0) {
        _perf_window_start = now;
    } else if (now - _perf_window_start >= 3000) {
        const uint32_t elapsed = now - _perf_window_start;
        const uint32_t emuFps = _perf_emulated_frames * 1000 / elapsed;
        _video_frame_interval_ms = emuFps < 45 ? GB_VIDEO_STRESS_INTERVAL_MS : GB_VIDEO_NORMAL_INTERVAL_MS;
        mclog::tagInfo(getAppInfo().name,
                       "perf: emu_fps={} draw_fps={} skip={} video_ms={} frame_avg={} frame_max={} draw_ms={} sound={} mode=gearboy-cgb heap={}",
                       static_cast<unsigned>(emuFps),
                       static_cast<unsigned>(_perf_drawn_frames * 1000 / elapsed),
                       static_cast<unsigned>(_perf_skipped_video_frames),
                       static_cast<unsigned>(_video_frame_interval_ms),
                       static_cast<unsigned>(_perf_emulated_frames ? _perf_frame_ms_total / _perf_emulated_frames : 0),
                       static_cast<unsigned>(_perf_frame_ms_max),
                       static_cast<unsigned>(_perf_draw_ms_total),
                       _sound_enabled ? "on" : "off",
                       static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        _perf_window_start = now;
        _perf_emulated_frames = 0;
        _perf_drawn_frames = 0;
        _perf_skipped_video_frames = 0;
        _perf_frame_ms_total = 0;
        _perf_frame_ms_max = 0;
        _perf_draw_ms_total = 0;
    }

    _emulator_frame_ready = true;
    flushSaveRam(false);
    GetHAL().feedTheDog();
}

bool AppPokemonYellow::allocateCgbFramebuffer()
{
    if (_cgb_framebuffer) {
        return true;
    }
    if (!ensureGameBoyBuffers()) {
        return false;
    }

    const size_t bytes = s_gb_buffers->cgb_framebuffer_storage.size() * sizeof(uint16_t);
    _cgb_framebuffer = s_gb_buffers->cgb_framebuffer_storage.data();
    std::fill(_cgb_framebuffer, _cgb_framebuffer + GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT, TFT_BLACK);
    mclog::tagInfo(getAppInfo().name, "CGB framebuffer ready: {} static bytes", static_cast<unsigned>(bytes));
    return true;
}

void AppPokemonYellow::releaseCgbFramebuffer()
{
    if (!_cgb_framebuffer) {
        return;
    }
    _cgb_framebuffer = nullptr;
    mclog::tagInfo(getAppInfo().name, "CGB framebuffer detached");
}

bool AppPokemonYellow::allocateFramebuffer()
{
    if (_framebuffer) {
        return true;
    }
    if (!ensureGameBoyBuffers()) {
        return false;
    }

    const size_t bytes = GB_FRAMEBUFFER_PIXELS;
    const size_t storageBytes = s_gb_buffers->cgb_framebuffer_storage.size() * sizeof(uint16_t);
    if (bytes > storageBytes) {
        mclog::tagError(getAppInfo().name,
                        "static framebuffer storage too small: need={} have={}",
                        static_cast<unsigned>(bytes),
                        static_cast<unsigned>(storageBytes));
        return false;
    }

    _framebuffer = reinterpret_cast<uint8_t*>(s_gb_buffers->cgb_framebuffer_storage.data());
    std::fill(_framebuffer, _framebuffer + GB_FRAMEBUFFER_PIXELS, 0);
    mclog::tagInfo(getAppInfo().name, "framebuffer ready: {} static bytes", static_cast<unsigned>(bytes));
    return true;
}

void AppPokemonYellow::releaseFramebuffer()
{
    if (!_framebuffer) {
        return;
    }
    _framebuffer = nullptr;
    mclog::tagInfo(getAppInfo().name, "framebuffer detached");
}

bool AppPokemonYellow::allocateRomCache()
{
    if (_rom_cache_slot_count > 0) {
        return true;
    }

    std::fill(_rom_bank_cache.begin(), _rom_bank_cache.end(), nullptr);
    std::fill(_rom_bank_cache_index.begin(), _rom_bank_cache_index.end(), -1);
    std::fill(_rom_bank_cache_stamp.begin(), _rom_bank_cache_stamp.end(), 0);

    for (size_t slot = 0; slot < kMaxRomCacheSlotCount; ++slot) {
        auto* bank = static_cast<uint8_t*>(
            heap_caps_malloc(GB_ROM_BANK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!bank) {
            break;
        }
        std::fill(bank, bank + GB_ROM_BANK_SIZE, 0xFF);
        _rom_bank_cache[slot] = bank;
        ++_rom_cache_slot_count;
    }

    if (_rom_cache_slot_count < 2) {
        releaseRomCache();
        return false;
    }

    mclog::tagInfo(getAppInfo().name,
                   "ROM bank cache allocated: {} split slots ({} bytes)",
                   static_cast<unsigned>(_rom_cache_slot_count),
                   static_cast<unsigned>(_rom_cache_slot_count * GB_ROM_BANK_SIZE));
    return true;
}

void AppPokemonYellow::releaseRomCache()
{
    if (_rom_cache_slot_count == 0) {
        return;
    }
    for (auto*& bank : _rom_bank_cache) {
        if (bank) {
            heap_caps_free(bank);
            bank = nullptr;
        }
    }
    _rom_cache_slot_count = 0;
    std::fill(_rom_bank_cache_index.begin(), _rom_bank_cache_index.end(), -1);
    std::fill(_rom_bank_cache_stamp.begin(), _rom_bank_cache_stamp.end(), 0);
    mclog::tagInfo(getAppInfo().name, "ROM bank cache released");
}

bool AppPokemonYellow::prepareFlashRomCache()
{
    releaseFlashRomCache();

    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        static_cast<esp_partition_subtype_t>(0x40),
        ROM_FLASH_CACHE_LABEL);
    if (!partition) {
        mclog::tagWarn(getAppInfo().name, "ROM flash cache partition missing, using SD backend");
        return false;
    }
    if (_state.romSize == 0 || _state.romSize > partition->size - ROM_FLASH_CACHE_DATA_OFFSET) {
        mclog::tagWarn(getAppInfo().name,
                       "ROM too large for flash cache: rom={} cache_capacity={}",
                       static_cast<unsigned>(_state.romSize),
                       static_cast<unsigned>(partition->size - ROM_FLASH_CACHE_DATA_OFFSET));
        return false;
    }

    struct stat st {};
    uint32_t romMtime = 0;
    if (stat(_state.romPath.c_str(), &st) == 0) {
        romMtime = static_cast<uint32_t>(st.st_mtime);
    }

    RomFlashCacheHeader header {};
    esp_err_t err = esp_partition_read(partition, 0, &header, sizeof(header));
    const bool pathMatch = std::strncmp(header.rom_path, _state.romPath.c_str(), sizeof(header.rom_path)) == 0;
    if (err == ESP_OK &&
        header.magic == ROM_FLASH_CACHE_MAGIC &&
        header.version == ROM_FLASH_CACHE_VERSION &&
        header.rom_size == _state.romSize &&
        header.rom_mtime == romMtime &&
        pathMatch) {
        if (mapFlashRomCache(partition, header.rom_size)) {
            mclog::tagInfo(getAppInfo().name,
                           "using flash ROM cache: size={} hash=0x{:08X}",
                           static_cast<unsigned>(header.rom_size),
                           static_cast<unsigned>(header.rom_hash));
            return true;
        }
    }

    setStatus("Installing ROM cache");
    renderBrowser();
    if (!installFlashRomCache(partition, romMtime)) {
        releaseFlashRomCache();
        return false;
    }
    return mapFlashRomCache(partition, _state.romSize);
}

bool AppPokemonYellow::installFlashRomCache(const esp_partition_t* partition, uint32_t romMtime)
{
    if (!partition) {
        return false;
    }

    FILE* fp = std::fopen(_state.romPath.c_str(), "rb");
    if (!fp) {
        mclog::tagWarn(getAppInfo().name, "flash cache install open failed: {}", errno);
        return false;
    }
    std::setvbuf(fp, s_gb_buffers->rom_file_buffer.data(), _IOFBF, s_gb_buffers->rom_file_buffer.size());

    const size_t eraseSize = (ROM_FLASH_CACHE_DATA_OFFSET + _state.romSize + 4095) & ~static_cast<size_t>(4095);
    mclog::tagInfo(getAppInfo().name,
                   "installing ROM flash cache: rom='{}' size={} erase={}",
                   _state.romPath,
                   static_cast<unsigned>(_state.romSize),
                   static_cast<unsigned>(eraseSize));

    esp_err_t err = esp_partition_erase_range(partition, 0, eraseSize);
    if (err != ESP_OK) {
        std::fclose(fp);
        mclog::tagWarn(getAppInfo().name, "flash cache erase failed: {}", esp_err_to_name(err));
        return false;
    }

    size_t written = 0;
    uint32_t hash = 2166136261U;
    bool patchedGoldHeader = false;
    while (written < _state.romSize) {
        const size_t want = std::min(s_gb_buffers->rom_file_buffer.size(), _state.romSize - written);
        const size_t got = std::fread(s_gb_buffers->rom_file_buffer.data(), 1, want, fp);
        if (got == 0) {
            std::fclose(fp);
            mclog::tagWarn(getAppInfo().name, "flash cache read stopped at {}", static_cast<unsigned>(written));
            return false;
        }
        if (written == 0 && got > 0x150 &&
            forcePokemonGoldRtcMapper(reinterpret_cast<uint8_t*>(s_gb_buffers->rom_file_buffer.data()), _state.romPath)) {
            patchedGoldHeader = true;
        }
        hash = fnv1a_update(hash, reinterpret_cast<const uint8_t*>(s_gb_buffers->rom_file_buffer.data()), got);
        err = esp_partition_write(partition, ROM_FLASH_CACHE_DATA_OFFSET + written, s_gb_buffers->rom_file_buffer.data(), got);
        if (err != ESP_OK) {
            std::fclose(fp);
            mclog::tagWarn(getAppInfo().name, "flash cache write failed at {}: {}", static_cast<unsigned>(written), esp_err_to_name(err));
            return false;
        }
        written += got;
        if ((written & 0x1FFFF) == 0 || written == _state.romSize) {
            mclog::tagInfo(getAppInfo().name,
                           "flash cache install progress: {}/{}",
                           static_cast<unsigned>(written),
                           static_cast<unsigned>(_state.romSize));
        }
    }
    std::fclose(fp);

    RomFlashCacheHeader header {};
    header.magic = ROM_FLASH_CACHE_MAGIC;
    header.version = ROM_FLASH_CACHE_VERSION;
    header.rom_size = static_cast<uint32_t>(_state.romSize);
    header.rom_mtime = romMtime;
    header.rom_hash = hash;
    std::strncpy(header.rom_path, _state.romPath.c_str(), sizeof(header.rom_path) - 1);
    err = esp_partition_write(partition, 0, &header, sizeof(header));
    if (err != ESP_OK) {
        mclog::tagWarn(getAppInfo().name, "flash cache header write failed: {}", esp_err_to_name(err));
        return false;
    }

    mclog::tagInfo(getAppInfo().name,
                   "ROM flash cache installed: size={} hash=0x{:08X} patched_gold_header={}",
                   static_cast<unsigned>(_state.romSize),
                   static_cast<unsigned>(hash),
                   patchedGoldHeader ? "yes" : "no");
    return true;
}

bool AppPokemonYellow::mapFlashRomCache(const esp_partition_t* partition, size_t romSize)
{
    releaseFlashRomCache();
    if (!partition || romSize == 0) {
        return false;
    }

    const void* mapped = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    esp_err_t err = esp_partition_mmap(partition,
                                       ROM_FLASH_CACHE_DATA_OFFSET,
                                       romSize,
                                       ESP_PARTITION_MMAP_DATA,
                                       &mapped,
                                       &handle);
    if (err != ESP_OK || !mapped) {
        mclog::tagWarn(getAppInfo().name, "flash cache mmap failed: {}", esp_err_to_name(err));
        return false;
    }

    _rom_flash_partition = partition;
    _rom_flash_data = static_cast<const uint8_t*>(mapped);
    _rom_flash_mmap_handle = handle;
    _rom_flash_size = romSize;
    return true;
}

void AppPokemonYellow::releaseFlashRomCache()
{
    if (_rom_flash_mmap_handle) {
        esp_partition_munmap(_rom_flash_mmap_handle);
    }
    _rom_flash_partition = nullptr;
    _rom_flash_data = nullptr;
    _rom_flash_mmap_handle = 0;
    _rom_flash_size = 0;
}

void AppPokemonYellow::toggleSound()
{
    _sound_enabled = !_sound_enabled;
    if (_mode == Mode::Emulator) {
        if (_gnuboy_active) {
            s_gnuboy_sound_enabled = _sound_enabled;
            GetHAL().speaker.stop(0);
        } else {
            audio_set_output_enabled(_sound_enabled);
        }
        _video_frame_interval_ms = _sound_enabled ? GB_VIDEO_NORMAL_INTERVAL_MS : GB_VIDEO_FAST_INTERVAL_MS;
        _next_video_frame_due = 0;
    }
    GetHAL().getSettings().SetInt("gb_sound", _sound_enabled ? 1 : 0);
    setStatus(_sound_enabled ? "Sound on" : "Sound off");
    mclog::tagInfo(getAppInfo().name,
                   "sound toggled: {} ({})",
                   _sound_enabled ? "on" : "off",
                   _gnuboy_active ? "gnuboy" : "MiniGB APU");
}

void AppPokemonYellow::cycleGnuboyColorMode()
{
    _gnuboy_color_mode = (_gnuboy_color_mode + 1) & 7;
    gnuboy_set_color_mode(_gnuboy_color_mode);
    GetHAL().getSettings().SetInt("gb_color_mode", _gnuboy_color_mode);
    setStatus(fmt::format("Color {}", gnuboyColorModeName(_gnuboy_color_mode)));
    mclog::tagInfo(getAppInfo().name,
                   "gnuboy color mode changed: {} ({})",
                   _gnuboy_color_mode,
                   gnuboyColorModeName(_gnuboy_color_mode));
}

bool AppPokemonYellow::handleSystemShortcut(const Keyboard::KeyEvent_t& keyEvent)
{
    if (!keyEvent.state || !GetHAL().keyboard.isFnPressed()) {
        return false;
    }

    if (keyEvent.keyCode == KEY_MINUS || keyEvent.keyCode == KEY_EQUAL) {
        int brightness = GetHAL().getDeviceBrightnessPercent();
        brightness += keyEvent.keyCode == KEY_EQUAL ? 10 : -10;
        brightness = std::max(1, std::min(100, brightness));
        GetHAL().setDeviceBrightnessPercent(brightness);
        mclog::tagInfo(getAppInfo().name, "brightness shortcut: {}%", brightness);
        return true;
    }

    if (keyEvent.keyCode == KEY_LEFTBRACE || keyEvent.keyCode == KEY_RIGHTBRACE) {
        int volume = GetHAL().getDeviceVolumePercent();
        volume += keyEvent.keyCode == KEY_RIGHTBRACE ? 5 : -5;
        volume = std::max(0, std::min(100, volume));
        GetHAL().setDeviceVolumePercent(volume);
        mclog::tagInfo(getAppInfo().name, "volume shortcut: {}%", volume);
        return true;
    }

    if (keyEvent.keyCode == KEY_C) {
        cycleGnuboyColorMode();
        return true;
    }

    return false;
}

void AppPokemonYellow::emulatorTaskEntry(void* arg)
{
    static_cast<AppPokemonYellow*>(arg)->emulatorTaskLoop();
}

void AppPokemonYellow::emulatorTaskLoop()
{
    _emulator_task_running = true;
    while (!_emulator_task_stop && (_gb || _cgb_core || _gnuboy_active)) {
        if (_gnuboy_active) {
            runGnuboyEmulatorFrame();
            taskYIELD();
            continue;
        }

        if (_cgb_core) {
            runCgbEmulatorFrame();
            taskYIELD();
            continue;
        }

        int64_t nowUs = esp_timer_get_time();
        if (_next_emulation_frame_due_us == 0) {
            _next_emulation_frame_due_us = nowUs;
        } else if (nowUs < _next_emulation_frame_due_us) {
            const int64_t waitUs = _next_emulation_frame_due_us - nowUs;
            if (waitUs >= 1000) {
                vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(waitUs / 1000)));
            }
        }

        const uint32_t frameStart = GetHAL().millis();
        bool drawThisFrame = false;
        if (_next_video_frame_due == 0 || frameStart >= _next_video_frame_due) {
            drawThisFrame = true;
            _next_video_frame_due = frameStart + _video_frame_interval_ms;
        }

        _gb->direct.frame_skip = !drawThisFrame;
        _gb->display.frame_skip_count = drawThisFrame;
        _gb->gb_frame = false;
        while (!_emulator_task_stop && _gb && !_gb->gb_frame) {
            __gb_step_cpu(_gb);
        }
        ++_perf_emulated_frames;
        const uint32_t frameElapsed = GetHAL().millis() - frameStart;
        _perf_frame_ms_total += frameElapsed;
        if (frameElapsed > _perf_frame_ms_max) {
            _perf_frame_ms_max = frameElapsed;
        }
        if (!drawThisFrame) {
            ++_perf_skipped_video_frames;
        }
        _next_emulation_frame_due_us += GB_EMULATION_FRAME_PERIOD_US;
        nowUs = esp_timer_get_time();
        if (nowUs - _next_emulation_frame_due_us > GB_EMULATION_FRAME_PERIOD_US * 2) {
            _next_emulation_frame_due_us = nowUs;
        }
        const uint32_t now = GetHAL().millis();
        if (_perf_window_start == 0) {
            _perf_window_start = now;
        } else if (now - _perf_window_start >= 3000) {
            const uint32_t elapsed = now - _perf_window_start;
            const uint32_t drawn = _perf_drawn_frames;
            const uint32_t emuFps = _perf_emulated_frames * 1000 / elapsed;
            if (_sound_enabled) {
                if (emuFps < 35) {
                    _video_frame_interval_ms = GB_VIDEO_STRESS_INTERVAL_MS;
                } else if (emuFps < 52) {
                    _video_frame_interval_ms = GB_VIDEO_SLOW_INTERVAL_MS;
                } else {
                    _video_frame_interval_ms = GB_VIDEO_NORMAL_INTERVAL_MS;
                }
            } else {
                if (emuFps < 45) {
                    _video_frame_interval_ms = GB_VIDEO_SLOW_INTERVAL_MS;
                } else if (emuFps < 58) {
                    _video_frame_interval_ms = GB_VIDEO_NORMAL_INTERVAL_MS;
                } else {
                    _video_frame_interval_ms = GB_VIDEO_FAST_INTERVAL_MS;
                }
            }
            const auto romMisses = rom_miss_summary_and_reset();
            mclog::tagInfo(getAppInfo().name,
                           "perf: emu_fps={} draw_fps={} skip={} video_ms={} frame_avg={} frame_max={} draw_ms={} rom_loads={} rom_ms={} miss={} sound={}",
                           static_cast<unsigned>(emuFps),
                           static_cast<unsigned>(drawn * 1000 / elapsed),
                           static_cast<unsigned>(_perf_skipped_video_frames),
                           static_cast<unsigned>(_video_frame_interval_ms),
                           static_cast<unsigned>(_perf_emulated_frames ? _perf_frame_ms_total / _perf_emulated_frames : 0),
                           static_cast<unsigned>(_perf_frame_ms_max),
                           static_cast<unsigned>(_perf_draw_ms_total),
                           static_cast<unsigned>(_perf_rom_loads),
                           static_cast<unsigned>(_perf_rom_load_ms),
                           romMisses,
                           _sound_enabled ? "on" : "off");
            _perf_window_start = now;
            _perf_emulated_frames = 0;
            _perf_drawn_frames = 0;
            _perf_skipped_video_frames = 0;
            _perf_frame_ms_total = 0;
            _perf_frame_ms_max = 0;
            _perf_draw_ms_total = 0;
            _perf_rom_loads = 0;
            _perf_rom_load_ms = 0;
        }
        _emulator_frame_ready = true;
        taskYIELD();
    }
    _emulator_task_running = false;
    _emulator_task_handle = nullptr;
    vTaskDelete(nullptr);
}

void AppPokemonYellow::startEmulatorTask()
{
    stopEmulatorTask();
    _emulator_task_stop = false;
    _emulator_task_running = false;
    _emulator_frame_ready = false;

    auto ok = xTaskCreatePinnedToCore(emulatorTaskEntry,
                                      "gb_emu",
                                      EMULATOR_TASK_STACK_SIZE,
                                      this,
                                      EMULATOR_TASK_PRIORITY,
                                      &_emulator_task_handle,
                                      EMULATOR_TASK_CORE);
    if (ok != pdPASS) {
        _emulator_task_handle = nullptr;
        mclog::tagWarn(getAppInfo().name, "failed to create emulator task, falling back to app loop");
    } else {
        mclog::tagInfo(getAppInfo().name, "emulator task started on core {}", EMULATOR_TASK_CORE);
    }
}

void AppPokemonYellow::stopEmulatorTask()
{
    if (!_emulator_task_handle) {
        _emulator_task_stop = false;
        _emulator_task_running = false;
        return;
    }

    _emulator_task_stop = true;
    const uint32_t start = GetHAL().millis();
    while (_emulator_task_running && GetHAL().millis() - start < 500) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (_emulator_task_running) {
        mclog::tagWarn(getAppInfo().name, "emulator task stop timeout");
    }
    _emulator_task_handle = nullptr;
    _emulator_task_running = false;
    _emulator_task_stop = false;
}

bool AppPokemonYellow::loadRomBank(int bank, uint8_t* destination, size_t destinationSize)
{
    if (bank < 0 || !destination || destinationSize != GB_ROM_BANK_SIZE) {
        return false;
    }

    const uint32_t start = GetHAL().millis();
    std::fill(destination, destination + destinationSize, 0xFF);
    if (_rom_flash_data) {
        const size_t startOffset = static_cast<size_t>(bank) * GB_ROM_BANK_SIZE;
        if (startOffset >= _rom_flash_size) {
            return false;
        }
        const size_t bytes = std::min(destinationSize, _rom_flash_size - startOffset);
        std::memcpy(destination, _rom_flash_data + startOffset, bytes);
        return true;
    }
    if (!_rom_file) {
        return false;
    }

    if (std::fseek(_rom_file, static_cast<long>(bank * GB_ROM_BANK_SIZE), SEEK_SET) != 0) {
        return false;
    }

    auto bytesRead = std::fread(destination, 1, destinationSize, _rom_file);
    ++_perf_rom_loads;
    if (static_cast<size_t>(bank) < s_rom_bank_miss_counts.size() && s_rom_bank_miss_counts[bank] < UINT16_MAX) {
        ++s_rom_bank_miss_counts[bank];
    }
    _perf_rom_load_ms += GetHAL().millis() - start;
    return bytesRead > 0;
}

uint8_t AppPokemonYellow::emulatorRomRead(uint_fast32_t addr)
{
    if (_rom_flash_data) {
        return addr < _rom_flash_size ? _rom_flash_data[addr] : 0xFF;
    }

    const int bank = static_cast<int>(addr / GB_ROM_BANK_SIZE);
    const size_t offset = static_cast<size_t>(addr % GB_ROM_BANK_SIZE);

    if (bank == 0) {
        return offset < s_gb_buffers->rom_bank0.size() ? s_gb_buffers->rom_bank0[offset] : 0xFF;
    }

    if (_rom_cache_slot_count == 0) {
        return 0xFF;
    }

    ++_rom_bank_access_clock;
    for (size_t i = 0; i < _rom_cache_slot_count; ++i) {
        if (_rom_bank_cache_index[i] == bank) {
            _rom_bank_cache_stamp[i] = _rom_bank_access_clock;
            return _rom_bank_cache[i] ? _rom_bank_cache[i][offset] : 0xFF;
        }
    }

    size_t slot = 0;
    for (size_t i = 0; i < _rom_cache_slot_count; ++i) {
        if (_rom_bank_cache_index[i] < 0) {
            slot = i;
            break;
        }
        if (_rom_bank_cache_stamp[i] < _rom_bank_cache_stamp[slot]) {
            slot = i;
        }
    }

    auto* slotData = _rom_bank_cache[slot];
    if (!slotData) {
        _rom_bank_cache_index[slot] = -1;
        return 0xFF;
    }
    if (!loadRomBank(bank, slotData, GB_ROM_BANK_SIZE)) {
        _rom_bank_cache_index[slot] = -1;
        return 0xFF;
    }

    _rom_bank_cache_index[slot] = bank;
    _rom_bank_cache_stamp[slot] = _rom_bank_access_clock;
    return slotData[offset];
}

uint8_t AppPokemonYellow::emulatorSaveRead(uint_fast32_t addr) const
{
    if (addr >= _save_ram_size) {
        return 0xFF;
    }
    return s_gb_buffers->save_ram[addr];
}

void AppPokemonYellow::emulatorSaveWrite(uint_fast32_t addr, uint8_t value)
{
    if (addr >= _save_ram_size) {
        return;
    }
    if (s_gb_buffers->save_ram[addr] != value) {
        s_gb_buffers->save_ram[addr] = value;
        _save_dirty = true;
        _last_save_write = GetHAL().millis();
    }
}

void AppPokemonYellow::emulatorDrawLine(const uint8_t* pixels, uint_fast8_t line)
{
    if (line >= GB_SCREEN_HEIGHT || !_framebuffer) {
        return;
    }

    if (!_render_geometry_valid) {
        const int displayW = GetHAL().display.width();
        const int displayH = GetHAL().display.height();
        _render_w = std::min(displayW, GB_RENDER_MAX_WIDTH);
        _render_h = std::min(displayH, GB_RENDER_MAX_HEIGHT);
        _render_x = (displayW - _render_w) / 2;
        _render_y = (displayH - _render_h) / 2;
        if (_render_w > 0 && _render_w <= static_cast<int>(s_gb_buffers->render_x_map.size())) {
            for (int x = 0; x < _render_w; ++x) {
                s_gb_buffers->render_x_map[x] = static_cast<uint8_t>(x * GB_SCREEN_WIDTH / _render_w);
            }
        }
        _render_geometry_valid = true;
    }

    if (_render_w <= 0 || _render_h <= 0 || _render_w > GB_RENDER_MAX_WIDTH || _render_h > GB_RENDER_MAX_HEIGHT) {
        return;
    }

    const int frameRow = static_cast<int>(line) * _render_h / GB_SCREEN_HEIGHT;
    if (frameRow < 0 || frameRow >= _render_h) {
        return;
    }

    if (line == 0) {
        GetHAL().display.waitDMA();
        std::fill(_framebuffer, _framebuffer + GB_FRAMEBUFFER_PIXELS, 0);
    }

    uint8_t* row = _framebuffer + static_cast<size_t>(frameRow) * GB_RENDER_MAX_WIDTH;
    for (int x = 0; x < _render_w; ++x) {
        const uint8_t pixel = pixels[s_gb_buffers->render_x_map[x]];
        uint8_t paletteGroup = GB_PALETTE_GROUP_OBJ0;
        if ((pixel & LCD_PALETTE_ALL) == LCD_PALETTE_BG) {
            paletteGroup = GB_PALETTE_GROUP_BG;
        } else if ((pixel & LCD_PALETTE_ALL) == OBJ_PALETTE) {
            paletteGroup = GB_PALETTE_GROUP_OBJ1;
        }
        row[x] = static_cast<uint8_t>(paletteGroup * 4 + (pixel & LCD_COLOUR));
    }

    if (line == GB_SCREEN_HEIGHT - 1) {
        const uint32_t drawStart = GetHAL().millis();
        for (int y = 0; y < _render_h; y += GB_PUSH_CHUNK_HEIGHT) {
            const int rows = std::min(GB_PUSH_CHUNK_HEIGHT, _render_h - y);
            for (int rowIndex = 0; rowIndex < rows; ++rowIndex) {
                const uint8_t* src = _framebuffer + static_cast<size_t>(y + rowIndex) * GB_RENDER_MAX_WIDTH;
                uint16_t* dst = s_gb_buffers->push_buffer.data() + static_cast<size_t>(rowIndex) * GB_RENDER_MAX_WIDTH;
                for (int x = 0; x < _render_w; ++x) {
                    dst[x] = s_gameboy_palette[src[x] % s_gameboy_palette.size()];
                }
            }
            GetHAL().display.pushImage(_render_x, _render_y + y, _render_w, rows, s_gb_buffers->push_buffer.data());
        }
        _perf_draw_ms_total += GetHAL().millis() - drawStart;
        ++_perf_drawn_frames;
    }
}

void AppPokemonYellow::loadSaveRam(size_t saveSize)
{
    if (saveSize == 0 || saveSize > s_gb_buffers->save_ram.size()) {
        mclog::tagWarn(getAppInfo().name,
                       "unexpected save size {}, using {}",
                       static_cast<unsigned>(saveSize),
                       static_cast<unsigned>(s_gb_buffers->save_ram.size()));
        saveSize = s_gb_buffers->save_ram.size();
    }

    _save_ram_size = saveSize;
    std::fill(s_gb_buffers->save_ram.begin(), s_gb_buffers->save_ram.end(), 0xFF);
    if (_state.savePath.empty()) {
        return;
    }

    FILE* fp = std::fopen(_state.savePath.c_str(), "rb");
    if (!fp) {
        mclog::tagWarn(getAppInfo().name, "save not found, starting blank: {}", _state.savePath);
        return;
    }

    auto bytesRead = std::fread(s_gb_buffers->save_ram.data(), 1, _save_ram_size, fp);
    std::fclose(fp);
    mclog::tagInfo(getAppInfo().name, "save loaded: {} ({} bytes)", _state.savePath, static_cast<unsigned>(bytesRead));
}

void AppPokemonYellow::flushSaveRam(bool force)
{
    if (_gnuboy_active) {
        if (_state.savePath.empty()) {
            return;
        }
        if (!force && !gnuboy_sram_dirty()) {
            return;
        }
        if (!force && GetHAL().millis() - _last_save_flush < SAVE_FLUSH_INTERVAL_MS) {
            return;
        }
        const int result = gnuboy_save_sram(_state.savePath.c_str(), !force);
        _last_save_flush = GetHAL().millis();
        _save_dirty = result != 0;
        mclog::tagInfo(getAppInfo().name,
                       "gnuboy save flushed: {} result={}",
                       _state.savePath,
                       result);
        return;
    }

    if (_cgb_core) {
        if (_state.savePath.empty()) {
            return;
        }
        if (!force && !s_cgb_save_dirty_flag) {
            return;
        }
        if (!force && GetHAL().millis() - _last_save_flush < SAVE_FLUSH_INTERVAL_MS) {
            return;
        }
        _cgb_core->SaveRam(_state.savePath.c_str(), true);
        _last_save_flush = GetHAL().millis();
        _save_dirty = false;
        s_cgb_save_dirty_flag = false;
        mclog::tagInfo(getAppInfo().name, "CGB save flushed: {}", _state.savePath);
        return;
    }

    if (_save_ram_size == 0 || _state.savePath.empty()) {
        return;
    }
    if (!force && !_save_dirty) {
        return;
    }
    if (!force && GetHAL().millis() - _last_save_flush < SAVE_FLUSH_INTERVAL_MS) {
        return;
    }
    if (!force && _last_save_write != 0 && GetHAL().millis() - _last_save_write < SAVE_FLUSH_QUIET_MS) {
        return;
    }

    FILE* fp = std::fopen(_state.savePath.c_str(), "wb");
    if (!fp) {
        mclog::tagError(getAppInfo().name, "save flush failed: {}", _state.savePath);
        return;
    }
    std::fwrite(s_gb_buffers->save_ram.data(), 1, _save_ram_size, fp);
    std::fclose(fp);
    _last_save_flush = GetHAL().millis();
    _save_dirty = false;
    mclog::tagInfo(getAppInfo().name, "save flushed: {} ({} bytes)", _state.savePath, static_cast<unsigned>(_save_ram_size));
}

void AppPokemonYellow::setJoypadButton(uint8_t button, bool pressed)
{
    if (_gnuboy_active) {
        int mapped = 0;
        switch (button) {
            case JOYPAD_A:
                mapped = GB_PAD_A;
                break;
            case JOYPAD_B:
                mapped = GB_PAD_B;
                break;
            case JOYPAD_SELECT:
                mapped = GB_PAD_SELECT;
                break;
            case JOYPAD_START:
                mapped = GB_PAD_START;
                break;
            case JOYPAD_RIGHT:
                mapped = GB_PAD_RIGHT;
                break;
            case JOYPAD_LEFT:
                mapped = GB_PAD_LEFT;
                break;
            case JOYPAD_UP:
                mapped = GB_PAD_UP;
                break;
            case JOYPAD_DOWN:
                mapped = GB_PAD_DOWN;
                break;
            default:
                return;
        }
        if (pressed) {
            _gnuboy_pad |= mapped;
        } else {
            _gnuboy_pad &= ~mapped;
        }
        gnuboy_set_pad(_gnuboy_pad);
        return;
    }

    if (_cgb_core) {
        Gameboy_Keys key = A_Key;
        switch (button) {
            case JOYPAD_A:
                key = A_Key;
                break;
            case JOYPAD_B:
                key = B_Key;
                break;
            case JOYPAD_SELECT:
                key = Select_Key;
                break;
            case JOYPAD_START:
                key = Start_Key;
                break;
            case JOYPAD_RIGHT:
                key = Right_Key;
                break;
            case JOYPAD_LEFT:
                key = Left_Key;
                break;
            case JOYPAD_UP:
                key = Up_Key;
                break;
            case JOYPAD_DOWN:
                key = Down_Key;
                break;
            default:
                return;
        }
        if (pressed) {
            _cgb_core->KeyPressed(key);
        } else {
            _cgb_core->KeyReleased(key);
        }
        return;
    }

    if (!_gb) {
        return;
    }
    if (pressed) {
        _gb->direct.joypad &= ~button;
    } else {
        _gb->direct.joypad |= button;
    }
}

void AppPokemonYellow::updateExternalInput()
{
    const auto now = GetHAL().millis();
    if (now - _external_input_last_poll < EXTERNAL_INPUT_POLL_INTERVAL_MS) {
        return;
    }
    _external_input_last_poll = now;

    const bool connected = GetHAL().externalInput.isConnected();
    const uint8_t buttons = GetHAL().externalInput.getButtons();
    if (!connected && buttons == 0) {
        if (_external_pad_buttons != 0) {
            applyExternalPadButtons(0);
        }
        _external_pad_last_buttons = 0;
        _external_pad_buttons = 0;
        return;
    }

    if (_mode == Mode::Emulator) {
        applyExternalPadButtons(buttons);
    } else {
        handleBrowserExternalInput(buttons);
    }

    _external_pad_last_buttons = _external_pad_buttons;
    _external_pad_buttons = buttons;
}

void AppPokemonYellow::probeExternalInput()
{
    const auto previousJoystick = _external_joystick_type;
    const bool previousButtons = _external_buttons_connected;
    auto* previousBus = _external_i2c;
    const auto now = GetHAL().millis();

    M5.Ex_I2C.begin();

    auto probeBus = [this](m5::I2C_Class& bus) {
        ExternalJoystickType joystick = ExternalJoystickType::None;
        if (bus.scanID(JOYSTICK2_ADDR, EXTERNAL_INPUT_I2C_FREQ)) {
            joystick = ExternalJoystickType::Joystick2;
        } else if (bus.scanID(JOYSTICK_UNIT_ADDR, EXTERNAL_INPUT_I2C_FREQ)) {
            joystick = ExternalJoystickType::JoystickUnit;
        }

        const bool byteButtons = bus.scanID(BYTE_BUTTON_ADDR, EXTERNAL_INPUT_I2C_FREQ);
        if (joystick == ExternalJoystickType::None && !byteButtons) {
            return false;
        }

        _external_i2c = &bus;
        _external_joystick_type = joystick;
        _external_buttons_connected = byteButtons;
        return true;
    };

    _external_i2c = nullptr;
    _external_buttons_connected = false;
    if (!probeBus(M5.Ex_I2C) && !probeBus(M5.In_I2C)) {
        _external_joystick_type = ExternalJoystickType::None;
    }
    _external_input_last_probe = now;

    const bool stateChanged = previousJoystick != _external_joystick_type ||
                              previousButtons != _external_buttons_connected ||
                              previousBus != _external_i2c;
    const bool shouldLogScan = stateChanged ||
                               !_external_input_scan_logged ||
                               now - _external_input_last_scan_log >= EXTERNAL_INPUT_SCAN_LOG_INTERVAL_MS;

    if (shouldLogScan) {
        _external_input_scan_logged = true;
        _external_input_last_scan_log = now;
        mclog::tagInfo(getAppInfo().name,
                       "external i2c scan: ex=[{}] in=[{}]",
                       scanI2CBus(M5.Ex_I2C),
                       scanI2CBus(M5.In_I2C));
    }

    if (stateChanged || shouldLogScan) {
        const char* joystickName = "none";
        if (_external_joystick_type == ExternalJoystickType::JoystickUnit) {
            joystickName = "joystick-unit";
        } else if (_external_joystick_type == ExternalJoystickType::Joystick2) {
            joystickName = "joystick2";
        }
        mclog::tagInfo(getAppInfo().name,
                       "external input: bus={} joystick={} byte_buttons={}",
                       _external_i2c == &M5.Ex_I2C ? "ex" : (_external_i2c == &M5.In_I2C ? "in" : "none"),
                       joystickName,
                       _external_buttons_connected ? "yes" : "no");
    }
}

std::string AppPokemonYellow::scanI2CBus(m5::I2C_Class& bus)
{
    std::string addresses;
    for (uint8_t address = 0x08; address < 0x78; ++address) {
        if (!bus.scanID(address, EXTERNAL_INPUT_I2C_FREQ)) {
            continue;
        }
        if (!addresses.empty()) {
            addresses += ",";
        }
        addresses += fmt::format("0x{:02X}", address);
    }
    if (addresses.empty()) {
        return "none";
    }
    return addresses;
}

bool AppPokemonYellow::readExternalInput(uint8_t& buttons)
{
    buttons = 0;
    const auto now = GetHAL().millis();
    if (_external_joystick_type == ExternalJoystickType::None && !_external_buttons_connected &&
        now - _external_input_last_probe >= EXTERNAL_INPUT_PROBE_INTERVAL_MS) {
        probeExternalInput();
    }

    bool connected = false;
    uint8_t joystickButtons = 0;
    if (_external_joystick_type == ExternalJoystickType::JoystickUnit) {
        if (readJoystickUnit(joystickButtons)) {
            buttons |= joystickButtons;
            connected = true;
        } else {
            mclog::tagWarn(getAppInfo().name, "joystick-unit read failed");
            _external_joystick_type = ExternalJoystickType::None;
        }
    } else if (_external_joystick_type == ExternalJoystickType::Joystick2) {
        if (readJoystick2(joystickButtons)) {
            buttons |= joystickButtons;
            connected = true;
        } else {
            mclog::tagWarn(getAppInfo().name, "joystick2 read failed");
            _external_joystick_type = ExternalJoystickType::None;
        }
    }

    uint8_t byteButtons = 0;
    if (_external_buttons_connected) {
        if (readByteButtons(byteButtons)) {
            buttons |= byteButtons;
            connected = true;
        } else {
            mclog::tagWarn(getAppInfo().name, "byte buttons read failed");
            _external_buttons_connected = false;
        }
    }

    if (!connected && now - _external_input_last_probe >= EXTERNAL_INPUT_PROBE_INTERVAL_MS) {
        probeExternalInput();
    }
    return connected;
}

bool AppPokemonYellow::readJoystickUnit(uint8_t& buttons)
{
    buttons = 0;
    if (!_external_i2c) {
        return false;
    }
    uint8_t data[3] = {};
    if (!_external_i2c->start(JOYSTICK_UNIT_ADDR, true, EXTERNAL_INPUT_I2C_FREQ)) {
        return false;
    }
    const bool ok = _external_i2c->read(data, sizeof(data), true);
    _external_i2c->stop();
    if (!ok) {
        return false;
    }

    const int x = data[0];
    const int y = data[1];
    if (x < JOYSTICK_UNIT_CENTER - JOYSTICK_UNIT_DEAD_ZONE) {
        buttons |= EXT_PAD_LEFT;
    } else if (x > JOYSTICK_UNIT_CENTER + JOYSTICK_UNIT_DEAD_ZONE) {
        buttons |= EXT_PAD_RIGHT;
    }
    if (y < JOYSTICK_UNIT_CENTER - JOYSTICK_UNIT_DEAD_ZONE) {
        buttons |= EXT_PAD_UP;
    } else if (y > JOYSTICK_UNIT_CENTER + JOYSTICK_UNIT_DEAD_ZONE) {
        buttons |= EXT_PAD_DOWN;
    }
    if (data[2] != 0) {
        buttons |= EXT_PAD_A;
    }
    return true;
}

bool AppPokemonYellow::readJoystick2(uint8_t& buttons)
{
    buttons = 0;
    if (!_external_i2c) {
        return false;
    }
    uint8_t offsets[2] = {};
    if (!_external_i2c->readRegister(JOYSTICK2_ADDR,
                                     JOYSTICK2_OFFSET_ADC_VALUE_8BITS_REG,
                                     offsets,
                                     sizeof(offsets),
                                     EXTERNAL_INPUT_I2C_FREQ)) {
        return false;
    }

    const int x = static_cast<int8_t>(offsets[0]);
    const int y = static_cast<int8_t>(offsets[1]);
    if (x < -JOYSTICK2_DEAD_ZONE) {
        buttons |= EXT_PAD_LEFT;
    } else if (x > JOYSTICK2_DEAD_ZONE) {
        buttons |= EXT_PAD_RIGHT;
    }
    if (y < -JOYSTICK2_DEAD_ZONE) {
        buttons |= EXT_PAD_UP;
    } else if (y > JOYSTICK2_DEAD_ZONE) {
        buttons |= EXT_PAD_DOWN;
    }

    uint8_t button = 1;
    if (_external_i2c->readRegister(JOYSTICK2_ADDR, JOYSTICK2_BUTTON_REG, &button, 1, EXTERNAL_INPUT_I2C_FREQ) &&
        button == 0) {
        buttons |= EXT_PAD_A;
    }
    return true;
}

bool AppPokemonYellow::readByteButtons(uint8_t& buttons)
{
    buttons = 0;
    if (!_external_i2c) {
        return false;
    }
    uint8_t status[8] = {};
    if (!_external_i2c->readRegister(BYTE_BUTTON_ADDR,
                                     BYTE_BUTTON_STATUS_8BYTE_REG,
                                     status,
                                     sizeof(status),
                                     EXTERNAL_INPUT_I2C_FREQ)) {
        return false;
    }

    if (status[0]) {
        buttons |= EXT_PAD_B;
    }
    if (status[1]) {
        buttons |= EXT_PAD_A;
    }
    if (status[2]) {
        buttons |= EXT_PAD_SELECT;
    }
    if (status[3]) {
        buttons |= EXT_PAD_START;
    }
    if (status[4]) {
        buttons |= EXT_PAD_UP;
    }
    if (status[5]) {
        buttons |= EXT_PAD_DOWN;
    }
    if (status[6]) {
        buttons |= EXT_PAD_LEFT;
    }
    if (status[7]) {
        buttons |= EXT_PAD_RIGHT;
    }
    return true;
}

void AppPokemonYellow::applyExternalPadButtons(uint8_t buttons)
{
    const uint8_t changed = buttons ^ _external_pad_buttons;
    if (changed == 0) {
        return;
    }

    auto apply = [this, buttons, changed](uint8_t externalButton, uint8_t joypadButton) {
        if ((changed & externalButton) == 0) {
            return;
        }
        setJoypadButton(joypadButton, (buttons & externalButton) != 0);
    };

    apply(EXT_PAD_UP, JOYPAD_UP);
    apply(EXT_PAD_DOWN, JOYPAD_DOWN);
    apply(EXT_PAD_LEFT, JOYPAD_LEFT);
    apply(EXT_PAD_RIGHT, JOYPAD_RIGHT);
    apply(EXT_PAD_A, JOYPAD_A);
    apply(EXT_PAD_B, JOYPAD_B);
    apply(EXT_PAD_SELECT, JOYPAD_SELECT);
    apply(EXT_PAD_START, JOYPAD_START);
}

void AppPokemonYellow::handleBrowserExternalInput(uint8_t buttons)
{
    const uint8_t pressed = buttons & ~_external_pad_buttons;
    if (pressed == 0) {
        return;
    }

    if (pressed & EXT_PAD_UP) {
        selectRom(-1);
        setStatus("Selected");
        render();
    } else if (pressed & EXT_PAD_DOWN) {
        selectRom(1);
        setStatus("Selected");
        render();
    } else if (pressed & (EXT_PAD_A | EXT_PAD_START)) {
        activateSelectedRom();
        createSaveIfNeeded();
        if (_state.romFound && !_state.savePath.empty()) {
            _state.saveFound = fileExists(_state.savePath.c_str(), &_state.saveSize);
        }
        setStatus("Launching");
        _pending_launch = true;
        render();
    } else if (pressed & EXT_PAD_SELECT) {
        toggleSound();
        render();
    }
}

void AppPokemonYellow::createSaveIfNeeded()
{
    auto saveStart = GetHAL().millis();
    if (!_state.sdMounted) {
        setStatus("No SD card");
        mclog::tagWarn(getAppInfo().name, "save skipped: no SD card");
        return;
    }
    if (!_state.romFound || _state.savePath.empty()) {
        setStatus("Select ROM first");
        mclog::tagWarn(getAppInfo().name, "save skipped: no ROM selected");
        return;
    }
    if (_state.saveFound) {
        setStatus("Save already exists");
        mclog::tagInfo(getAppInfo().name, "save already exists: {}", _state.savePath);
        return;
    }

    createDirectories();

    mclog::tagInfo(getAppInfo().name, "creating save: {}", _state.savePath);
    FILE* fp = std::fopen(_state.savePath.c_str(), "wb");
    if (!fp) {
        setStatus("Save create failed");
        mclog::tagError(getAppInfo().name, "save create failed: {}", _state.savePath);
        return;
    }

    uint8_t blank[256];
    std::memset(blank, 0xFF, sizeof(blank));
    for (size_t written = 0; written < DEFAULT_SAVE_SIZE; written += sizeof(blank)) {
        std::fwrite(blank, 1, sizeof(blank), fp);
    }
    std::fclose(fp);
    setStatus("Save created");
    mclog::tagInfo(getAppInfo().name,
                   "save created: {} ({} bytes) elapsed={}ms",
                   _state.savePath,
                   DEFAULT_SAVE_SIZE,
                   GetHAL().millis() - saveStart);
}

void AppPokemonYellow::createDirectories()
{
    mkdir("/sdcard/roms", 0775);
    mkdir("/sdcard/saves", 0775);
}

bool AppPokemonYellow::scanRomDirectory(const char* directory)
{
    mclog::tagInfo(getAppInfo().name, "scanning ROM directory: {}", directory);
    DIR* dir = opendir(directory);
    if (!dir) {
        mclog::tagWarn(getAppInfo().name, "cannot open ROM directory: {}", directory);
        return false;
    }

    bool found = false;
    while (auto* entry = readdir(dir)) {
        mclog::tagInfo(getAppInfo().name, "dir entry: {}", entry->d_name);
        if (!isSupportedRomName(entry->d_name)) {
            continue;
        }

        addRomEntry(directory, entry->d_name);
        found = true;
    }

    closedir(dir);
    return found;
}

void AppPokemonYellow::addRomEntry(const char* directory, const char* name)
{
    auto path = std::string(directory) + "/" + name;
    size_t size = 0;
    if (!fileExists(path.c_str(), &size)) {
        return;
    }

    for (const auto& rom : _roms) {
        if (strcasecmp(rom.name.c_str(), name) == 0 && rom.size == size) {
            return;
        }
    }

    RomEntry entry;
    entry.name = name;
    entry.path = path;
    entry.size = size;
    entry.savePath = makeSavePath(entry.name);
    entry.saveFound = fileExists(entry.savePath.c_str(), &entry.saveSize);
    _roms.push_back(entry);
    mclog::tagInfo(getAppInfo().name, "ROM listed: {} -> {} ({} bytes)", entry.name, entry.savePath, entry.size);
}

void AppPokemonYellow::selectRom(int delta)
{
    if (_roms.empty()) {
        return;
    }

    _selected_rom += delta;
    if (_selected_rom < 0) {
        _selected_rom = static_cast<int>(_roms.size()) - 1;
    } else if (_selected_rom >= static_cast<int>(_roms.size())) {
        _selected_rom = 0;
    }
    activateSelectedRom();
    if (_state.romFound) {
        mclog::tagInfo(getAppInfo().name, "selected ROM index={} name='{}'", _selected_rom, _roms[_selected_rom].name);
    }
}

void AppPokemonYellow::activateSelectedRom()
{
    if (_roms.empty() || _selected_rom < 0 || _selected_rom >= static_cast<int>(_roms.size())) {
        _state.romFound = false;
        return;
    }

    const auto& rom = _roms[_selected_rom];
    _state.romFound = true;
    _state.romPath = rom.path;
    _state.romSize = rom.size;
    _state.savePath = rom.savePath;
    _state.saveFound = fileExists(_state.savePath.c_str(), &_state.saveSize);
    setStatus("Selected ROM");
    mclog::tagInfo(getAppInfo().name,
                   "active ROM: index={} path='{}' size={} save='{}' save_found={} save_size={}",
                   _selected_rom,
                   _state.romPath,
                   static_cast<unsigned>(_state.romSize),
                   _state.savePath,
                   _state.saveFound ? "yes" : "no",
                   static_cast<unsigned>(_state.saveSize));
}

bool AppPokemonYellow::fileExists(const char* path, size_t* size) const
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (size) {
        *size = static_cast<size_t>(st.st_size);
    }
    return true;
}

bool AppPokemonYellow::isSupportedRomName(const char* name) const
{
    auto length = std::strlen(name);
    if (length < 4) {
        return false;
    }
    if (name[0] == '.' || name[0] == '_') {
        return false;
    }

    auto hasSuffix = [name, length](const char* suffix) {
        auto suffixLength = std::strlen(suffix);
        if (length < suffixLength) {
            return false;
        }
        return strcasecmp(name + length - suffixLength, suffix) == 0;
    };

    return hasSuffix(".gbc") || hasSuffix(".gb") || hasSuffix(".gba");
}

std::string AppPokemonYellow::makeSavePath(const std::string& romName) const
{
    auto dot = romName.find_last_of('.');
    auto base = dot == std::string::npos ? romName : romName.substr(0, dot);
    return "/sdcard/saves/" + base + ".sav";
}

void AppPokemonYellow::printClipped(const std::string& text, int maxChars)
{
    if (static_cast<int>(text.size()) <= maxChars) {
        GetHAL().canvas.print(text.c_str());
        return;
    }

    auto clipped = text.substr(0, std::max(0, maxChars - 3)) + "...";
    GetHAL().canvas.print(clipped.c_str());
}

void AppPokemonYellow::setStatus(const std::string& status)
{
    _status = status;
    mclog::tagInfo(getAppInfo().name, "status: {}", _status);
}
