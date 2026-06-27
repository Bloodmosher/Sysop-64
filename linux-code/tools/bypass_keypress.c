/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <memory.h>
#include "sysop64.h"

// Pattern matching types
typedef enum {
    MATCH_DATA,      // Match on sample.data
    MATCH_ADDR,      // Match on sample.addr
    MATCH_WILDCARD,   // Match any value (wildcard)
    MATCH_WILDCARD_WRITE // Match any value (wildcard write)
} match_type_t;

typedef struct {
    match_type_t match_type;
    uint16_t value;
} pattern_step_t;

typedef struct {
    const char *description;
    pattern_step_t *steps;
    int num_steps;
    int patch_offset;      // Offset from match start to patch
    uint8_t patch_byte1;   // First byte to write
    uint8_t patch_byte2;   // Second byte to write (optional, 0 if not used)
    uint8_t patch_byte3;   // Third byte to write (optional, 0 if not used)
} pattern_t;

// Match a pattern against sampled bus activity
// Returns the starting address if found, 0 otherwise
uint16_t find_pattern(pattern_t *pattern, uint8_t *captured_wildcard)
{
    struct sysop_c64_bus_sample sample;
    int state = 0;
    uint16_t match_start_address = 0;
    int wildcard_count = 0;
    int match_start_sample_number = 0;

    // There's a trade-off here between how many samples we check and how long it takes. 1000000 samples is about 1 c64 frame, each c64 cycle gets 50 samples
    //for (int i = 0; i < SYSOP64_SAMPLER_MAX_SAMPLES; i++)
    for (int i = 0; i < 1000000; i++)
    {
        sysop_sampler_get_sample(i, &sample);

        //if (sample.sample_tick == 44 && sample.r__w == 1 && sample.phi2 == 1 && sample.ba == 1)
        if (sample.sample_tick == 44 && sample.phi2 == 1 && sample.ba == 1)
        {
            bool matched = false;
            
            if (state < pattern->num_steps)
            {
                pattern_step_t *step = &pattern->steps[state];
                
                if (step->match_type == MATCH_DATA && sample.r__w == 1)
                {
                    matched = (sample.data == step->value);
                }
                else if (step->match_type == MATCH_ADDR && sample.r__w == 1)
                {
                    matched = (sample.addr == step->value);
                }
                else if (step->match_type == MATCH_WILDCARD && sample.r__w == 1)
                {
                    matched = true;  // Always matches
                    // Capture the first wildcard value if buffer provided
                    if (captured_wildcard != NULL && wildcard_count == 0)
                    {
                        *captured_wildcard = sample.data;
                    }
                    wildcard_count++;
                }
                else if (step->match_type == MATCH_WILDCARD_WRITE && sample.r__w == 0)
                {
                    matched = true;  // Always matches
                    // Capture the first wildcard value if buffer provided
                    if (captured_wildcard != NULL && wildcard_count == 0)
                    {
                        *captured_wildcard = sample.data;
                    }
                    wildcard_count++;
                }

                if (matched)
                {
                    if (state == 0)
                    {
                        match_start_address = sample.addr;
                        match_start_sample_number = i;
                    }
                    state++;
                    
                    if (state == pattern->num_steps)
                    {
                        printf("Found pattern '%s' starting at %04X (sample %d)\n", 
                               pattern->description, match_start_address, match_start_sample_number);
                        return match_start_address;
                    }
                }
                else
                {
                    state = 0;
                    match_start_address = 0;
                    wildcard_count = 0;
                }
            }
        }
    }

    return 0;
}

int patch_space_loop()
{
    printf("Examining samples...\n");

    //11A0: AD 01 DC LDA $DC01
    //11A3: C9 EF    CMP #$EF
    //11A5: D0 F9    BNE $11A0

    // Define the pattern to match
    pattern_step_t keyboard_pattern_steps[] = {
        {MATCH_DATA, 0xAD},     // LDA opcode
        {MATCH_DATA, 0x01},     // Low byte of address
        {MATCH_DATA, 0xDC},     // High byte of address
        {MATCH_ADDR, 0xDC01},   // Address being read
        {MATCH_DATA, 0xC9},     // CMP opcode
        {MATCH_DATA, 0xEF},     // Compare value
        {MATCH_DATA, 0xD0}      // BNE opcode
    };

    pattern_t keyboard_pattern = {
        .description = "keyboard poll loop",
        .steps = keyboard_pattern_steps,
        .num_steps = sizeof(keyboard_pattern_steps) / sizeof(pattern_step_t),
        .patch_offset = 5,
        .patch_byte1 = 0xEA,    // NOP
        .patch_byte2 = 0xEA,    // NOP
        .patch_byte3 = 0x00
    };

    uint16_t match_address = find_pattern(&keyboard_pattern, NULL);

    if (match_address != 0)
    {
        //printf("Done examining samples - hit enter to patch\n");
        //getchar();

        sysop_server_dma_lock();
        // Apply the patch
        sysop_poke(match_address + keyboard_pattern.patch_offset, keyboard_pattern.patch_byte1);
        sysop_poke(match_address + keyboard_pattern.patch_offset + 1, keyboard_pattern.patch_byte2);
        sysop_server_dma_unlock();
        printf("Patch completed.\n");
        return 1;
    }
    else
    {
        printf("Pattern not found.\n");
    }
    return 0;
}

