/*
 * Sysop-64 
 * https://github.com/Bloodmosher/Sysop-64 
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project 
 *
 * sysop64_c64_keyboard.cpp
 *
 * C64 hardware keyboard matrix scanning and key-code translation.
 *
 * Contains the raw_keys[] lookup table that maps every C64 keyboard matrix
 * position (row, col) to a character and its shifted variant, map_c64_key()
 * which converts C64_KEY_* codes to Linux KEY_* codes, and scan_keys_pipe()
 * which actively drives the CIA1 port lines via DMA to scan all 8 rows of
 * the physical C64 keyboard and emits Linux input_event structs on a pipe
 * whenever a key state changes.
 */

#include "sysop64_internal.h"
// c64 keyboard handling
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


/**
 * Maps a C64 key code to a standard Linux input event key code.
 *
 * This function takes a C64 key code (as defined by the C64_KEY_* macros)
 * and returns the corresponding KEY_* code from <linux/input-event-codes.h>.
 *
 * For C64 keys that don't have a direct or sensible mapping to a modern
 * keyboard layout (e.g., C64_KEY_CMD, C64_KEY_PI), this function
 * returns KEY_RESERVED (0).
 *
 * Note: Some C64 keys are ambiguous without knowing the Shift key state (e.g.
 * CRSR vs F-keys). This function maps to the un-shifted version. A more
 * complex handler would be needed to manage shift states.
 */
ushort map_c64_key(ushort c64_key) {
    switch (c64_key) {
        case C64_KEY_RUN:         return KEY_STOP; // RUN/STOP key
        case C64_KEY_Q:           return KEY_Q;
        case C64_KEY_SPACE:       return KEY_SPACE;
        case C64_KEY_2:           return KEY_2;
        case C64_KEY_CTRL:        return KEY_TAB; //KEY_LEFTCTRL;
        case C64_KEY_ESC:         return KEY_ESC;
        case C64_KEY_1:           return KEY_1;
        case C64_KEY_SLASH:       return KEY_SLASH;
        case C64_KEY_ARROW_UP:    return KEY_UP; // PI is unmapped
        case C64_KEY_EQUAL:       return KEY_EQUAL;
        case C64_KEY_RIGHT_SHIFT: return KEY_RIGHTSHIFT;
        case C64_KEY_HOME:        return KEY_HOME; // CLR/HOME key
        case C64_KEY_SEMICOLON:   return KEY_SEMICOLON;
        case C64_KEY_STAR:        return KEY_KPASTERISK;
        case C64_KEY_COMMA:       return KEY_COMMA;
        case C64_KEY_AT:          return KEY_APOSTROPHE; // Often mapped here
        case C64_KEY_COLON:       return KEY_SEMICOLON;
        case C64_KEY_PERIOD:      return KEY_DOT;
        case C64_KEY_MINUS:       return KEY_MINUS;
        case C64_KEY_L:           return KEY_L;
        case C64_KEY_P:           return KEY_P;
        case C64_KEY_PLUS:        return KEY_EQUAL;
        case C64_KEY_N:           return KEY_N;
        case C64_KEY_O:           return KEY_O;
        case C64_KEY_K:           return KEY_K;
        case C64_KEY_M:           return KEY_M;
        case C64_KEY_ZERO:        return KEY_0;
        case C64_KEY_J:           return KEY_J;
        case C64_KEY_I:           return KEY_I;
        case C64_KEY_9:           return KEY_9;
        case C64_KEY_V:           return KEY_V;
        case C64_KEY_U:           return KEY_U;
        case C64_KEY_H:           return KEY_H;
        case C64_KEY_B:           return KEY_B;
        case C64_KEY_8:           return KEY_8;
        case C64_KEY_G:           return KEY_G;
        case C64_KEY_Y:           return KEY_Y;
        case C64_KEY_7:           return KEY_7;
        case C64_KEY_X:           return KEY_X;
        case C64_KEY_T:           return KEY_T;
        case C64_KEY_F:           return KEY_F;
        case C64_KEY_C:           return KEY_C;
        case C64_KEY_6:           return KEY_6;
        case C64_KEY_D:           return KEY_D;
        case C64_KEY_R:           return KEY_R;
        case C64_KEY_5:           return KEY_5;
        case C64_KEY_LEFT_SHIFT:  return KEY_LEFTSHIFT;
        case C64_KEY_E:           return KEY_E;
        case C64_KEY_S:           return KEY_S;
        case C64_KEY_Z:           return KEY_Z;
        case C64_KEY_4:           return KEY_4;
        case C64_KEY_A:           return KEY_A;
        case C64_KEY_W:           return KEY_W;
        case C64_KEY_3:           return KEY_3;
        case C64_KEY_CRSR_DOWN:   return KEY_DOWN; // Maps to CRSR DOWN/UP
        case C64_KEY_F5:          return KEY_F5;     // Maps to F5/F6
        case C64_KEY_F3:          return KEY_F3;     // Maps to F3/F4
        case C64_KEY_F1:          return KEY_F1;     // Maps to F1/F2
        case C64_KEY_F7:          return KEY_F7;     // Maps to F7/F8
        case C64_KEY_CRSR_RIGHT:  return KEY_RIGHT;  // Maps to CRSR RIGHT/LEFT
        case C64_KEY_RETURN:      return KEY_ENTER;
        case C64_KEY_DELETE:      return KEY_BACKSPACE; // Maps to INST/DEL
        case C64_KEY_CMD:         return KEY_LEFTCTRL; // Maps to CMD key, often used as CTRL in modern keyboards
        // Unmapped keys
        
        case C64_KEY_LBS:
        default:
            return KEY_RESERVED; // Return 0 for unmapped keys
    }
}


