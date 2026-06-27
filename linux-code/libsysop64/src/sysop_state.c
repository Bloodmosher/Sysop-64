/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

volatile uint8_t *sysop64_fpga_status_map = NULL;
uint64_t timestamp = 0;

// TODO: rename this as sysop_peek
volatile uint8_t *sysop64_dma_address_map = NULL;
volatile uint8_t *sysop64_dma_data_map = NULL;

uint8_t *sysop64_poke_dma_address_map = NULL;
uint8_t *sysop64_poke_dma_data_map = NULL;

uint8_t *sysop64_internal_read_address_map = NULL;
uint8_t *sysop64_internal_read_data_map = NULL;

uint8_t *sysop64_c64_signals_map = NULL;

uint8_t *sysop64_cmd_address = NULL;
uint8_t *sysop64_cmd2_map = NULL;
uint8_t *sysop64_set_palette_map = NULL;
uint8_t *sysop64_cmd3_map = NULL;
uint8_t *sysop64_cmd3_result_map = NULL;
uint8_t *sysop64_hdmi_info_result_map = NULL;
uint8_t *sysop64_sid_voices_data_map = NULL;
uint8_t *sysop64_gpio_data_map = NULL;
uint8_t *sysop64_dma_read_key_data_map = NULL;
uint8_t *sysop64_dma_tag_data_map = NULL;
uint8_t *sysop64_dma_joystick_data_map = NULL;
uint8_t *sysop64_audio_status_map = NULL;
uint8_t *sysop64_audio_command_map = NULL;

uint8_t *sysop64_bridge_map = NULL;
uint8_t *sysop64_hdmi_cmd_data_map = NULL;
uint8_t *sysop64_cmd3_param_map = NULL;
volatile uint8_t *sysop64_phi2_counter_map = NULL;
volatile uint8_t *sysop64_debug_data1_map = NULL;
