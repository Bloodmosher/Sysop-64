/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

/*
 * Text / character-set utilities for the Sysop-64 project.
 *
 * Provides:
 *   - ASCII <-> PETSCII conversion (sysop_map_ascii_to_petscii, sysop_map_petscii_to_ascii)
 *   - sysop_sys(): execute a machine-language routine via BASIC SYS by stuffing
 *     the keyboard buffer
 *   - explain(): decode and print VIC-II register bit-fields to stdout
 *   - printByteAsBinary(): print a byte as an 8-digit binary string
 */

/*
 * Lookup tables built once by initMappingTables().
 * Index by raw byte value; result is the mapped byte in the other encoding.
 */
unsigned char mapping_asciiToPetscii[256];
unsigned char mapping_petsciiToAscii[256];

/*
 * Build the ASCII<->PETSCII lookup tables on the first call.
 * Subsequent calls are no-ops (guarded by the static `init` flag).
 *
 * Mapping rules applied:
 *   - ASCII uppercase A-Z  -> PETSCII 0x01-0x1A
 *   - ASCII lowercase a-z  -> PETSCII 0x01-0x1A (same codes as uppercase)
 *   - ASCII digits 0-9 and most punctuation share code points with PETSCII
 *   - Space through ')' and '+', '-' are mapped explicitly
 *   - '@' -> 0x00, '*' -> 0x2A, '.' -> 0x2E, '/' -> 0x2F, '_' -> 0x20 (space)
 *   - The reverse table (PETSCII->ASCII) is derived automatically
 */
static int init = 0;
void initMappingTables()
{
    unsigned char c,k;
    if (!init)
    {
        for (c=0;c<255;c++)
        {
            mapping_asciiToPetscii[c] = 0x20;
            mapping_petsciiToAscii[c] = '.';
        }

        for (c='A';c!='Z'+1;c++)
        {
            mapping_asciiToPetscii[c] = c - 'A' + 1;
        }
        for (c='a';c!='z'+1;c++)
        {
            mapping_asciiToPetscii[c] = c - 'a' + 1;
        }
        for (c=0x30;c!=0x40;c++)
        {
            mapping_asciiToPetscii[c] = c;
        }

        c = 0x20;
        mapping_asciiToPetscii[' '] = c++;
        mapping_asciiToPetscii['!'] = c++;
        mapping_asciiToPetscii['"'] = c++;
        mapping_asciiToPetscii['#'] = c++;
        mapping_asciiToPetscii['$'] = c++;
        mapping_asciiToPetscii['%'] = c++;
        mapping_asciiToPetscii['&'] = c++;
        mapping_asciiToPetscii['\''] = c++;
        mapping_asciiToPetscii['('] = c++;
        mapping_asciiToPetscii[')'] = c++;
        mapping_asciiToPetscii['+'] = c++;
        mapping_asciiToPetscii['-'] = c++;

        mapping_asciiToPetscii['@'] = 0;
        mapping_asciiToPetscii['*'] = 0x2a;

        mapping_asciiToPetscii['.'] = 0x2e;
        mapping_asciiToPetscii['/'] = 0x2f;
        mapping_asciiToPetscii['_'] = 0x20;

        for (c=0;c<255;c++)
        {
            k = mapping_asciiToPetscii[c];
            mapping_petsciiToAscii[k] = c;
            //printf("%hc -> A2P %02X | P2A %02X\n", c, mapping_asciiToPetscii[c], mapping_petsciiToAscii[k]);
        }
        init = 1;
    }
}

/*
unsigned char sysop_map_ascii_to_petscii(char c)
{
    initMappingTables();
    return mapping_asciiToPetscii[c];
}
*/
/*
 * Map an ASCII character to C64 PETSCII (uppercase/unshifted mode).
 *
 * - Lowercase 'a'-'z' are folded to PETSCII uppercase 'A'-'Z' (subtract 0x20).
 * - Characters with no PETSCII equivalent (backtick, braces, pipe, tilde)
 *   are replaced with a space (0x20).
 * - All other code points pass through unchanged.  Note that some glyphs
 *   differ between ASCII and PETSCII:
 *     '\' (0x5C) = PETSCII '£'
 *     '^' (0x5E) = PETSCII '↑'
 *     '_' (0x5F) = PETSCII '←'
 * Returns the PETSCII byte as an unsigned char.
 */