int patch_space_loop2()
{
    printf("Looking for space bar loop pattern #2\n");

    // 1949: A9 EF    LDA #$EF
    // 194B: CD 01 DC CMP $DC01
    // 194E: D0 F9    BNE $1949    

    // Define the pattern to match
    pattern_step_t keyboard_pattern_steps[] = {
        {MATCH_DATA, 0xA9},     // LDA opcode
        {MATCH_DATA, 0xEF},     // Immediate value
        {MATCH_DATA, 0xCD},     // CMP opcode
        {MATCH_DATA, 0x01},     // Low byte of address
        {MATCH_DATA, 0xDC},     // High byte of address
        {MATCH_ADDR, 0xDC01},   // Address being read
        {MATCH_DATA, 0xD0}      // BNE opcode
    };

    pattern_t keyboard_pattern = {
        .description = "keyboard poll loop 2",
        .steps = keyboard_pattern_steps,
        .num_steps = sizeof(keyboard_pattern_steps) / sizeof(pattern_step_t),
        .patch_offset = 5,
        .patch_byte1 = 0xEA,    // NOP
        .patch_byte2 = 0xEA,    // NOP
        .patch_byte3 = 0x00
    };

    uint16_t match_address = find_pattern(&keyboard_pattern, NULL);

    if (match_address != 0)
    {
        //printf("Done examining samples - hit enter to patch\n");
        //getchar();

        printf("Found space bar loop pattern #2 at %04X\n", match_address);

        sysop_server_dma_lock();
        // Apply the patch
        sysop_poke(match_address + keyboard_pattern.patch_offset, keyboard_pattern.patch_byte1);
        sysop_poke(match_address + keyboard_pattern.patch_offset + 1, keyboard_pattern.patch_byte2);
        sysop_server_dma_unlock();
        printf("Patch completed.\n");
        return 1;
    }
    else
    {
        printf("Pattern not found.\n");
    }
    return 0;
}

int patch_space_loop3()
{
    printf("Looking for space bar loop pattern #3\n");

    // 193A: AD 01 DC LDA $DC01
    // 193D: C9 EF    CMP #$EF
    // 193F: F0 01    BEQ $1942    
    // Define the pattern to match
    pattern_step_t keyboard_pattern_steps[] = {
        {MATCH_DATA, 0xAD},
        {MATCH_DATA, 0x01},
        {MATCH_DATA, 0xDC},
        {MATCH_ADDR, 0xDC01},
        {MATCH_DATA, 0xC9},
        {MATCH_DATA, 0xEF},
        {MATCH_DATA, 0xF0},
        {MATCH_WILDCARD, 0x00}     // Branch offset (wildcard)
    };

    pattern_t keyboard_pattern = {
        .description = "keyboard poll for space loop 3",
        .steps = keyboard_pattern_steps,
        .num_steps = sizeof(keyboard_pattern_steps) / sizeof(pattern_step_t),
        .patch_offset = 3
    };

    uint8_t beq_offset = 0;
    uint16_t match_address = find_pattern(&keyboard_pattern, &beq_offset);

    if (match_address != 0)
    {
        // Calculate the JMP target address
        // BEQ is at match_address + 5 offset byte is at match_address + 6
        // Branch offset is relative to PC after fetching the offset byte
        // PC after BEQ instruction = match_address + 7
        int8_t signed_offset = (int8_t)beq_offset;
        uint16_t branch_target = (match_address + 7) + signed_offset;
        
        printf("BEQ offset: $%02X (signed: %d)\n", beq_offset, signed_offset);
        printf("Calculated JMP target: $%04X\n", branch_target);

        sysop_server_dma_lock();
        // Replace BEQ with JMP (absolute addressing)
        // JMP opcode is 0x4C, followed by low byte then high byte of target address
        sysop_poke(match_address + keyboard_pattern.patch_offset, 0x4C);  // JMP opcode
        sysop_poke(match_address + keyboard_pattern.patch_offset + 1, branch_target & 0xFF);  // Low byte
        sysop_poke(match_address + keyboard_pattern.patch_offset + 2, (branch_target >> 8) & 0xFF);  // High byte
        sysop_server_dma_unlock();
        printf("Patch completed: BEQ replaced with JMP $%04X\n", branch_target);
        return 1;
    }
    else
    {
        printf("Pattern not found.\n");
    }
    return 0;
}


