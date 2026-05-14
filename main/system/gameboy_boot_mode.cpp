#include "gameboy_boot_mode.h"

#include <esp_attr.h>
#include <esp_system.h>
#include <cstdint>

static constexpr uint32_t GAMEBOY_BOOT_MAGIC = 0x47424D44;  // GBMD
static constexpr uint32_t GAMEBOY_BOOT_EXIT_MAGIC = 0x47425854;  // GBXT
static constexpr bool GAMEBOY_FORCE_AUTO_BOOT = true;

RTC_NOINIT_ATTR static uint32_t s_gameboy_boot_magic;
RTC_NOINIT_ATTR static uint32_t s_gameboy_boot_magic_inv;
RTC_NOINIT_ATTR static uint32_t s_gameboy_boot_exit_magic;
RTC_NOINIT_ATTR static uint32_t s_gameboy_boot_exit_magic_inv;

bool gameboy_boot_mode_is_active()
{
    const bool forceDisabled =
        s_gameboy_boot_exit_magic == GAMEBOY_BOOT_EXIT_MAGIC &&
        s_gameboy_boot_exit_magic_inv == ~GAMEBOY_BOOT_EXIT_MAGIC;
    if (GAMEBOY_FORCE_AUTO_BOOT && !forceDisabled) {
        return true;
    }
    if (esp_reset_reason() != ESP_RST_SW) {
        gameboy_boot_mode_exit();
        return false;
    }
    return s_gameboy_boot_magic == GAMEBOY_BOOT_MAGIC && s_gameboy_boot_magic_inv == ~GAMEBOY_BOOT_MAGIC;
}

void gameboy_boot_mode_enter()
{
    s_gameboy_boot_magic = GAMEBOY_BOOT_MAGIC;
    s_gameboy_boot_magic_inv = ~GAMEBOY_BOOT_MAGIC;
    s_gameboy_boot_exit_magic = 0;
    s_gameboy_boot_exit_magic_inv = 0;
}

void gameboy_boot_mode_exit()
{
    s_gameboy_boot_magic = 0;
    s_gameboy_boot_magic_inv = 0;
    s_gameboy_boot_exit_magic = GAMEBOY_BOOT_EXIT_MAGIC;
    s_gameboy_boot_exit_magic_inv = ~GAMEBOY_BOOT_EXIT_MAGIC;
}