unsigned char sysop_map_ascii_to_petscii(char ascii_char) {
    unsigned char uc = (unsigned char)ascii_char;

    // 1. Convert ASCII lowercase to PETSCII uppercase.
    //    In the C64's default character set, ASCII 'a' (0x61) displays as 'A'.
    //    The code for 'A' in both ASCII and PETSCII is 0x41.
    if (uc >= 'a' && uc <= 'z') {
        return uc - ('a' - 'A'); // or simply uc - 32
    }

    // 2. Map ASCII characters that do not exist in PETSCII to a space.
    switch (uc) {
        case '`':
        case '{':
        case '|':
        case '}':
        case '~':
            return ' '; // Map to space ' ' (0x20)
    }

    // 3. For all other characters, the code point is the same.
    //    Note: Some glyphs will be different. For example:
    //    - ASCII '_' (0x5F) is PETSCII '←' (0x5F)
    //    - ASCII '^' (0x5E) is PETSCII '↑' (0x5E)
    //    - ASCII '\' (0x5C) is PETSCII '£' (0x5C)
    return uc;
}

/*
 * Map a C64 PETSCII byte to the nearest printable ASCII character.
 * Uses the reverse lookup table built by initMappingTables().
 * Characters with no ASCII equivalent are mapped to '.' by the table.
 */
unsigned char sysop_map_petscii_to_ascii(char c) 
{
    initMappingTables();
    return mapping_petsciiToAscii[c];
}

/*
 * Execute a machine-language routine at address via the BASIC SYS command.
 * Writes "SYS <address>" followed by a carriage return into the C64 keyboard
 * input buffer ($0277-$0280) and sets the buffer length register ($C6), which
 * causes BASIC to execute the string on the next mainloop iteration.
 */
void sysop_sys(uint16_t address)
{
    char buffer[50];
    sprintf(buffer, "%d", (int)address);
    uint16_t addr = 0x277;
    int len = 5 + strlen(buffer);
    sysop_poke(addr++, 0x53);
    sysop_poke(addr++, 0x59);
    sysop_poke(addr++, 0x53);
    sysop_poke(addr++, 0x20);
    int i;
    int strLen = strlen(buffer);
    for (i=0;i<strLen;i++)
    {
        sysop_poke(addr++, buffer[i]);
    }
    sysop_poke(addr++, 0xd);
    sysop_poke(0x00c6, len);
}


/* Print n space characters to stdout. Used for indentation in explain(). */
void printSpaces(int n)
{
    for (int i=0;i<n;i++)
    {
        printf(" ");
    }
}

/*
 * Decode a VIC-II register value and print its bit-fields to stdout.
 * register_address: C64 address of the register (e.g. 0xD011, 0xD016).
 * value:            byte currently held in that register.
 * indentSpaces:     leading spaces to prepend to each output line.
 *
 * Supported registers:
 *   $D010 - sprite X coordinate high bits
 *   $D011 - vertical scroll, display enable, bitmap/text, EBC, raster hi
 *   $D012 - raster line (read/write)
 *   $D015 - sprite enable flags
 *   $D016 - horizontal scroll, multicolor, column width
 * Unrecognised addresses produce no output beyond the hex/binary header line.
 */
