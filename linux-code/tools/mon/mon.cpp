/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * mon.cpp — C64 hardware monitor: main entry point and command dispatch loop.
 *
 * Commands
 * --------
 *   a  <addr> [insn]     Assemble instruction(s) at address
 *   b                    Break into monitor via NMI
 *   bp  <addr>           Set breakpoint
 *   bpc [-norestore]     Clear all breakpoints
 *   bpd <addr>           Delete breakpoint
 *   bps                  Show breakpoints
 *   cart_o  <s> <e> <b…> Fill cartridge ROM range with byte pattern
 *   cart_poke <a> <b…>   Poke cartridge ROM
 *   charset <a> <n> [-v] Dump character ROM bitmap
 *   d  [start [end]]     Disassemble memory
 *   dec <val>            Convert decimal → hex
 *   dma-freeze           Assert DMA / freeze CPU
 *   dma-unfreeze         De-assert DMA / resume CPU
 *   dmab                 Break via DMA + sampler
 *   dmar                 Resume via DMA + sampler
 *   dmaz                 Step via DMA + sampler
 *   find "text"          Hunt all 64KB for PETSCII text and list matches
 *   g                    Go: resume from breakpoint
 *   h  <s> <e> <b…>      Hunt memory for byte pattern
 *   hex <val>            Convert hex → decimal
 *   info                 Show VIC/screen configuration
 *   load <filename>      Load PRG (like C64 LOAD command)
 *   loadbin <f> <addr>   Load raw binary at address
 *   m  [start [end]]     Memory dump
 *   nextpc               Show next PC value
 *   nmi                  Trigger NMI freeze
 *   nmi-resume           Signal C64 stub to resume
 *   o  <s> <e> <b…>      Fill memory with byte pattern
 *   sysop_peek$1               Read $0001 via on-C64 stub
 *   sysop_poke <a> <b…>        Poke memory
 *   sysop_poke$1 <b>           Write $0001 via on-C64 stub
 *   r                    Show registers
 *   reset                Reset C64
 *   resume               Resume from breakpoint (no unfreeze)
 *   run                  Type RUN into C64 keyboard buffer
 *   save <s> <e> <f> [-prg]  Save memory to file
 *   setpc <addr>         Set program counter
 *   stack <b>            Set stack via on-C64 stub
 *   sysop_sys <addr>           Type SYS <addr> into keyboard buffer
 *   x                    Exit monitor
 *   z                    Single-step
 *   ?                    Show this help
 */

#include "mon_private.h"
#include "mon_input.h"
#include "mon_breakpoints.h"
#include "mon_cpu.h"
#include "mon_dma.h"

// ---------------------------------------------------------------------------
// Global definitions
// ---------------------------------------------------------------------------

uint16_t g_pc     = 0;
uint8_t  g_status = 0;
int      own_dma  = 0;

// ---------------------------------------------------------------------------
// print_vic_info — display current VIC-II display configuration
// ---------------------------------------------------------------------------

static void print_vic_info(void)
{
    OwnDma odma;

    uint8_t d011 = sysop_peek(0xd011);
    printf("D011: $%02X\n", d011);
    printf("  Scroll: $%02X  Height: %d rows  Mode: %s\n",
           d011 & 0x3,
           (d011 & (1 << 3)) ? 25 : 24,
           (d011 & (1 << 5)) ? "Bitmap" : "Text");
    if (d011 & (1 << 6))
        printf("  Extended background color mode ON\n");
    printf("\n");

    uint8_t  dd00     = sysop_peek(0xdd00);
    uint16_t vic_base = (uint16_t)((3 - (dd00 & 0x3)) * 16384);
    printf("DD00: $%02X  VIC bank: $%04X – $%04X\n",
           dd00, vic_base, vic_base + 16383);
    printf("\n");

    uint8_t d018 = sysop_peek(0xd018);
    printf("D018: $%02X\n", d018);

    if (d011 & (1 << 5)) {
        // Bitmap mode
        uint16_t bmp_addr = vic_base + ((d018 & (1 << 3)) ? 0x2000 : 0);
        uint8_t  colors   = (d018 >> 4) & 0xf;
        printf("  Bitmap at $%04X\n", bmp_addr);
        printf("  Colors at $%04X\n", (uint16_t)(colors * 1024 + vic_base));
    } else {
        // Text mode
        uint16_t screen = (uint16_t)(((d018 >> 4) & 0xf) * 1024 + vic_base);
        printf("  Screen memory:   $%04X – $%04X\n", screen, screen + 999);
        printf("  Sprite pointers: $%04X – $%04X\n",
               screen + 1016, screen + 1023);
        for (int i = 0; i < 8; i++) {
            uint8_t  ptr        = sysop_peek(screen + 1016 + i);
            uint16_t sprite_addr = (uint16_t)(ptr * 64 + vic_base);
            printf("  Sprite %d: $%02X ($%04X)\n", i, ptr, sprite_addr);
        }
    }

    uint8_t d016 = sysop_peek(0xd016);
    printf("\nD016: $%02X  Multi-color: %s\n",
           d016, (d016 & (1 << 4)) ? "ON" : "OFF");

    uint64_t debug2 = sysop_debug2();
    uint8_t  chl    = (uint8_t)(((debug2 >> 49) & 0x7) + 0x30);
    printf("CHL: $%02X\n", chl);
}

