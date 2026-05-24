/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
// https://github.com/espressif/esp-idf/blob/v5.4.2/examples/peripherals/rmt/ir_nec_transceiver
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <driver/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

void ir_helper_init(gpio_num_t pin_tx);
void ir_helper_send(uint8_t addr, uint8_t cmd);
void ir_helper_send_xiaomi(uint8_t device, uint8_t function, uint8_t repeats);
void ir_helper_send_raw(uint32_t carrier_hz, const uint32_t* durations_us, size_t duration_count);

#ifdef __cplusplus
}
#endif
