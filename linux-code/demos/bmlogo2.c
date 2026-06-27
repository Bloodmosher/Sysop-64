/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * bmlogo2 — animated bitmap logo with a 3-D starfield background.
 *
 * A 320×200 single-colour bitmap image (loaded from bmlogo2.bin) is
 * decomposed into 8 vertical sprite slices that are swept across the
 * screen in a raster-split loop while a 80-star 3-D starfield is
 * animated behind them in C64 hires-bitmap mode.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include "c64keys.h"
#include "sysop64.h"

/* ------------------------------------------------------------------ */
/* VIC-II bank / address layout                                        */
/* ------------------------------------------------------------------ */

#define SPRITE_BASE_ADDR    0x4800u

static uint16_t c64_addr_bitmap_data = 0x6000;
static uint16_t c64_addr_screen_ram  = 0x4400;
static uint8_t  c64_bank_sel         = 0x96;

/* ------------------------------------------------------------------ */
/* Working buffers                                                     */
/* ------------------------------------------------------------------ */

/* Bitmap image loaded from disk (8 KB, one byte per 8 pixels). */
static unsigned char bitmap[8000];

/*
 * Write-back cache of every byte we have poked into C64 RAM.
 * Used by plot_pixel() to do read-modify-write without a costly sysop_peek().
 */
static unsigned char cache[0xFFFF];

/*
 * Secondary shadow used by set_sprite_pixel() for sprite data that is
 * batch-copied to the C64 after the full sprite layout is built.
 */
static unsigned char mirror[0xFFFF];

/* ------------------------------------------------------------------ */
/* C64 write helper                                                    */
/* ------------------------------------------------------------------ */

/* Write a byte to C64 RAM and keep cache[] in sync. */
static void cached_poke(uint16_t address, uint8_t data)
{
    sysop_poke(address, data);
    cache[address] = data;
}

/* ------------------------------------------------------------------ */
/* Bitmap pixel plotting                                               */
/* ------------------------------------------------------------------ */

/*
 * Set or clear a single pixel in the C64 hires bitmap and write the
 * colour byte to the corresponding screen-RAM cell.
 *
 * set_or_clear: 0 = clear pixel, non-zero = set pixel
 * color:        C64 colour index for the character cell
 */
static void plot_pixel(uint16_t base_addr, int x, int y, int set_or_clear, uint8_t color)
{
    if (x < 0 || x > 319 || y < 0 || y > 199)
        return;

    int row        = y / 8;
    int ncol       = x / 8;
    int line       = y & 7;
    int bit        = 7 - (x & 7);
    int byte_addr  = base_addr + (row * 320) + (ncol * 8) + line;
    int color_addr = c64_addr_screen_ram + (row * 40) + ncol;

    uint8_t value = set_or_clear
        ? (cache[byte_addr] |  (uint8_t)(1 << bit))
        : (cache[byte_addr] & (uint8_t)~(1 << bit));

    cached_poke((uint16_t)byte_addr,  value);
    cached_poke((uint16_t)color_addr, color);
}

/* ------------------------------------------------------------------ */
/* Sprite X/Y positioning                                              */
/* ------------------------------------------------------------------ */

static void set_sprite_xy(uint8_t sprite, uint16_t x, uint8_t y)
{
    uint16_t addr_x = 0xd000 + (sprite * 2);
    uint16_t addr_y = 0xd001 + (sprite * 2);

    uint8_t val = ((x >> 8) & 1)
        ? (mirror[0xd010] |  (uint8_t)(1 << sprite))
        : (mirror[0xd010] & (uint8_t)~(1 << sprite));

    mirror[0xd010] = val;
    cached_poke(0xd010,  val);
    cached_poke(addr_x, (uint8_t)(x & 0xFF));
    cached_poke(addr_y,  y);
}

/* ------------------------------------------------------------------ */
/* Bitmap asset loader                                                 */
/* ------------------------------------------------------------------ */

