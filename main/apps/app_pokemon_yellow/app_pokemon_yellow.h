/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <hal/hal.h>
#include <mooncake.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include <vector>

struct gb_s;
class GearboyCore;

class AppPokemonYellow : public mooncake::AppAbility {
public:
    AppPokemonYellow();
    ~AppPokemonYellow();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

    uint8_t emulatorRomRead(uint_fast32_t addr);
    uint8_t emulatorSaveRead(uint_fast32_t addr) const;
    void emulatorSaveWrite(uint_fast32_t addr, uint8_t value);
    void emulatorDrawLine(const uint8_t* pixels, uint_fast8_t line);

private:
    static constexpr size_t kRomBankSize = 16 * 1024;
    static constexpr size_t kMaxSaveRamSize = 32 * 1024;
    static constexpr size_t kMaxRomCacheSlotCount = 6;

    struct FileState {
        bool sdMounted = false;
        bool romFound = false;
        bool saveFound = false;
        std::string romPath;
        std::string savePath;
        size_t romSize = 0;
        size_t saveSize = 0;
    };

    struct RomEntry {
        std::string name;
        std::string path;
        std::string savePath;
        size_t size = 0;
        bool saveFound = false;
        size_t saveSize = 0;
    };

    enum class Mode {
        Browser,
        Emulator,
    };

    FileState _state;
    std::vector<RomEntry> _roms;
    int _key_event_slot_id = -1;
    int _selected_rom = 0;
    uint32_t _opened_at = 0;
    uint32_t _last_probe_time = 0;
    uint32_t _last_save_flush = 0;
    uint32_t _last_save_write = 0;
    Mode _mode = Mode::Browser;
    bool _pending_probe = false;
    bool _pending_launch = false;
    bool _save_dirty = false;
    bool _sound_enabled = false;
    std::string _status;
    FILE* _rom_file = nullptr;
    const esp_partition_t* _rom_flash_partition = nullptr;
    const uint8_t* _rom_flash_data = nullptr;
    esp_partition_mmap_handle_t _rom_flash_mmap_handle = 0;
    size_t _rom_flash_size = 0;
    gb_s* _gb = nullptr;
    GearboyCore* _cgb_core = nullptr;
    TaskHandle_t _emulator_task_handle = nullptr;
    std::array<uint8_t*, kMaxRomCacheSlotCount> _rom_bank_cache = {};
    size_t _rom_cache_slot_count = 0;
    std::array<int, kMaxRomCacheSlotCount> _rom_bank_cache_index = {};
    std::array<uint32_t, kMaxRomCacheSlotCount> _rom_bank_cache_stamp = {};
    size_t _save_ram_size = 0;
    uint32_t _rom_bank_access_clock = 0;
    volatile bool _emulator_task_stop = false;
    volatile bool _emulator_task_running = false;
    volatile bool _emulator_frame_ready = false;
    bool _render_geometry_valid = false;
    bool _display_write_active = false;
    uint8_t* _framebuffer = nullptr;
    uint16_t* _cgb_framebuffer = nullptr;
    bool _cgb_save_dirty = false;
    uint32_t _perf_window_start = 0;
    uint32_t _perf_emulated_frames = 0;
    uint32_t _perf_drawn_frames = 0;
    uint32_t _perf_skipped_video_frames = 0;
    uint32_t _perf_frame_ms_total = 0;
    uint32_t _perf_frame_ms_max = 0;
    uint32_t _perf_draw_ms_total = 0;
    uint32_t _perf_rom_loads = 0;
    uint32_t _perf_rom_load_ms = 0;
    uint32_t _next_video_frame_due = 0;
    int64_t _next_emulation_frame_due_us = 0;
    uint32_t _video_frame_interval_ms = 66;
    int _render_x = 0;
    int _render_y = 0;
    int _render_w = 0;
    int _render_h = 0;

    void probe();
    void render();
    void renderBrowser();
    void renderEmulatorFrame();
    void handleKeyEvent(const Keyboard::KeyEvent_t& keyEvent);
    void handleBrowserKey(const Keyboard::KeyEvent_t& keyEvent, const Keyboard::KeyEventRaw_t& raw);
    void handleEmulatorKey(const Keyboard::KeyEvent_t& keyEvent, const Keyboard::KeyEventRaw_t& raw);
    bool isEnterKey(const Keyboard::KeyEvent_t& keyEvent, const Keyboard::KeyEventRaw_t& raw) const;
    void startEmulator();
    void startCgbEmulator();
    void stopEmulator();
    void runEmulatorFrame();
    void runCgbEmulatorFrame();
    void renderCgbEmulatorFrame();
    bool allocateFramebuffer();
    void releaseFramebuffer();
    bool allocateCgbFramebuffer();
    void releaseCgbFramebuffer();
    bool allocateRomCache();
    void releaseRomCache();
    bool prepareFlashRomCache();
    bool installFlashRomCache(const esp_partition_t* partition, uint32_t romMtime);
    bool mapFlashRomCache(const esp_partition_t* partition, size_t romSize);
    void releaseFlashRomCache();
    void toggleSound();
    bool handleSystemShortcut(const Keyboard::KeyEvent_t& keyEvent);
    static void emulatorTaskEntry(void* arg);
    void emulatorTaskLoop();
    void startEmulatorTask();
    void stopEmulatorTask();
    bool loadRomBank(int bank, uint8_t* destination, size_t destinationSize);
    void loadSaveRam(size_t saveSize);
    void flushSaveRam(bool force);
    void setJoypadButton(uint8_t button, bool pressed);
    void createSaveIfNeeded();
    void createDirectories();
    bool scanRomDirectory(const char* directory);
    void addRomEntry(const char* directory, const char* name);
    void selectRom(int delta);
    void activateSelectedRom();
    bool fileExists(const char* path, size_t* size = nullptr) const;
    bool isSupportedRomName(const char* name) const;
    std::string makeSavePath(const std::string& romName) const;
    void printClipped(const std::string& text, int maxChars);
    void setStatus(const std::string& status);
};