int patch_on_runstop_or_space()
{
    printf("Examining samples...\n");

    // 0884: AD 01 DC LDA $DC01
    // 0887: C9 7F    CMP #$7F
    // 0889: F0 56    BEQ $08E1   (wildcard offset - we'll capture this)
    // 088B: C9 EF    CMP #$EF
    // 088D: D0 F5    BNE $0884   (wildcard offset)

    // Define the pattern to match
    pattern_step_t runstop_space_pattern_steps[] = {
        {MATCH_DATA, 0xAD},        // LDA opcode
        {MATCH_DATA, 0x01},        // Low byte of address
        {MATCH_DATA, 0xDC},        // High byte of address
        {MATCH_ADDR, 0xDC01},      // Address being read
        {MATCH_DATA, 0xC9},        // CMP opcode
        {MATCH_DATA, 0x7F},        // Compare value #$7F (RUN/STOP)
        {MATCH_DATA, 0xF0},        // BEQ opcode
        {MATCH_WILDCARD, 0x00},    // Branch offset (wildcard - capture this)
        {MATCH_DATA, 0xC9},        // CMP opcode
        {MATCH_DATA, 0xEF},        // Compare value #$EF (SPACE)
        {MATCH_DATA, 0xD0},        // BNE opcode
        {MATCH_WILDCARD, 0x00}     // Branch offset (wildcard)
    };

    pattern_t runstop_space_pattern = {
        .description = "RUN/STOP or SPACE poll loop",
        .steps = runstop_space_pattern_steps,
        .num_steps = sizeof(runstop_space_pattern_steps) / sizeof(pattern_step_t),
        .patch_offset = 5,         // Offset to CMP #$7F value (one byte before BEQ)
        .patch_byte1 = 0x00,       // Will be calculated
        .patch_byte2 = 0x00,       // Will be calculated
        .patch_byte3 = 0x00        // Will be calculated
    };

    uint8_t beq_offset = 0;
    uint16_t match_address = find_pattern(&runstop_space_pattern, &beq_offset);

    if (match_address != 0)
    {
        // Calculate the JMP target address
        // BEQ is at match_address + 6, offset byte is at match_address + 7
        // Branch offset is relative to PC after fetching the offset byte
        // PC after BEQ instruction = match_address + 8
        // But we were calculating one too high, so subtract 1
        int8_t signed_offset = (int8_t)beq_offset;
        uint16_t branch_target = (match_address + 8 - 1) + signed_offset;
        
        printf("BEQ offset: $%02X (signed: %d)\n", beq_offset, signed_offset);
        printf("Calculated JMP target: $%04X\n", branch_target);
        //printf("Done examining samples - hit enter to patch\n");
        //getchar();

        sysop_server_dma_lock();
        // Replace BEQ with JMP (absolute addressing)
        // JMP opcode is 0x4C, followed by low byte then high byte of target address
        sysop_poke(match_address + runstop_space_pattern.patch_offset, 0x4C);  // JMP opcode
        sysop_poke(match_address + runstop_space_pattern.patch_offset + 1, branch_target & 0xFF);  // Low byte
        sysop_poke(match_address + runstop_space_pattern.patch_offset + 2, (branch_target >> 8) & 0xFF);  // High byte
        sysop_server_dma_unlock();
        printf("Patch completed: BEQ replaced with JMP $%04X\n", branch_target);
        return 1;
    }
    else
    {
        printf("Pattern not found.\n");
    }
    return 0;
}

// void patch_highscore_or_trainer()
// {
//     sysop_sampler_wait_not_busy();
//     sysop_sampler_start();
//     sysop_sampler_wait_not_busy();
//     printf("Examining samples...\n");

//     // CA4A: 20 E4 FF JSR $FFE4
//     // CA4D: C9 54    CMP #$54  ('T' for Trainer)
//     // CA4F: F0 07    BEQ $CA58
//     // CA51: C9 48    CMP #$48  ('H' for Highscore)
//     // CA53: D0 F5    BNE $CA4A
//     // CA55: 4C 28 CB JMP $CB28

//     // Define the pattern to match
//     pattern_step_t highscore_trainer_pattern_steps[] = {
//         {MATCH_DATA, 0x20},        // JSR opcode
//         {MATCH_DATA, 0xE4},        // Low byte of JSR address
//         {MATCH_DATA, 0xFF},        // High byte of JSR address
//         {MATCH_DATA, 0xC9},        // CMP opcode
//         {MATCH_DATA, 0x54},        // Compare value #$54 ('T')
//         {MATCH_DATA, 0xF0},        // BEQ opcode
//         {MATCH_WILDCARD, 0x00},    // Branch offset (wildcard)
//         {MATCH_DATA, 0xC9},        // CMP opcode
//         {MATCH_DATA, 0x48},        // Compare value #$48 ('H')
//         {MATCH_DATA, 0xD0},        // BNE opcode
//         {MATCH_WILDCARD, 0x00}     // Branch offset (wildcard)
//     };

//     pattern_t highscore_trainer_pattern = {
//         .description = "Highscore or Trainer selection",
//         .steps = highscore_trainer_pattern_steps,
//         .num_steps = sizeof(highscore_trainer_pattern_steps) / sizeof(pattern_step_t),
//         .patch_offset = 9,         // Offset to BNE instruction
//         .patch_byte1 = 0xEA,       // NOP
//         .patch_byte2 = 0xEA,       // NOP
//         .patch_byte3 = 0x00
//     };

//     uint16_t match_address = find_pattern(&highscore_trainer_pattern, NULL);

//     if (match_address != 0)
//     {
//         printf("Done examining samples - hit enter to patch\n");
//         getchar();

//         sysop_server_dma_lock();
//         // Apply the patch - NOP out the BNE instruction
//         sysop_poke(match_address + highscore_trainer_pattern.patch_offset, highscore_trainer_pattern.patch_byte1);
//         sysop_poke(match_address + highscore_trainer_pattern.patch_offset + 1, highscore_trainer_pattern.patch_byte2);
//         sysop_server_dma_unlock();
//         printf("Patch completed: BNE replaced with NOPs\n");
//     }
//     else
//     {
//         printf("Pattern not found.\n");
//     }
// }

