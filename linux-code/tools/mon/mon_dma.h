/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * mon_dma.h — DMA-based CPU sampling commands.
 *
 * These commands use the Sysop-64 bus sampler to observe or step the C64 CPU
 * by asserting/de-asserting the DMA line and capturing bus cycles.
 */

#pragma once

// Wait until the C64-side code signals that it has completed a command.
// Must not be called while own_dma is set.
void wait_for_command_finish(void);

// Assert DMA, capture bus samples until DMA is seen, and set own_dma = 1.
void command_dma_break(void);

// If not yet frozen: calls command_dma_break().
// If already frozen: release and re-assert DMA to advance one CPU cycle,
// then print the sampled bus state.
void command_dma_step(void);

// De-assert DMA, capture post-release bus samples, and set own_dma = 0.
void command_dma_resume(void);
