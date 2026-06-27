#include <time.h>
#include <error.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <termios.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include "sysop64.h"

/*
 * hdmi_colors — collection of small HDMI palette/border experiments.
 *
 * This file is intentionally closer to a demo playground than to a
 * reusable library: each demo exercises one specific feature of the
 * Sysop-64 HDMI path and most run until a key is pressed.
 *
 * Core hardware ideas used here:
 *   - The C64 still renders using its normal 16-color palette indexes.
 *   - The FPGA can change the RGB definition of a palette index at an
 *     exact HDMI scanline using sysop_wait_set_palette_entry().
 *   - Additional queue writes can batch the remaining palette updates
 *     for the same frame/line using sysop_queue_set_palette_entry().
 *   - Extended border modes let HDMI render outside the normal C64
 *     border using either direct RGB or a C64 palette index.
 *
 * Practical consequence: these demos do not rewrite bitmap pixels on the
 * C64 side every frame.  Instead they animate the *meaning* of palette
 * entries or the HDMI-only border color, which is much cheaper and makes
 * raster-like effects easy to prototype.
 */

/* Non-blocking keyboard poll used by all demos so they can animate until
 * the user presses a key, then return to the main demo loop. */
int kbhit(void)
{
    struct timeval tv = {0L, 0L};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
}

uint8_t save_r[16];
uint8_t save_g[16];
uint8_t save_b[16];

/* Save the current HDMI RGB definitions for all 16 C64 palette entries.
 * Most demos overwrite palette colors aggressively; this snapshot lets the
 * program restore the user's original colors on exit or Ctrl+C. */
void save_palette()
{
    for (int i = 0; i < 16; i++)
    {
        sysop_get_palette_entry(i, &save_r[i], &save_g[i], &save_b[i]);
        // printf("saved %d to %02X %02X %02X\n", i, save_r[i], save_g[i], save_b[i]);
    }
}

/* Restore the 16-entry palette saved by save_palette(). */
void restore_palette()
{
    for (int i = 0; i < 16; i++)
    {
        sysop_set_palette_entry(i, save_r[i], save_g[i], save_b[i]);
        // printf("restored %d to %02X %02X %02X\n", i, save_r[i], save_g[i], save_b[i]);
    }
}

extern void wait_hdmi_queue_not_full();

/* Demo 1: assign a different RGB palette to each *C64 text row*.
 *
 * Setup:
 *   - Writes a simple row-oriented text/color pattern into screen RAM and
 *     color RAM so each text row visibly references multiple palette slots.
 *   - Builds a long RGB lookup table containing seven oscillating color
 *     ramps (R, G, B, RG, RB, GB, RGB).
 *
 * Effect:
 *   - For HDMI lines aligned roughly with the 25 visible text rows, palette
 *     entry 0 is redefined at a specific line and entries 1..15 are queued
 *     immediately after.
 *   - The result is a "new palette per text row" look: the underlying C64
 *     color indexes stay fixed, but each row is rendered through a shifted
 *     RGB mapping.
 *
 * Notes for maintainers:
 *   - hdmi_y_start_line / hdmi_y_end_line are hand-tuned to the visible
 *     C64 area inside the HDMI frame.
 *   - The step size of 8 through color_table_* controls how quickly the
 *     palette drifts from row to row. */