static uint8_t key_state[0xff];
static int keyboard_init = 0;

// Scans all 8 rows of the C64 keyboard matrix via DMA sysop_poke/sysop_peek, then for
// any key whose state has changed emits a Linux input_event on fd (the write
// end of a pipe). Must be called while the caller holds the DMA lock.
// On first call, initialises the CIA1 data-direction registers and the
// internal key_state[] shadow array.
void scan_keys_pipe(int fd)
{
    if (!keyboard_init)
    {
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

    // trying out only doing this once
    //sysop_poke(0xdc00, 0xff);
    //sysop_poke(0xdc01, 0x0);

	sysop_poke(0xdc03, 0x00);  // port b ddr (input)
	sysop_poke(0xdc02, 0xff);  // port a ddr (output)

    // similar to the kernal routine at ea87, first see if any key is set
    sysop_poke(0xdc00, 0x00);

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    val = sysop_peek(0xdc01);

    if (val != 0xff)
    {
        for (row=0;row<8;row++)
        {
            //sysop_poke(0xdc00, 0xff);
            //sysop_poke(0xdc01, 0x0);

            pokeval = ~(1<<row);
            //printf("poking %02X\n", pokeval);
            sysop_poke(0xdc00, pokeval);
            //usleep(5000);
            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();

            val = sysop_peek(0xdc01);
            val = sysop_peek(0xdc01);
            if (val != 255) 
            {
                //printf("read %d\n", (int)val);
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
                                scan_key_state[i] = C64_KEY_DOWN;
                                //scan_key_state[(int)raw_keys[i].c] = KEY_DOWN;

                                //if (print)
                                //{
                                //    printf("setting key %d to down\n", i);
                                    //printf("setting key %d to down\n", (int)raw_keys[i].c);
                                //}
                                //printf("%hc", raw_keys[i].c);
                                //printf("{row %d, col %d, sysop_poke %02X, sysop_peek %02X, mask %02X} %hc\n", row, col, pokeval, val, mask, raw_keys[i].c);
                                //fflush(stdout);
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
            printf("key state change for %d, new state %d\n", i, scan_key_state[i]);
            key_state[i] = scan_key_state[i];

            struct input_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EV_KEY;
            ev.code = map_c64_key(i); // Map C64 key to Linux key code
            if (ev.code == KEY_RESERVED) {
                printf("key %d was not mapped\n", i);
                continue; // Skip unmapped keys
            }
            ev.value = (key_state[i] == C64_KEY_DOWN) ? 1 : 0;
            gettimeofday(&ev.time, NULL);

            write(fd, &ev, sizeof(ev));
        }
    }
}

