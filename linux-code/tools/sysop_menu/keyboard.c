/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

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
#include <signal.h>
#include "sysop64.h"
#include "keyboard.h"

// TODO: do we need to save/restore data direction registers?
// right now this assumes they're setup for reading the keyboard

typedef struct tagRowColumn
{
    uint8_t row;
    uint8_t col;
    char c;
    char shift_c;
} RowColumn;

#define NUM_RAW_KEYS 64
RowColumn raw_keys[NUM_RAW_KEYS] = {
    { 7, 7, KEY_STOP, KEY_RUN },
    { 7, 6, 'q', 'Q' },
    { 7, 5, KEY_CMD, KEY_CMD },
    { 7, 4, ' ', ' ' },
    { 7, 3, '2', '\"' },
    { 7, 2, KEY_CTRL, KEY_CTRL },
    { 7, 1, KEY_ESC, KEY_ESC },
    { 7, 0, '1', '!' },

    { 6, 7, '/', '?' },
    { 6, 6, KEY_ARROW_UP, KEY_PI },
    { 6, 5, '=', '=' },
    { 6, 4, KEY_RIGHT_SHIFT, KEY_RIGHT_SHIFT },
    { 6, 3, KEY_HOME, KEY_CLR },
    { 6, 2, ';', ']' },
    { 6, 1, '*', '*' },
    { 6, 0, KEY_LBS, KEY_LBS },

    { 5, 7, ',', '<' },
    { 5, 6, '@', '@'},
    { 5, 5, ':', '[' },
    { 5, 4, '.', '>'},
    { 5, 3, '-', '_' },
    { 5, 2, 'l', 'L' },
    { 5, 1, 'p', 'P' },
    { 5, 0, '+', '+' },

    { 4, 7, 'n', 'N' },
    { 4, 6, 'o', 'O'},
    { 4, 5, 'k', 'K' },
    { 4, 4, 'm', 'M'},
    { 4, 3, '0', '0' },
    { 4, 2, 'j', 'J' },
    { 4, 1, 'i', 'I' },
    { 4, 0, '9', ')' },

    { 3, 7, 'v', 'V' },
    { 3, 6, 'u', 'U'},
    { 3, 5, 'h', 'H' },
    { 3, 4, 'b', 'B'},
    { 3, 3, '8', '(' },
    { 3, 2, 'g', 'G' },
    { 3, 1, 'y', 'Y' },
    { 3, 0, '7', '\'' },

    { 2, 7, 'x', 'X' },
    { 2, 6, 't', 'T'},
    { 2, 5, 'f', 'H' },
    { 2, 4, 'c', 'C'},
    { 2, 3, '6', '&' },
    { 2, 2, 'd', 'D' },
    { 2, 1, 'r', 'R' },
    { 2, 0, '5', '%' },

    { 1, 7, KEY_LEFT_SHIFT, KEY_LEFT_SHIFT },
    { 1, 6, 'e', 'E'},
    { 1, 5, 's', 'S' },
    { 1, 4, 'z', 'Z'},
    { 1, 3, '4', '$' },
    { 1, 2, 'a', 'A' },
    { 1, 1, 'w', 'W' },
    { 1, 0, '3', '#' },

    { 0, 7, KEY_CRSR_DOWN, KEY_CRSR_UP },
    { 0, 6, KEY_F5, KEY_F6 },
    { 0, 5, KEY_F3, KEY_F4 },
    { 0, 4, KEY_F1, KEY_F2 },
    { 0, 3, KEY_F7, KEY_F8 },
    { 0, 2, KEY_CRSR_RIGHT, KEY_CRSR_LEFT },
    { 0, 1, KEY_RETURN, KEY_RETURN },
    { 0, 0, KEY_DELETE, KEY_INSERT }
};

#define KEY_DOWN 1
#define KEY_UP 0

uint8_t key_state[0xff];

int keyboard_init = 0;

void scan_keys(int print)
{
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    sysop_poke(0xdc02, 0xff);
    sysop_poke(0xdc03, 0x00);

    //sysop_poke(0xdc0d, 0x00);

    if (!keyboard_init)
    {
        printf("keyboard_init\n");
        memset(key_state, 0, sizeof(key_state));
        keyboard_init = 1;
        sysop_poke(0xdc00, 0xff);
        sysop_poke(0xdc01, 0x0);
    }

    int row;
    int col;
    int i;
    uint8_t pokeval, val;
    uint8_t mask;
    uint8_t scan_key_state[0xff];
    memset(scan_key_state, 0, sizeof(scan_key_state));

    // similar to the kernal routine at ea87, first see if any key is set
    sysop_poke(0xdc00, 0x00);

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    val = sysop_peek(0xdc01);

    if (val != 0xff)
    {
        for (row=0;row<8;row++)
        {
            pokeval = ~(1<<row);
            sysop_poke(0xdc00, pokeval);
            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();

            val = sysop_peek(0xdc01);
            val = sysop_peek(0xdc01);
            if (val != 255) 
            {
                for (col=0;col<8;col++)
                {
                    mask = (1<<col);
                    if ((val & mask)==0)
                    {
                        for(i=0;i<NUM_RAW_KEYS;i++)
                        {
                            if (raw_keys[i].row == row && raw_keys[i].col == col)
                            {
                                // if you see more than one key something might be wrong w/ data dir registers
                                scan_key_state[i] = KEY_DOWN;
                            }
                        }
                    }
                }
            }
        }
    }

    for (i=0;i<0xff;i++)
    {
        if (scan_key_state[i] != key_state[i])
        {
            key_state[i] = scan_key_state[i];
            if (print)
            {
                if (key_state[i] == KEY_DOWN && (i != 11 && i != 48)) // don't print shift key
                {
                    if (key_state[11] == KEY_DOWN || key_state[48] == KEY_DOWN)
                    {
                        //printf("%hc", raw_keys[i].shift_c);
                    }
                    else
                    {
                        if (raw_keys[i].c == KEY_DELETE)
                        {
                            printf("\b \b");
                        }
                        else if (raw_keys[i].c == KEY_RETURN)
                        {
                            printf("\n");
                        }
                        else 
                        {
                            printf("%hc", raw_keys[i].c);
                        }
                    }
                    fflush(stdout);
                }
                else
                {
                    //printf("Key %d up: %hc\n", (int)i, raw_keys[i].c);
                }
            }
        }
    }
}

int isKeyDown(int rawKeyIndex)
{
    return key_state[rawKeyIndex];
}

int isShiftKeyDown()
{
    int val = (isKeyDown(KEY_LEFT_SHIFT) || isKeyDown(KEY_RIGHT_SHIFT));
    return val;
}
