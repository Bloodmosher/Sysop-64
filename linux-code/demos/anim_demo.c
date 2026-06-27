/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * anim_demo — C64 character-set animation demo using Cairo-rendered frames.
 *
 * Loads pre-rendered 8 KB bitmap frames from ./frames/frame_NNNN.bin,
 * then streams them to the C64's character-set RAM every VBlank using
 * cycle-timed DMA writes.  A per-line colour-cycling effect is applied
 * by poking $D020/$D021 at exact raster positions.  The Cairo library
 * is used at setup time to render text bitmaps for frame generation.
 *
 * USAGE:
 *     anim_demo
 *     (expects ./frames/ to contain frame_0001.bin … frame_NNNN.bin)
 */

#include <stdio.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>

#include "sysop64.h"
#include "c64keys.h"

#define COLOR_TABLE_LENGTH 45

/* Each group of 9 entries ramps from black through mid-tones to white and back,
   cycling through different C64 colour palettes. */
uint8_t colorTable[COLOR_TABLE_LENGTH] =
{
     0, 11,12,15, 1,15,12,11, 0,   // grey
     0,  6,14, 3, 1, 3,14, 6, 0,   // green/yellow
     0,  2, 8,10, 1,10, 8, 2, 0,   // red
     0, 11, 5,13, 1,13, 5,11, 0,   // teal/cyan
     0, 11,12, 7, 1, 7,12,11, 0,   // orange
};

int g_colorIndex = 0;
uint8_t g_cyclesPerLine = 63;
uint16_t g_vic_lines = 312;

void AddColorPokes()
{
    sysop_poke(0xd021, colorTable[g_colorIndex]);
    sysop_poke(0xd020, colorTable[g_colorIndex]);

    g_colorIndex++;
    if (g_colorIndex == COLOR_TABLE_LENGTH)
        g_colorIndex = 0;
}

/* Stream `count` bytes from `buffer[startIndex]` into the C64 character-set
   RAM at $4800, one byte per DMA cycle.  Colour-cycling pokes are inserted at
   the start of each raster line.  VIC character-fetch lines (every 8th line,
   cycle >= 12) are skipped to avoid bus contention. */
void UpdateCharacterSet(int cycle, int line, int startIndex, int count, unsigned char* buffer)
{
    uint16_t address = 0x4800;

    for (int i=startIndex;i<(startIndex+count);)
    {
        if (cycle == 1)
        {
            AddColorPokes();
            cycle += 2;
        }
        if ((line & 7) == 3 && cycle >= 12)
        {
            // Skip to next line to avoid VIC character-fetch bus conflict.
            sysop_wait_vic2(line+1, 1);
            line++;
            cycle = 1;
        }
        else
        {
            sysop_poke(address++, buffer[i]);
            i++;
            cycle++;
            if (cycle == (g_cyclesPerLine+1)) {
                cycle = 1;
                line++;
            }
        }
        if (line >= g_vic_lines)
            line = 0;
    }
}

unsigned char* data = NULL;

void sigintHandler(int signal)
{
    sysop_poke(0xd020, 0);
    sysop_poke(0xd021, 0);
    sysop_poke(0xd015, 0);
    sysop_poke(0xd011, 0x1b);
    sysop_poke(0xd018, 0x15);
    sysop_poke(0xdd00, 0x97);
    sysop_screen_clear(0x400);

    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();

    if (data != NULL)
    {
        free(data);
    }
    exit(signal);
}

/* Returns the number of entries in `path`, excluding "." and "..". */
int countFrames(const char* path)
{
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    if ((dir = opendir(path)) == NULL) {
        perror("Error opening directory");
        exit(EXIT_FAILURE);
    }
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        count++;
    }
    closedir(dir);
    return count;
}

/* Reads exactly 8192 bytes from `filename` into `ptr`.  Returns 0 on success,
   1 if the file cannot be opened. */
int loadNextFrame(const char* filename, unsigned char* ptr)
{
    FILE* file = fopen(filename, "rb");
    if (file == NULL)
        return 1;
    fread(ptr, 1, 8192, file);
    fclose(file);
    return 0;
}


/* Allocates a contiguous buffer for all frames and loads them from
   `path/frame_NNNN.bin` starting at frame 1.  Returns the directory entry
   count (one more than the number of frame files actually loaded). */
int loadFrames(const char* path)
{
    int count = countFrames(path);
    data = (unsigned char*)malloc(count * 8192);
    memset(data, 0, count * 8192);

    unsigned char* ptr = data;
    char filename[255];
    for (int i=1; i!=count; i++)
    {
        sprintf(filename, "%s/frame_%04d.bin", path, i);
        loadNextFrame(filename, ptr);
        ptr += 8192;
    }
    return count;
}

