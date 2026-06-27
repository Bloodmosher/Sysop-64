/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

void sysop_set_palette_entry(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t val = ((uint32_t)r<<24) | ((uint32_t)g<<16) | ((uint32_t)b<<8) | index;
    *((uint32_t*)sysop64_set_palette_map) = val;
}

void sysop_get_palette_entry(uint8_t index, uint8_t* r, uint8_t* g, uint8_t* b)
{
    get_library_lock();

    //uint32_t cmdval =  (SYSOP64_CMD3_ID_GET_COLOR<<16) | (index & 0xF);
    uint32_t cmdval =  (SYSOP64_CMD3_ID_GET_COLOR<<24) | (index & 0xF);
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();
    *r = ((val & 0xFF0000)>>16);
    *g = ((val & 0xFF00)>>8);
    *b = (val & 0xFF);
}

uint64_t sysop_read_hdmi_info()
{
    volatile uint64_t val = *((uint64_t*)sysop64_hdmi_info_result_map);
    return val;
}

uint64_t sysop_read_sid_voices_data()
{
    volatile uint64_t val = *((uint64_t*)sysop64_sid_voices_data_map);
    return val;
}

uint64_t sysop_read_gpio_data()
{
    volatile uint64_t val = *((uint64_t*)sysop64_gpio_data_map);
    return val;
}

static void wait_hdmi_queue_not_full()
{
    volatile uint64_t status = sysop_read_hdmi_info();
    while ((status & 0x1)!=0)
    {
        status = sysop_read_hdmi_info();
    }
}

static void wait_hdmi_queue_empty()
{
    uint64_t status = sysop_read_hdmi_info();
    while ((status & 0x2)==0)
    {
        status = sysop_read_hdmi_info();
    }
}

void sysop_wait_set_palette_entry(uint16_t x, uint16_t y, uint8_t index, uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    wait_hdmi_queue_not_full();
    uint64_t data = ((uint64_t)1<<56ULL) | ((uint64_t)(a&1)<<55ULL) | (x & 0x7FFULL) | ((y & 0x7FFULL)<<11) | ((index & 0xFULL)<<22) | ((uint64_t)b<<26) | ((uint64_t)g<<34) | ((uint64_t)r<<42);
    *((uint64_t *)sysop64_hdmi_cmd_data_map) = data;
}

void sysop_queue_set_palette_entry(uint8_t index, uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    wait_hdmi_queue_not_full();
    uint64_t data = ((uint64_t)2<<56ULL) | ((uint64_t)(a&1)<<55ULL) | ((index & 0xFULL)<<22) | ((uint64_t)b<<26) | ((uint64_t)g<<34) | ((uint64_t)r<<42);
    *((uint64_t *)sysop64_hdmi_cmd_data_map) = data;
}

void sysop_hdmi_set_extended_borders(uint8_t mode)
{
    wait_hdmi_queue_not_full();
    uint64_t data = ((uint64_t)3<<56ULL) | ((uint64_t)(mode & 0xF));
    *((uint64_t *)sysop64_hdmi_cmd_data_map) = data;
}

void sysop_queue_set_extended_border_color(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    wait_hdmi_queue_not_full();
    uint64_t data = ((uint64_t)5<<56ULL) | ((uint64_t)(a&1)<<55ULL) | ((uint64_t)b<<26) | ((uint64_t)g<<34) | ((uint64_t)r<<42);
    *((uint64_t *)sysop64_hdmi_cmd_data_map) = data;
}

void sysop_queue_set_extended_border_color_index(uint8_t index)
{
    wait_hdmi_queue_not_full();
    uint64_t data = ((uint64_t)7<<56ULL) | ((uint64_t)(index&0xF)<<52ULL);
    *((uint64_t *)sysop64_hdmi_cmd_data_map) = data;
}

void sysop_wait_set_extended_border_color(uint16_t x, uint16_t y, uint8_t index, uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    wait_hdmi_queue_not_full();
    uint64_t data = ((uint64_t)4<<56ULL) | ((uint64_t)(a&1)<<55ULL) | (x & 0x7FFULL) | ((y & 0x7FFULL)<<11) | ((index & 0xFULL)<<22) | ((uint64_t)b<<26) | ((uint64_t)g<<34) | ((uint64_t)r<<42);
    *((uint64_t *)sysop64_hdmi_cmd_data_map) = data;
}

void sysop_set_pll_data(uint8_t index, uint32_t data)
{
    get_library_lock();
    // the data goes in the B parameter
    *((uint64_t *)sysop64_cmd3_param_map) = data;

    uint32_t cmdval =  (SYSOP64_CMD3_SET_PLL_DATA <<24) | (index & 0x3F);
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();
}

void sysop_set_dma_timing_ntsc(uint16_t dma_timing)
{
    get_library_lock();
    uint32_t cmdval =  (SYSOP64_CMD3_SET_DMA_TIMING_NTSC <<24) | (uint32_t)dma_timing;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }
    release_library_lock();
}

