/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#ifndef ASM_H
#define ASM_H

int assemble(uint16_t address, const char* input, unsigned char** ppBytes, uint8_t* pcBytes);

#endif /* ASM_H */