int main(int argc, char **argv) 
{
    signal(SIGINT, sigintHandler);
    sysop_init();


    uint8_t vic_info = sysop_get_vic_info();
    if (!(vic_info & 0x80)) {
        printf("FPGA has not yet determined VIC model.\n");
        return -1;
    }
    else {
        printf("VIC Model: ");
        switch (vic_info & 0x7)
        {
            case VIC_CHIP_6567R56A:
            {
                printf("NTSC OLD");
                g_cyclesPerLine = 64;
                g_vic_lines = 262;
            }
            break;
            case VIC_CHIP_6567R8:
            {
                printf("NTSC NEW");
                g_cyclesPerLine = 65;
                g_vic_lines = 263;
            }
            break;
            case VIC_CHIP_6572RO_DREAN:
            {
                printf("DREAN");
                g_cyclesPerLine = 65;
                g_vic_lines = 312;
            }
            break;
            case VIC_CHIP_6569:
            { 
                printf("PAL");
                g_cyclesPerLine = 63;
                g_vic_lines = 312;
            }
            break;

            default: 
            {
                printf("Unknown");
            }  
            break;
        }
        printf("\n");
    }

    int res = sysop_server_connect();
    if (res != 0) {
        printf("Failed to connect to sysop server\n");
        return -1;
    }

    sysop_server_dma_lock();
   
    sysop_poke(0xdd00, 0x96); // switch to vic bank #1 so we can use $4000-$7FFF
    sysop_poke(0xd011, 0x1b); // std char mode
    sysop_poke(0xd018, 0x12); // first charset at $4000
    sysop_poke(0xd020, 0);

    // Initialise screen RAM with sequential character codes so the charset
    // animation is immediately visible across the whole 40×25 screen.
    uint8_t value = 0;
    for (int i=0; i<1000; i++)
        sysop_poke(0x4400+i, value++);

    int holdFrames = 0;

    double start_font_size = 200.0;
    double font_size       = start_font_size;
    double hold_font_size  = 40.0;
    double step            = 2;
    int    frameSize       = 8192;

    int loadCount = loadFrames("./frames");
    printf("Loaded %d frames\n", loadCount);
    // countFrames returns one more than the number of frame_NNNN.bin files.
    int lineCount = loadCount - 1;

    unsigned char* ptr = data;

    int frameNumber     = 1;
    int lineIndex       = 0;
    int startColorIndex = 0;
    int colorSpeed      = 2;
    int speed           = colorSpeed;

    for (int i=0; i<1000; i++)
        sysop_poke(0xd800+i, 0); // initialise color RAM to black

    uint8_t wait_x = 1;
    while (1)
    {
        g_colorIndex = startColorIndex;

        // on NTSC we wrap around and use a few lines, so let's start here
        if (g_vic_lines > 263) // PAL or Drean
        {
            for (int i=5;i!=96;i++)
            {
                sysop_wait_vic2(i, wait_x);
                AddColorPokes();
            }
        }
        else { // ntsc
            for (int i=24;i!=96;i++)
            {
                sysop_wait_vic2(i, wait_x);
                AddColorPokes();
            }
        }

        sysop_wait_vic2(96, wait_x);
        UpdateCharacterSet(wait_x, 96, 2048, 2048, ptr);
        
        for (int i=134;i!=145;i++)
        {
            sysop_wait_vic2(i, wait_x);
            AddColorPokes();
        }

        sysop_wait_vic2(145, wait_x);
        UpdateCharacterSet(wait_x, 145, 4096, 2048, ptr);

        for (int i=183;i!=193;i++)
        {
            sysop_wait_vic2(i, wait_x);
            AddColorPokes();
        }

        sysop_wait_vic2(193, wait_x);
        UpdateCharacterSet(wait_x, 193, 6144, 2048, ptr);

        if (g_vic_lines > 263) // PAL or Drean
        {
            sysop_wait_vic2(231, wait_x);
            UpdateCharacterSet(wait_x, 231, 0, 2048, ptr);

            for (int i=269;i!=301;i++)
            {
                sysop_wait_vic2(i, wait_x);
                AddColorPokes();
            }
        }
        else {
            sysop_wait_vic2(231, wait_x);
            UpdateCharacterSet(wait_x, 231, 0, 2048, ptr);

            for (int i=5;i!=24;i++)
            {
                sysop_wait_vic2(i, wait_x);
                AddColorPokes();
            }
        }

        speed--;
        if (speed == 0)
        {
            startColorIndex++;
            if (startColorIndex == COLOR_TABLE_LENGTH)
            {
                startColorIndex = 0;
            }
            speed = colorSpeed;
        }

        if (holdFrames > 0)
        {
            holdFrames--;
            if (holdFrames == 0)
            {
                ptr += frameSize;
                font_size = start_font_size;
                lineIndex++;
                if (lineIndex == lineCount)
                {
                    lineIndex = 0;
                    ptr = data;
                }
            }
        }
        else
        {
            // Advance one frame per VBlank.  When font_size reaches
            // hold_font_size the current chunk is held for one extra frame
            // before moving to the next.
            font_size -= step;
            if (font_size == hold_font_size)
            {
                holdFrames = 1;
            }
            else
            {
                ptr += frameSize;
                frameNumber++;
                if (frameNumber > lineCount)
                {
                    frameNumber = 1;
                    ptr = data;
                }
            }
        }
    }

    sigintHandler(0);

    return 0;
}