// ---------------------------------------------------------------------------
// tool_screen_info — standalone VIC info (used by --screen-info flag)
// ---------------------------------------------------------------------------

static void tool_screen_info(void)
{
    int res = sysop_server_connect();
    if (res == -1) {
        perror("sysop_server_connect failed");
        return;
    }
    sysop_server_dma_lock();
    print_vic_info();
    sysop_server_dma_unlock();
}

// ---------------------------------------------------------------------------
// tool_disassemble — standalone disassembler (used by --disassemble flag)
// ---------------------------------------------------------------------------

static void tool_disassemble(int argc, char **argv)
{
    uint16_t start_addr = 0;
    if (argc > 2)
        start_addr = (uint16_t)strtol(argv[2], NULL, 16);

    uint16_t end_addr = start_addr + 32;
    if (argc > 3)
        end_addr = (uint16_t)strtol(argv[3], NULL, 16);

    int acme = (argc > 4 && strcmp(argv[4], "-acme") == 0);
    int bytes = end_addr - start_addr;

    uint8_t buffer[0xFFFF];
    for (int i = 0; i < bytes; i++)
        buffer[i] = sysop_internal_peek(start_addr + i);

    if (acme)
        printf("*=$%04X\n\n", start_addr);

    int i = 0, start = 0, bytesUsed = 0, bytesNeeded = 0;
    while (i < bytes) {
        Disassemble(1, g_pc, (uint16_t)(start_addr + i),
                    buffer[start], buffer, start, bytes,
                    &bytesUsed, &bytesNeeded, acme);
        i     += bytesUsed;
        start += bytesUsed;
    }
}

// ---------------------------------------------------------------------------
// show_help
// ---------------------------------------------------------------------------

static void show_help(void)
{
    printf("--- Assemble / Disassemble ---\n");
    printf("  a <addr> [insn]              Assemble instruction(s) at address\n");
    printf("  d [start [end]] [-acme]      Disassemble memory\n");
    printf("  charset <addr> <n> [-v]      Dump n character bitmaps (8 bytes each)\n");
    printf("\n");
    printf("--- Memory ---\n");
    printf("  m [start [end]] [-c|-acme]   Memory dump\n");
    printf("  o  <start> <end> <b1> [b…]   Fill memory with repeating byte pattern\n");
    printf("  h  <start> <end> <b1> [b…]   Hunt memory for byte pattern (* = wildcard)\n");
    printf("  find \"text\"                 Hunt all memory for PETSCII text\n");
    printf("  sysop_poke  <addr> <b1> [b…]       Write bytes to C64 memory\n");
    printf("  save  <start> <end> <file> [-prg]  Save memory range to file\n");
    printf("  loadbin <file> <addr>         Load raw binary at address\n");
    printf("  load <filename>               Load PRG (like C64 LOAD)\n");
    printf("  load_stub                     Enable I/O, load /usr/local/bin/mon_io at $DF00\n");
    printf("\n");
    printf("--- Cartridge ---\n");
    printf("  cart_poke <addr> <b1> [b…]   Write bytes to cartridge ROM\n");
    printf("  cart_o <start> <end> <b1> [b…]  Fill cartridge ROM with byte pattern\n");
    printf("  cart_enable_8k                Enable 8K cartridge ($8000-$9FFF)\n");
    printf("  cart_enable_16k               Enable 16K cartridge ($8000-$BFFF)\n");
    printf("  cart_disable                  Disable cartridge\n");
    printf("\n");
    printf("--- Registers / CPU ---\n");
    printf("  r                             Show CPU registers\n");
    printf("  setpc <addr>                  Set program counter\n");
    printf("  b                             Break into monitor via NMI\n");
    printf("  g                             Go: resume from breakpoint\n");
    printf("  resume                        Resume from breakpoint (no NMI unfreeze)\n");
    printf("  z                             Single-step\n");
    printf("  nextpc                        Show next PC value\n");
    printf("  sysop_sys <addr>                    Type 'SYS <addr>' into keyboard buffer\n");
    printf("  run                           Type 'RUN' into keyboard buffer\n");
    printf("\n");
    printf("--- Breakpoints ---\n");
    printf("  bp  <addr>                    Set breakpoint\n");
    printf("  bpd <addr>                    Delete breakpoint\n");
    printf("  bpc [-norestore]              Clear all breakpoints\n");
    printf("  bps                           Show breakpoints\n");
    printf("\n");
    printf("--- DMA / sampler ---\n");
    printf("  dma-freeze                    Assert DMA (freeze CPU)\n");
    printf("  dma-unfreeze                  De-assert DMA (resume CPU)\n");
    printf("  dmab                          Break via DMA + bus sampler\n");
    printf("  dmar                          Resume via DMA + bus sampler\n");
    printf("  dmaz                          Step via DMA + bus sampler\n");
    printf("\n");
    printf("--- NMI stub commands ---\n");
    printf("  nmi                           Trigger NMI freeze\n");
    printf("  nmi-resume                    Signal C64 stub to resume\n");
    printf("  sysop_peek$1                        Read $0001 via on-C64 stub\n");
    printf("  sysop_poke$1 <b>                    Write $0001 via on-C64 stub\n");
    printf("  stack  <b>                    Set stack pointer via on-C64 stub\n");
    printf("\n");
    printf("--- Info / conversion ---\n");
    printf("  info                          Show VIC-II / screen configuration\n");
    printf("  dec <val>                     Decimal  -> hexadecimal\n");
    printf("  hex <val>                     Hexadecimal -> decimal\n");
    printf("  reset                         Reset C64\n");
    printf("  x                             Exit\n");
    printf("  ?                             Show this help\n");
}

