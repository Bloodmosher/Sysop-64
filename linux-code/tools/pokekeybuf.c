/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * pokekeybuf — inject keystrokes into the C64 keyboard buffer.
 *
 * The C64 KERNAL maintains a 10-byte software keyboard buffer at $0277–$0280.
 * The byte at $00C6 holds the number of characters currently in that buffer.
 * Any PETSCII value written there is treated exactly like a real keypress —
 * BASIC, machine-code programs, or the KERNAL input routines will consume
 * the bytes as if the user had typed them.
 *
 * This tool reads text lines from stdin on the host and injects them into
 * the running C64 via the sysop DMA bridge, chunk by chunk (at most 10 bytes
 * per write, matching the hardware buffer size).  It waits for the buffer
 * to drain to zero before writing each chunk so it never overwrites pending
 * keystrokes.  ASCII is converted to PETSCII before writing.
 *
 * Usage:
 *   echo "LOAD\"$\",8,1" | pokekeybuf
 *   pokekeybuf          (interactive: type lines, Ctrl-C to quit)
 *
 * Note: newline (\n) is mapped to PETSCII $0D (RETURN) so that each line
 * entered on the host triggers a RETURN on the C64 side.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sysop64.h"

/*
 * poke_bytes — write up to 10 PETSCII bytes into the C64 keyboard buffer.
 *
 * Blocks until the buffer is empty ($00C6 == 0), then acquires the DMA lock,
 * writes each byte (converted from ASCII to PETSCII) to $0277 onward, and
 * finally sets $00C6 to the number of bytes written to inform the KERNAL that
 * new keystrokes are available.
 */
void poke_bytes(const uint8_t *buf, size_t len)
{
    /* The C64 keyboard buffer holds at most 10 characters. */
    if (len > 10)
        len = 10;

    /* Wait for the C64 to drain the buffer before writing new keys.
     * $00C6 is the KERNAL pending-key count; poll until it reaches 0. */
    while (1) {
        uint8_t pending = sysop_internal_peek(0x00c6);
        if (pending == 0)
            break;
        usleep(5000); /* 5 ms polling interval */
    }

    /* Acquire DMA ownership and write characters to the keyboard buffer
     * starting at $0277 (KEYD — the 10-byte KERNAL key queue). */
    sysop_server_dma_lock();
    uint16_t addr = 0x0277;
    for (size_t i = 0; i < len; i++) {
        unsigned char petscii_val = sysop_map_ascii_to_petscii((char)buf[i]);
        if (buf[i] == '\n')
            petscii_val = 0x0d; /* map Unix newline to C64 RETURN */
        sysop_poke(addr++, petscii_val);
    }
    /* Tell the KERNAL how many new characters are waiting. */
    sysop_poke(0x00c6, (uint8_t)len);
    sysop_server_dma_unlock();
}

int main(int argc, char **argv)
{
    sysop_init();
    int result = sysop_server_connect();
    if (result != 0) {
        printf("Error connecting to sysop %d\n", result);
        exit(-1);
    }

    printf("Enter text, CTRL-C to quit\n");

    /* Only show the interactive prompt when stdin is a terminal.
     * When piping (e.g. echo "..." | pokekeybuf) the prompt is suppressed
     * and the loop exits cleanly when the pipe closes (fgets returns NULL). */
    int interactive = isatty(STDIN_FILENO);

    char line[1024];
    const int CHUNK_SIZE = 10; /* matches the C64 keyboard buffer length */

    while (1) {
        if (interactive)
            printf("> ");

        if (fgets(line, sizeof(line), stdin) == NULL)
            break; /* EOF: pipe closed or Ctrl-D in interactive mode */

        size_t len = strlen(line);

        /* Send the line in 10-byte chunks so we never exceed the buffer. */
        const uint8_t *ptr = (const uint8_t *)line;
        while (len > 0) {
            size_t chunk = (len > CHUNK_SIZE) ? CHUNK_SIZE : len;
            poke_bytes(ptr, chunk);
            ptr += chunk;
            len -= chunk;
        }
    }

    sysop_server_disconnect();
    sysop_uninit();
    return result;
}
