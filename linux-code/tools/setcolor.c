/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <error.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "sysop64.h"

int main(int argc, char **argv)
{
    uint8_t index, r, g, b;

    /* Default Sysop-64 color palette, as defined in the Verilog source. */
    static const uint8_t default_palette[16][3] = {
        {  0,   0,   0}, /* 0  black        */
        {255, 255, 255}, /* 1  white        */
        {190,  26,  36}, /* 2  red          */
        { 48, 230, 198}, /* 3  cyan         */
        {180,  26, 226}, /* 4  purple       */
        { 31, 210,  30}, /* 5  green        */
        { 33,  27, 174}, /* 6  blue         */
        {223, 246,  10}, /* 7  yellow       */
        {184,  65,   4}, /* 8  orange       */
        {106,  51,   4}, /* 9  brown        */
        {254,  74,  87}, /* 10 light red    */
        { 66,  69,  64}, /* 11 dark grey    */
        {112, 116, 111}, /* 12 grey         */
        { 89, 254,  89}, /* 13 light green  */
        { 85,  83, 254}, /* 14 light blue   */
        {164, 167, 162}, /* 15 light grey   */
    };

    int count = 1;
    int i = 0, j = 0;
    int repeat = 1;

    if (argc < 2)
    {
        // Mode selection is argument-driven:
        // 1) --defaults                       -> restore factory palette
        // 2) <index>                          -> read current palette value
        // 3) <index> <r> <g> <b>             -> immediate palette write
        // 4) <index> <r> <g> <b> <alpha>     -> queued HDMI write with alpha
        printf("Expected arguments (in decimal): --defaults | <index> [<red> <green> <blue>] [alpha]\n"
               "  --defaults                  Restore all 16 palette entries to factory values.\n"
               "  <index>                     Read current RGB value of palette entry <index>.\n"
               "  <index> <r> <g> <b>         Write palette entry <index> directly.\n"
               "  <index> <r> <g> <b> <alpha> Queue an HDMI palette write with explicit alpha bit.\n");
        return -1;
    }

    if (strcmp(argv[1], "--defaults") == 0)
    {
        sysop_open_bridge();
        for (i = 0; i < 16; i++)
            sysop_set_palette_entry((uint8_t)i,
                              default_palette[i][0],
                              default_palette[i][1],
                              default_palette[i][2]);
        sysop_close_bridge();
        printf("Palette restored to Sysop-64 defaults.\n");
        return 0;
    }

    index = strtoll(argv[1], NULL, 10);
    sysop_open_bridge();

    if (argc > 2)
    {
        // "Set color" path: update palette entry by index using decimal RGB values.
        // This is useful for quick visual validation of palette slots.
        r = strtoll(argv[2], NULL, 10);
        g = strtoll(argv[3], NULL, 10);
        b = strtoll(argv[4], NULL, 10);
        if (argc == 6)
        {
            // Queued HDMI command path with explicit alpha bit.
            // Use this when testing HDMI command queue behavior and transparency-enabled entries.
            uint8_t a = strtoll(argv[5], NULL, 10);
            sysop_queue_set_palette_entry((uint8_t)index, (uint8_t)a, (uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
        else
        {
            // Direct palette register write path with RGB only.
            // Keeps behavior simple for basic color table edits.
            sysop_set_palette_entry((uint8_t)index, (uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
    else
    {
        // "Read color" path: print current RGB values for one palette entry.
        // Handy when comparing expected palette state before/after demo runs.
        sysop_get_palette_entry((uint8_t)index, &r, &g, &b);
        printf("Color %d: %d, %d, %d\n", (int)index, (int)r, (int)g, (int)b);
    }

    sysop_close_bridge();
    return 0;
}