// ---------------------------------------------------------------------------
// build_arg_string — join arguments[start..arg_count-1] into buf with spaces
// ---------------------------------------------------------------------------

static void build_arg_string(char **arguments, int start, int arg_count,
                              char *buf, int buf_size)
{
    buf[0] = '\0';
    int remaining = buf_size - 1;
    for (int z = start; z < arg_count && remaining > 0; z++) {
        strncat(buf, arguments[z], remaining);
        remaining -= (int)strlen(arguments[z]);
        if (z < arg_count - 1 && remaining > 0) {
            strncat(buf, " ", remaining);
            remaining--;
        }
    }
}

// ---------------------------------------------------------------------------
// extract_find_query — parse query text from: find "text with spaces"
// ---------------------------------------------------------------------------

static int extract_find_query(const char *line, char *out, int out_size)
{
    if (line == NULL || out == NULL || out_size < 2)
        return -1;

    out[0] = '\0';

    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;

    // Skip command token ("find") and leading whitespace before the payload.
    while (*p != '\0' && *p != ' ' && *p != '\t')
        p++;
    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '\0')
        return -1;

    const char *start = p;
    const char *end = NULL;

    if (*p == '"') {
        start = ++p;
        end = strrchr(start, '"');
        if (end == NULL)
            end = start + strlen(start);
    } else {
        end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
    }

    int len = (int)(end - start);
    if (len <= 0)
        return -1;

    if (len >= out_size)
        len = out_size - 1;

    memcpy(out, start, len);
    out[len] = '\0';
    return len;
}

// ---------------------------------------------------------------------------
// map_ascii_to_petscii_legacy — monitor text mapping used by many old tools
// ---------------------------------------------------------------------------