int patch_highscore_or_trainer_part_two(uint16_t address)
{
    uint8_t mem[0xFFFF];
    for (int i=0;i<0xFFFF;i++)
    {
        mem[i] = sysop_internal_peek(i);
    }

    if (mem[address] == 0x20 &&
        mem[address+1] == 0xE4 &&
        mem[address+2] == 0xFF &&
        mem[address+3] == 0xC9 &&
        mem[address+4] == 0x54 &&
        mem[address+5] == 0xF0 &&
        mem[address+7] == 0xC9 &&
        mem[address+8] == 0x48 &&
        mem[address+9] == 0xD0)
    {
        printf("Found JSR $FFE4; CMP at %04X\n", address);
        sysop_server_dma_lock();
        sysop_poke(address+9, 0xEA); // NOP
        sysop_poke(address+10, 0xEA); // NOP
        sysop_server_dma_unlock();
        printf("Patched BNE to NOPs at %04X\n", address+9);
        return 1;
    }
    else if (mem[address] == 0x20 &&
        mem[address+1] == 0xE4 &&
        mem[address+2] == 0xFF &&
        mem[address+3] == 0xC5 &&
        mem[address+4] == 0x02 &&
        mem[address+5] == 0xF0 &&
        mem[address+7] == 0xC5 &&
        mem[address+8] == 0x03 &&
        mem[address+9] == 0xD0)
    {
        /*3763: 20 E4 FF JSR $FFE4 ; GETIN (get a byte)
        3766: C5 02    CMP $02
        3768: F0 04    BEQ $376E
        376A: C5 03    CMP $03
        376C: D0 F5    BNE $3763
        */
        
        if (mem[0x2] == 0x48 && mem[0x03] == 0x54)
        {
            printf("Detected 'H' and 'T' at $02 and $03\n");
            printf("Patching BEQ to always select 'H'\n");

            int8_t signed_offset = (int8_t)mem[address+6];
            uint16_t branch_target = (address + 7) + signed_offset;
            
            printf("Calculated JMP target: $%04X\n", branch_target);
            //printf("Hit enter to patch\n");
            //getchar();

            sysop_server_dma_lock();
            // Replace BEQ with JMP (absolute addressing)
            // JMP opcode is 0x4C, followed by low byte then high byte of target address
            sysop_poke(address + 5, 0x4C);  // JMP opcode
            sysop_poke(address + 6, branch_target & 0xFF);  // Low byte
            sysop_poke(address + 7, (branch_target >> 8) & 0xFF);  // High byte
            sysop_server_dma_unlock();
            printf("Patch completed: BEQ replaced with JMP $%04X\n", branch_target);
            return 1;
        }
        else if (mem[0x2] == 0x54 && mem[0x03] == 0x48)
        {
            printf("Detected 'T' and 'H' at $02 and $03\n");
            printf("Patching BNE to always select 'H'\n");

            sysop_server_dma_lock();
            sysop_poke(address+9, 0xEA); // NOP
            sysop_poke(address+10, 0xEA); // NOP
            sysop_server_dma_unlock();
            printf("Patched BNE to NOPs at %04X\n", address+9);
            return 1;
        }
        else
        {
            printf("Unexpected values at $02 and $03: $%02X, $%02X\n", mem[0x2], mem[0x3]);
        }
    }
    else
    {
        printf("No JSR $FFE4 at %04X\n", address);
    }
    return 0;
}


int patch_highscore_or_trainer()
{
    printf("Examining samples...\n");

    // CA4A: 20 E4 FF JSR $FFE4
    // CA4D: C9 54    CMP #$54  ('T' for Trainer)
    // CA4F: F0 07    BEQ $CA58
    // CA51: C9 48    CMP #$48  ('H' for Highscore)
    // CA53: D0 F5    BNE $CA4A
    // CA55: 4C 28 CB JMP $CB28

    // Define the pattern to match
    pattern_step_t highscore_trainer_pattern_steps[] = {
        {MATCH_DATA, 0x20},        // JSR opcode
        {MATCH_DATA, 0xE4},        // Low byte of JSR address
        {MATCH_WILDCARD, 0x00},    // stack read
        {MATCH_WILDCARD_WRITE, 0x00},    // stack write
        {MATCH_WILDCARD_WRITE, 0x00},    // stack write
        {MATCH_DATA, 0xFF}        // High byte of JSR address
    };

    pattern_t highscore_trainer_pattern = {
        .description = "Highscore or Trainer selection",
        .steps = highscore_trainer_pattern_steps,
        .num_steps = sizeof(highscore_trainer_pattern_steps) / sizeof(pattern_step_t),
        .patch_offset = 9,         // Offset to BNE instruction
        .patch_byte1 = 0xEA,       // NOP
        .patch_byte2 = 0xEA,       // NOP
        .patch_byte3 = 0x00
    };

    uint16_t match_address = find_pattern(&highscore_trainer_pattern, NULL);

    if (match_address != 0)
    {
        //printf("Done examining samples - hit enter to patch\n");
        //getchar();

        return patch_highscore_or_trainer_part_two(match_address);
    }
    else
    {
        printf("Pattern not found.\n");
    }

    return 0;
}

int patch_load_or_reset_highscore_part_two(uint16_t address)
{
    uint8_t mem[0xFFFF];
    for (int i=0;i<0xFFFF;i++)
    {
        mem[i] = sysop_internal_peek(i);
    }

    if (mem[address] == 0x20 &&
        mem[address+1] == 0xE4 &&
        mem[address+2] == 0xFF &&
        mem[address+3] == 0xC9 &&
        mem[address+4] == 0x52 &&
        mem[address+5] == 0xF0 &&
        mem[address+7] == 0xC9 &&
        mem[address+8] == 0x4C &&
        mem[address+9] == 0xD0)
    {
        printf("Found JSR $FFE4 at %04X\n", address);
        sysop_server_dma_lock();
        
        // this should end up doing "L" instead of "R"
        sysop_poke(address+9, 0xEA); // NOP
        sysop_poke(address+10, 0xEA); // NOP
        sysop_server_dma_unlock();
        printf("Patched BNE to NOPs at %04X\n", address+9);
        return 1;
    }
    else
    {
        printf("No JSR $FFE4 at %04X\n", address);
    }
    return 0;
}