void demo1()
{
    printf("Starting demo #1 - change palette on every C64 row\n");
    sysop_server_dma_lock();
    sysop_poke(0xd020, 0);
    sysop_poke(0xd021, 0);
    uint8_t table[16] = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x01, 0x2, 0x3, 0x4, 0x5, 0x6};
    int j = 0;
    for (int r = 0; r < 25; r++)
    {
        for (int i = 0; i < 40; i++)
        {
            if (i < 16)
                sysop_poke(0x400 + j, table[i % 16]);
            else
                sysop_poke(0x400 + j, 0xa0);
            sysop_poke(0xd800 + j, i % 16);
            j++;
        }
    }
    sysop_server_dma_unlock();
    uint8_t *color_table_r = (uint8_t *)malloc(512 * 7);
    uint8_t *color_table_g = (uint8_t *)malloc(512 * 7);
    uint8_t *color_table_b = (uint8_t *)malloc(512 * 7);
    int index = 0;

    // Each entry defines which RGB component varies (1=vary, 0=fixed)
    const int pattern[7][3] = {
        {1, 0, 0}, // R varies, G,B=0
        {0, 1, 0}, // G varies, R,B=0
        {0, 0, 1}, // B varies, R,G=0
        {1, 1, 0}, // R,G vary
        {1, 0, 1}, // R,B vary
        {0, 1, 1}, // G,B vary
        {1, 1, 1}  // all vary
    };

    for (int p = 0; p < 7; p++)
    {
        int r = 0, g = 0, b = 0;
        int r_dir = pattern[p][0] ? 1 : 0;
        int g_dir = pattern[p][1] ? 1 : 0;
        int b_dir = pattern[p][2] ? 1 : 0;

        for (int j = 0; j < 512; j++)
        {
            color_table_r[index] = r;
            color_table_g[index] = g;
            color_table_b[index] = b;
            index++;

            if (pattern[p][0])
            {
                if ((r == 255 && r_dir == 1) || (r == 0 && r_dir == -1))
                    r_dir = -r_dir;
                r += r_dir;
            }
            if (pattern[p][1])
            {
                if ((g == 255 && g_dir == 1) || (g == 0 && g_dir == -1))
                    g_dir = -g_dir;
                g += g_dir;
            }
            if (pattern[p][2])
            {
                if ((b == 255 && b_dir == 1) || (b == 0 && b_dir == -1))
                    b_dir = -b_dir;
                b += b_dir;
            }
        }
    }
    int y_offset = 0;
    index = 0;
    int color_table_size = 512 * 7;
    int hdmi_y_start_line = 136;
    int hdmi_y_end_line = 936;
    while (!kbhit())
    {
        int color_index = 0;
        int row = 0;
        for (int j = hdmi_y_start_line; j < hdmi_y_end_line; j += 32)
        {
            sysop_wait_set_palette_entry(0, j, 0, 255,
                                         color_table_r[color_index],
                                         color_table_g[color_index],
                                         color_table_b[color_index]);

            color_index = (color_index + 8) % color_table_size;
            for (int i = 1; i < 16; i++)
            {
                sysop_queue_set_palette_entry(i, 255,
                                              color_table_r[color_index],
                                              color_table_g[color_index],
                                              color_table_b[color_index]);

                color_index = (color_index + 8) % color_table_size;
            }
            row++;
        }
    }
    getchar();
    restore_palette();
    free(color_table_r);
    free(color_table_g);
    free(color_table_b);
    sysop_server_dma_lock();
    sysop_poke(0xd020, 14);
    sysop_poke(0xd021, 6);
    sysop_server_dma_unlock();
}

/* Demo 8: moving vertical color bars by stretching palette changes.
 *
 * Extended border mode 2 is enabled so the effect fills the full HDMI view.
 * The code walks down every HDMI line and updates palette entry 0 using a
 * cyclic RGB ramp.  The inner "stretch" loop deliberately burns extra ramp
 * entries without issuing more palette writes, so one color persists for a
 * wider band of lines.  Changing stretch over time makes the bars expand and
 * contract, which reads visually as bouncing color bands. */
