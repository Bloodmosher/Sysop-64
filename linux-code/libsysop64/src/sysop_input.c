/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

uint8_t sysop_read_joystick(uint8_t joystick_number)
{
    get_library_lock();

    dma_wait_not_full();

    // 68 is read joystick; 
    uint32_t val = ((uint32_t)68<<24) | ((joystick_number == 2) ? 1 : 0);
    *((uint32_t*)sysop64_cmd2_map) = val;

    uint64_t data = *((uint64_t*)sysop64_dma_joystick_data_map);
    uint8_t joy_data = 0;

    if (joystick_number == 2) 
        joy_data = (uint8_t) ((data>>8) & 0xFF);
    else
        joy_data = (uint8_t) (data & 0xFF);
    
    release_library_lock();
    return joy_data;
}

uint64_t sysop_read_key_data()
{
    get_library_lock();

    dma_wait_not_full();
    
    // 66 is scan keys in the fpga's dma manager; no payload bytes
    uint32_t val = ((uint32_t)66<<24);
    *((uint32_t*)sysop64_cmd2_map) = val;

    uint64_t keys = *((uint64_t*)sysop64_dma_read_key_data_map);

    release_library_lock();
    return keys;
}

int sysop_is_button_pressed(uint8_t id)
{
    uint64_t gpio_data = sysop_read_gpio_data();
    uint64_t val = id == 1 ? 0x000000000002 : 0x000000000001;
    return (gpio_data & val)==0;
}

#include "c64keys.h"

typedef struct tagRowColumn
{
    uint8_t row;
    uint8_t col;
    char c;
    char shift_c;
} RowColumn;

#define NUM_RAW_KEYS 64
static RowColumn raw_keys[NUM_RAW_KEYS] = {
    { 7, 7, C64_KEY_STOP, C64_KEY_RUN },
    { 7, 6, 'q', 'Q' },
    { 7, 5, C64_KEY_CMD, C64_KEY_CMD },
    { 7, 4, ' ', ' ' },
    { 7, 3, '2', '\"' },
    { 7, 2, C64_KEY_CTRL, C64_KEY_CTRL },
    { 7, 1, C64_KEY_ESC, C64_KEY_ESC },
    { 7, 0, '1', '!' },

    { 6, 7, '/', '?' },
    { 6, 6, C64_KEY_ARROW_UP, C64_KEY_PI },
    { 6, 5, '=', '=' },
    { 6, 4, C64_KEY_RIGHT_SHIFT, C64_KEY_RIGHT_SHIFT },
    { 6, 3, C64_KEY_HOME, C64_KEY_CLR },
    { 6, 2, ';', ']' },
    { 6, 1, '*', '*' },
    { 6, 0, C64_KEY_LBS, C64_KEY_LBS },

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

    { 1, 7, C64_KEY_LEFT_SHIFT, C64_KEY_LEFT_SHIFT },
    { 1, 6, 'e', 'E'},
    { 1, 5, 's', 'S' },
    { 1, 4, 'z', 'Z'},
    { 1, 3, '4', '$' },
    { 1, 2, 'a', 'A' },
    { 1, 1, 'w', 'W' },
    { 1, 0, '3', '#' },

    { 0, 7, C64_KEY_CRSR_DOWN, C64_KEY_CRSR_UP },
    { 0, 6, C64_KEY_F5, C64_KEY_F6 },
    { 0, 5, C64_KEY_F3, C64_KEY_F4 },
    { 0, 4, C64_KEY_F1, C64_KEY_F2 },
    { 0, 3, C64_KEY_F7, C64_KEY_F8 },
    { 0, 2, C64_KEY_CRSR_RIGHT, C64_KEY_CRSR_LEFT },
    { 0, 1, C64_KEY_RETURN, C64_KEY_RETURN },
    { 0, 0, C64_KEY_DELETE, C64_KEY_INSERT }
};

static uint8_t system_key_state[0xff];
static int system_keyboard_init = 0;

void sysop_scan_keys()
{
    if (!system_keyboard_init)
    {
        memset(system_key_state, 0, sizeof(system_key_state));
        system_keyboard_init = 1;
    }

    uint8_t scan_key_state[0xff];
    memset(scan_key_state, 0, sizeof(scan_key_state));
    
    uint64_t key_data = sysop_read_key_data();

    for (int row=0;row < 8;row++)
    {
        uint8_t val = (key_data >> (row * 8)) & 0xff;
        if (val != 255)
        {
            for (int col=0;col<8;col++)
            {
                uint8_t mask = (1<<col);
                if ((val & mask)==0)
                {
                    for(int k=0;k<NUM_RAW_KEYS;k++)
                    {
                        if (raw_keys[k].row == row && raw_keys[k].col == col)
                        {
                            // if you see more than one key something might be wrong w/ data dir registers
                            scan_key_state[k] = C64_KEY_DOWN;
                        }
                    }
                }
            }
        }
    }

    for (int i=0;i<0xff;i++)
    {
        if (scan_key_state[i] != system_key_state[i])
        {
            system_key_state[i] = scan_key_state[i];
        }
    }
}


int sysop_is_key_down(int rawKeyIndex)
{
    return system_key_state[rawKeyIndex];
}

int sysop_is_shift_key_down()
{
    int val = (sysop_is_key_down(C64_KEY_LEFT_SHIFT) || sysop_is_key_down(C64_KEY_RIGHT_SHIFT));
    return val;
}


int system_any_key_down()
{
    sysop_poke(0xDC00, 0x00); // clear VIA 1 DRA, keyboard column drive
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    uint8_t val = sysop_peek(0xDC01); // read VIA 1 DRB, keyboard row port
    val = sysop_peek(0xDC01); // read VIA 1 DRB, keyboard row port
    if (val == 0xFF)
    {
        // no key pressed
        return 0;
    }

    // if we get here, a key is pressed
    return 1;
}