int patch_load_or_reset_highscore()
{
    printf("Examining samples...looking for [L]oad or [R]eset loop pattern 1\n");

    /*CB34: 20 E4 FF JSR $FFE4 ; GETIN (get a byte)
    CB37: C9 52    CMP #$52
    CB39: F0 24    BEQ $CB5F
    CB3B: C9 4C    CMP #$4C
    CB3D: D0 F5    BNE $CB34
    CB3F: 20 44 E5 JSR $E544 ; clear screen
    */

     // Define the pattern to match
    pattern_step_t highscore_trainer_pattern_steps[] = {
        {MATCH_DATA, 0x20},        // JSR opcode
        {MATCH_DATA, 0xE4},        // Low byte of JSR address
        {MATCH_WILDCARD, 0x00},    // stack read
        {MATCH_WILDCARD_WRITE, 0x00},    // stack write
        {MATCH_WILDCARD_WRITE, 0x00},    // stack write
        {MATCH_DATA, 0xFF}        // High byte of JSR address
    };

    pattern_t highscore_trainer_pattern = {
        .description = "[L]oad or [R]eset highscorelist",
        .steps = highscore_trainer_pattern_steps,
        .num_steps = sizeof(highscore_trainer_pattern_steps) / sizeof(pattern_step_t),
        .patch_offset = 9,         // Offset to BNE instruction
        .patch_byte1 = 0xEA,       // NOP
        .patch_byte2 = 0xEA,       // NOP
        .patch_byte3 = 0x00
    };

    uint16_t match_address = find_pattern(&highscore_trainer_pattern, NULL);

    if (match_address != 0)
    {
        printf("Found SR$FFE4 at %04X\n", match_address);
        //printf("Done examining samples - hit enter to patch\n");
        getchar();

        return patch_load_or_reset_highscore_part_two(match_address);
    }
    else
    {
        printf("Pattern not found.\n");
    }

    return 0;
}

int patch_any_key1()
{
    printf("Examining samples...\n");

    //098F: 20 E4 FF JSR $FFE4 ; GETIN (get a byte)
    //0992: D0 08    BNE $099C    

    // Define the pattern to match
    pattern_step_t highscore_trainer_pattern_steps[] = {
        {MATCH_DATA, 0x20},        // JSR opcode
        {MATCH_DATA, 0xE4},        // Low byte of JSR address
        {MATCH_WILDCARD, 0x00},    // stack read
        {MATCH_WILDCARD_WRITE, 0x00},    // stack write
        {MATCH_WILDCARD_WRITE, 0x00},    // stack write
        {MATCH_DATA, 0xFF}        // High byte of JSR address
    };

    pattern_t highscore_trainer_pattern = {
        .description = "Wait for any keypress",
        .steps = highscore_trainer_pattern_steps,
        .num_steps = sizeof(highscore_trainer_pattern_steps) / sizeof(pattern_step_t)
    };

    uint16_t match_address = find_pattern(&highscore_trainer_pattern, NULL);

    if (match_address != 0)
    {
        printf("Found executing JSR $FFE4 at %04X\n", match_address);

        uint8_t mem[0xFFFF];
        for (int i=0;i<0xFFFF;i++)
        {
            mem[i] = sysop_internal_peek(i);
        }

        if (mem[match_address] == 0x20 &&
            mem[match_address+1] == 0xE4 &&
            mem[match_address+2] == 0xFF &&
            mem[match_address+3] == 0xD0)
        {
            printf("Found JSR $FFE4; BNE at %04X\n", match_address);
            
            int8_t signed_offset = (int8_t)mem[match_address+4];
            uint16_t branch_target = (match_address + 5) + signed_offset;
            
            printf("Calculated JMP target: $%04X\n", branch_target);
            //printf("Hit enter to patch\n");
            //getchar();

            sysop_server_dma_lock();
            // Replace BEQ with JMP (absolute addressing)
            // JMP opcode is 0x4C, followed by low byte then high byte of target address
            sysop_poke(match_address + 3, 0x4C);  // JMP opcode
            sysop_poke(match_address + 4, branch_target & 0xFF);  // Low byte
            sysop_poke(match_address + 5, (branch_target >> 8) & 0xFF);  // High byte
            sysop_server_dma_unlock();
            printf("Patch completed: BNE replaced with JMP $%04X\n", branch_target);
            return 1;
        }
        else if (mem[match_address] == 0x20 &&
            mem[match_address+1] == 0xE4 &&
            mem[match_address+2] == 0xFF &&
            mem[match_address+3] == 0xF0 &&
            mem[match_address+4] == 0xFB)
        {
            //71FD: 20 E4 FF JSR $FFE4 ; GETIN (get a byte)
            //7200: F0 FB    BEQ $71FD
            
            printf("Found JSR $FFE4; BEQ at %04X\n", match_address);
            
            sysop_server_dma_lock();
            // Replace BEQ with NOPs
            sysop_poke(match_address + 3, 0xEA);  // NOP
            sysop_poke(match_address + 4, 0xEA);  // NOP
            sysop_server_dma_unlock();
            printf("Patch completed: BEQ replaced with NOPs at $%04X\n", match_address+3);
            return 1;
        }
        else
        {
            printf("No JSR $FFE4; BNE at %04X\n", match_address);
            return 0;
        }
    }
    else
    {
        printf("Pattern not found.\n");
    }

    return 0;
}