void demo8()
{
    printf("Starting demo #8 - simple bouncing bars\n");
    sysop_hdmi_set_extended_borders(2);
    sysop_server_dma_lock();
    sysop_poke(0xd020, 0);
    sysop_poke(0xd021, 0);
    uint8_t table[16] = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x01, 0x2, 0x3, 0x4, 0x5, 0x6};
    int j = 0;
    for (int r = 0; r < 25; r++)
    {
        for (int i = 0; i < 40; i++)
        {
            sysop_poke(0x400 + j, table[i % 16]);
            sysop_poke(0xd800 + j, i % 16);
            j++;
        }
    }
    sysop_server_dma_unlock();
    uint8_t *color_table_r = (uint8_t *)malloc(512 * 7);
    uint8_t *color_table_g = (uint8_t *)malloc(512 * 7);
    uint8_t *color_table_b = (uint8_t *)malloc(512 * 7);
    int index = 0;

    // Each entry defines which RGB component varies (1=vary, 0=fixed)
    const int pattern[7][3] = {
        {1, 0, 0}, // R varies, G,B=0
        {0, 1, 0}, // G varies, R,B=0
        {0, 0, 1}, // B varies, R,G=0
        {1, 1, 0}, // R,G vary
        {1, 0, 1}, // R,B vary
        {0, 1, 1}, // G,B vary
        {1, 1, 1}  // all vary
    };

    for (int p = 0; p < 7; p++)
    {
        int r = 0, g = 0, b = 0;
        int r_dir = pattern[p][0] ? 1 : 0;
        int g_dir = pattern[p][1] ? 1 : 0;
        int b_dir = pattern[p][2] ? 1 : 0;

        for (int j = 0; j < 512; j++)
        {
            color_table_r[index] = r;
            color_table_g[index] = g;
            color_table_b[index] = b;
            index++;

            if (pattern[p][0])
            {
                if ((r == 255 && r_dir == 1) || (r == 0 && r_dir == -1))
                    r_dir = -r_dir;
                r += r_dir;
            }
            if (pattern[p][1])
            {
                if ((g == 255 && g_dir == 1) || (g == 0 && g_dir == -1))
                    g_dir = -g_dir;
                g += g_dir;
            }
            if (pattern[p][2])
            {
                if ((b == 255 && b_dir == 1) || (b == 0 && b_dir == -1))
                    b_dir = -b_dir;
                b += b_dir;
            }
        }
    }
    int y_offset = 0;
    index = 0;
    int color_table_size = 512 * 7;
    int stretch = 16;
    int stretch_dir = 1;
    int stretch_speed = 1;
    uint64_t frame = 0;
    while (!kbhit())
    {
        frame++;
        // sysop_wait_set_palette_entry(0, 136, 0xe, 255, 255, 255, 255);
        // sysop_wait_set_palette_entry(0, 168, 0xe, 255, 0, 0, 0);
        int color_index = 0;
        for (int j = 0; j < 1080; j++)
        {
            sysop_wait_set_palette_entry(0, j, 0, 255, color_table_r[color_index], color_table_g[color_index], color_table_b[color_index]);
            color_index++;
            if (color_index >= color_table_size)
                color_index = 0;
            for (int i = 1; i < stretch; i++)
            {
                // sysop_queue_set_palette_entry(i, 255, color_table_r[color_index], color_table_g[color_index], color_table_b[color_index]);
                color_index++;
                if (color_index >= color_table_size)
                    color_index = 0;
            }
        }
        if (frame % 2 == 0)
        {
            stretch += (stretch_dir * stretch_speed);
            if (stretch < 2 || stretch > 30)
            {
                stretch_dir = -stretch_dir;
                stretch += (stretch_dir * stretch_speed);
            }
        }
    }
    getchar();
    restore_palette();
    free(color_table_r);
    free(color_table_g);
    free(color_table_b);
    sysop_server_dma_lock();
    sysop_poke(0xd020, 14);
    sysop_poke(0xd021, 6);
    sysop_server_dma_unlock();
    sysop_hdmi_set_extended_borders(0);
}

/* Demo 7: coarse checkerboard using palette changes on every HDMI line.
 *
 * The checkerboard is not drawn into bitmap RAM.  Instead the demo issues
 * palette writes at x positions spaced by SQUARE pixels and flips between
 * black and white as x/y move through square boundaries.  Increasing SQUARE
 * over time zooms the checkerboard outward; decreasing it zooms back in. */