static uint8_t map_ascii_to_petscii_legacy(char c)
{
    uint8_t uc = (uint8_t)c;

    if (uc >= 'A' && uc <= 'Z')
        return (uint8_t)(uc - 'A' + 1);
    if (uc >= 'a' && uc <= 'z')
        return (uint8_t)(uc - 'a' + 1);
    if (uc >= '0' && uc <= '9')
        return uc;

    switch (uc) {
        case ' ': return 0x20;
        case '!': return 0x21;
        case '"': return 0x22;
        case '#': return 0x23;
        case '$': return 0x24;
        case '%': return 0x25;
        case '&': return 0x26;
        case '\'': return 0x27;
        case '(': return 0x28;
        case ')': return 0x29;
        case '+': return 0x2A;
        case '-': return 0x2B;
        case '@': return 0x00;
        case '*': return 0x2A;
        case '.': return 0x2E;
        case '/': return 0x2F;
        case '_': return 0x20;
        default:
            return sysop_map_ascii_to_petscii((char)uc);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    sysop_init();

    // Non-interactive one-shot modes
    if (argc >= 3 && strcmp(argv[1], "--disassemble") == 0) {
        InitInstructionTables();
        tool_disassemble(argc, argv);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--screen-info") == 0) {
        tool_screen_info();
        return 0;
    }

    setup_raw_terminal();
    signal(SIGINT, sigintHandler);

    InitInstructionTables();

    int connect = sysop_server_connect();
    if (connect != 0) {
        printf("Unable to connect to sysop64 server.\n");
        sysop_uninit();
        return -1;
    }

    // ---------------------------------------------------------------------------
    // Command loop state
    // ---------------------------------------------------------------------------
    uint16_t address   = 0;     // address cursor (advances across d/m commands)
    uint8_t  data      = 0;
    int      count     = 32;
    char     line[MAX_BUFFER_SIZE];
    char     line_raw[MAX_BUFFER_SIZE];
    char    *arguments[MAX_ARGUMENTS];
    int      arg_count = 0;

    // Shared buffers used by several commands
    uint8_t  buffer[64 * 1024];
    int      buffer_index = 0;
    int      i = 0, j = 0;
    int      bytesUsed = 0, bytesNeeded = 0;

    int runloop = 1;
    while (runloop) {
        printf("\n> ");
        fflush(stdout);
        line[0] = '\0';
        arg_count = 0;

        if (get_line(line, sizeof(line)) == NULL)
            continue;

        printf("\n");
        strncpy(line_raw, line, sizeof(line_raw) - 1);
        line_raw[sizeof(line_raw) - 1] = '\0';
        printf("%s\n", line_raw);  // echo the command (was: printf(line) — format string bug fixed)

        // Tokenise the line
        char *token = strtok(line, " \t\n");
        while (token != NULL && arg_count < MAX_ARGUMENTS - 1) {
            arguments[arg_count++] = token;
            token = strtok(NULL, " \t\n");
        }
        arguments[arg_count] = NULL;
        if (arg_count == 0)
            continue;

        // ----------------------------------------------------------------
        // Command dispatch
        // ----------------------------------------------------------------

        if (strcasecmp(arguments[0], "?") == 0) {
            show_help();
            continue;
        }

        if (strcasecmp(arguments[0], "x") == 0) {
            printf("Exiting...\n");
            break;
        }

        // -- DMA sampler commands ----------------------------------------

        if (strcasecmp(arguments[0], "dmab") == 0) {
            command_dma_break();
            continue;
        }
        if (strcasecmp(arguments[0], "dma-freeze") == 0) {
            sysop_dma_enable();
            own_dma = 1;
            continue;
        }
        if (strcasecmp(arguments[0], "dmar") == 0) {
            command_dma_resume();
            continue;
        }
        if (strcasecmp(arguments[0], "dmaz") == 0) {
            command_dma_step();
            continue;
        }
        if (strcasecmp(arguments[0], "dma-unfreeze") == 0) {
            sysop_dma_disable();
            own_dma = 0;
            continue;
        }

        // -- NMI break ---------------------------------------------------

        if (strcasecmp(arguments[0], "b") == 0) {
            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();
            *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_ENABLE_IO;
            *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_ENABLE_NMI_TRAP;
            usleep(50000);
            *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_NMI_FREEZE;
            usleep(50000);
            printf("Waiting for debugger...");
            fflush(stdout);
            wait_for_command_finish();
            printf("ready.\n");
            *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_NMI_UNFREEZE;
            usleep(50000);
            show_registers();
            continue;
        }

        // -- Reset -------------------------------------------------------

        if (strcasecmp(arguments[0], "reset") == 0) {
            *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_RESET;
            continue;
        }

        // -- Load PRG ----------------------------------------------------

        if (strcasecmp(arguments[0], "load") == 0) {
            if (arg_count < 2) {
                printf("load <filename>\n");
                continue;
            }
            char filename[MAX_BUFFER_SIZE];
            build_arg_string(arguments, 1, arg_count, filename, sizeof(filename));
            sysop_load(filename);
            continue;
        }

        // -- SYS ---------------------------------------------------------

        if (strcasecmp(arguments[0], "sysop_sys") == 0) {
            if (arg_count < 2) {
                printf("sysop_sys <address>\n");
                continue;
            }
            uint16_t sysaddr = (uint16_t)strtol(arguments[1], NULL, 10);
            OwnDma odma;
            sysop_sys(sysaddr);
            continue;
        }

        // -- Assemble ----------------------------------------------------

        if (strcasecmp(arguments[0], "a") == 0) {
            uint16_t start_addr = address;
            if (arg_count > 1)
                start_addr = (uint16_t)strtol(arguments[1], NULL, 16);

            char instruction[MAX_BUFFER_SIZE];
            if (arg_count > 2) {
                build_arg_string(arguments, 2, arg_count,
                                 instruction, sizeof(instruction));
            } else {
                printf(GREEN " %04X: " RESET, start_addr);
                if (get_line(instruction, sizeof(instruction)) == NULL)
                    continue;
            }

            while (1) {
                unsigned char *pBytes = NULL;
                uint8_t cBytes = 0;

                if (assemble(start_addr, instruction, &pBytes, &cBytes) != 0) {
                    printf("Error\n");
                    break;
                }

                {
                    OwnDma odma;
                    for (i = 0; i < cBytes; i++) {
                        sysop_poke(start_addr + i, pBytes[i]);
                        buffer[i] = pBytes[i];
                    }
                    printf("\033[2K\r");
                    Disassemble(1, g_pc, start_addr, buffer[0], buffer,
                                0, count, &bytesUsed, &bytesNeeded, 0);
                }

                start_addr += bytesUsed;
                address     = start_addr;

                printf(GREEN " %04X: " RESET, start_addr);
                if (get_line(instruction, sizeof(instruction)) == NULL)
                    break;
            }
            continue;
        }

        // -- Breakpoints -------------------------------------------------

        if (strcasecmp(arguments[0], "bp") == 0) {
            if (arg_count < 2) { printf("bp <address>\n"); continue; }
            OwnDma odma;
            set_breakpoint((uint16_t)strtol(arguments[1], NULL, 16));
            continue;
        }
        if (strcasecmp(arguments[0], "bpd") == 0) {
            if (arg_count < 2) { printf("bpd <address>\n"); continue; }
            remove_breakpoint((uint16_t)strtol(arguments[1], NULL, 16));
            continue;
        }
        if (strcasecmp(arguments[0], "bpc") == 0) {
            int restore = 1;
            if (arg_count > 1 && strcmp(arguments[1], "-norestore") == 0) {
                printf("Clearing breakpoint table without restoring opcodes.\n");
                restore = 0;
            }
            clear_breakpoints(restore);
            continue;
        }
        if (strcasecmp(arguments[0], "bps") == 0) {
            show_breakpoints();
            continue;
        }

        // -- Next PC -----------------------------------------------------

        if (strcasecmp(arguments[0], "nextpc") == 0) {
            uint16_t nextPc = 0;
            if (determine_next_pc(&nextPc) == 0)
                printf("Next pc: %04X\n", nextPc);
            else
                printf("Could not determine next PC\n");
            continue;
        }

        // -- Disassemble -------------------------------------------------

        if (strcasecmp(arguments[0], "d") == 0) {
            OwnDma odma;
            refresh_breakpoints_if_needed(0);

            uint16_t start_addr = address;
            if (arg_count > 1)
                start_addr = (uint16_t)strtol(arguments[1], NULL, 16);

            uint16_t end_addr = start_addr + 32;
            if (arg_count > 2)
                end_addr = (uint16_t)strtol(arguments[2], NULL, 16);

            int acme = (arg_count > 3 && strcmp(arguments[3], "-acme") == 0);
            address = start_addr;
            int bytes = end_addr - start_addr;
            count = bytes;

            buffer_index = 0;
            for (i = 0; i < bytes; i++)
                buffer[buffer_index++] = sysop_peek(start_addr + i);

            if (acme)
                printf("*=$%04X\n\n", start_addr);

            i = 0;
            int start = 0;
            while (i < count) {
                Disassemble(1, g_pc, (uint16_t)(address + i),
                            buffer[start], buffer, start, count,
                            &bytesUsed, &bytesNeeded, acme);
                i     += bytesUsed;
                start += bytesUsed;
            }
            address += (bytesNeeded > 0) ? (count - bytesUsed) : count;
            continue;
        }

        // -- Memory dump -------------------------------------------------

        if (strcasecmp(arguments[0], "m") == 0) {
            uint16_t start_addr = address;
            if (arg_count > 1)
                start_addr = (uint16_t)strtol(arguments[1], NULL, 16);

            uint16_t end_addr = start_addr + 32;
            if (arg_count > 2)
                end_addr = (uint16_t)strtol(arguments[2], NULL, 16);

            address = start_addr;
            int bytes = end_addr - start_addr + 1;
            count = bytes;

            int c_flag   = (arg_count > 3 && strcmp(arguments[3], "-c")    == 0);
            int acme     = (arg_count > 3 && strcmp(arguments[3], "-acme") == 0);

            OwnDma odma;
            buffer_index = 0;
            for (i = 0; i < bytes; i++)
                buffer[buffer_index++] = sysop_peek(start_addr + i);

            if (acme) {
                printf("*=$%04X\n\n", start_addr);
            } else if (c_flag) {
                printf("char foo[0x%x] = {\n", count);
            }

            j = 0;
            char text[17];
            for (i = 0; i < count; i++) {
                if (j == 0) {
                    if      (acme)   printf("!byte ");
                    else if (c_flag) { /* no prefix */ }
                    else             printf("%04x: ", (uint16_t)(start_addr + i));
                }

                if (acme) {
                    if (j != 0) printf(", ");
                    printf("$%02x", buffer[i]);
                } else if (c_flag) {
                    if (j == 0) printf("\t");
                    printf("0x%02x", buffer[i]);
                    if (i != count - 1) printf(", ");
                } else {
                    printf("%02x ", buffer[i]);
                    text[j] = sysop_map_petscii_to_ascii(buffer[i]);
                }

                j++;
                if (j == 16) {
                    text[j] = '\0';
                    j = 0;
                }

                if ((i + 1) % 16 == 0 || i == count - 1) {
                    if (!acme && !c_flag)
                        printf("%s", text);
                    printf("\n");
                }
            }
            if (c_flag)
                printf("};\n");
            address += count;
            continue;
        }

        // -- Charset dump ------------------------------------------------

        if (strcasecmp(arguments[0], "charset") == 0) {
            if (arg_count < 3) {
                printf("charset <address> <char-count> [-v]\n");
                continue;
            }
            uint16_t start_addr = (uint16_t)strtol(arguments[1], NULL, 16);
            uint16_t charcount  = (uint16_t)strtol(arguments[2], NULL, 10);
            int      v_flag     = (arg_count > 3 && strcmp(arguments[3], "-v") == 0);
            int      bytes      = charcount * 8;

            OwnDma odma;
            buffer_index = 0;
            for (i = 0; i < bytes; i++)
                buffer[buffer_index++] = sysop_peek(start_addr + i);

            for (i = 0; i < charcount; i++) {
                for (j = 0; j < 8; j++) {
                    if (v_flag)
                        printf("charset[%d][%d] <= 8'b", i, j);
                    for (int k = 7; k >= 0; k--)
                        printf("%d", ((buffer[8 * i + j] >> k) & 1) ? 0 : 1);
                    if (v_flag) printf(";");
                    printf("\n");
                }
                printf("\n");
            }
            continue;
        }

        // -- Fill memory (o) ---------------------------------------------

        if (strcasecmp(arguments[0], "o") == 0) {
            if (arg_count < 4) {
                printf("o <start> <end> <b1> [b2 …]\n");
                continue;
            }
            uint16_t start_addr = (uint16_t)strtol(arguments[1], NULL, 16);
            uint16_t end_addr   = (uint16_t)strtol(arguments[2], NULL, 16);
            int bytes = end_addr - start_addr;
            printf("Fill memory %04X–%04X (%d bytes)...\n",
                   start_addr, end_addr, bytes);
            OwnDma odma;
            for (i = 0; i < bytes; ) {
                for (j = 3; j < arg_count; j++) {
                    uint8_t b = (uint8_t)strtol(arguments[j], NULL, 16);
                    if (start_addr + i + (j - 3) <= end_addr)
                        sysop_poke(start_addr + i + (j - 3), b);
                }
                i += (arg_count - 3);
            }
            continue;
        }

        // -- Fill cartridge ROM (cart_o) ----------------------------------

        if (strcasecmp(arguments[0], "cart_o") == 0) {
            if (arg_count < 4) {
                printf("cart_o <start> <end> <b1> [b2 …]\n");
                continue;
            }
            uint16_t start_addr = (uint16_t)strtol(arguments[1], NULL, 16);
            uint16_t end_addr   = (uint16_t)strtol(arguments[2], NULL, 16);
            int bytes = end_addr - start_addr;
            printf("Fill cartridge %04X–%04X (%d bytes)...\n",
                   start_addr, end_addr, bytes);
            OwnDma odma;
            for (i = 0; i < bytes; ) {
                for (j = 3; j < arg_count; j++) {
                    uint8_t b = (uint8_t)strtol(arguments[j], NULL, 16);
                    if (start_addr + i + (j - 3) <= end_addr)
                        sysop_cartridge_poke(start_addr + i + (j - 3), b);
                }
                i += (arg_count - 3);
            }
            continue;
        }

        // -- Converters --------------------------------------------------

        if (strcasecmp(arguments[0], "dec") == 0) {
            if (arg_count != 2) { printf("dec <decimal value>\n"); continue; }
            uint32_t v = (uint32_t)strtoul(arguments[1], NULL, 10);
            printf("Decimal %u -> $%08X hexadecimal\n", v, v);
            continue;
        }
        if (strcasecmp(arguments[0], "hex") == 0) {
            if (arg_count != 2) { printf("hex <hex value>\n"); continue; }
            uint32_t v = (uint32_t)strtoul(arguments[1], NULL, 16);
            printf("Hexadecimal $%08X -> %u decimal\n", v, v);
            continue;
        }

        // -- Save memory to file -----------------------------------------

        if (strcasecmp(arguments[0], "save") == 0) {
            if (arg_count < 4) {
                printf("save <start> <end> <filename> [-prg]\n");
                continue;
            }
            uint16_t start_addr = (uint16_t)strtol(arguments[1], NULL, 16);
            uint16_t end_addr   = (uint16_t)strtol(arguments[2], NULL, 16);
            int prg = (arg_count >= 5 && strcmp(arguments[4], "-prg") == 0);
            OwnDma odma;
            save(start_addr, end_addr, arguments[3], prg);
            continue;
        }

        // -- Hunt memory (h) ---------------------------------------------

        if (strcasecmp(arguments[0], "h") == 0) {
            if (arg_count < 4) {
                printf("h <start> <end> <b1> [b2 …]\n");
                continue;
            }
            OwnDma odma;
            uint16_t start_addr = (uint16_t)strtol(arguments[1], NULL, 16);
            uint16_t end_addr   = (uint16_t)strtol(arguments[2], NULL, 16);
            int bytes = end_addr - start_addr;
            printf("Hunt %04X–%04X (%d bytes)...\n",
                   start_addr, end_addr, bytes);
            buffer_index = 0;
            for (i = 0; i < bytes; i++)
                buffer[buffer_index++] = sysop_peek(start_addr + i);

            int pattern_len = arg_count - 3;
            for (i = 0; i <= bytes - pattern_len; i++) {
                int matched = 1;
                for (j = 0; j < pattern_len; j++) {
                    if (arguments[3 + j][0] == '*') continue;  // wildcard
                    uint8_t b = (uint8_t)strtol(arguments[3 + j], NULL, 16);
                    if (buffer[i + j] != b) { matched = 0; break; }
                }
                if (matched) {
                    printf("%04X:", start_addr + i);
                    for (j = 0; j < pattern_len; j++)
                        printf(" %02X", buffer[i + j]);
                    printf("\n");
                    i += pattern_len - 1;
                }
            }
            continue;
        }

        // -- Hunt PETSCII text in full memory (find) --------------------

        if (strcasecmp(arguments[0], "find") == 0) {
            char query[MAX_BUFFER_SIZE];
            int query_len = extract_find_query(line_raw, query, sizeof(query));
            if (query_len <= 0) {
                printf("find \"text\"\n");
                continue;
            }

            uint8_t pattern_modern[MAX_BUFFER_SIZE];
            uint8_t pattern_legacy[MAX_BUFFER_SIZE];
            int same_pattern = 1;
            for (i = 0; i < query_len; i++) {
                pattern_modern[i] = sysop_map_ascii_to_petscii(query[i]);
                pattern_legacy[i] = map_ascii_to_petscii_legacy(query[i]);
                if (pattern_modern[i] != pattern_legacy[i])
                    same_pattern = 0;
            }

            const int memory_size = 65536;
            int matches = 0;
            uint16_t first_match = 0;

            printf("Searching all memory for PETSCII text: \"%s\"\n", query);
            OwnDma odma;

            for (i = 0; i < memory_size; i++)
                buffer[i] = sysop_peek((uint16_t)i);

            for (i = 0; i <= memory_size - query_len; i++) {
                int matched_modern = 1;
                int matched_legacy = 1;
                for (j = 0; j < query_len; j++) {
                    if (buffer[i + j] != pattern_modern[j])
                        matched_modern = 0;
                    if (buffer[i + j] != pattern_legacy[j])
                        matched_legacy = 0;
                    if (!matched_modern && !matched_legacy)
                        break;
                }

                if (matched_modern || (!same_pattern && matched_legacy)) {
                    uint16_t addr = (uint16_t)i;
                    if (matches == 0)
                        first_match = addr;
                    matches++;
                    printf("%04X\n", addr);
                }
            }

            if (matches > 0)
                printf("Found %d instance(s). First match: %04X\n", matches, first_match);
            else
                printf("Found 0 instance(s).\n");

            continue;
        }

        // -- VIC/screen info ---------------------------------------------

        if (strcasecmp(arguments[0], "info") == 0) {
            print_vic_info();
            continue;
        }

        // -- Load binary at address --------------------------------------

        if (strcasecmp(arguments[0], "loadbin") == 0) {
            if (arg_count != 3) {
                printf("loadbin <filename> <address>\n");
                continue;
            }
            uint16_t load_address = (uint16_t)strtol(arguments[2], NULL, 16);
            sysop_loadbin(arguments[1], load_address);
            continue;
        }

        // -- Load NMI monitor stub into C64 RAM at $DF00 -----------------
        //
        // Enables the C64 I/O space first (required for $DF00 to be
        // accessible as I/O rather than RAM), then loads the pre-assembled
        // stub binary from /usr/local/bin/mon_io at $DF00.

        if (strcasecmp(arguments[0], "load_stub") == 0) {
            sysop_enable_io();
            OwnDma odma;
            sysop_loadbin("/usr/local/bin/mon_io", 0xDF00);
            continue;
        }

        if (strcasecmp(arguments[0], "cart_enable_8k") == 0) {
            sysop_cartridge_enable(0x2000);
            printf("8K cartridge enabled ($8000-$9FFF).\n");
            continue;
        }

        if (strcasecmp(arguments[0], "cart_enable_16k") == 0) {
            sysop_cartridge_enable(0x4000);
            printf("16K cartridge enabled ($8000-$BFFF).\n");
            continue;
        }

        if (strcasecmp(arguments[0], "cart_disable") == 0) {
            sysop_cartridge_disable();
            printf("Cartridge disabled.\n");
            continue;
        }

        // -- Run (types RUN into keyboard buffer) ------------------------

        if (strcasecmp(arguments[0], "run") == 0) {
            run();
            continue;
        }

        // -- NMI ---------------------------------------------------------

        if (strcasecmp(arguments[0], "nmi") == 0) {
            *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_ENABLE_IO;
            *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_ENABLE_NMI_TRAP;
            *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_NMI_FREEZE;
            continue;
        }
        if (strcasecmp(arguments[0], "nmi-resume") == 0) {
            sysop_poke(0xdff1, 0xff);
            continue;
        }

        // -- Resume / go -------------------------------------------------

        if (strcasecmp(arguments[0], "resume") == 0) {
            resume();
            continue;
        }

        if (strcasecmp(arguments[0], "g") == 0) {
            resume();
            {
                OwnDma odma;
                sysop_poke(0xdff1, 0xff);
            }
            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();
            *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_NMI_UNFREEZE;
            continue;
        }

        // -- Single step (z) ---------------------------------------------

        if (strcasecmp(arguments[0], "z") == 0) {
            {
                OwnDma odma;
                clear_breakpoints(1);

                uint16_t nextPc = 0;
                if (determine_next_pc(&nextPc) != 0 || nextPc == 0) {
                    printf("Unable to determine next PC value\n");
                    continue;
                }
                set_breakpoint(nextPc);

                if (g_status & 0x10)
                    setpc(g_pc);

                resume();
                sysop_poke(0xdff1, 0xff);
            }

            *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_NMI_UNFREEZE;
            wait_for_command_finish();

            OwnDma odma2;
            show_registers();

            int bcount = 3;
            for (i = 0; i < bcount; i++)
                buffer[i] = sysop_peek(g_pc + i);
            for (i = 0; i < MAX_BREAKPOINTS; i++) {
                if (g_breakpoints[i].address == g_pc)
                    buffer[0] = g_breakpoints[i].opcode;
            }

            printf("\n");
            Disassemble(2, g_pc, g_pc, buffer[0], buffer, 0, bcount,
                        &bytesUsed, &bytesNeeded, 0);
            continue;
        }

        // -- Registers ---------------------------------------------------

        if (strcasecmp(arguments[0], "r") == 0) {
            show_registers();
            continue;
        }

        // -- Low-level C64 stub commands ---------------------------------

        if (strcasecmp(arguments[0], "sysop_peek$1") == 0) {
            {
                OwnDma odma;
                sysop_poke(0xdff1, 3);  // cmd 3 = read $01
            }
            wait_for_command_finish();
            OwnDma odma;
            data = sysop_peek(0xdff4);
            printf("%04X: %02X\n", (uint16_t)1, data);
            continue;
        }

        if (strcasecmp(arguments[0], "stack") == 0) {
            if (arg_count != 2) { printf("stack <data>\n"); continue; }
            sscanf(arguments[1], " %02hhX", &data);
            sysop_poke(0xdff2, data);
            sysop_poke(0xdff1, 2);  // cmd 2 = set stack with TSX
            wait_for_command_finish();
            continue;
        }

        if (strcasecmp(arguments[0], "sysop_poke$1") == 0) {
            if (arg_count != 2) { printf("sysop_poke$1 <data>\n"); continue; }
            sscanf(arguments[1], " %02hhX", &data);
            {
                OwnDma odma;
                sysop_poke(0xdff2, data);
                sysop_poke(0xdff1, 1);  // cmd 1 = write to $01
            }
            wait_for_command_finish();
            continue;
        }

        if (strcasecmp(arguments[0], "sysop_poke") == 0) {
            if (arg_count < 3) { printf("sysop_poke <address> <data>\n"); continue; }
            sscanf(arguments[1], " %hx", &address);
            for (i = 0; i < arg_count - 2; i++) {
                sscanf(arguments[2 + i], " %02hhX", &data);
                OwnDma odma;
                sysop_poke(address + i, data);
            }
            continue;
        }

        if (strcasecmp(arguments[0], "cart_poke") == 0) {
            if (arg_count < 3) { printf("cart_poke <address> <data>\n"); continue; }
            sscanf(arguments[1], " %hx", &address);
            for (i = 0; i < arg_count - 2; i++) {
                sscanf(arguments[2 + i], " %02hhX", &data);
                printf("cart_poke %04X %02X\n", address + i, data);
                OwnDma odma;
                sysop_cartridge_poke(address + i, data);
            }
            continue;
        }

        if (strcasecmp(arguments[0], "setpc") == 0) {
            if (arg_count < 2) { printf("setpc <address>\n"); continue; }
            sscanf(arguments[1], " %hx", &address);
            setpc(address);
            printf("PC set to %04X\n", address);
            continue;
        }

        // -- Unknown command ---------------------------------------------

        printf("Unknown command '%s'. Type ? for help.\n", arguments[0]);
    }

    // Disable NMI trap so it doesn't persist in the FPGA after mon exits
    // and interfere with other tools (e.g. easyflash).
    *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_DISABLE_NMI_TRAP;
    reset_term();
    sysop_uninit();
    return 0;
}
