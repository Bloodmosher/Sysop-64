/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "mon_private.h"
#include "mon_breakpoints.h"
#include "mon_cpu.h"
#include "mon_dma.h"
#include "mon_input.h"

// ---------------------------------------------------------------------------
// Internal helper: print status register flags in a readable format
// ---------------------------------------------------------------------------

static void print_status_flags(uint8_t status)
{
    printf("C=%d Z=%d I=%d D=%d B=%d V=%d N=%d\n",
           (status >> 0) & 1,
           (status >> 1) & 1,
           (status >> 2) & 1,
           (status >> 3) & 1,
           (status >> 4) & 1,
           (status >> 6) & 1,
           (status >> 7) & 1);
}

// ---------------------------------------------------------------------------
// show_registers
// ---------------------------------------------------------------------------

void show_registers(void)
{
    OwnDma odma;

    uint8_t  sp           = sysop_peek(STORED_STACK_ADDRESS);
    uint16_t stack_base   = 0x100 + sp;
    uint8_t  y            = sysop_peek(stack_base + 1);
    uint8_t  x            = sysop_peek(stack_base + 2);
    uint8_t  a            = sysop_peek(stack_base + 3);
    uint8_t  status       = sysop_peek(stack_base + 4);
    uint8_t  lobyte       = sysop_peek(stack_base + 5);
    uint8_t  hibyte       = sysop_peek(stack_base + 6);

    g_status = status;
    g_pc     = (uint16_t)((hibyte << 8) | lobyte);
    if (status & 0x10)
        g_pc -= 2;   // BRK flag set: return address is PC+2, adjust back

    printf("PC:%04X SR:%02X A:%02X X:%02X Y:%02X SP:%02X\n",
           g_pc, status, a, x, y, sp);
    print_status_flags(status);
}

// ---------------------------------------------------------------------------
// setpc â€” write a new return address into the C64 stack frame
// ---------------------------------------------------------------------------

void setpc(uint16_t address)
{
    OwnDma odma;
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    uint8_t  sp         = sysop_peek(STORED_STACK_ADDRESS);
    uint16_t stack_base = 0x100 + sp;
    uint16_t lo_addr    = stack_base + 5;

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    sysop_poke(lo_addr,     (uint8_t)(address & 0xff));
    sysop_poke(lo_addr + 1, (uint8_t)(address >> 8));
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
}

// ---------------------------------------------------------------------------
// resume â€” resume the CPU from a breakpoint, handling PC patch-up
// ---------------------------------------------------------------------------

void resume(void)
{
    OwnDma odma;

    uint8_t  sp         = sysop_peek(STORED_STACK_ADDRESS);
    uint16_t stack_base = 0x100 + sp;
    uint8_t  status     = sysop_peek(stack_base + 4);
    uint8_t  lobyte     = sysop_peek(stack_base + 5);
    uint8_t  hibyte     = sysop_peek(stack_base + 6);

    read_breakpoints();

    // The return address on the stack points two bytes past the BRK instruction.
    uint16_t addr_on_stack = (uint16_t)((hibyte << 8) | lobyte) - 2;

    int write_needed = 0;
    int active = 0;

    for (int i = 0; i < MAX_BREAKPOINTS; i++) {
        if (g_breakpoints[i].address == addr_on_stack && (status & 0x10)) {
            printf("Found match, opcode is %02X\n", g_breakpoints[i].opcode);
            sysop_poke(addr_on_stack, g_breakpoints[i].opcode);
            printf("Patching up PC to point to %04X\n", addr_on_stack);
            setpc(addr_on_stack);
            // Disable this breakpoint; without trace support we can't re-arm it safely.
            g_breakpoints[i].address = 0;
            g_breakpoints[i].opcode  = 0;
            write_needed = 1;
        } else if (g_breakpoints[i].address != 0) {
            active++;
        }
    }

    if (write_needed)
        write_breakpoints();

    if (active > 0) {
        // Point the BRK vector at our stub so the next breakpoint is caught.
        sysop_poke(0x0316, 0x06);
        sysop_poke(0x0317, 0xdf);
        // Mirror to hardware NMI vector in case the kernel is banked out.
        sysop_poke(0xFFFE, 0x00);
        sysop_poke(0xFFFF, 0xdf);
    }

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    sysop_poke(0xdff1, 0xff);  // signal the C64-side stub to resume execution
}

// ---------------------------------------------------------------------------
// determine_next_pc â€” compute the PC value after the current instruction
// ---------------------------------------------------------------------------