void demo7()
{
    printf("Starting demo #7 - checkerboard effect\n");
    sysop_server_dma_lock();
    sysop_poke(0xd020, 0);
    sysop_poke(0xd021, 0);
    for (int i = 0; i < 1000; i++)
    {
        sysop_poke(0xd800 + i, 14);
    }
    sysop_server_dma_unlock();

    uint8_t c = 0;
    int SQUARE = 90;
    int square_dir = 1;
    int square_speed = 1;
    // while(1)
    while (!kbhit())
    {
        uint8_t start_c = 0;
        // for (int y=0;y<1080;y++)
        uint8_t white_color = 255;
        uint8_t white_step = 10; // 256/SQUARE;
        if (white_step <= 0)
            white_step = 1;
        for (int y = 0; y < 1080; y++)
        {
            for (int x = 0; x < 1920; x += SQUARE)
            {
                if (x == 0)
                    c = start_c;
                if (c == 255)
                {
                    sysop_wait_set_palette_entry(x, y, 0, 255, white_color, white_color, white_color);
                }
                else
                    sysop_wait_set_palette_entry(x, y, 0, 255, c, c, c);
                c = (c == 0 ? 255 : 0);
            }
            if (y != 0 && y % SQUARE == 0)
            {
                start_c = (start_c == 0 ? 255 : 0);
            }
        }
        sysop_queue_set_palette_entry(0, 255, 0, 0, 0x00);

        SQUARE += (square_dir * square_speed);
        if (SQUARE <= 90)
        {
            SQUARE = 90;
            square_dir = 1;
        }
        else if (SQUARE >= 800)
        {
            SQUARE = 800;
            square_dir = -1;
        }
    }
    getchar();
    restore_palette();
}

/* Demo 6: full-height color cycling of the background plus extended border.
 *
 * This is similar to demo2/demo3, but it specifically uses extended border
 * mode so the color treatment continues outside the normal C64 active area.
 * "which_color = 0" means palette slot 0 is treated as the animated color,
 * and sysop_queue_set_extended_border_color_index() tells the HDMI border to
 * reference that same C64 color index.  The border therefore tracks the
 * background automatically as palette entry 0 changes per scanline. */
void demo6()
{
    printf("Starting demo #6 - background and border cycling with extended border on\n");
    sysop_server_dma_lock();
    sysop_poke(0xd020, 0);
    sysop_poke(0xd021, 0);
    sysop_server_dma_unlock();

    uint8_t *color_table_r = (uint8_t *)malloc(512 * 7);
    uint8_t *color_table_g = (uint8_t *)malloc(512 * 7);
    uint8_t *color_table_b = (uint8_t *)malloc(512 * 7);
    int index = 0;

    // Each entry defines which RGB component varies (1=vary, 0=fixed)
    const int pattern[7][3] = {
        {1, 0, 0}, // R varies, G,B=0
        {0, 1, 0}, // G varies, R,B=0
        {0, 0, 1}, // B varies, R,G=0
        {1, 1, 0}, // R,G vary
        {1, 0, 1}, // R,B vary
        {0, 1, 1}, // G,B vary
        {1, 1, 1}  // all vary
    };

    for (int p = 0; p < 7; p++)
    {
        int r = 0, g = 0, b = 0;
        int r_dir = pattern[p][0] ? 1 : 0;
        int g_dir = pattern[p][1] ? 1 : 0;
        int b_dir = pattern[p][2] ? 1 : 0;

        for (int j = 0; j < 512; j++)
        {
            color_table_r[index] = r;
            color_table_g[index] = g;
            color_table_b[index] = b;
            index++;

            if (pattern[p][0])
            {
                if ((r == 255 && r_dir == 1) || (r == 0 && r_dir == -1))
                    r_dir = -r_dir;
                r += r_dir;
            }
            if (pattern[p][1])
            {
                if ((g == 255 && g_dir == 1) || (g == 0 && g_dir == -1))
                    g_dir = -g_dir;
                g += g_dir;
            }
            if (pattern[p][2])
            {
                if ((b == 255 && b_dir == 1) || (b == 0 && b_dir == -1))
                    b_dir = -b_dir;
                b += b_dir;
            }
        }
    }
    int y_offset = 0;
    index = 0;
    int color_table_size = 512 * 7;

    uint8_t which_color = 0;
    sysop_queue_set_extended_border_color_index(which_color);

    while (!kbhit())
    {
        for (int j = 0; j < 1080; j++)
        {
            int color_index = (j + index) % color_table_size;
            sysop_wait_set_palette_entry(0, y_offset + j, which_color, 255, color_table_r[color_index], color_table_g[color_index], color_table_b[color_index]);
            if (j == 1079)
                sysop_wait_set_palette_entry(0, j, which_color, 255, 0, 0, 0);
        }
        index += 5;
    }

    getchar();
    restore_palette();
    free(color_table_r);
    free(color_table_g);
    free(color_table_b);
}