void sysop_set_dma_timing_pal(uint16_t dma_timing)
{
    get_library_lock();
    uint32_t cmdval =  (SYSOP64_CMD3_SET_DMA_TIMING_PAL <<24) | (uint32_t)dma_timing;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }
    release_library_lock();
}

void sysop_pll_reconfig()
{
    get_library_lock();
    uint32_t cmdval =  (SYSOP64_CMD3_PLL_RECONFIG <<24);
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();
}

uint32_t sysop_get_cpu_freq()
{
    get_library_lock();
    uint32_t cmdval =  SYSOP64_CMD3_READ_CPU_FREQ << 24;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();
    return (uint32_t)(val & 0xFFFFFFFF);
}

uint32_t sysop_get_line_timing()
{
    get_library_lock();
    uint32_t cmdval =  SYSOP64_CMD3_READ_LINE_TIMING << 24;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }
    release_library_lock();

    return (uint32_t)(val & 0xFFFFFFFF);
}

uint32_t sysop_get_dma_info()
{
    //get_library_lock();
    uint32_t cmdval =  SYSOP64_CMD3_GET_DMA_INFO << 24;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }
    //release_library_lock();

    //printf("SYSOP64_DMA_INFO: 0x%016" PRIx64 "\n", val);
    return (uint32_t)(val & 0xFFFFFFFF);
}

uint32_t sysop_get_version_info()
{
    get_library_lock();

    uint32_t cmdval =  SYSOP64_CMD3_GET_VERSION_INFO << 24;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();

    return (uint32_t)(val & 0xFFFFFFFF);
}

void sysop_screenshot_request()
{
    get_library_lock();
    uint32_t cmdval = SYSOP64_CMD3_SCREENSHOT_REQUEST << 24;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;
    release_library_lock();
}

uint8_t sysop_screenshot_status()
{
    get_library_lock();
    uint32_t cmdval = SYSOP64_CMD3_SCREENSHOT_STATUS << 24;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();
    return (uint8_t)(val & 0x1);
}

uint32_t sysop_screenshot_read(uint16_t addr)
{
//    get_library_lock();
    uint32_t cmdval =  SYSOP64_CMD3_SCREENSHOT_READ << 24 | (addr & 0x7FFF);
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

//    release_library_lock();
    return (uint32_t)(val & 0xFFFFFFFF);
}

uint8_t sysop_get_vic_info()
{
    get_library_lock();

    uint32_t cmdval =  SYSOP64_CMD3_READ_VIC_INFO << 24;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
        //printf("Waiting for cmd3 result 0x%016" PRIx64 "...\n", val);
    }

    release_library_lock();

    //printf("0x%016" PRIx64 "\n", val);
    return (uint8_t)(val & 0xFF);
}

uint32_t sysop_read_status_1()
{
    get_library_lock();

    uint32_t cmdval = SYSOP64_CMD3_READ_STATUS_1 << 24;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
        //printf("Waiting for cmd3 result 0x%016" PRIx64 "...\n", val);
    }

    release_library_lock();

    //printf("0x%016" PRIx64 "\n", val);
    return (uint32_t)(val & 0xFFFFFFFF);
}

void sysop_video_reset()
{
    get_library_lock();
    uint32_t cmdval = SYSOP64_CMD3_VIDEO_RESET << 24;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;
    release_library_lock();
}

void sysop_wait_vic(uint16_t lineNum, uint8_t charNum)
{
    dma_wait_not_full();

    uint32_t val = ((uint32_t)2<<24) | (((uint32_t)lineNum)<<8) | charNum;
    *((uint32_t*)sysop64_cmd2_map) = val;
}

// Queue a DMA wait for VIC line and cycle
// lineNum is 0 based, so one less than what you would do with c64 code that reads $d012
// e.g. lda $d012; cmp $33 for line 51 would be sysop_wait_vic2(50, 1);
// cycles go from 1 to however many your VIC has (e.g. 63 on PAL)
void sysop_wait_vic2(uint16_t lineNum, uint8_t cycle)
{
    dma_wait_not_full();

    uint32_t val = ((uint32_t)4<<24) | (((uint32_t)lineNum)<<8) | cycle;
    *((uint32_t*)sysop64_cmd2_map) = val;
}

void sysop_wait_vic2_no_wait(uint16_t lineNum, uint8_t cycle)
{
    uint32_t val = ((uint32_t)4<<24) | (((uint32_t)lineNum)<<8) | cycle;
    *((uint32_t*)sysop64_cmd2_map) = val;
}

