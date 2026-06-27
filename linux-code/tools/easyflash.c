/*
 * easyflash.c — EasyFlash Cartridge Emulator
 *
 * Loads a C64 EasyFlash .crt file and emulates it in real time on the
 * Sysop-64 hardware.  The program:
 *
 *   1. Parses the .crt file and loads all ROM banks into host memory.
 *   2. Initialises the Sysop-64 bridge, enables DMA, puts the C64 into
 *      Ultimax mode, loads bank 0, and resets the C64.
 *   3. Runs an event loop that waits for the EasyFlash DMA trigger, reads
 *      the new values of $DE00 (bank select) and $DE02 (mode/game/exrom
 *      control), and responds accordingly by reloading the cartridge ROM
 *      window and switching cartridge mode.
 *
 * EasyFlash register summary:
 *   $DE00 — bank select (bits 5:0, 0..63)
 *   $DE02 — mode control (bits 2:0):
 *             0 / 7 — 16K mode: LO at $8000, HI at $A000
 *             4     — cartridge off
 *             5     — Ultimax mode: LO at $8000, HI at $E000
 *
 * Usage:  easyflash <file.crt> [debug]
 */

/* =========================================================================
 * Includes
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <arpa/inet.h> /* ntohs() / ntohl() for big-endian .crt fields */
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include "sysop64.h"

/* =========================================================================
 * Constants
 * ========================================================================= */

/* Maximum number of ROM banks an EasyFlash cartridge can contain. */
#define MAX_BANKS 256

/*
 * Size of one ROM bank half (LO or HI) in bytes.
 * Each full bank is 16 KB split into two 8 KB halves.
 */
#define BANK_SIZE 8192

/* =========================================================================
 * CRT File Format Structures
 *
 * EasyFlash cartridges are distributed as .crt files.  The format starts
 * with a fixed-size CrtHeader, followed by one or more CHIP chunks, each
 * with a ChipHeader immediately preceding its ROM data payload.
 *
 * All multi-byte fields in the file are big-endian; we convert them with
 * ntohl()/ntohs() after reading.
 *
 * __attribute__((packed)) prevents the compiler from inserting alignment
 * padding, so the struct layout matches the on-disk format byte-for-byte.
 * ========================================================================= */

typedef struct __attribute__((packed))
{
    char     signature[16];  /* "C64 CARTRIDGE   " (16 bytes, no NUL) */
    uint32_t header_length;  /* Total length of this header block */
    uint16_t version;        /* CRT format version */
    uint16_t cart_type;      /* Hardware type: 0x0020 = EasyFlash */
    uint8_t  exrom_line;     /* Initial EXROM line state */
    uint8_t  game_line;      /* Initial GAME line state */
    uint8_t  reserved[6];
    char     cart_name[32];  /* Human-readable cartridge name */
} CrtHeader;

typedef struct __attribute__((packed))
{
    char     signature[4];   /* "CHIP" */
    uint32_t total_length;   /* Length of this entire CHIP packet (header + data) */
    uint16_t chip_type;      /* 0 = ROM */
    uint16_t bank_number;    /* Which bank this data belongs to (0..MAX_BANKS-1) */
    uint16_t start_address;  /* Load address: 0x8000 = LO half, 0xA000/0xE000 = HI half */
    uint16_t data_length;    /* Bytes of ROM data that follow this header */
} ChipHeader;

/* =========================================================================
 * Global State
 * ========================================================================= */

/*
 * ROM bank storage.  Each bank is split into a LO half (mapped at $8000)
 * and a HI half (mapped at $A000 in 16K mode or $E000 in Ultimax mode).
 * Entries are NULL until the corresponding CHIP chunk is loaded from the
 * .crt file.
 */
uint8_t *memory_banks_lo[MAX_BANKS] = { NULL };
uint8_t *memory_banks_hi[MAX_BANKS] = { NULL };

/* Number of LO banks successfully loaded (counted in main after parsing). */
int loaded_banks_count = 0;

/*
 * mxg — current cartridge mode, derived from bits 2:0 of $DE02.
 * Values: 0/7 = 16K, 4 = off, 5 = Ultimax (see file header for full table).
 */
uint8_t mxg = 0;

/* Index of the bank currently loaded into the cartridge ROM window. */
uint8_t g_bank = 0xff; /* 0xff = none loaded yet */