int patch_spacebar_variation2()
{
    //1268: 20 E4 FF JSR $FFE4 ; GETIN (get a byte)
    //126B: C9 20    CMP #$20
    //126D: D0 F9    BNE $1268

    printf("Examining samples...\n");

    //098F: 20 E4 FF JSR $FFE4 ; GETIN (get a byte)
    //0992: D0 08    BNE $099C    

    // Define the pattern to match
    pattern_step_t pattern_steps[] = {
        {MATCH_DATA, 0x20},        // JSR opcode
        {MATCH_DATA, 0xE4},        // Low byte of JSR address
        {MATCH_WILDCARD, 0x00},    // stack read
        {MATCH_WILDCARD_WRITE, 0x00},    // stack write
        {MATCH_WILDCARD_WRITE, 0x00},    // stack write
        {MATCH_DATA, 0xFF}        // High byte of JSR address
    };

    pattern_t match_pattern = {
        .description = "Wait for space key press",
        .steps = pattern_steps,
        .num_steps = sizeof(pattern_steps) / sizeof(pattern_step_t)
    };

    uint16_t match_address = find_pattern(&match_pattern, NULL);

    if (match_address != 0)
    {
        printf("Found executing JSR $FFE4 at %04X\n", match_address);

        uint8_t mem[0xFFFF];
        for (int i=0;i<0xFFFF;i++)
        {
            mem[i] = sysop_internal_peek(i);
        }

        if (mem[match_address] == 0x20 &&
            mem[match_address+1] == 0xE4 &&
            mem[match_address+2] == 0xFF &&
            mem[match_address+3] == 0xC9 &&
            mem[match_address+4] == 0x20 &&
            mem[match_address+5] == 0xD0)
        {
            printf("Found JSR $FFE4; CMP #$20; BNE at %04X\n", match_address);
            
            sysop_server_dma_lock();
            sysop_poke(match_address + 5, 0xEA); // NOP
            sysop_poke(match_address + 6, 0xEA); // NOP
            sysop_server_dma_unlock();
            printf("Patch completed: BNE replaced with NOPs\n");
            return 1;
        }
        else
        {
            printf("No JSR $FFE4; CMP#$20; BNE at %04X\n", match_address);
            return 0;
        }
    }
    else
    {
        printf("Pattern not found.\n");
    }

    return 0;

}

int patch_y_n(char choice)
{
    //B91C: 20 E4 FF JSR $FFE4 ; GETIN (get a byte)
    //B91F: C9 4E    CMP #$4E
    //B921: F0 09    BEQ $B92C
    //B923: C9 59    CMP #$59
    //B925: D0 F5    BNE $B91C    

    pattern_step_t highscore_trainer_pattern_steps[] = {
        {MATCH_DATA, 0x20},        // JSR opcode
        {MATCH_DATA, 0xE4},        // Low byte of JSR address
        {MATCH_WILDCARD, 0x00},    // stack read
        {MATCH_WILDCARD_WRITE, 0x00},    // stack write
        {MATCH_WILDCARD_WRITE, 0x00},    // stack write
        {MATCH_DATA, 0xFF}        // High byte of JSR address
    };

    pattern_t highscore_trainer_pattern = {
        .description = "Wait for Y or N keypress",
        .steps = highscore_trainer_pattern_steps,
        .num_steps = sizeof(highscore_trainer_pattern_steps) / sizeof(pattern_step_t)
    };

    uint16_t match_address = find_pattern(&highscore_trainer_pattern, NULL);

    if (match_address != 0)
    {
        printf("Found executing JSR $FFE4 at %04X\n", match_address);

        uint8_t mem[0xFFFF];
        for (int i=0;i<0xFFFF;i++)
        {
            mem[i] = sysop_internal_peek(i);
        }

        if (mem[match_address] == 0x20 &&
            mem[match_address+1] == 0xE4 &&
            mem[match_address+2] == 0xFF &&
            mem[match_address+3] == 0xC9 &&
            mem[match_address+5] == 0xF0 && 
            mem[match_address+7] == 0xC9 &&
            mem[match_address+9] == 0xD0)
        {
            printf("Found JSR $FFE4 with two CMP and Branch pattern at %04X\n", match_address);
            
            uint8_t cmp1_value = mem[match_address+4];
            uint8_t cmp2_value = mem[match_address+8];
            uint16_t branch_offset_address = 0;
            int8_t signed_offset = 0;
            uint16_t branch_target = 0;

            int patch_with_jmp = 0;
            if (choice == 'y')
            {
                if (cmp1_value == 0x59) // 'Y'
                {
                    branch_offset_address = match_address + 5; // first branch
                    printf("Branch offset address for 'Y': %04X\n", branch_offset_address);
                    signed_offset = (int8_t)mem[branch_offset_address+1];
                    branch_target = (branch_offset_address + 2) + signed_offset;
                    printf("Calculated JMP target for 'Y': $%04X\n", branch_target);
                    patch_with_jmp = 1;
                }
                else if (cmp2_value == 0x59) // 'Y'
                {
                    printf("Matching 2nd branch will be patched with NOP\n");
                    // branch_offset_address = match_address + 9; // second branch
                    // printf("Branch offset address for 'Y': %04X\n", branch_offset_address);
                    // signed_offset = (int8_t)mem[branch_offset_address+1];
                    // branch_target = (branch_offset_address + 2) + signed_offset;
                    // printf("Calculated JMP target for 'Y': $%04X\n", branch_target);
                }
                else
                {
                    printf("Neither CMP matches 'Y' keycode.\n");
                    return 0;
                }
            }
            else if (choice == 'n')
            {
                if (cmp1_value == 0x4E) // 'N'
                {
                    branch_offset_address = match_address + 5; // first branch
                    printf("Branch offset address for 'N': %04X\n", branch_offset_address);
                    signed_offset = (int8_t)mem[branch_offset_address+1];
                    branch_target = (branch_offset_address + 2) + signed_offset;
                    printf("Calculated JMP target for 'N': $%04X\n", branch_target);
                    patch_with_jmp = 1;
                }
                else if (cmp2_value == 0x4E) // 'N'
                {
                    printf("Matching 2nd branch will be patched with NOP\n");
                    // branch_offset_address = match_address + 9; // second branch
                    // printf("Branch offset address for 'N': %04X\n", branch_offset_address);
                    // signed_offset = (int8_t)mem[branch_offset_address+1];
                    // branch_target = (branch_offset_address + 2) + signed_offset;
                    // printf("Calculated JMP target for 'N': $%04X\n", branch_target);
                }
                else
                {
                    printf("Neither CMP matches 'N' keycode.\n");
                    return 0;
                }
            }
            else
            {
                printf("Invalid choice: %c. Use 'y' or 'n'.\n", choice);
                return 0;
            }

            //printf("Hit enter to patch\n");
            //getchar();

            sysop_server_dma_lock();
            
            if (patch_with_jmp)
            {
                sysop_poke(match_address+5, 0x4C);  // JMP opcode
                sysop_poke(match_address+6, branch_target & 0xFF);  // Low byte
                sysop_poke(match_address+7, (branch_target >> 8) & 0xFF);  // High byte
                printf("Patch completed: second branch replaced with JMP $%04X\n", branch_target);
            }
            else
            {
                // NOP out the second branch
                sysop_poke(match_address+9, 0xEA); // NOP
                sysop_poke(match_address+10, 0xEA); // NOP
                printf("Patch completed: second branch replaced with NOPs\n");
            }
            sysop_server_dma_unlock();
            return 1;
        }
        else
        {
            printf("No JSR $FFE4 with two CMP and Branch pattern at %04X\n", match_address);
            return 0;
        }
    }
    else
    {
        printf("Pattern not found.\n");
    }

    return 0;
}