static uint32_t bcd_extract_range(uint32_t bcd, int last_digit, int first_digit) {
    if (first_digit < 0 || last_digit > 7 || first_digit > last_digit) {
        return 0; // invalid range
    }

    uint32_t result = 0;
    uint32_t multiplier = 1;

    for (int i = first_digit; i <= last_digit; i++) {
        uint8_t digit = (bcd >> (i * 4)) & 0xF;

        if (digit > 9) {
            return 0xFFFFFFFF; // invalid BCD digit
        }

        result += digit * multiplier;
        multiplier *= 10;
    }

    return result;
}


// for pal, in the scale file we adjust so that 
// lines 16-285 are visible
uint16_t sysop_hdmi_offset(uint16_t lineNum)
{
    uint32_t line_timing = sysop_get_line_timing();
    uint32_t tof_line = bcd_extract_range(line_timing, 7, 5);
    tof_line -= 16;
    lineNum = (lineNum + tof_line) % 312; // TODO: make this work for NTSC
    lineNum += 4;
    return lineNum;
}

uint16_t sysop_hdmi_tof()
{
    uint32_t line_timing = sysop_get_line_timing();
    uint32_t tof_line = bcd_extract_range(line_timing, 7, 5);
    return tof_line;
}

void sysop_wait_hdmi(uint16_t lineNum, uint8_t cycle)
{
    uint16_t input = lineNum;
    lineNum = sysop_hdmi_offset(lineNum);
    printf("sysop_wait_hdmi asked for %d, adjusted to %d\n", input, lineNum);

    dma_wait_not_full();

    uint32_t val = ((uint32_t)4<<24) | (((uint32_t)lineNum)<<8) | cycle;
    *((uint32_t*)sysop64_cmd2_map) = val;
}

void sysop_command(uint16_t cmd)
{
    *((uint16_t *)sysop64_cmd_address) = cmd;
}

void sysop_framebuffer_show()
{
    *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_SHOW_FRAMEBUFFER;
}

void sysop_framebuffer_hide()
{
    *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_HIDE_FRAMEBUFFER;
}

void sysop_framebuffer_flip()
{
    *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_FLIP_FRAMEBUFFER;
}

int sysop_vic_getline()
{
  get_library_lock();
  int line = (int)(*((uint64_t *)sysop64_fpga_status_map) & 0xFFF);
  release_library_lock();
  return line;
}

int sysop_vic_getline2(uint8_t* pChar)
{
    get_library_lock();
    uint64_t data = *((uint64_t *)sysop64_fpga_status_map);

    int line = (int)data & 0xFFF;

    if (pChar != NULL)
    {
        *pChar = (data & 0x0ff000)>>12;
        //*pChar = (data & 0x0FF00000)>>20; // this is "char_num", not "vic_charnum"
    }

    release_library_lock();
    return line;
}

// This one returns the cycle counter
int sysop_vic_getline3(uint8_t* pCycle)
{
    get_library_lock();
    uint64_t data = *((uint64_t *)sysop64_fpga_status_map);

    int line = (int)data & 0xFFF;

    if (pCycle != NULL)
    {
        *pCycle = (data & 0x1FE0000000000000)>>53;
    }

    release_library_lock();
    return line;
}

void sysop_set_38column_mode()
{
    uint8_t val = sysop_peek(0xd016);
    val &= ~(1<<3);
    sysop_poke(0xd016, val);
}

void sysop_set_40column_mode()
{
    uint8_t val = sysop_peek(0xd016);
    val |= (1<<3);
    sysop_poke(0xd016, val);
}

void sysop_set_hscroll(uint8_t hscroll)
{
    uint8_t val = sysop_peek(0xd016);
    val = (val & 0xF8) | (hscroll & 0x7);
    sysop_poke(0xd016, val);
}

void sysop_sprite_set_xy(uint8_t sprite, uint16_t x, uint8_t y)
{
    uint16_t addr_x = 0xd000 + (sprite * 2);
    sysop_poke(addr_x, x & 0xFF);

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    
    uint8_t mask_others = ~(1<<sprite);
    uint8_t val = sysop_peek(0xd010) & mask_others;
    uint8_t bitval = (x >> 8) & 0x1;
    val = val | (bitval<<sprite);
    sysop_poke(0xd010, val);

    uint16_t addr_y = 0xd001 + (sprite * 2);
    sysop_poke(addr_y, y);
}

void sysop_sprite_set_color(uint8_t sprite, uint8_t color)
{
    if (sprite >= 0 && sprite <= 7)
    {
        sysop_poke(0xd027+sprite, color);
    }
}

void sysop_screen_clear(uint16_t baseAddr)
{
    for (int i=0;i<1000;i++)
    {
        sysop_poke(baseAddr+i, 0x20);
    }
}

void sysop_wait_vblank()
{
    sysop_wait_vic2(250, 1); // TODO: pick the correct line for PAL/NTSC
}

int sysop_is_pal()
{
    uint8_t vic_info = sysop_get_vic_info();
    if ((vic_info & 0x07) == VIC_CHIP_6569)
        return 1;

    return 0;
}