/* Demo 2: change one palette slot on every HDMI scanline.
 *
 * which_color selects which C64 palette entry to animate.  The body builds
 * the same long RGB ramp used elsewhere, then redefines that palette entry
 * on each of the 1080 HDMI lines.  Because the same C64 pixels are rendered
 * through a different RGB value on each line, the picture becomes a smooth
 * vertical gradient / raster effect without touching the C64 framebuffer.
 *
 * This is the core example future developers should look at when they want
 * line-exact palette effects tied to HDMI scanline position. */
void demo2(uint8_t which_color)
{
    printf("Starting demo #2 - cycle colors on every HDMI line\n");
    sysop_server_dma_lock();
    sysop_poke(0xd020, 0);
    sysop_poke(0xd021, 0);
    for (int i = 0; i < 1000; i++)
    {
        sysop_poke(0x400 + i, (uint8_t)i);
        sysop_poke(0xd800 + i, 1);
    }
    sysop_server_dma_unlock();

    uint8_t *color_table_r = (uint8_t *)malloc(512 * 7);
    uint8_t *color_table_g = (uint8_t *)malloc(512 * 7);
    uint8_t *color_table_b = (uint8_t *)malloc(512 * 7);
    int index = 0;

    // Each entry defines which RGB component varies (1=vary, 0=fixed)
    const int pattern[7][3] = {
        {1, 0, 0}, // R varies, G,B=0
        {0, 1, 0}, // G varies, R,B=0
        {0, 0, 1}, // B varies, R,G=0
        {1, 1, 0}, // R,G vary
        {1, 0, 1}, // R,B vary
        {0, 1, 1}, // G,B vary
        {1, 1, 1}  // all vary
    };

    for (int p = 0; p < 7; p++)
    {
        int r = 0, g = 0, b = 0;
        int r_dir = pattern[p][0] ? 1 : 0;
        int g_dir = pattern[p][1] ? 1 : 0;
        int b_dir = pattern[p][2] ? 1 : 0;

        for (int j = 0; j < 512; j++)
        {
            color_table_r[index] = r;
            color_table_g[index] = g;
            color_table_b[index] = b;
            index++;

            if (pattern[p][0])
            {
                if ((r == 255 && r_dir == 1) || (r == 0 && r_dir == -1))
                    r_dir = -r_dir;
                r += r_dir;
            }
            if (pattern[p][1])
            {
                if ((g == 255 && g_dir == 1) || (g == 0 && g_dir == -1))
                    g_dir = -g_dir;
                g += g_dir;
            }
            if (pattern[p][2])
            {
                if ((b == 255 && b_dir == 1) || (b == 0 && b_dir == -1))
                    b_dir = -b_dir;
                b += b_dir;
            }
        }
    }
    index = 0;
    int color_table_size = 512 * 7;

    while (!kbhit())
    {
        for (int hdmi_y = 0; hdmi_y < 1080; hdmi_y++)
        {
            int color_index = (hdmi_y + index) % color_table_size;
            sysop_wait_set_palette_entry(0, hdmi_y, which_color, 255, color_table_r[color_index], color_table_g[color_index], color_table_b[color_index]);
            if (hdmi_y == 1079)
                sysop_wait_set_palette_entry(0, hdmi_y, which_color, 255, 0, 0, 0);
        }
        index += 5;
    }

    getchar();
    restore_palette();
    free(color_table_r);
    free(color_table_g);
    free(color_table_b);
}

/* bmlogo3: single-channel red sweep used by a specific logo experiment.
 *
 * This is essentially a stripped-down demo2() with only one ramp pattern
 * (red only) and reverse motion through the table.  It exists as a special-
 * purpose effect rather than a general demo, but it is useful as a minimal
 * example of how to drive one palette slot with a single-component ramp. */