/*
 * Deferred bank switch support.
 * Some EasyFlash titles write the bank number to $DE00 before enabling the
 * cartridge (i.e. while mxg == 4 / cartridge is off).  We save the
 * requested bank here and apply it once the cartridge is turned on.
 */
uint8_t g_deferred_bank = 0xff;
int     g_switch_bank_on_next_cartridge_enable = 0;

/* Tracks whether the C64 is currently in Ultimax or 16K cartridge mode. */
int ultimax_enabled  = 1;
int cartridge_enabled = 0;

/* Set to 1 by passing "debug" on the command line.  Enables extra output
 * and interactive pause-points (press Enter to continue). */
int g_debug_mode = 0;

/* =========================================================================
 * Forward Declarations
 * ========================================================================= */

void cleanup_memory(void);
int  load_crt_file(const char *filename);
int  switch_bank(uint8_t bank);

/* =========================================================================
 * Debug Utilities
 * ========================================================================= */

/* printf wrapper that only produces output when debug mode is active. */
void dbgprintf(const char *format, ...)
{
    if (g_debug_mode)
    {
        va_list args;
        va_start(args, format);
        vprintf(format, args);
        va_end(args);
    }
}

/* =========================================================================
 * CRT File Loading
 * ========================================================================= */

/*
 * load_crt_file — parse a .crt file and populate memory_banks_lo/hi.
 *
 * Reads the CrtHeader to validate the file, then iterates over every CHIP
 * chunk.  Each chunk is placed into either the LO or HI bank array based
 * on its start_address (0x8000 -> LO, anything else -> HI).
 *
 * Returns 1 on success, 0 on fatal error.
 */
int load_crt_file(const char *filename)
{
    FILE *file = fopen(filename, "rb");
    if (!file)
    {
        perror("Error opening file");
        return 0;
    }

    /* Read and validate the file header. */
    CrtHeader crt_header;
    if (fread(&crt_header, sizeof(CrtHeader), 1, file) != 1)
    {
        fprintf(stderr, "Error: Could not read CRT header.\n");
        fclose(file);
        return 0;
    }

    /* All multi-byte fields are big-endian in the file. */
    crt_header.header_length = ntohl(crt_header.header_length);
    crt_header.version       = ntohs(crt_header.version);
    crt_header.cart_type     = ntohs(crt_header.cart_type);

    if (strncmp(crt_header.signature, "C64 CARTRIDGE   ", 16) != 0)
    {
        fprintf(stderr, "Error: Invalid CRT file signature.\n");
        fclose(file);
        return 0;
    }

    if (crt_header.cart_type != 0x20)
    {
        fprintf(stderr, "Warning: Cartridge type is not EasyFlash (Type: 0x%04X).\n",
                crt_header.cart_type);
    }

    printf("CRT Header validated. Cartridge Name: %s\n", crt_header.cart_name);

    /*
     * Seek past any extra header bytes the file declares.  header_length
     * covers the CrtHeader struct itself, so subtract its size.
     */
    if (crt_header.header_length > sizeof(CrtHeader))
        fseek(file, crt_header.header_length - sizeof(CrtHeader), SEEK_CUR);

    /* Read CHIP chunks until EOF. */
    while (!feof(file) && !ferror(file))
    {
        ChipHeader chip_header;
        if (fread(&chip_header, sizeof(ChipHeader), 1, file) != 1)
        {
            if (feof(file))
                break;
            fprintf(stderr, "Error reading CHIP header.\n");
            continue;
        }

        if (strncmp(chip_header.signature, "CHIP", 4) != 0)
        {
            fprintf(stderr, "Error: Expected 'CHIP' signature, found '%.4s'. Stopping.\n",
                    chip_header.signature);
            break;
        }

        /* Convert CHIP header fields from big-endian to host byte order. */
        chip_header.total_length  = ntohl(chip_header.total_length);
        chip_header.chip_type     = ntohs(chip_header.chip_type);
        chip_header.bank_number   = ntohs(chip_header.bank_number);
        chip_header.start_address = ntohs(chip_header.start_address);
        chip_header.data_length   = ntohs(chip_header.data_length);

        uint16_t bank     = chip_header.bank_number;
        uint16_t data_len = chip_header.data_length;

        printf("Found CHIP chunk: Bank %d, %u bytes at $%04X\n",
               bank, data_len, chip_header.start_address);

        if (bank >= MAX_BANKS)
        {
            fprintf(stderr, "Warning: Bank number %d out of range. Skipping.\n", bank);
            fseek(file, data_len, SEEK_CUR);
            continue;
        }
        if (data_len > BANK_SIZE)
        {
            fprintf(stderr, "Warning: Data length %u exceeds BANK_SIZE %d. Truncating.\n",
                    data_len, BANK_SIZE);
            data_len = BANK_SIZE;
        }
        if (data_len == 0)
        {
            fprintf(stderr, "Warning: CHIP chunk has zero data length. Skipping.\n");
            continue;
        }

        /*
         * Allocate a full BANK_SIZE buffer regardless of data_len so that
         * sysop_cartridge_poke() always has a complete 8 KB region to write.
         * The start_address determines whether this is the LO ($8000) or
         * HI ($A000 / $E000) bank half.
         */
        uint8_t **slot = (chip_header.start_address == 0x8000)
                         ? &memory_banks_lo[bank]
                         : &memory_banks_hi[bank];

        printf("Allocating %s bank %d\n",
               (chip_header.start_address == 0x8000) ? "LO" : "HI", bank);

        *slot = (uint8_t *)malloc(BANK_SIZE);
        if (!*slot)
        {
            fprintf(stderr, "Error: malloc failed for bank %d.\n", bank);
            fclose(file);
            return 0;
        }
        memset(*slot, 0xff, BANK_SIZE); /* fill unused bytes with $FF (open bus) */

        if (fread(*slot, 1, data_len, file) != data_len)
            fprintf(stderr, "Error: Failed to read data for bank %d.\n", bank);
        else
            printf("Successfully loaded bank %d.\n", bank);
    }

    fclose(file);
    return 1;
}