static int load_bitmap_file(const char *filename, unsigned char *buf)
{
    printf("Loading %s\n", filename);
    FILE *file = fopen(filename, "rb");
    if (!file)
        return 1;
    fread(buf, 1, 8000, file);
    fclose(file);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 3-D starfield data                                                  */
/* ------------------------------------------------------------------ */

static int starfield_width  = 384;
static int starfield_height = 256;

/* Bytes to iterate over in g_stars (80 stars × 6 bytes each). */
static unsigned long g_draw_count = 80 * 6;

/* Phase offsets into g_inc_table for each axis; three independent
 * starting positions so the axes drift out of phase with each other. */
static unsigned long x_inc_offset = 980;
static unsigned long y_inc_offset = 1492;
static unsigned long z_inc_offset = 1748;

static uint8_t x_inc_dir = 1;
static uint8_t y_inc_dir = 1;
static uint8_t z_inc_dir = 1;

/* Greyscale colour ramp indexed by depth bucket (0 = far/dark, 8 = near/bright). */
static uint8_t g_star_colors[] = {
    0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
    0xb0, 0xb0, 0xb0,
    0xb0, 0xb0, 0xb0,
    0xc0, 0xc0, 0xc0,
    0xf0, 0xf0, 0xf0,
    0x10, 0x10, 0x10,
    0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
};

/* 80 stars, each 6 bytes: x(2) y(2) z(2), signed 16-bit big-endian — backup. */
static uint8_t g_stars_ref[] = {
    0xfe,0xa7,0x00,0xcc,0x01,0x6f, 0xfe,0xd3,0xff,0x11,0x02,0x0a, 0x00,0x1a,0x00,0x22,
    0x02,0xec,0x00,0x0d,0x00,0xae, 0x00,0xaf,0x01,0x6e,0x00,0x30, 0x03,0x4a,0x00,0x11,
    0x00,0xa2,0x01,0xf7,0x00,0xb5, 0x00,0x6a,0x02,0xb8,0x01,0x22, 0x00,0x4e,0x03,0x97,
    0xff,0xc6,0xff,0x3e,0x01,0x5f, 0x00,0x87,0xff,0x0a,0x01,0x51, 0xff,0x52,0xff,0x38,
    0x03,0xcf,0xff,0x95,0x00,0xd4, 0x03,0x94,0x00,0xa0,0x00,0x89, 0x02,0x4c,0xfe,0xcc,
    0x00,0xed,0x03,0x5a,0xfe,0xfd, 0xff,0x3e,0x01,0x9a,0x00,0x8a, 0xff,0xc4,0x02,0x58,
    0x00,0x9b,0xff,0xd8,0x00,0xc5, 0xff,0x4e,0xff,0xee,0x02,0x93, 0x00,0xc1,0xff,0x15,
    0x01,0xf7,0x01,0x71,0x00,0x9f, 0x02,0x86,0x00,0x3d,0x00,0xfa, 0x02,0x20,0x01,0x14,
    0xff,0x5d,0x02,0x35,0xff,0x9b, 0x00,0x38,0x01,0xc3,0x00,0x9b, 0xff,0x75,0x01,0x0f,
    0x00,0xed,0x00,0xb1,0x02,0x32, 0x00,0x9a,0x00,0x26,0x01,0x7a, 0xff,0xf8,0x00,0xc5,
    0x02,0x44,0x01,0x34,0x00,0x69, 0x00,0xb4,0xfe,0xc6,0x00,0x52, 0x03,0xf5,0xff,0xc5,
    0x00,0xaf,0x01,0xb4,0xff,0x8b, 0x00,0x9c,0x01,0xe0,0x00,0xa8, 0xff,0x2d,0x02,0x6b,
    0x00,0x57,0x00,0xc6,0x03,0x31, 0xff,0x88,0xff,0x0c,0x01,0x0f, 0x00,0x44,0xff,0x08,
    0x01,0xb0,0x00,0x52,0xff,0x41, 0x02,0x65,0xfe,0xdc,0x00,0x03, 0x00,0xbe,0xff,0xbc,
    0xff,0x41,0x03,0x9e,0x00,0x5b, 0xff,0x53,0x02,0xb4,0xff,0x2c, 0xff,0x68,0x03,0x93,
    0x00,0xb7,0x00,0x67,0x02,0x67, 0x01,0x58,0x00,0x67,0x01,0xac, 0x00,0xaa,0x00,0x8a,
    0x01,0xdb,0xff,0x08,0x00,0x0e, 0x01,0x9d,0xff,0x15,0xff,0x3e, 0x01,0x38,0x01,0x5c,
    0x00,0xed,0x01,0xe4,0x01,0x4f, 0xff,0xb6,0x01,0xe4,0x00,0x35, 0xff,0x94,0x02,0xf8,
    0x00,0xbe,0xff,0x1c,0x02,0xd2, 0x01,0x36,0xff,0xf5,0x00,0xd4, 0x01,0x43,0xff,0x0c,
    0x00,0xb7,0xff,0x19,0xff,0xd7, 0x00,0x51,0x01,0x01,0x00,0x8a, 0x00,0x64,0x01,0x03,
    0xff,0x89,0x01,0xa0,0x01,0x40, 0x00,0x00,0x02,0xbe,0xff,0xff, 0xff,0x95,0x00,0x89,
    0x01,0x75,0xff,0x62,0x01,0xff, 0x00,0x5b,0x00,0xa7,0x03,0xdc, 0xfe,0xe0,0xff,0x74,
    0x02,0x40,0xfe,0xc1,0xff,0x9e, 0x02,0x8b,0xff,0x28,0x00,0xda, 0x02,0xcb,0xff,0x88,
    0xff,0xd6,0x02,0x11,0x00,0xa0, 0xff,0x4a,0x02,0x0d,0x01,0x77, 0x00,0x95,0x02,0x38,
    0x00,0x8d,0xff,0x8e,0x01,0x74, 0x00,0x51,0x00,0x1a,0x01,0x5b, 0x00,0x0b,0x00,0xde,
    0x02,0x69,0x00,0x80,0x00,0xdb, 0x00,0xb0,0x01,0x54,0xff,0xf6, 0x03,0x09,0x01,0x7b,
    0x00,0x8b,0x00,0x8c,0x00,0x41, 0xff,0x08,0x02,0x5a,0xff,0x14, 0xff,0x93,0x01,0xde,
    0x00,0x7a,0xff,0xbc,0x02,0xd3, 0xff,0x4d,0x00,0xcb,0x03,0x01, 0xff,0xe1,0xff,0x85,
    0x03,0xc4,0x00,0xac,0x00,0xff, 0x01,0x5a,0xff,0x38,0x00,0x1e, 0x00,0xc4,0xff,0x51,
    0xff,0xf5,0x01,0x07,0x00,0x33, 0x00,0xbf,0x01,0x58,0xff,0x0f, 0x00,0x9b,0x00,0x7c,
};

/* 80 stars generated at startup; same layout as g_stars_ref. */
static uint8_t g_stars[sizeof(g_stars_ref)];

/*
 * Velocity increment look-up table — generated at startup by init_inc_table().
 * Triangle wave (0 → +8 → 0 → -8 → 0), each step held for 32 big-endian
 * int16_t pairs (64 bytes).  33 steps × 64 bytes = 2112 bytes total.
 */
static uint8_t g_inc_table[2112];

/* ------------------------------------------------------------------ */
/* Starfield update                                                     */
/* ------------------------------------------------------------------ */

static void update_stars(void)
{
    int16_t x_inc = (int16_t)(g_inc_table[x_inc_offset]   << 8 | g_inc_table[x_inc_offset+1]);
    int16_t y_inc = (int16_t)(g_inc_table[y_inc_offset]   << 8 | g_inc_table[y_inc_offset+1]);
    int16_t z_inc = (int16_t)(g_inc_table[z_inc_offset]   << 8 | g_inc_table[z_inc_offset+1]);

    for (int i = 0; i < (int)sizeof(g_stars); i += 6) {
        int16_t x = (int16_t)(g_stars[i]   << 8 | g_stars[i+1]);
        int16_t y = (int16_t)(g_stars[i+2] << 8 | g_stars[i+3]);
        int16_t z = (int16_t)(g_stars[i+4] << 8 | g_stars[i+5]);

        x += x_inc;
        if      (x >  starfield_width)  x -= (int16_t)(starfield_width  * 2);
        else if (x < -starfield_width)  x += (int16_t)(starfield_width  * 2);

        y += y_inc;
        if      (y >  starfield_height) y -= (int16_t)(starfield_height * 2);
        else if (y < -starfield_height) y += (int16_t)(starfield_height * 2);

        z += z_inc;
        if      (z > 1024) z -= 960;
        else if (z <   64) z += 960;

        g_stars[i]   = (uint8_t)((x >> 8) & 0xff);
        g_stars[i+1] = (uint8_t)(x & 0xff);
        g_stars[i+2] = (uint8_t)((y >> 8) & 0xff);
        g_stars[i+3] = (uint8_t)(y & 0xff);
        g_stars[i+4] = (uint8_t)((z >> 8) & 0xff);
        g_stars[i+5] = (uint8_t)(z & 0xff);
    }
}

static void advance_stars(void)
{
    x_inc_offset += 2 * x_inc_dir;
    if (x_inc_offset >= sizeof(g_inc_table)) x_inc_offset = 0;

    y_inc_offset += 2 * y_inc_dir;
    if (y_inc_offset >= sizeof(g_inc_table)) y_inc_offset = 0;

    z_inc_offset += 2 * z_inc_dir;
    if (z_inc_offset >= sizeof(g_inc_table)) z_inc_offset = 0;
}

static void update_animation(void)
{
    update_stars();
    advance_stars();
}

/* ------------------------------------------------------------------ */
/* Starfield rendering                                                  */
/* ------------------------------------------------------------------ */

static void draw_starfield(int set_or_clear)
{
    int center_x = 320 / 2;
    int center_y = 200 / 2;
    int x_scale  = 0x80;
    int y_scale  = 0x7f;

    for (int i = 0; i < (int)g_draw_count; i += 6) {
        int16_t x = (int16_t)(g_stars[i]   << 8 | g_stars[i+1]);
        int16_t y = (int16_t)(g_stars[i+2] << 8 | g_stars[i+3]);
        int16_t z = (int16_t)(g_stars[i+4] << 8 | g_stars[i+5]);

        x = (int16_t)floor((x * x_scale) / (float)z);
        y = (int16_t)floor((y * y_scale) / (float)z);
        x += center_x;
        y += center_y;

        int color_index = 7 - (int)floor(z >> 7);
        if (color_index < 0) color_index = 0;
        if (color_index > 7) color_index = 7;

        uint8_t r = g_star_colors[color_index * 3];
        plot_pixel(c64_addr_bitmap_data, x, y, set_or_clear, r);
    }
}

/* ------------------------------------------------------------------ */
/* Sprite pixel building from bitmap                                    */
/* ------------------------------------------------------------------ */

#define SPRITE_STRIDE   0x40u   /* 64 bytes per sprite slot */
#define NUM_SPRITES     8
#define SPRITE_W        24
#define SPRITE_H        21
#define BYTES_PER_ROW   3       /* 24 px / 8 */

/* Return the state (0/1) of a single pixel from a flat bitmap buffer. */
static int get_bitmap_pixel(const char *buf, int base_addr, int x, int y)
{
    int row   = y / 8;
    int ncol  = x / 8;
    int line  = y & 7;
    int nbyte = base_addr + (row * 320) + (ncol * 8) + line;
    int bit   = 7 - (x & 7);
    return ((buf[nbyte] >> bit) & 1);
}

/*
 * Write a single pixel into the mirror[] sprite data shadow.
 * group_x / group_y define the top-left of the 8-sprite strip in
 * screen coordinates.  The actual C64 sysop_poke happens later in batch.
 */
static void set_sprite_pixel(int x, int y, int group_x, int group_y, uint8_t value)
{
    if (x < 0 || x >= 320 || y < 0 || y >= 200) return;

    int lx = x - group_x;
    int ly = y - group_y;
    if (ly < 0 || ly >= SPRITE_H) return;
    if (lx < 0 || lx >= (NUM_SPRITES * SPRITE_W)) return;

    unsigned sprite_idx = (unsigned)lx / SPRITE_W;
    unsigned sx         = (unsigned)lx % SPRITE_W;
    unsigned row_off    = (unsigned)ly * BYTES_PER_ROW;
    unsigned byte_inrow = sx / 8;
    unsigned bit_inbyte = 7 - (sx % 8);

    uint8_t *p    = (uint8_t *)SPRITE_BASE_ADDR
                    + sprite_idx * SPRITE_STRIDE
                    + row_off + byte_inrow;
    uint8_t  mask = (uint8_t)(1u << bit_inbyte);
    uint8_t  val  = mirror[(uint16_t)(uintptr_t)p];

    mirror[(uint16_t)(uintptr_t)p] = value
        ? (val & (uint8_t)~mask)
        : (val |  mask);
}

/* Build sprite pixel data from the bitmap for a given vertical offset. */
static void set_sprite_data(int y_offset)
{
    static const int sprite_anchor_x = 80;
    for (int x = 0; x < 320; x++)
        for (int y = 0; y < 200; y++)
            set_sprite_pixel(x, y, sprite_anchor_x, y_offset,
                             (uint8_t)get_bitmap_pixel(bitmap, 0, x, y));
}

/* ------------------------------------------------------------------ */
/* Sprite control helpers                                              */
/* ------------------------------------------------------------------ */

static void set_sprite_colors(uint8_t color)
{
    for (int i = 0; i < 8; i++)
        sysop_sprite_set_color(i, color);
}

static int sprite0_x;   /* anchor X used by set_sprite_data() */

/* Fill g_stars with pseudo-random initial positions using a fixed-seed
 * Linear Congruential Generator (LCG).
 *
 * Algorithm:  x[n+1] = (a * x[n] + c) mod 2^32
 *   a = 1664525     well-known multiplier for 32-bit LCGs
 *   c = 1013904223  odd increment (ensures full period)
 * The mod-2^32 happens implicitly via uint32_t overflow.
 *
 * Only the upper 16 bits of each output are used (rng >> 16); the lower
 * bits of an LCG have shorter sub-cycles and poorer randomness.
 *
 * Value ranges chosen to match the wrap limits enforced by update_stars():
 *   x  : scaled to [−starfield_width,  +starfield_width]  (±384)
 *        (int16_t)(rng >> 16) gives a signed value in [−32768, 32767];
 *        multiplying by starfield_width and shifting right 15 maps that
 *        to approximately ±384 without a division.
 *   y  : same technique scaled to ±starfield_height (±256)
 *   z  : uniform in [64, 1024] via modulo on the upper 16 bits;
 *        update_stars() keeps z in this range so initialising inside it
 *        avoids an immediate wrap on the first frame.
 *
 * The seed 0x12345678 is arbitrary; any non-zero constant works.  A
 * fixed seed makes the initial cloud deterministic across runs.  To
 * get a different layout each time, replace it with (uint32_t)time(NULL). */
static void init_stars(void)
{
    uint32_t rng = 0x12345678u;
    for (int i = 0; i < (int)sizeof(g_stars); i += 6) {
        rng = rng * 1664525u + 1013904223u;
        int16_t x = (int16_t)((int32_t)(int16_t)(rng >> 16) * starfield_width  >> 15);
        rng = rng * 1664525u + 1013904223u;
        int16_t y = (int16_t)((int32_t)(int16_t)(rng >> 16) * starfield_height >> 15);
        rng = rng * 1664525u + 1013904223u;
        int16_t z = (int16_t)(64 + (rng >> 16) % (1024 - 64 + 1));
        g_stars[i]   = (uint8_t)((uint16_t)x >> 8);
        g_stars[i+1] = (uint8_t)(x  & 0xff);
        g_stars[i+2] = (uint8_t)((uint16_t)y >> 8);
        g_stars[i+3] = (uint8_t)(y  & 0xff);
        g_stars[i+4] = (uint8_t)((uint16_t)z >> 8);
        g_stars[i+5] = (uint8_t)(z  & 0xff);
    }
}

/* Fill g_inc_table with the triangle wave at runtime. */
static void init_inc_table(void)
{
    /* Triangle wave 0 → +8 → 0 → -8 → 0, each step held for 32 pairs
     * (64 bytes).  Entries are signed 16-bit big-endian values. */
    static const int16_t wave[] = {
         0, 1, 2, 3, 4, 5, 6, 7, 8,
         7, 6, 5, 4, 3, 2, 1,
         0, 0,  /* two groups at the zero-crossing */
        -1, -2, -3, -4, -5, -6, -7, -8,
        -7, -6, -5, -4, -3, -2, -1
    };
    int out = 0;
    for (int i = 0; i < (int)(sizeof(wave) / sizeof(wave[0])); i++) {
        uint16_t uv = (uint16_t)wave[i];
        uint8_t  hi = (uint8_t)(uv >> 8);
        uint8_t  lo = (uint8_t)(uv & 0xff);
        for (int j = 0; j < 32; j++) {
            g_inc_table[out++] = hi;
            g_inc_table[out++] = lo;
        }
    }
}

static void init_sprites(void)
{
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    cached_poke(0xd01c, 0x00);  /* disable multicolor */
    for (int i = 0; i < 8; i++)
        sysop_sprite_set_color(i, 0x1);

    cached_poke(0xd025, 1);     /* sprite extra colours */
    cached_poke(0xd026, 6);
    cached_poke(0xd017, 0x00);  /* no Y expand */
    cached_poke(0xd01d, 0x00);  /* no X expand */

    uint8_t x = 80;
    uint8_t y = 80;
    sprite0_x = x;
    for (int i = 0; i < 8; i++, x += 24)
        sysop_sprite_set_xy(i, x, y);

    /* Sprite pointers: sprite slot 0x20 = address 0x0800 relative to bank */
    uint16_t ptr_base = c64_addr_screen_ram + 0x3f8;
    uint8_t  ptr      = 0x20;
    for (int i = 0; i < 8; i++)
        cached_poke(ptr_base + i, ptr++);

    cached_poke(0xd015, 0xff);  /* enable all 8 sprites */
}

static void update_sprite_pointers(uint8_t sprite0_ptr)
{
    uint16_t ptr_base = c64_addr_screen_ram + 0x3f8;
    for (int i = 0; i < 8; i++)
        cached_poke(ptr_base + i, sprite0_ptr + i);
}

static void set_sprite_group_position(uint16_t x, uint8_t y)
{
    for (int i = 0; i < 8; i++, x += 24)
        set_sprite_xy(i, x, y);
}

/* ------------------------------------------------------------------ */
/* Signal handler                                                      */
/* ------------------------------------------------------------------ */

static void sig_handler(int sig)
{
    sysop_poke(0xd020, 0);
    sysop_poke(0xd021, 0);
    sysop_poke(0xd015, 0);
    sysop_poke(0xd011, 0x1b);
    sysop_poke(0xd018, 0x15);
    sysop_poke(0xdd00, 0x97);
    sysop_screen_clear(0x400);

    sysop_dma_disable();
    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();
    exit(sig);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    signal(SIGINT, sig_handler);
    memset(mirror, 0, sizeof(mirror));

    if (load_bitmap_file("./bmlogo2.bin", bitmap) != 0) {
        fprintf(stderr, "Failed to load bmlogo2.bin\n");
        return 1;
    }

    /* Use the hand-crafted star positions for now.
     * Switch to init_stars() above for the procedurally generated cloud. */
    memcpy(g_stars, g_stars_ref, sizeof(g_stars));
    /* init_stars(); */
    init_inc_table();

    sysop_init();

    if (sysop_server_connect() != 0) {
        fprintf(stderr, "Could not connect to sysop server\n");
        return -1;
    }
    sysop_server_dma_lock();

    cached_poke(0xdd00, c64_bank_sel);
    cached_poke(0xd018, 0x8);
    cached_poke(0xd020, 0);
    cached_poke(0xd021, 0);
    cached_poke(0xd418, 0);
    sysop_screen_clear(c64_addr_screen_ram);

    init_sprites();

    /* Zero out the bitmap region and verify */
    for (int i = c64_addr_bitmap_data; i < c64_addr_bitmap_data + 0x2000; i++) {
        cached_poke(i, 0);
        if (sysop_peek(i) != 0)
            printf("Bitmap verify error at %04X\n", i);
    }
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    cached_poke(0xd011, 27 | 0x20);
    cached_poke(0xd018, 21 | 0x8);
    cached_poke(0xd020, 0);

    /* Pre-build sprite frames from the bitmap into VRAM */
    uint16_t addr = SPRITE_BASE_ADDR;
    for (int j = 0, row = 24; j < 8; j++, row += 21) {
        set_sprite_group_position(100, 80);
        set_sprite_data(row);
        for (int k = 0; k < 512; k++)
            cached_poke(addr + k, mirror[SPRITE_BASE_ADDR + k]);
        addr += 512;
    }

    printf("Starting main loop\n");

    int sprite_group_x = 100;
    int sprite_group_y = 72;
    int sprite_dir     = 1;
    /* C64 colour index: 1 = white, 2 = red */
    uint8_t sprite_color = 2;

    set_sprite_colors(sprite_color);

    static uint32_t frame_tag = 0;

    while (1) {
        /* Wait for the tag we injected at the START of the previous frame.
         * When the FPGA echoes it, the batch before that tag (two frames
         * ago) is fully done and it has begun working through the previous
         * frame's writes — the queue is at most one frame deep. */
        if (frame_tag != 0) {
            uint32_t tag;
            do {
                tag = sysop_dma_tag_data();
                usleep(50);
            } while (tag != frame_tag);
        }

        /* Tag the beginning of this frame's DMA batch so the next
         * iteration can use it as the one-frame-deep watermark. */
        frame_tag++;
        sysop_dma_write_tag(frame_tag);

        sysop_wait_vic2(251, 1);

        draw_starfield(0);
        update_animation();
        draw_starfield(1);

        /* Bounce the sprite strip horizontally; flip colour on direction change */
        sprite_group_x += sprite_dir;
        if (sprite_group_x > 200 || sprite_group_x < 0) {
            sprite_dir   = -sprite_dir;
            sprite_color = (sprite_color == 2) ? 1 : 2;  /* toggle red / white */
            set_sprite_colors(sprite_color);
        }

        /* Raster-split: reposition the sprite strip 7 times down the screen */
        uint16_t vic_wait    = (uint16_t)(sprite_group_y - 1);
        uint8_t  sprite0_ptr = 0x20;
        int      sy          = sprite_group_y;
        for (int j = 0; j < 7; j++) {
            /* With YSCROLL=3, bad lines fall at raster & 7 == 3.  Waiting for
             * cycle 50 on a bad line returns inside the stolen window, deferring
             * writes to the next free window (>= 1 line late).  Step back one
             * line so all writes land in the free window that precedes sy. */
            uint16_t actual_wait = vic_wait;
            if ((actual_wait & 7u) == 3u) actual_wait--;
            sysop_wait_vic2(actual_wait, 50);
            set_sprite_group_position((uint16_t)sprite_group_x, (uint8_t)sy);
            update_sprite_pointers(sprite0_ptr);
            sy          += 21;
            sprite0_ptr += 8;
            vic_wait    += 21;
        }
    }

    sig_handler(0);
    return 0;
}