void explain(int indentSpaces, uint16_t register_address, uint8_t value)
{
    printSpaces(indentSpaces);
    printf("$%02X=%%", value);
    printByteAsBinary(value);
    printf(", %d decimal\n", value);

    switch(register_address)
    {
        case 0xD010:
            {
                const char* msg = "%1d=Sprite %d x-coordinate high bit\n";
                printSpaces(indentSpaces);
                printf(msg, (value & 0x80)>>7, 7);
                printSpaces(indentSpaces);
                printf(msg, (value & 0x40)>>6, 6);
                printSpaces(indentSpaces);
                printf(msg, (value & 0x20)>>5, 5);
                printSpaces(indentSpaces);
                printf(msg, (value & 0x10)>>4, 4);
                printSpaces(indentSpaces);
                printf(msg, (value & 0x8)>>3, 3);
                printSpaces(indentSpaces);
                printf(msg, (value & 0x4)>>2, 2);
                printSpaces(indentSpaces);
                printf(msg, (value & 0x2)>>1, 1);
                printSpaces(indentSpaces);
                printf(msg, (value & 0x1), 0);
            }
            break;
        case 0xD011:
            {
                printSpaces(indentSpaces);
                printf("%1d=RASTER line HI\n", (value & 0x80)>>7);
                printSpaces(indentSpaces);
                printf("%1d=EBC %s\n", (value & 0x40)>>6, ((value & 0x40)!=0) ? "ON" : "OFF");
                printSpaces(indentSpaces);
                printf("%1d=TEXT/BITMAP\n", (value & 0x20)>>5);
                printSpaces(indentSpaces);
                printf("%1d=Display Enable (DEN)\n", (value & 0x10)>>4);
                printSpaces(indentSpaces);
                printf("%1d=(0=24 rows, 1=25 rows)\n", (value & 0x8)>>3);
                printSpaces(indentSpaces);
                printf("%1d=VERT scroll (%d)\n", (value & 0x4)>>2, (value & 0x7));
                printSpaces(indentSpaces);
                printf("%1d=VERT scroll\n", (value & 0x2)>>1);
                printSpaces(indentSpaces);
                printf("%1d=VERT scroll\n", (value & 0x1));
            }
            break;
        case 0xD012:
            {
                printSpaces(indentSpaces);
                printf("Read/write current raster line\n");
            }
            break;
        case 0xD015:
            {
                printSpaces(indentSpaces);
                printf("%1d=Sprite %d enable/disable\n", (value & 0x80)>>7, 7);
                printSpaces(indentSpaces);
                printf("%1d=Sprite %d enable/disable\n", (value & 0x40)>>6, 6);
                printSpaces(indentSpaces);
                printf("%1d=Sprite %d enable/disable\n", (value & 0x20)>>5, 5);
                printSpaces(indentSpaces);
                printf("%1d=Sprite %d enable/disable\n", (value & 0x10)>>4, 4);
                printSpaces(indentSpaces);
                printf("%1d=Sprite %d enable/disable\n", (value & 0x8)>>3, 3);
                printSpaces(indentSpaces);
                printf("%1d=Sprite %d enable/disable\n", (value & 0x4)>>2, 2);
                printSpaces(indentSpaces);
                printf("%1d=Sprite %d enable/disable\n", (value & 0x2)>>1, 1);
                printSpaces(indentSpaces);
                printf("%1d=Sprite %d enable/disable\n", (value & 0x1), 0);
            }
            break;
        case 0xD016:
            {
                printSpaces(indentSpaces);
                printf("%1d=Unused\n", (value & 0x80)>>7);
                printSpaces(indentSpaces);
                printf("%1d=Unused\n", (value & 0x40)>>6);
                printSpaces(indentSpaces);
                printf("%1d=Video chip reset (unused?)\n", (value & 0x20)>>5);
                printSpaces(indentSpaces);
                printf("%1d=Multicolor (%s)\n", (value & 0x10)>>4, ((value & 0x10)!= 0) ? "Enabled" : "Disabled");
                printSpaces(indentSpaces);
                printf("%1d=(0=38 cols, 1=40 cols)\n", (value & 0x8)>>3);
                printSpaces(indentSpaces);
                printf("%1d=HORZ scroll (%d)\n", (value & 0x4)>>2, (value & 0x7));
                printSpaces(indentSpaces);
                printf("%1d=HORZ scroll\n", (value & 0x2)>>1);
                printSpaces(indentSpaces);
                printf("%1d=HORZ scroll\n", (value & 0x1));
            }
            break;


        default:
            break;
    }
}

/* Print a byte as an 8-digit binary string (MSB first) with no newline. */
void printByteAsBinary(unsigned char byte) 
{
    int i;
    for (i = 7; i >= 0; i--) 
    {
        printf("%d", (byte & (1 << i)) ? 1 : 0);
    }
}