/*
 * cleanup_memory — free all loaded bank buffers.
 *
 * Registered with atexit() so it runs automatically on program exit,
 * preventing memory leaks even when the emulation loop is interrupted.
 */
void cleanup_memory(void)
{
    printf("\n--- Cleaning up allocated memory ---\n");
    int freed_count = 0;
    for (int i = 0; i < MAX_BANKS; i++)
    {
        if (memory_banks_lo[i] != NULL)
        {
            free(memory_banks_lo[i]);
            memory_banks_lo[i] = NULL;
            freed_count++;
        }
        if (memory_banks_hi[i] != NULL)
        {
            free(memory_banks_hi[i]);
            memory_banks_hi[i] = NULL;
            freed_count++;
        }
    }
    printf("%d bank(s) freed.\n", freed_count);
}

/* =========================================================================
 * Cartridge Emulation - Bank Switching
 * ========================================================================= */

/*
 * assert_dma_enabled — sanity check that DMA is currently held.
 *
 * Bit 31 of the DMA info word is set when DMA is NOT active (the FPGA is
 * idle / waiting).  If that bit is set here it means we are about to write
 * to the cartridge ROM window without holding the bus, which would corrupt
 * memory.  We pause so a developer can investigate.
 */
void assert_dma_enabled(void)
{
    uint32_t dmainfo = sysop_get_dma_info();
    if ((dmainfo & 0x80000000) != 0)
    {
        printf("assert_dma_enabled failed, hit enter\n");
        getchar();
    }
}

/*
 * switch_bank — load the ROM data for the requested bank into the C64's
 * cartridge ROM window via DMA.
 *
 * The bank is split into two 8 KB halves:
 *   LO half (sysop_cartridge_poke offset 0x0000..0x1FFF) -> mapped at $8000
 *   HI half (sysop_cartridge_poke offset 0x2000..0x3FFF) -> mapped at $A000 or $E000
 *
 * Which halves are written depends on mxg (the current cartridge mode):
 *   mxg == 4         -> cartridge is off; neither half is written (deferred)
 *   mxg == 0,5,7     -> both LO and HI halves are written
 *   mxg == anything else where only LO is relevant -> only LO is written
 *
 * Returns:
 *   1  — bank loaded successfully
 *   2  — cartridge is currently disabled; bank switch deferred
 */
