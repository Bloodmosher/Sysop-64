#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h> // For usleep
#include <inttypes.h>
#include <sys/types.h>
#include <stdarg.h>
#include "sysop64.h"

// sysop_internal_poke exists in libsysop64 but is not currently declared in public headers.
void sysop_internal_poke(uint16_t address, uint8_t value);

// --- Global REU State Variables ---
unsigned char *reu_memory = NULL; // Pointer to the allocated REU memory
size_t reu_total_size = 0;        // Total size of REU memory in bytes
int reu_num_pages = 0;            // Configured number of 64KB pages

// REU Registers (simulated DF00-DF0A)
// DF00: Status Register
// DF01: Command Register
// DF02: C64 Memory Address Low
// DF03: C64 Memory Address High
// DF04: REU Memory Address Low
// DF05: REU Memory Address High
// DF06: REU Memory Bank
// DF07: Transfer Length Low
// DF08: Transfer Length High
// DF09: Interrupt Mask Register
// DF0A: Address Control Register
unsigned char reu_registers[11];

enum {
    REU_REG_STATUS = 0,
    REU_REG_COMMAND = 1,
    REU_REG_C64_ADDR_LO = 2,
    REU_REG_C64_ADDR_HI = 3,
    REU_REG_REU_ADDR_LO = 4,
    REU_REG_REU_ADDR_HI = 5,
    REU_REG_REU_BANK = 6,
    REU_REG_LEN_LO = 7,
    REU_REG_LEN_HI = 8,
    REU_REG_IRQ_MASK = 9,
    REU_REG_ADDR_CTRL = 10
};

enum {
    REU_CMD_EXECUTE = 0x80,
    REU_CMD_LOAD = 0x20,
    REU_CMD_TYPE_MASK = 0x03
};

enum {
    REU_STATUS_INT_PENDING = 0x80,
    REU_STATUS_END_OF_BLOCK = 0x40,
    REU_STATUS_FAULT = 0x20
};

enum {
    REU_ADDRCTRL_FIX_C64 = 0x80,
    REU_ADDRCTRL_FIX_REU = 0x40
};

// Flag to control the main loop
volatile sig_atomic_t running = 1;
static int g_reu_trace = 0;
static int g_ignore_io_vector_writes = 0;

enum {
    REU_WATCH_START = 0x0314,
    REU_WATCH_END = 0x0333
};