int patch_y_n_variation2(char choice)
{
    printf("Examining samples...\n");
    printf("Looking for Y/N keypress pattern variation 2...\n");
    // 400F: 20 E4 FF JSR $FFE4 ; GETIN (get a byte)
    // 4012: C9 59    CMP #$59
    // 4014: F0 07    BEQ $401D
    // 4016: C9 4E    CMP #$4E
    // 4018: F0 08    BEQ $4022
    // 401A: 4C 0F 40 JMP $400F

    pattern_step_t keyboard_pattern_steps[] = {
        {MATCH_DATA, 0x20},        // JSR opcode
        {MATCH_DATA, 0xE4},        // Low byte of JSR address
        {MATCH_WILDCARD, 0x00},    // stack read
        {MATCH_WILDCARD_WRITE, 0x00},    // stack write
        {MATCH_WILDCARD_WRITE, 0x00},    // stack write
        {MATCH_DATA, 0xFF}        // High byte of JSR address
    };

    pattern_t keyboard_pattern = {
        .description = "Wait for Y or N keypress",
        .steps = keyboard_pattern_steps,
        .num_steps = sizeof(keyboard_pattern_steps) / sizeof(pattern_step_t)
    };

    uint16_t match_address = find_pattern(&keyboard_pattern, NULL);

    if (match_address != 0)
    {
        printf("Found executing JSR $FFE4 at %04X\n", match_address);

        uint8_t mem[0xFFFF];
        for (int i=0;i<0xFFFF;i++)
        {
            mem[i] = sysop_internal_peek(i);
        }

        for (int i=0;i<14;i++)
        {
            printf("%d %04X: %02X\n", i, match_address+i, mem[match_address+i]);
        }
        printf("%02X\n", (match_address >> 8) & 0xFF);
        printf("%02X\n", match_address & 0xFF);

        
        if (mem[match_address] == 0x20 &&
            mem[match_address+1] == 0xE4 &&
            mem[match_address+2] == 0xFF &&
            mem[match_address+3] == 0xC9 &&
            mem[match_address+5] == 0xF0 && 
            mem[match_address+7] == 0xC9 &&
            mem[match_address+9] == 0xF0 &&
            mem[match_address+11] == 0x4C && 
            mem[match_address+12] == (match_address & 0xFF) && 
            mem[match_address+13] == ((match_address >> 8) & 0xFF)
        )
        {
            printf("Found JSR $FFE4 with two CMP and BEQ/JMP pattern at %04X\n", match_address);
            
            uint8_t cmp1_value = mem[match_address+4];
            uint8_t cmp2_value = mem[match_address+8];
            uint16_t branch_offset_address = 0;
            int8_t signed_offset = 0;
            uint16_t branch_target = 0;

            if (choice == 'y')
            {
                if (cmp1_value == 0x59) // 'Y'
                {
                    branch_offset_address = match_address + 5; // first branch
                    printf("Branch offset address for 'Y': %04X\n", branch_offset_address);
                    signed_offset = (int8_t)mem[branch_offset_address+1];
                    branch_target = (branch_offset_address + 2) + signed_offset;
                    printf("Calculated JMP target for 'Y': $%04X\n", branch_target);
                }
                else if (cmp2_value == 0x59) // 'Y'
                {
                    branch_offset_address = match_address + 9; // second branch
                    printf("Branch offset address for 'Y': %04X\n", branch_offset_address);
                    signed_offset = (int8_t)mem[branch_offset_address+1];
                    branch_target = (branch_offset_address + 2) + signed_offset;
                    printf("Calculated JMP target for 'Y': $%04X\n", branch_target);
                }
                else
                {
                    printf("Neither CMP matches 'Y' keycode.\n");
                    return 0;
                }
            }
            else if (choice == 'n')
            {
                if (cmp1_value == 0x4E) // 'N'
                {
                    branch_offset_address = match_address + 5; // first branch
                    printf("Branch offset address for 'N': %04X\n", branch_offset_address);
                    signed_offset = (int8_t)mem[branch_offset_address+1];
                    branch_target = (branch_offset_address + 2) + signed_offset;
                    printf("Calculated JMP target for 'N': $%04X\n", branch_target);
                }
                else if (cmp2_value == 0x4E) // 'N'
                {
                    branch_offset_address = match_address + 9; // second branch
                    printf("Branch offset address for 'N': %04X\n", branch_offset_address);
                    signed_offset = (int8_t)mem[branch_offset_address+1];
                    branch_target = (branch_offset_address + 2) + signed_offset;
                    printf("Calculated JMP target for 'N': $%04X\n", branch_target);
                }
                else
                {
                    printf("Neither CMP matches 'N' keycode.\n");
                    return 0;
                }
            }
            else
            {
                printf("Invalid choice: %c. Use 'y' or 'n'.\n", choice);
                return 0;
            }

            sysop_server_dma_lock();
            
            sysop_poke(match_address+5, 0x4C);  // JMP opcode
            sysop_poke(match_address+6, branch_target & 0xFF);  // Low byte
            sysop_poke(match_address+7, (branch_target >> 8) & 0xFF);  // High byte
            printf("Patch completed: branch replaced with JMP $%04X\n", branch_target);
            sysop_server_dma_unlock();
            return 1;
        }
        else
        {
            printf("No JSR $FFE4 with two CMP and Branch pattern at %04X\n", match_address);
            return 0;
        }
    }
    else
    {
        printf("Pattern not found.\n");
    }

    return 0;
}