int switch_bank(uint8_t bank)
{
    if (!ultimax_enabled && !cartridge_enabled)
    {
        printf("Cannot switch banks while cartridge not enabled\n");
        printf("Deferring bank switch\n");
        return 2;
    }

    assert_dma_enabled();

    dbgprintf("Switching to bank 0x%02X, mxg %02X\n", bank, mxg);

    if (memory_banks_lo[bank] == NULL)
    {
        printf("Trying to switch to bank %d but memory_banks_lo was NULL\n", bank);
        if (memory_banks_hi[bank] == NULL)
        {
            printf("hi bank also NULL, aborting...\n");
            exit(-1);
        }
        else
        {
            printf("hi bank not NULL, allowing\n");
        }
    }

    /*
     * Write the LO half ($8000-$9FFF) unless the cartridge is in "off" mode
     * (mxg == 4), in which case there is no cartridge window to write to.
     */
    if ((mxg != 4) && memory_banks_lo[bank] != NULL)
    {
        uint16_t base_addr_for_compare = 0x8000;
        for (uint16_t i = 0; i < 0x2000; i++)
        {
            uint8_t val = memory_banks_lo[bank][i];
            sysop_cartridge_poke(i, val);
            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();
            (void)sysop_peek(base_addr_for_compare + i); /* read-back (comparison disabled) */
        }
    }

    /*
     * Write the HI half ($A000-$BFFF in 16K mode, $E000-$FFFF in Ultimax).
     * Only written when the cartridge mode includes a HI bank (mxg 0, 5, 7).
     * sysop_cartridge_poke offset 0x2000 maps to the HI window base address.
     */
    if ((mxg == 0 || mxg == 5 || mxg == 7) && memory_banks_hi[bank] != NULL)
    {
        uint16_t base_addr_for_compare = ultimax_enabled ? 0xe000 : 0xa000;
        for (uint16_t i = 0; i < 0x2000; i++)
        {
            uint16_t poke_addr = 0x2000 + i;
            uint8_t val = memory_banks_hi[bank][i];
            sysop_cartridge_poke(poke_addr, val);
            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();
            (void)sysop_peek((uint16_t)(base_addr_for_compare + i)); /* read-back (comparison disabled) */
        }
    }

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    if (g_debug_mode && ultimax_enabled)
    {
        printf("ultimax enabled, debug and hit enter\n");
        getchar();
    }

    return 1;
}

/* =========================================================================
 * Main Emulation Loop
 * ========================================================================= */

/*
 * handle_de02 — process a write to $DE02 (cartridge mode register).
 *
 * The C64 cartridge mode is controlled by the EXROM and GAME lines.
 * EasyFlash uses $DE02 bits 2:0 to select the mode:
 *
 *   7 — 16K cartridge  (EXROM=0, GAME=0): LO at $8000, HI at $A000
 *   4 — cartridge off  (EXROM=1, GAME=1): no ROM visible
 *   5 — Ultimax        (EXROM=0, GAME=1): LO at $8000, HI at $E000
 *
 * A deferred bank switch is applied immediately after enabling the cartridge
 * if the C64 had already written a new bank number while the cartridge was
 * disabled.
 */
static void handle_de02(uint8_t val)
{
    mxg = val & 0x7;

    if (mxg == 7)
    {
        printf("Switching to 16K cartridge mode\n");
        cartridge_enabled = 1;
        sysop_cartridge_enable(0x4000);
        usleep(10000); /* brief settle time after mode change */
        ultimax_enabled = 0;

        /* Apply any bank switch that was requested while the cartridge was off. */
        if (g_switch_bank_on_next_cartridge_enable)
        {
            int result = switch_bank(g_deferred_bank);
            if (result == 1)
            {
                g_bank = g_deferred_bank;
                g_switch_bank_on_next_cartridge_enable = 0;
                dbgprintf("Done switching banks\n");
            }
            else if (result == 2)
            {
                printf("WEIRD: Cartridge enable followed by deferred bank switch "
                       "but result was defer again.\n");
                getchar();
            }
        }
    }
    else if (mxg == 4)
    {
        if (cartridge_enabled == 0)
        {
            printf("Cartridge already disabled, ignoring\n");
        }
        else
        {
            cartridge_enabled = 0;
            printf("Disabling cartridge\n");
            sysop_cartridge_disable();
            usleep(10000);
            ultimax_enabled = 0;
            printf("Cartridge disabled\n");
        }
    }
    else if (mxg == 5)
    {
        if (!ultimax_enabled)
        {
            printf("Switching to ultimax...\n");
            sysop_cartridge_enable_ultimax();
            usleep(10000);
            ultimax_enabled = 1;
        }
        else
        {
            printf("Already in ultimax mode, ignoring...\n");
        }
    }
    else
    {
        printf("Don't know what to do with DE02 value %d (hit enter)\n", val);
        getchar();
    }
}

