/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <stdio.h>
#include <regex.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "sysop_defines.h"
#include "instructions.h"

void convertToUppercase(char *str) 
{
    for (int i = 0; str[i]; i++) {
        str[i] = toupper((unsigned char)str[i]);
    }
}

int assemble(uint16_t address, const char* input, unsigned char** ppBytes, uint8_t* pcBytes)
{
    //printf("assemble: '%s' (%d)\n", input, strlen(input));
    char* token = NULL;
    char* instruction = NULL;
    char* argument = NULL;

    int argumentBytes = 0;
    uint8_t argByte1 = 0;
    uint8_t argByte2 = 0;
    int opCode = 0;
    AddressingMode addrMode = Invalid;
    
    //
    // Define regular expressions for each addressing mode
    const char* immediatePattern = "^#\\$[0-9A-Fa-f]{2}$";
    const char* zeroPagePattern = "^\\$[0-9A-Fa-f]{2}$";
    const char* absolutePattern = "^\\$[0-9A-Fa-f]{4}$";
    const char* indirectXPattern = "^\\(\\$[0-9A-Fa-f]{2},X\\)$";
    const char* indirectYPattern = "^\\(\\$[0-9A-Fa-f]{2}\\),Y$";
    const char* zeroPageXPattern = "^\\$[0-9A-Fa-f]{2},X$";
    const char* zeroPageYPattern = "^\\$[0-9A-Fa-f]{2},Y$";
    const char* absoluteXPattern = "^\\$[0-9A-Fa-f]{4},X$";
    const char* absoluteYPattern = "^\\$[0-9A-Fa-f]{4},Y$";
    const char* absoluteIndirectPattern = "^\\(\\$[0-9A-Fa-f]{4}\\)$";
    
    if (ppBytes == NULL || pcBytes == NULL || input == NULL)
        return -1;

    if (ppBytes != NULL)
    {
        *ppBytes = NULL;
    }
    if (pcBytes != NULL)
    {
        *pcBytes = 0;
    }

    // Compile regular expressions
    regex_t immediateRegex, zeroPageRegex, absoluteRegex, indirectXRegex, indirectYRegex;
    regex_t zeroPageXRegex, zeroPageYRegex, absoluteXRegex, absoluteYRegex;
    regex_t absoluteIndirectRegex;

    regcomp(&immediateRegex, immediatePattern, REG_EXTENDED);
    regcomp(&zeroPageRegex, zeroPagePattern, REG_EXTENDED);
    regcomp(&absoluteRegex, absolutePattern, REG_EXTENDED);
    regcomp(&indirectXRegex, indirectXPattern, REG_EXTENDED);
    regcomp(&indirectYRegex, indirectYPattern, REG_EXTENDED);
    regcomp(&zeroPageXRegex, zeroPageXPattern, REG_EXTENDED);
    regcomp(&zeroPageYRegex, zeroPageYPattern, REG_EXTENDED);
    regcomp(&absoluteXRegex, absoluteXPattern, REG_EXTENDED);
    regcomp(&absoluteYRegex, absoluteYPattern, REG_EXTENDED);
    regcomp(&absoluteIndirectRegex, absoluteIndirectPattern, REG_EXTENDED);

    int result = 0;
    char* copy = (char*)strdup(input);
    if (copy == NULL)
    {
        printf("Allocation failed\n");
        result = -1;
        goto exit;
    }

    convertToUppercase(copy);

    token = strtok(copy, " ");
    if (!token)
    {
        printf("Invalid statement\n");
        result = -1;
        goto exit;
    }        
    instruction = (char*)strdup(token);
    argument = NULL;
    token = strtok(NULL, " " );
    if (token)
    {
        argument = (char*)strdup(token);
    }
    else
    {
        addrMode = Implied;
    }

    // check if this is a branch first
    opCode = GetOpCodeForInstruction(instruction, Relative);
    if (opCode != -1)
    {
        addrMode = Relative;
        argumentBytes = 1;
        uint16_t jumpTarget = (uint16_t)strtoll(&argument[1], NULL, 16);
        int diff = jumpTarget - (address + 2); // add in the length of this instruction
        if (diff > 127 || diff < -128)
        {
            printf("Invalid jump range\n");
            result = -1;
            goto exit;
        }

        signed char offset = (signed char)diff;
        argByte1 = offset;
    }

    if (addrMode != Implied && addrMode != Relative)
    {
        // Match argument with each addressing mode
        if (regexec(&immediateRegex, argument, 0, NULL, 0) == 0) {
            addrMode = Immediate;
            argumentBytes = 1;
            sscanf(argument, "#$%2hhx", &argByte1);
        } else if (regexec(&zeroPageRegex, argument, 0, NULL, 0) == 0) {
            addrMode = ZeroPage;
            argumentBytes = 1;
            sscanf(argument, "$%2hhx", &argByte1);
        } else if (regexec(&absoluteRegex, argument, 0, NULL, 0) == 0) {
            addrMode = Absolute;
            argumentBytes = 2;
            sscanf(&argument[3], "%2hhx", &argByte1);
            sscanf(argument, "$%2hhx", &argByte2);
        } else if (regexec(&absoluteIndirectRegex, argument, 0, NULL, 0) == 0) {
            addrMode = AbsoluteIndirect;
            argumentBytes = 2;
            sscanf(&argument[4], "%2hhx", &argByte1);
            sscanf(&argument[1], "$%2hhx", &argByte2);
        } else if (regexec(&indirectXRegex, argument, 0, NULL, 0) == 0) {
            addrMode = IndirectX;
            argumentBytes = 1;
            sscanf(argument, "($%2hhx,X)", &argByte1);
        } else if (regexec(&indirectYRegex, argument, 0, NULL, 0) == 0) {
            addrMode = IndirectY;
            argumentBytes = 1;
            sscanf(argument, "($%2hhx),Y", &argByte1);
        } else if (regexec(&zeroPageXRegex, argument, 0, NULL, 0) == 0) {
            addrMode = ZeroPageX;
            argumentBytes = 1;
            sscanf(argument, "$%2hhx,X", &argByte1);
        } else if (regexec(&zeroPageYRegex, argument, 0, NULL, 0) == 0) {
            addrMode = ZeroPageY;
            argumentBytes = 1;
            sscanf(argument, "$%2hhx,Y", &argByte1);
        } else if (regexec(&absoluteXRegex, argument, 0, NULL, 0) == 0) {
            addrMode = AbsoluteX;
            argumentBytes = 2;
            sscanf(&argument[3], "%2hhx", &argByte1);
            sscanf(argument, "$%2hhx", &argByte2);
        } else if (regexec(&absoluteYRegex, argument, 0, NULL, 0) == 0) {
            addrMode = AbsoluteY;
            argumentBytes = 2;
            sscanf(&argument[3], "%2hhx", &argByte1);
            sscanf(argument, "$%2hhx", &argByte2);
        } else {
            printf("Invalid addressing mode\n");
        }
    }

    opCode = GetOpCodeForInstruction(instruction, addrMode);
    if (opCode != -1)
    {
        *ppBytes = (unsigned char*)malloc(argumentBytes + 1);
        (*ppBytes)[0] = opCode;
        if (argumentBytes >= 1)
        {
            (*ppBytes)[1] = argByte1;
        }
        if (argumentBytes >=2)
        {
            (*ppBytes)[2] = argByte2;
        }
        *pcBytes = argumentBytes + 1;
    }
    else
    {
        printf("No opcode found for %s with argument '%s'\n", instruction, argument);
        result = -1;
    }

exit:
    if (copy != NULL)
        free(copy);
    if (instruction != NULL)
        free(instruction);
    if (argument != NULL)
        free(argument);

    // Free allocated resources
    regfree(&immediateRegex);
    regfree(&zeroPageRegex);
    regfree(&absoluteRegex);
    regfree(&indirectXRegex);
    regfree(&indirectYRegex);
    regfree(&zeroPageXRegex);
    regfree(&zeroPageYRegex);
    regfree(&absoluteXRegex);
    regfree(&absoluteYRegex);
    
    return result;
}

