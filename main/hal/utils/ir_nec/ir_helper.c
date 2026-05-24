/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "ir_helper.h"
#include "../../hal_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "ir_nec_encoder.h"

#define EXAMPLE_IR_RESOLUTION_HZ     1000000  // 1MHz resolution, 1 tick = 1us
#define EXAMPLE_IR_NEC_DECODE_MARGIN 200      // Tolerance for parsing RMT symbols into bit stream

/**
 * @brief NEC timing spec
 */
#define NEC_LEADING_CODE_DURATION_0 9000
#define NEC_LEADING_CODE_DURATION_1 4500
#define NEC_PAYLOAD_ZERO_DURATION_0 560
#define NEC_PAYLOAD_ZERO_DURATION_1 560
#define NEC_PAYLOAD_ONE_DURATION_0  560
#define NEC_PAYLOAD_ONE_DURATION_1  1690
#define NEC_REPEAT_CODE_DURATION_0  9000
#define NEC_REPEAT_CODE_DURATION_1  2250

#define XIAOMI_IR_CARRIER_HZ 36000
#define XIAOMI_IR_UNIT_US    290
#define XIAOMI_IR_SYMBOL_US  (2 * XIAOMI_IR_UNIT_US)
#define XIAOMI_IR_HEADER_US  1000
#define XIAOMI_IR_FRAME_US   30000
#define RMT_MAX_DURATION_US  0x7FFF

static const char *TAG                  = "ir";
static rmt_channel_handle_t tx_channel  = NULL;
static rmt_encoder_handle_t nec_encoder = NULL;
static rmt_encoder_handle_t raw_encoder = NULL;

static void ir_helper_apply_carrier(uint32_t frequency_hz)
{
    rmt_carrier_config_t carrier_cfg = {
        .duty_cycle   = 0.33,
        .frequency_hz = frequency_hz,
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel, &carrier_cfg));
}

static uint8_t ir_helper_xiaomi_checksum(uint8_t device, uint8_t function)
{
    return (uint8_t)(((device >> 4) ^ device ^ (function >> 4) ^ function) & 0x0F);
}

static size_t ir_helper_encode_xiaomi(uint8_t device, uint8_t function, rmt_symbol_word_t *symbols)
{
    const uint8_t checksum = ir_helper_xiaomi_checksum(device, function);
    const uint32_t payload = ((uint32_t)device << 12) | ((uint32_t)function << 4) | checksum;
    size_t index           = 0;
    uint32_t elapsed_us    = XIAOMI_IR_HEADER_US + XIAOMI_IR_SYMBOL_US;

    symbols[index++] = (rmt_symbol_word_t) {
        .duration0 = XIAOMI_IR_HEADER_US,
        .level0    = 1,
        .duration1 = XIAOMI_IR_SYMBOL_US,
        .level1    = 0,
    };

    for (int shift = 18; shift >= 0; shift -= 2) {
        const uint8_t dibit = (payload >> shift) & 0x03;
        const uint16_t space_us = (uint16_t)((2 + dibit) * XIAOMI_IR_UNIT_US);
        symbols[index++]    = (rmt_symbol_word_t) {
            .duration0 = XIAOMI_IR_SYMBOL_US,
            .level0    = 1,
            .duration1 = space_us,
            .level1    = 0,
        };
        elapsed_us += XIAOMI_IR_SYMBOL_US + space_us;
    }

    elapsed_us += XIAOMI_IR_SYMBOL_US;
    symbols[index++] = (rmt_symbol_word_t) {
        .duration0 = XIAOMI_IR_SYMBOL_US,
        .level0    = 1,
        .duration1 = (uint16_t)(elapsed_us < XIAOMI_IR_FRAME_US ? XIAOMI_IR_FRAME_US - elapsed_us
                                                                 : XIAOMI_IR_SYMBOL_US),
        .level1    = 0,
    };
    return index;
}