/*
 * run_cartridge — initialise hardware and run the bank-switching event loop.
 *
 * Sequence:
 *   1. sysop_init() — open the Sysop-64 bridge device.
 *   2. Enable DMA so we can write to the C64 address space.
 *   3. Disable the EasyFlash DMA trigger (so we don't get spurious events
 *      during setup).
 *   4. Enable I/O so we can access the $DE00/$DE02 registers.
 *   5. Put the C64 into Ultimax mode and load bank 0.
 *   6. Re-enable the EasyFlash DMA trigger.
 *   7. Release DMA and reset the C64 — the game starts running.
 *   8. Poll sysop_get_dma_info() in a tight loop.  When the EasyFlash
 *      trigger fires (bit 31 of dmainfo clears), read $DE00 and $DE02,
 *      handle bank switches and mode changes, then release DMA again.
 *
 * Returns 0 on clean exit (currently the loop runs forever until SIGINT).
 */
int run_cartridge(void)
{
    /* --- Step 1: open the bridge --- */
    int res = sysop_init();
    if (res != 0)
    {
        printf("sysop_init failed %d\n", res);
        return -1;
    }

    /* --- Step 2-4: DMA + I/O setup --- */
    sysop_command(36); /* reset DMA trigger state on the FPGA */

    uint32_t status1 = sysop_read_status_1();
    printf("status1: %08X\n", status1);

    sysop_dma_enable();

    printf("Disabling EasyFlash DMA trigger during setup\n");
    sysop_disable_easyflash_dma_trigger();

    printf("Enabling I/O\n");
    sysop_enable_io();

    /* Clear both EasyFlash registers before enabling the cartridge. */
    sysop_io_poke(0xde00, 0);
    sysop_io_poke(0xde02, 0);

    /* --- Step 5: put the C64 into Ultimax mode and load bank 0 --- */
    printf("Enabling Ultimax mode\n");
    sysop_cartridge_enable_ultimax();
    ultimax_enabled = 1;

    switch_bank(0);

    /* --- Step 6: arm the EasyFlash DMA trigger ---
     * The trigger fires each time the C64 writes to $DE00 or $DE02. */
    sysop_enable_easyflash_dma_trigger();

    /* --- Step 7: release DMA and reset the C64 --- */
    uint8_t last_de00 = 0;
    uint8_t last_de02 = 0;

    if (g_debug_mode)
    {
        printf("hit enter to reset\n");
        getchar();
    }

    printf("Resetting C64\n");
    sysop_dma_disable();
    sysop_c64_reset();

    /* --- Step 8: event loop ---
     *
     * We use the interrupt-driven approach: the FPGA halts the C64 bus
     * (asserts DMA) when it detects a write to $DE00 or $DE02, then signals
     * us by clearing bit 31 of the DMA info word.  We handle the event and
     * call sysop_dma_disable() to release the bus and let the C64 continue. */
    int spin = 0;
    while (1)
    {
        /*
         * sysop_get_dma_info() returns the current DMA status word.
         * Bit 31 set   = no trigger pending; sleep briefly and retry.
         * Bit 31 clear = the EasyFlash trigger fired; handle the event.
         */
        uint32_t dmainfo = sysop_get_dma_info();
        if ((dmainfo & 0x80000000) != 0)
        {
            usleep(10000);
            spin++;
            if ((spin % 1000) == 0)
                dbgprintf("no dma, sleeping, dmainfo %08X\n", dmainfo);
            continue;
        }
        spin = 0;

        /*
         * DMA is now held.  Read the status register to find out which
         * register triggered (bits 22:21 of status1 = dma_trigger field).
         */
        volatile uint32_t st1 = sysop_read_status_1();
        volatile uint8_t dma_trigger = (st1 >> 21) & 0x3;

        /* Read the current values of both EasyFlash registers. */
        uint8_t current_de00 = sysop_internal_peek(0xde00);
        uint8_t current_de02 = sysop_internal_peek(0xde02);

        printf("DMA Trigger: %02X, status1: %08X\n", dma_trigger, st1);
        printf("$DE00: %02X -> %02X\n", last_de00, current_de00);
        printf("$DE02: %02X -> %02X\n", last_de02, current_de02);

        if (last_de00 == current_de00 && last_de02 == current_de02)
        {
            /* A trigger fired but neither register changed — unexpected. */
            if (g_debug_mode)
            {
                printf("Trigger without value change, not expected. Debug and hit enter\n");
                getchar();
            }
        }

        if (dma_trigger == 3)
        {
            /* Both triggers fired simultaneously — should not occur. */
            printf("Unexpected DMA trigger 3, investigate\n");
            getchar();
        }

        /* --- Handle $DE00: bank select --- */
        int handled_de00 = 0;
        if (current_de00 != last_de00)
        {
            last_de00 = current_de00;
            uint8_t val  = sysop_internal_peek(0xde00);
            uint8_t bank = val & 0x3F; /* only bits 5:0 are the bank number */
            printf("Detected write to $DE00: bank %02X\n", bank);

            int result = switch_bank(bank);
            if (result == 1)
            {
                g_bank = bank;
                g_switch_bank_on_next_cartridge_enable = 0;
                dbgprintf("Done switching banks\n");
            }
            else if (result == 2)
            {
                /* Cartridge is off; defer the switch until it is re-enabled. */
                g_deferred_bank = val;
                g_switch_bank_on_next_cartridge_enable = 1;
            }

            handled_de00 = 1;
        }

        /* --- Handle $DE02: mode/game/exrom control ---
         *
         * Bit 7 of $DE02 is the LED control bit; mask it off so that an
         * LED-only change does not trigger an unnecessary mode switch. */
        uint8_t current_ignore_led = (uint8_t)(current_de02 & 0x7F);
        uint8_t last_ignore_led    = (uint8_t)(last_de02    & 0x7F);
        int ledChangeOnly = (current_ignore_led == last_ignore_led);
        int handled_de02 = 0;

        if (current_ignore_led != last_ignore_led)
        {
            handled_de02 = 1;
            printf("Detected write to $DE02: %02X -> %02X\n", last_de02, current_de02);

            uint8_t val = sysop_internal_peek(0xde02);
            dbgprintf("Read 0x%02X at $DE02\n", val);
            handle_de02(val);

            last_de02 = current_de02;
        }

        /*
         * If only DE02 changed, re-read DE00 in case a simultaneous write
         * was missed by the trigger (belt-and-suspenders check).
         */
        if (handled_de02 && !handled_de00)
        {
            current_de00 = sysop_internal_peek(0xde00);
            /* If current_de00 != last_de00 here, a race occurred. */
        }

        if (ledChangeOnly)
        {
            dbgprintf("ignoring LED change\n");
            last_de02 = current_de02;
        }

        /* Release DMA so the C64 can continue executing. */
        dbgprintf("Releasing DMA...\n");
        sysop_dma_disable();
    }

    return 0;
}