void bmlogo3(uint8_t which_color)
{
    /*sysop_server_dma_lock();
    sysop_poke(0xd020, 0);
    sysop_poke(0xd021, 0);
    for (int i=0;i<1000;i++) {
        sysop_poke(0x400+i, (uint8_t)i);
        sysop_poke(0xd800+i, 1);
    }
    sysop_server_dma_unlock();
    */

    uint8_t *color_table_r = (uint8_t *)malloc(512 * 1);
    uint8_t *color_table_g = (uint8_t *)malloc(512 * 1);
    uint8_t *color_table_b = (uint8_t *)malloc(512 * 1);
    int index = 0;

    // Each entry defines which RGB component varies (1=vary, 0=fixed)
    const int pattern[1][3] = {
        {1, 0, 0}, // R varies, G,B=0
    };

    for (int p = 0; p < 1; p++)
    {
        int r = 0, g = 0, b = 0;
        int r_dir = pattern[p][0] ? 1 : 0;
        int g_dir = pattern[p][1] ? 1 : 0;
        int b_dir = pattern[p][2] ? 1 : 0;

        for (int j = 0; j < 512; j++)
        {
            color_table_r[index] = r;
            color_table_g[index] = g;
            color_table_b[index] = b;
            index++;

            if (pattern[p][0])
            {
                if ((r == 255 && r_dir == 1) || (r == 0 && r_dir == -1))
                    r_dir = -r_dir;
                r += r_dir;
            }
            if (pattern[p][1])
            {
                if ((g == 255 && g_dir == 1) || (g == 0 && g_dir == -1))
                    g_dir = -g_dir;
                g += g_dir;
            }
            if (pattern[p][2])
            {
                if ((b == 255 && b_dir == 1) || (b == 0 && b_dir == -1))
                    b_dir = -b_dir;
                b += b_dir;
            }
        }
    }
    index = 511;
    int color_table_size = 512 * 1;

    while (!kbhit())
    {
        for (int hdmi_y = 0; hdmi_y < 1080; hdmi_y++)
        {
            int color_index = (hdmi_y + index) % color_table_size;
            sysop_wait_set_palette_entry(0, hdmi_y, which_color, 255, color_table_r[color_index], color_table_g[color_index], color_table_b[color_index]);
            if (hdmi_y == 1079)
                sysop_wait_set_palette_entry(0, hdmi_y, which_color, 255, 0, 0, 0);
        }
        index -= 5;
        if (index < 0)
            index = 511;
    }

    getchar();
    restore_palette();
    free(color_table_r);
    free(color_table_g);
    free(color_table_b);
}

/* Demo 3: same "one palette slot per HDMI line" idea as demo2(), intended
 * for background/border color experiments.
 *
 * In practice this is very close to demo2(which_color=0).  It was likely
 * kept separate while experimenting with palette slot 0, which the C64 often
 * uses for background/border related rendering.  Future cleanup could merge
 * demo2 and demo3 if the project no longer needs both entry points. */
void demo3()
{
    printf("Starting demo #3 - cycle background/border color on every HDMI line\n");
    sysop_server_dma_lock();
    sysop_poke(0xd020, 0);
    sysop_poke(0xd021, 0);
    for (int i = 0; i < 1000; i++)
    {
        sysop_poke(0x400 + i, (uint8_t)i);
        sysop_poke(0xd800 + i, 1);
    }
    sysop_server_dma_unlock();

    uint8_t *color_table_r = (uint8_t *)malloc(512 * 7);
    uint8_t *color_table_g = (uint8_t *)malloc(512 * 7);
    uint8_t *color_table_b = (uint8_t *)malloc(512 * 7);
    int index = 0;

    // Each entry defines which RGB component varies (1=vary, 0=fixed)
    const int pattern[7][3] = {
        {1, 0, 0}, // R varies, G,B=0
        {0, 1, 0}, // G varies, R,B=0
        {0, 0, 1}, // B varies, R,G=0
        {1, 1, 0}, // R,G vary
        {1, 0, 1}, // R,B vary
        {0, 1, 1}, // G,B vary
        {1, 1, 1}  // all vary
    };

    for (int p = 0; p < 7; p++)
    {
        int r = 0, g = 0, b = 0;
        int r_dir = pattern[p][0] ? 1 : 0;
        int g_dir = pattern[p][1] ? 1 : 0;
        int b_dir = pattern[p][2] ? 1 : 0;

        for (int j = 0; j < 512; j++)
        {
            color_table_r[index] = r;
            color_table_g[index] = g;
            color_table_b[index] = b;
            index++;

            if (pattern[p][0])
            {
                if ((r == 255 && r_dir == 1) || (r == 0 && r_dir == -1))
                    r_dir = -r_dir;
                r += r_dir;
            }
            if (pattern[p][1])
            {
                if ((g == 255 && g_dir == 1) || (g == 0 && g_dir == -1))
                    g_dir = -g_dir;
                g += g_dir;
            }
            if (pattern[p][2])
            {
                if ((b == 255 && b_dir == 1) || (b == 0 && b_dir == -1))
                    b_dir = -b_dir;
                b += b_dir;
            }
        }
    }
    index = 0;
    int color_table_size = 512 * 7;
    uint8_t which_color = 0;

    while (!kbhit())
    {
        for (int hdmi_y = 0; hdmi_y < 1080; hdmi_y++)
        {
            int color_index = (hdmi_y + index) % color_table_size;
            sysop_wait_set_palette_entry(0, hdmi_y, which_color, 255, color_table_r[color_index], color_table_g[color_index], color_table_b[color_index]);
            if (hdmi_y == 1079)
                sysop_wait_set_palette_entry(0, hdmi_y, which_color, 255, 0, 0, 0);
        }
        index += 5;
    }

    getchar();
    restore_palette();
    free(color_table_r);
    free(color_table_g);
    free(color_table_b);
}

