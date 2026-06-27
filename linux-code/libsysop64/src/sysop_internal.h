/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#ifndef SYSOP_INTERNAL_H
#define SYSOP_INTERNAL_H

#include <error.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>

#include "sysop64.h"

extern uint8_t *sysop64_cmd_address;
extern uint8_t *sysop64_cmd3_map;
extern uint8_t *sysop64_cmd3_result_map;
extern uint8_t *sysop64_hdmi_info_result_map;
extern uint8_t *sysop64_sid_voices_data_map;
extern uint8_t *sysop64_gpio_data_map;
extern uint8_t *sysop64_dma_read_key_data_map;
extern uint8_t *sysop64_dma_tag_data_map;
extern uint8_t *sysop64_dma_joystick_data_map;
extern uint8_t *sysop64_audio_status_map;
extern uint8_t *sysop64_audio_command_map;
extern uint8_t *sysop64_bridge_map;
extern uint8_t *sysop64_hdmi_cmd_data_map;
extern uint8_t *sysop64_cmd3_param_map;
extern volatile uint8_t *sysop64_phi2_counter_map;
extern volatile uint8_t *sysop64_debug_data1_map;

void get_library_lock(void);
void release_library_lock(void);
void dma_wait_not_full(void);
int sysop_bridge_fd(void);

#endif /* SYSOP_INTERNAL_H */