static void reu_trace(const char *fmt, ...)
{
    if (!g_reu_trace)
        return;

    va_list args;
    va_start(args, fmt);
    printf("[REU TRACE] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

static void reu_trace_watch_write(unsigned int c64_addr, unsigned char value, size_t reu_offset, const char *kind)
{
    if (!g_reu_trace)
        return;

    if (c64_addr < REU_WATCH_START || c64_addr > REU_WATCH_END)
        return;

    printf("[REU WATCH] %s C64[$%04X] <= 0x%02X (REU[$%05zX])\n",
           kind,
           c64_addr,
           value,
           reu_offset);
}

static int should_ignore_io_vector_write(unsigned int c64_addr)
{
    if (!g_ignore_io_vector_writes)
        return 0;

    if ((c64_addr >= 0x031A && c64_addr <= 0x032D) ||
        (c64_addr >= 0x0330 && c64_addr <= 0x0333))
        return 1;

    return 0;
}

void read_registers()
{
    if (g_reu_trace)
        printf("Refreshed registers:\n");
    for (int i=0;i<=10;i++) {
        reu_registers[i] = sysop_internal_peek((uint16_t)(0xdf00+i));
        if (g_reu_trace)
            printf("%04X: %02X\n", (uint16_t)((0xdf00+i)), reu_registers[i]);
    }
}

// Function to perform the DMA transfer
void reu_perform_dma() {
    // DMA is already asserted by the FPGA trigger when this function runs.
    read_registers();

    unsigned char command = reu_registers[REU_REG_COMMAND];
    unsigned char addr_ctrl = reu_registers[REU_REG_ADDR_CTRL];
    unsigned char transfer_type = command & REU_CMD_TYPE_MASK;

    reu_trace("DF01(command)=0x%02X DF0A(addr_ctrl)=0x%02X type=%u autoload=%u",
              command,
              addr_ctrl,
              (unsigned int)transfer_type,
              (unsigned int)((command & REU_CMD_LOAD) != 0));
    
    // Extract parameters from REU registers
    unsigned int c64_addr = ((unsigned int)reu_registers[REU_REG_C64_ADDR_HI] << 8) | reu_registers[REU_REG_C64_ADDR_LO];
    // REU base address is DF04 (low) + DF05 (high).
    unsigned int reu_addr_low = ((unsigned int)reu_registers[REU_REG_REU_ADDR_HI] << 8) | reu_registers[REU_REG_REU_ADDR_LO];
    unsigned int raw_reu_bank = reu_registers[REU_REG_REU_BANK] & 0x1F; // Raw register bank value
    unsigned int reu_bank = raw_reu_bank;
    if (reu_num_pages > 0)
        reu_bank = raw_reu_bank % (unsigned int)reu_num_pages;
    unsigned int transfer_len = ((unsigned int)reu_registers[REU_REG_LEN_HI] << 8) | reu_registers[REU_REG_LEN_LO];

    // REU semantics: length register value 0 means 65536 bytes.
    if (transfer_len == 0)
        transfer_len = 65536;

    // Calculate the absolute REU memory offset
    size_t reu_memory_offset = (size_t)reu_bank * 0x10000 + reu_addr_low;
    int fix_c64_addr = (addr_ctrl & REU_ADDRCTRL_FIX_C64) != 0;
    int fix_reu_addr = (addr_ctrl & REU_ADDRCTRL_FIX_REU) != 0;

    reu_trace("Effective start: C64=$%04X REU=$%05zX len=$%04X fix_c64=%d fix_reu=%d raw_bank=%u mapped_bank=%u",
              c64_addr,
              reu_memory_offset,
              transfer_len,
              fix_c64_addr,
              fix_reu_addr,
              raw_reu_bank,
              reu_bank);

    // Clear end/fault before a new command execution.
    reu_registers[REU_REG_STATUS] &= (unsigned char)~(REU_STATUS_END_OF_BLOCK | REU_STATUS_FAULT);

    // Check for valid REU memory access range.
    // If REU address is fixed, only one byte slot is touched regardless of length.
    size_t reu_last_offset = reu_memory_offset + (fix_reu_addr ? 0 : (size_t)(transfer_len - 1));
    if (reu_last_offset >= reu_total_size) {
        fprintf(stderr, "[REU ERROR] DMA transfer out of REU memory bounds! "
                        "Requested: 0x%zX..0x%zX > Total: 0x%zX\n",
                reu_memory_offset, reu_last_offset, reu_total_size);
        // Set verify error bit for out-of-bounds access
        reu_registers[REU_REG_STATUS] |= REU_STATUS_FAULT;
        reu_registers[REU_REG_STATUS] |= REU_STATUS_END_OF_BLOCK;
        sysop_internal_poke(0xdf00, reu_registers[REU_REG_STATUS]);
        return;
    }

    reu_trace("Starting DMA: C64=0x%04X, REU=Bank %d (0x%04X), Len=0x%04X",
              c64_addr, reu_bank, reu_addr_low, transfer_len);

    // Perform the transfer byte by byte
    unsigned int c64_cursor = c64_addr;
    size_t reu_cursor = reu_memory_offset;
    int mismatch_reported = 0;
    for (unsigned int i = 0; i < transfer_len; ++i) {
        unsigned char c64_byte;
        unsigned char reu_byte;
        unsigned int current_c64_addr = c64_cursor & 0xFFFF;
        size_t current_reu_offset = reu_cursor;

        // Transfer type (DF01 bits 1..0): 00 C64->REU, 01 REU->C64, 10 swap, 11 compare
        if (transfer_type == 1) {
            // REU to C64
            reu_byte = reu_memory[current_reu_offset];
            if (should_ignore_io_vector_write(current_c64_addr)) {
                reu_trace("Ignored write REU->C64 C64[$%04X] <= 0x%02X", current_c64_addr, reu_byte);
            } else {
                reu_trace_watch_write(current_c64_addr, reu_byte, current_reu_offset, "REU->C64");
                sysop_poke(current_c64_addr, reu_byte);
            }
        } else if (transfer_type == 0) {
            // C64 to REU
            c64_byte = sysop_peek(current_c64_addr);
            reu_memory[current_reu_offset] = c64_byte;
        }

        if (transfer_type == 3) {
            c64_byte = sysop_peek(current_c64_addr); // Re-read C64 byte if not already read
            reu_byte = reu_memory[current_reu_offset]; // Re-read REU byte if not already read

            if (c64_byte != reu_byte) {
                printf("[REU] VERIFY ERROR at C64[0x%04X] (0x%02X) vs REU[0x%zX] (0x%02X)\n",
                       current_c64_addr, c64_byte, current_reu_offset, reu_byte);
                if (!mismatch_reported) {
                    reu_trace("First compare mismatch @ offset=$%04X C64[$%04X]=0x%02X REU[$%05zX]=0x%02X",
                              i,
                              current_c64_addr,
                              c64_byte,
                              current_reu_offset,
                              reu_byte);
                    mismatch_reported = 1;
                }
                reu_registers[REU_REG_STATUS] |= REU_STATUS_FAULT;
            }
        }

        if (transfer_type == 2) {
            // Swap operation: C64 byte <-> REU byte
            unsigned char temp_c64_byte = sysop_peek(current_c64_addr);
            unsigned char temp_reu_byte = reu_memory[current_reu_offset];

            if (should_ignore_io_vector_write(current_c64_addr)) {
                reu_trace("Ignored write SWAP C64[$%04X] <= 0x%02X", current_c64_addr, temp_reu_byte);
            } else {
                reu_trace_watch_write(current_c64_addr, temp_reu_byte, current_reu_offset, "SWAP");
                sysop_poke(current_c64_addr, temp_reu_byte);
            }
            reu_memory[current_reu_offset] = temp_c64_byte;
            reu_trace("SWAP: C64[0x%04X] <-> REU[0x%zX]", current_c64_addr, current_reu_offset);
        }

        // Honor DF0A address-control: bit7 fixes C64 address, bit6 fixes REU address.
        if (!fix_c64_addr)
            c64_cursor = (c64_cursor + 1) & 0xFFFF;
        if (!fix_reu_addr)
            reu_cursor++;
    }

    reu_trace("DMA transfer complete.");
    reu_registers[REU_REG_STATUS] |= REU_STATUS_END_OF_BLOCK;
    sysop_internal_poke(0xdf00, reu_registers[REU_REG_STATUS]);
    reu_trace("DF00(status) after transfer: 0x%02X", reu_registers[REU_REG_STATUS]);
}

// Signal handler for graceful exit (Ctrl+C)
void sigint_handler(int signum) {
    printf("\n[REU] SIGINT received. Shutting down...\n");
    running = 0; // Stop the main loop
}

// Initialize the REU emulator
int reu_init(int pages) {
    if (pages <= 0 || pages > 32) { // Max 32 pages = 2MB
        fprintf(stderr, "[REU ERROR] Invalid number of REU pages specified: %d (must be 1-32)\n", pages);
        return -1;
    }

    reu_num_pages = pages;
    reu_total_size = (size_t)reu_num_pages * 65536; // 64KB per page

    reu_memory = (unsigned char *)malloc(reu_total_size);
    if (reu_memory == NULL) {
        perror("[REU ERROR] Failed to allocate REU memory");
        return -1;
    }
    // Initialize REU memory to zeros
    memset(reu_memory, 0, reu_total_size);
    printf("[REU] Allocated %zu bytes (%d pages) for REU memory.\n", reu_total_size, reu_num_pages);

    // Initialize REU registers to 0
    memset(reu_registers, 0, sizeof(reu_registers));
    // Set initial status: no interrupt pending, no end-of-block, no fault
    reu_registers[REU_REG_STATUS] = 0x00;

    // Set up SIGINT handler
    signal(SIGINT, sigint_handler);

    return 0;
}

// Clean up REU resources
void reu_cleanup() {
    if (reu_memory != NULL) {
        free(reu_memory);
        reu_memory = NULL;
        printf("[REU] Freed REU memory.\n");
    }
    printf("[REU] Emulator shut down.\n");
}

// --- Main function for demonstration ---
int main(int argc, char *argv[]) {
    int pages = 16; // Default to 1MB REU

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ignore-io-vector-writes") == 0) {
            g_ignore_io_vector_writes = 1;
            continue;
        }

        if (argv[i][0] == '-') {
            fprintf(stderr, "Usage: %s [num_64kb_pages (1-32)] [--ignore-io-vector-writes]\n", argv[0]);
            return 1;
        }

        pages = atoi(argv[i]);
        if (pages == 0 && strcmp(argv[i], "0") != 0) {
            fprintf(stderr, "Usage: %s [num_64kb_pages (1-32)] [--ignore-io-vector-writes]\n", argv[0]);
            return 1;
        }
    }

    if (reu_init(pages) != 0) {
        return 1; // Initialization failed
    }

    const char *trace_env = getenv("REU_TRACE");
    if (trace_env != NULL && trace_env[0] != '\0' && strcmp(trace_env, "0") != 0)
        g_reu_trace = 1;

    printf("\n[REU] C64 REU Emulator Running. Press Ctrl+C to exit.\n");
    if (g_reu_trace)
        printf("[REU] Trace enabled (REU_TRACE=%s).\n", trace_env);
    if (g_ignore_io_vector_writes)
        printf("[REU] Ignoring writes to C64 $031A-$032D and $0330-$0333.\n");

    if (sysop_init() != 0) {
        printf("sysop_init failed\n");
        return 1;
    }

    // Reset any stale DMA trigger state on the FPGA.
    sysop_command(36);

    printf("[REU] Enabling I/O so DF00-DF0A are accessible.\n");
    sysop_dma_enable();
    sysop_disable_reu_dma_trigger();
    sysop_enable_io();

    // Clear REU status register before arming the trigger.
    sysop_internal_poke(0xdf00, reu_registers[REU_REG_STATUS]);

    // Arm the REU DMA trigger: the FPGA will assert DMA and halt the C64
    // the moment it sees a write to DF01 with the EXECUTE bit set.
    sysop_enable_reu_dma_trigger();

    printf("[REU] REU DMA trigger armed. Releasing bus.\n");
    sysop_dma_disable();

    // --- Event loop ---
    //
    // The FPGA halts the C64 bus (asserts DMA) when it detects a write to
    // DF01 with bit 7 set, then signals us by clearing bit 31 of dmainfo.
    // We service the transfer and call sysop_dma_disable() to release the bus.
    int spin = 0;
    while (running) {
        uint32_t dmainfo = sysop_get_dma_info();
        if ((dmainfo & 0x80000000) != 0) {
            // No trigger pending; sleep briefly and retry.
            usleep(1000);
            spin++;
            if ((spin % 1000) == 0)
                reu_trace("Waiting for trigger (spin=%d dmainfo=%08X)", spin, dmainfo);
            continue;
        }
        spin = 0;

        // DMA is now held by the trigger — snapshot DF01 to confirm execute.
        unsigned char command = sysop_internal_peek(0xDF01);
        reu_trace("Trigger fired DF01=0x%02X", command);

        if (command & REU_CMD_EXECUTE) {
            // Clear EXECUTE bit before performing the DMA so the C64 sees it
            // cleared when the bus is released (matches hardware behavior).
            sysop_internal_poke(0xdf01, (unsigned char)(command & ~REU_CMD_EXECUTE));
            reu_perform_dma();

            // Release the bus only for transfers we actually handled.
            sysop_dma_disable();
        } else {
            // DMA active but no REU execute command observed.
            // Another tool may currently own DMA (e.g. read_d64 load/verify),
            // so do not force sysop_dma_disable() here.
            reu_trace("Spurious trigger (no EXECUTE bit) DF01=0x%02X", command);
        }
    }

    sysop_disable_reu_dma_trigger();
    reu_cleanup();
    sysop_uninit();
    return 0;
}
