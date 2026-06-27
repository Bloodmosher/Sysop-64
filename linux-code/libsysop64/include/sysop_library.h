/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#ifndef SYSOP_LIBRARY_H
#define SYSOP_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>

extern volatile uint8_t *sysop64_fpga_status_map;
extern volatile uint64_t sum;
extern uint64_t timestamp;

extern volatile uint8_t *sysop64_dma_address_map;
extern volatile uint8_t *sysop64_dma_data_map;
extern uint8_t *sysop64_internal_read_address_map;
extern uint8_t *sysop64_internal_read_data_map;
extern uint8_t *sysop64_poke_dma_address_map;
extern uint8_t *sysop64_poke_dma_data_map;
extern uint8_t *sysop64_c64_signals_map;
extern uint8_t *sysop64_cmd2_map;
extern uint8_t *sysop64_set_palette_map;

#ifdef __cplusplus
extern "C" {
#endif

uint64_t sysop_read_c64_signals();
uint64_t sysop_phi2_counter();

int sysop_open_bridge();
int sysop_close_bridge();

void sysop_wait_vic(uint16_t lineNum, uint8_t charNum);
void sysop_wait_vic2(uint16_t lineNum, uint8_t cycle);
void sysop_wait_vic2_no_wait(uint16_t lineNum, uint8_t cycle);
void sysop_wait_hdmi(uint16_t lineNum, uint8_t cycle);
uint16_t sysop_hdmi_offset(uint16_t lineNum);
uint16_t sysop_hdmi_tof();
void sysop_poke(uint16_t address, uint8_t value);
uint8_t sysop_peek(uint16_t address);
void sysop_poke_no_wait(uint16_t address, uint8_t value);

void sysop_cartridge_poke(uint16_t address, uint8_t value);
uint8_t sysop_internal_peek(uint16_t address);

// same as internal 
uint8_t sysop_io_peek(uint16_t address);
void sysop_io_poke(uint16_t address, uint8_t value);

void sysop_kernal_poke(uint16_t address, uint8_t value);

int sysop_vic_getline();
int sysop_vic_getline2(uint8_t* pChar);
int sysop_vic_getline3(uint8_t* pCycle);

unsigned char sysop_map_petscii_to_ascii(char c); 
unsigned char sysop_map_ascii_to_petscii(char c);

void sysop_dma_enable();
void sysop_dma_disable();

void sysop_dma_wait_not_busy();
void sysop_dma_wait_empty();
//void dma_wait_for_empty();
uint16_t sysop_dma_write_queue_length();
void sysop_dma_freeze();
void sysop_dma_unfreeze();
void sysop_dma_queue_freeze();
void sysop_dma_queue_unfreeze();

uint32_t sysop_dma_tag_data();
void sysop_dma_write_tag(uint32_t tag);

int sysop_load(const char* filename);
int sysop_load_buffer(uint8_t* buffer, int count);
int sysop_load_buffer_at(uint8_t* buffer, int count, uint16_t load_address);
int sysop_loadbin(const char* filename, uint16_t address);

int sysop_cartridge_load(const char* filename, int verifyOnly);
void sysop_cartridge_enable(uint16_t rom_size);
void sysop_cartridge_enable_ultimax();
void sysop_cartridge_disable();
int sysop_kernal_load(const char* filename, int verifyOnly);

void sysop_set_38column_mode();
void sysop_set_40column_mode();
void sysop_set_hscroll(uint8_t val);

void sysop_sprite_set_xy(uint8_t sprite, uint16_t x, uint8_t y);
void sysop_sprite_set_color(uint8_t sprite, uint8_t color);

void sysop_c64_reset();

void sysop_wait_vblank();

void sysop_screen_clear(uint16_t baseAddr);

void sysop_sys(uint16_t address);

void explain(int indentSpaces, uint16_t register_address, uint8_t value);

void printByteAsBinary(unsigned char byte);

void sysop_framebuffer_show();
void sysop_framebuffer_hide();
void sysop_framebuffer_flip();
void sysop_wait_hdmi_vblank();

struct sysop_c64_bus_sample
{
  uint64_t raw;
  uint8_t r__w;
  uint16_t addr;
  uint8_t phi2;
  uint8_t ba;
  uint8_t data;
  uint8_t freeze_signal;
  uint8_t _dma;
  uint8_t _irq;
  uint16_t vic_line;
  uint8_t cycle;
  uint32_t sample_tick;
  //uint32_t phi2_counter_val;
  uint8_t _roml;
  uint8_t _romh;
  uint8_t _io1;
  uint8_t _io2;
  uint8_t _charen;
  uint8_t _hiram;
  uint8_t _loram;
  uint8_t _exrom;
  uint8_t _game;
};

void sysop_sampler_wait_not_busy();
void sysop_sampler_start();
uint64_t sysop_sampler_get_sample(uint32_t index, struct sysop_c64_bus_sample* pSample);

void sysop_set_palette_entry(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void sysop_get_palette_entry(uint8_t index, uint8_t* r, uint8_t* g, uint8_t* b);
void sysop_wait_set_palette_entry(uint16_t x, uint16_t y, uint8_t index, uint8_t a, uint8_t r, uint8_t g, uint8_t b);
void sysop_queue_set_palette_entry(uint8_t index, uint8_t a, uint8_t r, uint8_t g, uint8_t b);
void sysop_hdmi_set_extended_borders(uint8_t mode);
void sysop_queue_set_extended_border_color(uint8_t a, uint8_t r, uint8_t g, uint8_t b);
void sysop_wait_set_extended_border_color(uint16_t x, uint16_t y, uint8_t index, uint8_t a, uint8_t r, uint8_t g, uint8_t b);
void sysop_queue_set_extended_border_color_index(uint8_t index);

int sysop_is_button_pressed(uint8_t id);

// CRT file header structure
typedef struct {
    char signature[16];    // "C64 CARTRIDGE   "
    uint32_t header_length;
    uint16_t version;
    uint16_t hardware_type;
    uint8_t exrom_line;
    uint8_t game_line;
    char reserved[6];
    char name[32];
} CRTHeader;

// CRT chip packet structure
typedef struct {
    char chip_type[4];     // "CHIP"
    uint32_t packet_length;
    uint16_t chip_type_id; // Should be 2 for ROM chips
    uint16_t bank_number;
    uint16_t load_address;
    uint16_t rom_size;
} CRTChip;


#define SYSOP_SERVER_PORT 6510
#define SYSOP_SERVER_ADDR "127.0.0.1"

// Server command identifiers
#define SYSOP_SERVER_CMD_DMA_LOCK 0x01
#define SYSOP_SERVER_CMD_DMA_UNLOCK 0x02
#define SYSOP_SERVER_CMD_CONSOLE_CLOSE 0x3
#define SYSOP_SERVER_CMD_SHOW_MESSAGE 0x4
#define SYSOP_SERVER_CMD_HIDE_MESSAGE 0x5
#define SYSOP_SERVER_CMD_QUEUE_HIDE_MESSAGE 0x6

int sysop_init();
void sysop_uninit();

int sysop_server_connect();
void sysop_server_disconnect();
void sysop_server_dma_lock();
void sysop_server_dma_lock2();
void sysop_server_dma_unlock();
void sysop_server_console_close();
void sysop_server_display_message(const char* msg, int length, int displayTimeMilliseconds);
void sysop_server_queue_hide_messages();
void sysop_server_hide_messages();

void sysop_scan_keys();
int sysop_is_key_down(int rawKeyIndex);
int sysop_is_shift_key_down();
int system_any_key_down();


void sysop_interrupt_test(uint8_t value, int setOrClear);
void sysop_add_write_strobe(uint8_t index, uint16_t address); // TODO: chl options, value, etc.
void sysop_add_write_io_strobe(uint8_t index, uint16_t address); // TODO: chl options, value, etc.
void sysop_add_read_strobe(uint8_t index, uint16_t address); // TODO: chl options, value, etc.
void sysop_add_raster_strobe(uint8_t index, uint16_t line, uint8_t cycle);
void sysop_reset_strobe(uint8_t index);

uint32_t sysop_get_cpu_freq();
uint32_t sysop_get_line_timing();
uint8_t sysop_get_vic_info();
int sysop_is_pal();
void sysop_set_pll_data(uint8_t index, uint32_t data);
void sysop_pll_reconfig();
uint32_t sysop_get_dma_info();
uint32_t sysop_get_version_info();

void sysop_screenshot_request();
uint8_t sysop_screenshot_status();
uint32_t sysop_screenshot_read(uint16_t addr);

uint32_t sysop_read_status_1();
uint32_t sysop_dropped_frames();
void sysop_video_reset();
void sysop_set_dma_timing_ntsc(uint16_t timing);
void sysop_set_dma_timing_pal(uint16_t timing);

void sysop_enable_easyflash_dma_trigger();
void sysop_disable_easyflash_dma_trigger();
void sysop_enable_reu_dma_trigger();
void sysop_disable_reu_dma_trigger();

void sysop_enable_io();
void sysop_disable_io();
void sysop_command(uint16_t cmd);

uint64_t sysop_debug1();
uint64_t sysop_debug2();

uint64_t sysop_read_sid_voices_data();
uint64_t sysop_read_gpio_data();
uint64_t sysop_read_key_data();
uint8_t sysop_read_joystick(uint8_t joystick_number);

int sysop_framebuffer_lock();
void sysop_framebuffer_unlock();

uint8_t sysop_namesoft_midi_ready();
void sysop_namesoft_midi_write(uint8_t data);
void sysop_set_nmi_vector(uint16_t addr);

void sysop_audio_set_base_addr(uint32_t base_addr);
void sysop_audio_set_length_frames(uint32_t length_frames);
void sysop_audio_set_loop_enable(bool enable);
void sysop_audio_start(void);
void sysop_audio_stop(void);
void sysop_audio_play_pcm(uint32_t base_addr, uint32_t length_frames, bool loop);
void sysop_audio_wait_until_done(void);
uint64_t sysop_audio_read_status(void);
bool sysop_audio_is_playing(void);
bool sysop_audio_has_underrun(void);
uint32_t sysop_audio_get_underrun_count(void);
uint16_t sysop_audio_get_fifo_usedw(void);
void sysop_audio_print_status(void);

uint16_t sysop_audio_dbg_cmd_count(void);
uint16_t sysop_audio_dbg_start_cmd_count(void);
uint16_t sysop_audio_dbg_bridge_start_count(void);

void sysop_audio_set_sample_format(uint32_t format);
void sysop_audio_set_phase_step(uint32_t phase_step);
uint32_t sysop_audio_phase_step_from_rate(uint32_t source_rate);

void sysop_audio_select_channel(uint32_t channel);
void sysop_audio_select_status_channel(uint32_t channel);

void sysop_audio_set_volume_left(uint32_t volume);
void sysop_audio_set_volume_right(uint32_t volume);
void sysop_audio_set_volume(uint32_t left, uint32_t right);

void sysop_audio_set_sid_volume_left(uint32_t volume);
void sysop_audio_set_sid_volume_right(uint32_t volume);
void sysop_audio_set_sid_volume(uint32_t left, uint32_t right);

#ifdef __cplusplus
}
#endif

#endif /* SYSOP_LIBRARY_H */
