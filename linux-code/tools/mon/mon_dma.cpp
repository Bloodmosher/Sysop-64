/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "mon_private.h"
#include "mon_dma.h"

// ---------------------------------------------------------------------------
// wait_for_command_finish
//
// Poll until the C64-side stub clears the resume-pending flag at $DFF1.
// Must not be called while own_dma is held by this process.
// ---------------------------------------------------------------------------

void wait_for_command_finish(void)
{
    if (own_dma) {
        printf("Error: must not own DMA when waiting for command finish\n");
        exit(1);
    }

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    while (1) {
        usleep(50000);
        OwnDma odma;
        uint8_t waitSignal = sysop_peek(0xdff1);
        printf("%02X\n", waitSignal);
        if (waitSignal == 0)
            break;
    }
}

// ---------------------------------------------------------------------------
// command_dma_break
//
// Assert DMA and capture bus samples until DMA is acknowledged.
// Sets own_dma = 1.
// ---------------------------------------------------------------------------

void command_dma_break(void)
{
    sysop_sampler_start();
    sysop_dma_enable();

    // Wait for the sampler to finish collecting
    while (1) {
        uint32_t status1 = sysop_read_status_1();
        if (((status1 >> 15) & 0x1) == 0)
            break;
        printf("Waiting for sampler to finish...\n");
    }

    struct sysop_c64_bus_sample sample;
    for (uint32_t i = 0; i < SYSOP64_SAMPLER_MAX_SAMPLES; i++) {
        uint64_t raw = sysop_sampler_get_sample(i, &sample);
        if (raw == 0)
            break;
        
        if (sample.phi2 == 1 && sample.ba == 1 && sample.sample_tick == 44) {
            printf("%d %04X %02X %c IRQ=%d DMA=%d\n",
               sample.sample_tick, sample.addr, sample.data,
               sample.r__w == 0 ? 'W' : 'R', sample._irq, sample._dma);
        }
        if (sample._dma == 0 && sample.ba == 1)
            break;
    }

    own_dma = 1;
}

// ---------------------------------------------------------------------------
// command_dma_step
//
// Single-step: if not yet frozen, calls command_dma_break.
// If already frozen, releases DMA briefly to let one cycle execute, then
// re-asserts and prints the first bus sample on resumption.
// ---------------------------------------------------------------------------

void command_dma_step(void)
{
    if (!own_dma) {
        command_dma_break();
        return;
    }

    sysop_sampler_start();
    sysop_dma_disable();
    sysop_dma_enable();

    while (1) {
        uint32_t status1 = sysop_read_status_1();
        if (((status1 >> 15) & 0x1) == 0)
            break;
        printf("Waiting for sampler to finish...\n");
    }

    struct sysop_c64_bus_sample sample;
    int count = 0;
    uint32_t i = 0;

    // Print the first phi2=44 cycle with DMA de-asserted (the stepped cycle)
    for (; i < SYSOP64_SAMPLER_MAX_SAMPLES; i++) {
        uint64_t raw = sysop_sampler_get_sample(i, &sample);
        if (raw == 0)
            break;
        if (sample.phi2 == 1 && sample.ba == 1 && sample.sample_tick == 44 && sample._dma == 1) {
            printf("%04X %02X %c IRQ=%d DMA=%d\n",
                   sample.addr, sample.data,
                   sample.r__w == 0 ? 'W' : 'R',
                   sample._irq, sample._dma);
            if (++count == 1)
                break;
        }
    }
    i++;

    printf("\n");

    // Print subsequent phi2=44 cycles for context
    for (; i < SYSOP64_SAMPLER_MAX_SAMPLES; i++) {
        uint64_t raw = sysop_sampler_get_sample(i, &sample);
        if (raw == 0)
            break;
        if (sample.sample_tick == 44) {
            printf("%04X %02X %c IRQ=%d DMA=%d\n",
                   sample.addr, sample.data,
                   sample.r__w == 0 ? 'W' : 'R',
                   sample._irq, sample._dma);
        }
        if (sample._dma == 0)
            break;
    }

    printf("Own dma: %d\n", own_dma);
}

// ---------------------------------------------------------------------------
// command_dma_resume
//
// De-assert DMA and capture post-release bus samples.
// Sets own_dma = 0.
// ---------------------------------------------------------------------------

void command_dma_resume(void)
{
    sysop_sampler_start();
    sysop_dma_disable();

    while (1) {
        uint32_t status1 = sysop_read_status_1();
        if (((status1 >> 15) & 0x1) == 0)
            break;
        printf("Waiting for sampler to finish...\n");
    }

    struct sysop_c64_bus_sample sample;
    int count = 0;
    for (uint32_t i = 0; i < SYSOP64_SAMPLER_MAX_SAMPLES; i++) {
        uint64_t raw = sysop_sampler_get_sample(i, &sample);
        if (raw == 0)
            break;
        if (sample._dma == 1) {
            printf("%04X %02X %c IRQ=%d\n",
                   sample.addr, sample.data,
                   sample.r__w == 0 ? 'W' : 'R', sample._irq);
            if (++count == 4)  // a couple of cycles is enough
                break;
        }
    }

    own_dma = 0;
}