static size_t ir_helper_encode_raw(const uint32_t* durations_us, size_t duration_count, rmt_symbol_word_t* symbols,
                                   size_t symbol_capacity)
{
    size_t symbol_count = 0;
    for (size_t i = 0; i + 1 < duration_count && symbol_count < symbol_capacity; i += 2) {
        const uint32_t mark_us  = durations_us[i];
        const uint32_t space_us = durations_us[i + 1];
        symbols[symbol_count++] = (rmt_symbol_word_t) {
            .duration0 = (uint16_t)(mark_us > RMT_MAX_DURATION_US ? RMT_MAX_DURATION_US : mark_us),
            .level0    = 1,
            .duration1 = (uint16_t)(space_us > RMT_MAX_DURATION_US ? RMT_MAX_DURATION_US : space_us),
            .level1    = 0,
        };
    }
    return symbol_count;
}

void ir_helper_init(gpio_num_t pin_tx)
{
    ESP_LOGI(TAG, "create RMT TX channel");
    rmt_tx_channel_config_t tx_channel_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = EXAMPLE_IR_RESOLUTION_HZ,
        .mem_block_symbols = 64,  // amount of RMT symbols that the channel can store at a time
        .trans_queue_depth = 4,  // number of transactions that allowed to pending in the background, this example won't
                                 // queue multiple transactions, so queue depth > 1 is sufficient
        .gpio_num = pin_tx,
    };

    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_channel_cfg, &tx_channel));

    ESP_LOGI(TAG, "modulate carrier to TX channel");
    ir_helper_apply_carrier(38000);

    ESP_LOGI(TAG, "install IR NEC encoder");
    ir_nec_encoder_config_t nec_encoder_cfg = {
        .resolution = EXAMPLE_IR_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_ir_nec_encoder(&nec_encoder_cfg, &nec_encoder));
    rmt_copy_encoder_config_t raw_encoder_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&raw_encoder_cfg, &raw_encoder));

    ESP_LOGI(TAG, "enable RMT TX channel");
    ESP_ERROR_CHECK(rmt_enable(tx_channel));
}

void ir_helper_send(uint8_t addr, uint8_t cmd)
{
    ir_helper_apply_carrier(38000);

    ir_nec_scan_code_t scan_code = {
        .address = ((uint16_t)addr) | ((uint16_t)(~addr) << 8),  // addr + addr反码
        .command = ((uint16_t)cmd) | ((uint16_t)(~cmd) << 8),    // cmd  + cmd反码
    };

    // this example won't send NEC frames in a loop
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,  // no loop
    };

    ESP_ERROR_CHECK(rmt_transmit(tx_channel, nec_encoder, &scan_code, sizeof(scan_code), &transmit_config));
}

void ir_helper_send_xiaomi(uint8_t device, uint8_t function, uint8_t repeats)
{
    if (repeats == 0) {
        repeats = 1;
    }

    ir_helper_apply_carrier(XIAOMI_IR_CARRIER_HZ);

    rmt_symbol_word_t symbols[12];
    const size_t symbol_count = ir_helper_encode_xiaomi(device, function, symbols);
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };

    for (uint8_t i = 0; i < repeats; ++i) {
        ESP_ERROR_CHECK(rmt_transmit(tx_channel, raw_encoder, symbols, symbol_count * sizeof(symbols[0]), &transmit_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(tx_channel, pdMS_TO_TICKS(80)));
    }
}

void ir_helper_send_raw(uint32_t carrier_hz, const uint32_t* durations_us, size_t duration_count)
{
    if (!durations_us || duration_count < 2) {
        return;
    }

    ir_helper_apply_carrier(carrier_hz);

    rmt_symbol_word_t symbols[96];
    const size_t symbol_count = ir_helper_encode_raw(durations_us, duration_count, symbols,
                                                     sizeof(symbols) / sizeof(symbols[0]));
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };
    ESP_ERROR_CHECK(rmt_transmit(tx_channel, raw_encoder, symbols, symbol_count * sizeof(symbols[0]), &transmit_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(tx_channel, pdMS_TO_TICKS(300)));
}