/* Demo 4: extended border mode 1 with direct RGB control.
 *
 * Mode 1 bypasses the normal 16-color C64 palette for the extended border
 * region and accepts explicit RGBA values.  This demo updates the border to
 * a new random color every 500 ms, making it the simplest demonstration of
 * the "HDMI-only border independent of C64 color indexes" feature. */
void demo4()
{
    printf("Starting demo #4 - extended border mode 1 - independent RGB color\n");
    sysop_hdmi_set_extended_borders(1);
    while (!kbhit())
    {
        sysop_wait_set_extended_border_color(0, 0, 0, 255, rand() % 255, rand() % 255, rand() % 255);
        usleep(500000);
    }
    getchar();
    restore_palette();
}

/* Demo 5: extended border mode 2 using C64 palette indexes.
 *
 * Unlike demo4, mode 2 renders the extended border by referencing one of the
 * normal C64 palette slots (0..15).  The demo cycles through all indexes,
 * updates C64 color RAM to make the current index visible in text mode, and
 * then points the HDMI extended border at the same index.  This is useful
 * for validating that HDMI border rendering stays in sync with the active
 * C64 palette and color-RAM contents. */
void demo5()
{
    printf("Starting demo #5 - extended border mode 2 - C64 color index 0-15\n");
    sysop_hdmi_set_extended_borders(2);
    uint8_t color_index = 0;
    while (!kbhit())
    {
        sysop_server_dma_lock();
        for (int i = 0; i < 1000; i++)
        {
            sysop_poke(0xd800 + i, color_index);
        }
        sysop_server_dma_unlock();

        sysop_queue_set_extended_border_color_index(color_index);
        printf("C64 color index %d\n", color_index);
        usleep(500000);
        color_index++;
    }
    getchar();
    sysop_server_dma_lock();
    for (int i = 0; i < 1000; i++)
    {
        sysop_poke(0xd800 + i, 1);
    }
    sysop_server_dma_unlock();
    restore_palette();
}

/* Chop-N-Drop palette timing demo.
 *
 * This targets a very specific raster trick: two palette entries are changed
 * partway down the HDMI frame and restored near the top of the next frame.
 * That makes colors 10, 6, and 2 appear different only in a vertical band,
 * imitating classic raster-split behavior while still operating through the
 * HDMI palette queue.  Useful when validating scanline-precise palette
 * timing against a known art asset or logo. */