int determine_next_pc(uint16_t *pNextPc)
{
    OwnDma odma;

    uint8_t  sp         = sysop_peek(STORED_STACK_ADDRESS);
    uint16_t stack_base = 0x100 + sp;
    uint8_t  status     = sysop_peek(stack_base + 4);
    uint8_t  lobyte     = sysop_peek(stack_base + 5);
    uint8_t  hibyte     = sysop_peek(stack_base + 6);
    uint16_t pc         = (uint16_t)((hibyte << 8) | lobyte);

    if (status & 0x10) {
        pc -= 2;
        read_breakpoints();
    }

    // Read up to 3 bytes at the current PC for disassembly
    uint8_t buf[3];
    for (int i = 0; i < 3; i++)
        buf[i] = sysop_peek(pc + i);

    // If a breakpoint is set here, substitute the original opcode
    for (int i = 0; i < MAX_BREAKPOINTS; i++) {
        if (g_breakpoints[i].address == pc)
            buf[0] = g_breakpoints[i].opcode;
    }

    int bytesUsed   = 0;
    int bytesNeeded = 0;
    Disassemble(0, pc, pc, buf[0], buf, 0, 3, &bytesUsed, &bytesNeeded, 0);
    if (bytesNeeded > 0)
        return -1;

    // Resolve branch targets using the current status register flags
    switch (buf[0]) {
        case 0xd0: {  // BNE: branch if Z clear
            int8_t off = (int8_t)buf[1];
            *pNextPc = (status & 0x02) ? (pc + bytesUsed) : (uint16_t)(pc + 2 + off);
            break;
        }
        case 0xf0: {  // BEQ: branch if Z set
            int8_t off = (int8_t)buf[1];
            *pNextPc = (status & 0x02) ? (uint16_t)(pc + 2 + off) : (pc + bytesUsed);
            break;
        }
        case 0x90: {  // BCC: branch if C clear
            int8_t off = (int8_t)buf[1];
            *pNextPc = (status & 0x01) ? (pc + bytesUsed) : (uint16_t)(pc + 2 + off);
            break;
        }
        case 0xb0: {  // BCS: branch if C set
            int8_t off = (int8_t)buf[1];
            *pNextPc = (status & 0x01) ? (uint16_t)(pc + 2 + off) : (pc + bytesUsed);
            break;
        }
        case 0x30: {  // BMI: branch if N set
            int8_t off = (int8_t)buf[1];
            *pNextPc = (status & 0x80) ? (uint16_t)(pc + 2 + off) : (pc + bytesUsed);
            break;
        }
        case 0x10: {  // BPL: branch if N clear
            int8_t off = (int8_t)buf[1];
            *pNextPc = (status & 0x80) ? (pc + bytesUsed) : (uint16_t)(pc + 2 + off);
            break;
        }
        case 0x70: {  // BVS: branch if V set
            int8_t off = (int8_t)buf[1];
            *pNextPc = (status & 0x40) ? (uint16_t)(pc + 2 + off) : (pc + bytesUsed);
            break;
        }
        case 0x50: {  // BVC: branch if V clear
            int8_t off = (int8_t)buf[1];
            *pNextPc = (status & 0x40) ? (pc + bytesUsed) : (uint16_t)(pc + 2 + off);
            break;
        }
        case 0x4c:    // JMP absolute
        case 0x20: {  // JSR absolute
            *pNextPc = (uint16_t)((buf[2] << 8) | buf[1]);
            break;
        }
        case 0x6c: {  // JMP (indirect)
            uint16_t vec = (uint16_t)((buf[2] << 8) | buf[1]);
            *pNextPc = (uint16_t)((sysop_peek(vec + 1) << 8) | sysop_peek(vec));
            break;
        }
        case 0x60: {  // RTS: return address is on caller's stack frame
            uint16_t ret = (uint16_t)((sysop_peek(stack_base + 8) << 8) | sysop_peek(stack_base + 7));
            *pNextPc = ret + 1;
            break;
        }
        default:
            *pNextPc = pc + bytesUsed;
            break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// run â€” type "RUN\r" into the C64 keyboard buffer
// ---------------------------------------------------------------------------

void run(void)
{
    uint16_t addr = 0x277;
    sysop_poke(addr++, 0x52);  // 'R'
    sysop_poke(addr++, 0x55);  // 'U'
    sysop_poke(addr++, 0x4e);  // 'N'
    sysop_poke(addr++, 0x0d);  // Return
    sysop_poke(0x00c6, 0x04);  // keyboard buffer count = 4
}

// ---------------------------------------------------------------------------
// Peek â€” single-byte read with DMA acquire/release
// ---------------------------------------------------------------------------

uint8_t Peek(uint16_t addr)
{
    sysop_dma_enable();
    uint8_t val = sysop_peek(addr);
    sysop_dma_disable();
    return val;
}

// ---------------------------------------------------------------------------
// save â€” write C64 memory range to a file
// ---------------------------------------------------------------------------

int save(uint16_t start_addr, uint16_t end_addr, const char *filename, int prg)
{
    FILE *file = fopen(filename, "wb");
    if (!file) {
        fprintf(stderr, "Error opening file '%s' for writing.\n", filename);
        return 1;
    }

    int count = end_addr - start_addr + 1;

    if (prg) {
        // PRG header: two-byte load address (little-endian $0801)
        uint8_t hdr[2] = { 0x01, 0x08 };
        fwrite(hdr, 1, 2, file);
    }

    for (int i = 0; i < count; i++) {
        uint8_t data = sysop_peek(start_addr + i);
        fwrite(&data, 1, 1, file);
    }
    fclose(file);

    if (prg)
        printf("Saved %d bytes as PRG to %s.\n", count, filename);
    else
        printf("Saved %d raw bytes to %s.\n", count, filename);

    return 0;
}