int main(int argc, char *argv[])
{
    sysop_init();

    int res = sysop_server_connect();
    if (res == -1) {
        perror("sysop_server_connect failed");
        exit(EXIT_FAILURE);
    }

    //sysop_enable_io();
    //sysop_io_poke(0xdefe, g_state);
    //sysop_io_poke(0xdeff, g_state_01);
    //sysop_io_poke(0xdefd, g_state_02);

    if (argc < 2) {
        printf("Usage: %s <all|any|space|space2|runstop_or_space|highscore_or_trainer|load_or_reset|y|n>\n", argv[0]);
        printf("  Use 'all' to attempt to apply all patches, stop after first success\n");
        return 0;
    }

    sysop_sampler_wait_not_busy();
    sysop_sampler_start();
    sysop_sampler_wait_not_busy();

    res = 0;

    if (strcmp(argv[1], "all") == 0) {
        res = patch_any_key1();
        if (res == 0) {
            res = patch_space_loop();
        }
        if (res == 0) {
            res = patch_space_loop2();
        }
        if (res == 0) {
            res = patch_space_loop3();
        }
        if (res == 0) {
            res = patch_spacebar_variation2();
        }
        if (res == 0) {
            res = patch_on_runstop_or_space();
        }
        if (res == 0) {
            res = patch_highscore_or_trainer();
        }
        if (res == 0) {
            res = patch_load_or_reset_highscore();
        }
        if (res == 0) {
            res = patch_y_n('Y');
            if (res == 0) {
                res = patch_y_n_variation2('Y');
            }
        }
        if (res == 0) {
            printf("No patches applied.\n");
        }
        else    {
            printf("Patch applied successfully.\n");
        }
    }
    else if (strcmp(argv[1], "any") == 0) {
        patch_any_key1();
    }
    else if (strcmp(argv[1], "y") == 0) {
        res = patch_y_n('y');
        if (res == 0) {
            patch_y_n_variation2('y');
        }
    }
    else if (strcmp(argv[1], "n") == 0) {
        res = patch_y_n('n');
        if (res == 0) {
            patch_y_n_variation2('n');
        }
    }
    else if (strcmp(argv[1], "space") == 0) {
        res = patch_space_loop();
        if (res == 0) {
            patch_space_loop2();
        }
        if (res == 0) {
            patch_space_loop3();
        }
    }
    else if (strcmp(argv[1], "space2") == 0) {
        patch_spacebar_variation2();
    }
    else if (strcmp(argv[1], "runstop_or_space") == 0) {
        patch_on_runstop_or_space();
    }
    else if (strcmp(argv[1], "highscore_or_trainer") == 0) {
        patch_highscore_or_trainer();
    }
    else if (strcmp(argv[1], "load_or_reset") == 0) {
        patch_load_or_reset_highscore();
    } else {
        printf("Unknown command: %s\n", argv[1]);
        printf("Valid commands: space, runstop_or_space, highscore_or_trainer\n");
    }

    return 0;
}


/*TODO: (for Shanghai Karate)

1769: AD 01 DC LDA $DC01
 176C: C9 EF    CMP #$EF
 176E: F0 3C    BEQ $17AC
 1770: 4C BC FE JMP $FEBC
 */