void chop_n_drop()
{
    printf("Starting Chop-N-Drop color palette demo\n");

    uint8_t r[3];
    uint8_t g[3];
    uint8_t b[3];

    uint64_t frames = 0;
    while (1)
    {
        if (frames % 50 == 0)
        {
            for (int i = 0; i < 3; i++)
            {
                r[i] = rand() % 255;
                g[i] = rand() % 255;
                b[i] = rand() % 255;
            }
        }
        frames++;

        // on line 580 set to the new colors for 10, 6, and 2
        sysop_wait_set_palette_entry(1, 580, 10, 255, 192, 128, 128);
        sysop_queue_set_palette_entry(6, 255, r[1], g[1], b[1]);
        sysop_queue_set_palette_entry(2, 255, r[2], g[2], b[2]);

        // on line 1 set back to the normal colors for 10, 6, and 2
        sysop_wait_set_palette_entry(1, 1, 10, 255, 254, 74, 87);
        sysop_queue_set_palette_entry(6, 255, 33, 27, 174);
        sysop_queue_set_palette_entry(2, 255, 190, 26, 36);
    }
    restore_palette();
}

/* Small in-place array rotation helper retained from earlier experiments.
 * It is currently unused by the active demo loop but can be useful if a
 * future effect wants to rotate palette tables without rebuilding them. */
void rotate_colors(uint8_t *color_table, int start, int end)
{
    if (start > end)
        return;

    uint8_t first = color_table[start];
    for (int i = start; i < end; i++)
    {
        color_table[i] = color_table[i + 1];
    }
    color_table[end] = first;
}

/* Switch stdin into non-canonical, no-echo mode so kbhit() works like a
 * simple "press any key to leave the current demo" control. */
void set_conio_terminal_mode(void)
{
    struct termios new_termios;
    tcgetattr(STDIN_FILENO, &new_termios);
    new_termios.c_lflag &= ~(ICANON | ECHO); // Disable canonical mode & echo
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
}

/* Restore normal cooked terminal mode after the demo exits. */
void reset_terminal_mode(void)
{
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

/* Common cleanup path for normal exit and SIGINT.
 *
 * Restores HDMI border mode, terminal mode, palette, and the default C64
 * border/background colors, then disconnects from the Sysop-64 server. */
void fixup_at_exit()
{
    sysop_hdmi_set_extended_borders(0);
    reset_terminal_mode();
    restore_palette();
    sysop_server_dma_lock();
    sysop_poke(0xd020, 14);
    sysop_poke(0xd021, 6);
    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();
}

/* Ctrl+C handler so aborted demos still put the machine back into a sane
 * visual/terminal state. */
void sigintHandler(int signal)
{
    fixup_at_exit();
    exit(128 + signal);
}

/* Main demo driver.
 *
 * Default behavior is an endless loop through demos 1..8, where each demo
 * runs until the user presses a key.  There are also a couple of ad-hoc
 * one-off modes used during development:
 *   extended_index <n> — enable extended border mode 2 and pin it to one
 *                        C64 palette index
 *   chop_n_drop        — run the raster-split palette experiment only
 *   bmlogo3            — run the single-channel palette sweep only
 */
int main(int argc, char **argv)
{
    sysop_init();
    int res = sysop_server_connect();
    if (res == -1)
    {
        printf("Failed to connect to Sysop-64 server\n");
        return 0;
    }

    printf("Press any key to jump to next demo\n\n");
    set_conio_terminal_mode();

    save_palette();

    uint8_t data = 1;
    uint8_t once = 0;
    uint8_t once2 = 0;
    uint16_t vic_line = 0;
    uint8_t cycle = 0;
    uint8_t charNum = 0;
    set_conio_terminal_mode();
    signal(SIGINT, sigintHandler);

    if (argc > 2 && strcmp(argv[1], "extended_index") == 0)
    {
        uint8_t index = (uint8_t)strtoll(argv[2], NULL, 10);
        sysop_hdmi_set_extended_borders(2);
        sysop_queue_set_extended_border_color_index(index);
        printf("Set extended border on with color index %d\n", index);
        getchar();
        sysop_hdmi_set_extended_borders(0);
        reset_terminal_mode();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "chop_n_drop") == 0)
    {
        chop_n_drop();
    }
    if (argc > 1 && strcmp(argv[1], "bmlogo3") == 0)
    {
        bmlogo3(2);
    }
    while (1)
    {
        demo1();
        demo2(1);
        demo3();
        demo4();
        demo5();
        demo6();
        demo7();
        demo8();

        /* sysop_hdmi_set_extended_borders(2);
         sysop_queue_set_extended_border_color_index(0);
         //queue_set_extended_border_color(255, 0, 0, 0);
         demo1();
         */
    }

    return 0;
}