int g_testFailures = 0;

int RunTestCase(const char* input, uint8_t* expectedBytes, uint8_t expectedBytesCount)
{
    int pass = 1;
    unsigned char* pBytes = NULL;
    uint8_t cBytes = 0;
    int result = assemble(0x5205, input, &pBytes, &cBytes); // base address for branches
    printf("Assemble result %d\n", result);    
    if (pBytes != NULL)
    {
        if (cBytes != expectedBytesCount)
        {
            printf("Incorrect # of bytes assembled: %s, expected %d but got %d\n", input, expectedBytesCount, cBytes);
            pass = 0;
        }
        else
        {
            for (int i=0;i<cBytes;i++)
            {
                printf("%02X ", pBytes[i]);
                if (pBytes[i] != expectedBytes[i])
                {
                    printf("Unexpected byte\n");
                    pass = 0;
                }
            }
            printf("\n");
        }
        free(pBytes);
    }
    else {
        printf("Test case failed: %s\n", input);
        pass = 0;
    }

    if (!pass)
    {
        g_testFailures++;
    }
    return pass;
}

/*
void RunTests()
{
    g_testFailures = 0;
    RunTestCase("LDA #$01", (unsigned char[]) { 0xA9, 0x01 }, 2);
    
    RunTestCase("STA $01", (unsigned char[]) { 0x85, 0x01 }, 2);
    RunTestCase("STA $01,X", (unsigned char[]) { 0x95, 0x01 }, 2);
    RunTestCase("STA $D020", (unsigned char[]) { 0x8d, 0x20, 0xd0 }, 3);
    RunTestCase("STA $2301,X", (unsigned char[]) { 0x9D, 0x01, 0x23 }, 3);
    RunTestCase("STA $2301,Y", (unsigned char[]) { 0x99, 0x01, 0x23 }, 3);
    RunTestCase("STA ($02,X)", (unsigned char[]) { 0x81, 0x02 }, 2);
    RunTestCase("STA ($02),Y", (unsigned char[]) { 0x91, 0x02 }, 2);

    RunTestCase("STX $02,Y", (unsigned char[]) { 0x96, 0x02 }, 2);

    RunTestCase("SEI", (unsigned char[]) { 0x78 }, 1);
    
    RunTestCase("BEQ $5200", (unsigned char[]) { 0xF0, 0xF9 }, 2);
    RunTestCase("BEQ $520F", (unsigned char[]) { 0xF0, 0x08 }, 2);

    printf("Test failures: %d\n", g_testFailures);
}
*/

/*
int main() {
    RunTests();
    return 0;
}
*/