/* =========================================================================
 * Signal Handling
 * ========================================================================= */

/*
 * sigintHandler — clean shutdown on Ctrl-C.
 *
 * Disarms the EasyFlash trigger, disables the cartridge, releases the bus,
 * and resets the C64 before exiting.  cleanup_memory() will be called
 * automatically by the atexit() handler.
 */
void sigintHandler(int sig)
{
    sysop_disable_easyflash_dma_trigger();
    sysop_cartridge_disable();
    sysop_dma_disable();
    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_c64_reset();
    sysop_uninit();
    exit(sig);
}

/* =========================================================================
 * Entry Point
 * ========================================================================= */

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <filename.crt> [debug]\n", argv[0]);
        return 1;
    }

    g_debug_mode = (argc > 2 && strcmp(argv[2], "debug") == 0);

    const char *filename = argv[1];
    printf("Attempting to load CRT file: %s\n", filename);

    /* Ensure bank buffers are always freed, even on abnormal exit. */
    atexit(cleanup_memory);

    if (!load_crt_file(filename))
    {
        fprintf(stderr, "Error: Failed to load or parse the CRT file.\n");
        return 1;
    }

    printf("\n--- CRT File Parsed Successfully ---\n");

    /* Count loaded LO banks (used for progress reporting). */
    for (int i = 0; i < MAX_BANKS; i++)
    {
        if (memory_banks_lo[i] != NULL)
            loaded_banks_count++;
    }

    if (loaded_banks_count == 0)
        printf("No CHIP chunks were found or loaded from the file.\n");
    else
        printf("Total of %d bank(s) loaded into memory.\n", loaded_banks_count);

    if (g_debug_mode)
    {
        printf("hit enter to start\n");
        getchar();
    }

    signal(SIGINT, sigintHandler);

    run_cartridge();

    return 0;
}