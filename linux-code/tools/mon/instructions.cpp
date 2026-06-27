/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "sysop_defines.h"
#include "instructions.h"


extern int is_breakpoint(uint16_t address);


uint8_t g_opcodeToAddressingMode[255];
Instruction* g_opcodeToInstructionPointer[255];

Instruction instructions[] = {
 { "ADC", {
	 { 0x69, Immediate},
	 { 0x65, ZeroPage},
	 { 0x75, ZeroPageX},
	 { 0x6D, Absolute},
	 { 0x7D, AbsoluteX},
	 { 0x79, AbsoluteY},
	 { 0x61, IndirectX},
	 { 0x71, IndirectY}
	}, 8},

 { "AND", {
	 { 0x29, Immediate},
	 { 0x25, ZeroPage},
	 { 0x35, ZeroPageX},
	 { 0x2D, Absolute},
	 { 0x3D, AbsoluteX},
	 { 0x39, AbsoluteY},
	 { 0x21, IndirectX},
	 { 0x31, IndirectY}
	}, 8},

 { "ASL", {
	 { 0x0A, Accumulator},
	 { 0x06, ZeroPage},
	 { 0x16, ZeroPageX},
	 { 0x0E, Absolute},
	 { 0x1E, AbsoluteX}
	}, 5},

 { "BCC", {
	 { 0x90, Relative}
	}, 1},

 { "BCS", {
	 { 0xB0, Relative}
	}, 1},

 { "BEQ", {
	 { 0xF0, Relative}
	}, 1},

 { "BIT", {
	 { 0x24, ZeroPage},
	 { 0x2C, Absolute}
	}, 2},

 { "BMI", {
	 { 0x30, Relative}
	}, 1},

 { "BNE", {
	 { 0xD0, Relative}
	}, 1},

 { "BPL", {
	 { 0x10, Relative}
	}, 1},

 { "BRK", {
	 { 0x00, Implied}
	}, 1},

 { "BVC", {
	 { 0x50, Relative}
	}, 1},

 { "BVS", {
	 { 0x70, Relative}
	}, 1},

 { "CLC", {
	 { 0x18, Implied}
	}, 1},

 { "CLD", {
	 { 0xD8, Implied}
	}, 1},

 { "CLI", {
	 { 0x58, Implied}
	}, 1},

 { "CLV", {
	 { 0xB8, Implied}
	}, 1},

 { "CMP", {
	 { 0xC9, Immediate},
	 { 0xC5, ZeroPage},
	 { 0xD5, ZeroPageX},
	 { 0xCD, Absolute},
	 { 0xDD, AbsoluteX},
	 { 0xD9, AbsoluteY},
	 { 0xC1, IndirectX},
	 { 0xD1, IndirectY}
	}, 8},

 { "CPX", {
	 { 0xE0, Immediate},
	 { 0xE4, ZeroPage},
	 { 0xEC, Absolute}
	}, 3},

 { "CPY", {
	 { 0xC0, Immediate},
	 { 0xC4, ZeroPage},
	 { 0xCC, Absolute}
	}, 3},

 { "DEC", {
	 { 0xC6, ZeroPage},
	 { 0xD6, ZeroPageX},
	 { 0xCE, Absolute},
	 { 0xDE, AbsoluteX}
	}, 4},

 { "DEX", {
	 { 0xCA, Implied}
	}, 1},

 { "DEY", {
	 { 0x88, Implied}
	}, 1},

 { "EOR", {
	 { 0x49, Immediate},
	 { 0x45, ZeroPage},
	 { 0x55, ZeroPageX},
	 { 0x4D, Absolute},
	 { 0x5D, AbsoluteX},
	 { 0x59, AbsoluteY},
	 { 0x41, IndirectX},
	 { 0x51, IndirectY}
	}, 8},

 { "INC", {
	 { 0xE6, ZeroPage},
	 { 0xF6, ZeroPageX},
	 { 0xEE, Absolute},
	 { 0xFE, AbsoluteX}
	}, 4},

 { "INX", {
	 { 0xE8, Implied}
	}, 1},

 { "INY", {
	 { 0xC8, Implied}
	}, 1},

 { "JMP", {
	 { 0x4C, Absolute},
	 { 0x6C, AbsoluteIndirect}
	}, 2},

 { "JSR", {
	 { 0x20, Absolute}
	}, 1},

 { "LDA", {
	 { 0xA9, Immediate},
	 { 0xA5, ZeroPage},
	 { 0xB5, ZeroPageX},
	 { 0xAD, Absolute},
	 { 0xBD, AbsoluteX},
	 { 0xB9, AbsoluteY},
	 { 0xA1, IndirectX},
	 { 0xB1, IndirectY}
	}, 8},

 { "LDX", {
	 { 0xA2, Immediate},
	 { 0xA6, ZeroPage},
	 { 0xB6, ZeroPageY},
	 { 0xAE, Absolute},
	 { 0xBE, AbsoluteY}
	}, 5},

 { "LAX", { // unofficial, TODO: finish the variations of this
	 { 0xAF, Absolute}
	}, 1},

{ "LDY", {
	 { 0xA0, Immediate},
	 { 0xA4, ZeroPage},
	 { 0xB4, ZeroPageX},
	 { 0xAC, Absolute},
	 { 0xBC, AbsoluteX}
	}, 5},

 { "LSR", {
	 { 0x4A, Accumulator},
	 { 0x46, ZeroPage},
	 { 0x56, ZeroPageX},
	 { 0x4E, Absolute},
	 { 0x5E, AbsoluteX}
	}, 5},

 { "NOP", {
	 { 0xEA, Implied}
	}, 1},

 { "ORA", {
	 { 0x09, Immediate},
	 { 0x05, ZeroPage},
	 { 0x15, ZeroPageX},
	 { 0x0D, Absolute},
	 { 0x1D, AbsoluteX},
	 { 0x19, AbsoluteY},
	 { 0x01, IndirectX},
	 { 0x11, IndirectY}
	}, 8},

 { "PHA", {
	 { 0x48, Implied}
	}, 1},

 { "PHP", {
	 { 0x08, Implied}
	}, 1},

 { "PLA", {
	 { 0x68, Implied}
	}, 1},

 { "PLP", {
	 { 0x28, Implied}
	}, 1},

 { "ROL", {
	 { 0x2A, Accumulator},
	 { 0x26, ZeroPage},
	 { 0x36, ZeroPageX},
	 { 0x2E, Absolute},
	 { 0x3E, AbsoluteX}
	}, 5},

 { "ROR", {
	 { 0x6A, Accumulator},
	 { 0x66, ZeroPage},
	 { 0x76, ZeroPageX},
	 { 0x6E, Absolute},
	 { 0x7E, AbsoluteX}
	}, 5},

 { "RTI", {
	 { 0x40, Implied}
	}, 1},

 { "RTS", {
	 { 0x60, Implied}
	}, 1},

 { "SBC", {
	 { 0xE9, Immediate},
	 { 0xE5, ZeroPage},
	 { 0xF5, ZeroPageX},
	 { 0xED, Absolute},
	 { 0xFD, AbsoluteX},
	 { 0xF9, AbsoluteY},
	 { 0xE1, IndirectX},
	 { 0xF1, IndirectY}
	}, 8},

 { "SEC", {
	 { 0x38, Implied}
	}, 1},

 { "SED", {
	 { 0xF8, Implied}
	}, 1},

 { "SEI", {
	 { 0x78, Implied}
	}, 1},

 { "STA", {
	 { 0x85, ZeroPage},
	 { 0x95, ZeroPageX},
	 { 0x8D, Absolute},
	 { 0x9D, AbsoluteX},
	 { 0x99, AbsoluteY},
	 { 0x81, IndirectX},
	 { 0x91, IndirectY}
	}, 7},

 { "STX", {
	 { 0x86, ZeroPage},
	 { 0x96, ZeroPageY},
	 { 0x8E, Absolute}
	}, 3},

 { "STY", {
	 { 0x84, ZeroPage},
	 { 0x94, ZeroPageX},
	 { 0x8C, Absolute}
	}, 3},

 { "TAX", {
	 { 0xAA, Implied}
	}, 1},

 { "TAY", {
	 { 0xA8, Implied}
	}, 1},

 { "TSX", {
	 { 0xBA, Implied}
	}, 1},

 { "TXA", {
	 { 0x8A, Implied}
	}, 1},

 { "TXS", {
	 { 0x9A, Implied}
	}, 1},

 { "TYA", {
	 { 0x98, Implied}
	}, 1}

};

void InitInstructionTables()
{
	int i;
	int j;
	Instruction* pIns;
	for (i=0;i<255;i++)
	{
		g_opcodeToInstructionPointer[i] = NULL;
		g_opcodeToAddressingMode[i] = 0;
	}

	for (i=0;i<INSTRUCTION_COUNT;i++)
	{
		pIns = &instructions[i];
	    for (j=0;j<pIns->opcode_count;j++)
		{
			g_opcodeToAddressingMode[pIns->opcodes[j].value] = pIns->opcodes[j].addrMode;
			g_opcodeToInstructionPointer[pIns->opcodes[j].value] = pIns;
		}
	}
}

int GetOpCodeForInstruction(const char* input, AddressingMode addrMode)
{
    Instruction* pIns = NULL;
    for (int i=0;i<INSTRUCTION_COUNT;i++)
    {
        pIns = &instructions[i];
        if (strcmp(pIns->Name, input) == 0)
        {
            for (int j=0;j<pIns->opcode_count;j++)
            {
			    if (pIns->opcodes[j].addrMode == addrMode)
                {
                    return pIns->opcodes[j].value;
                }
            }
        }
    }

    return -1; // not found; addressing mode probably not valid for instruction
}

void DumpInstructions()
{
	int i;
	int j;
	Instruction* pIns;
	for (i=0;i<INSTRUCTION_COUNT;i++)
	{
		pIns = &instructions[i];
	    for (j=0;j<pIns->opcode_count;j++)
		{
			printf("%s %02x %d\n", pIns->Name, pIns->opcodes[j].value, pIns->opcodes[j].addrMode);
		}
	}
}


int commentCount = 9;
CommentMatch comments[9] = 
{ 
    {"JSR $AB1E", " ; print string at (y, a)" },
    {"JSR $E544", " ; clear screen" },
    {"JSR $FD15", " ; Restore IO vectors"},
    {"JSR $FDA3", " ; IOINIT"},
    {"JSR $FF5B", " ; CINT: Init screen editor"},
    {"JSR $FFE4", " ; GETIN (get a byte)"},
    {"JSR $FFBA", " ; SETLFS (A=logical file, X=device #, Y=secondary address)"},
    {"JSR $FFBD", " ; SETNAM (A=length, X=low byte, Y=high byte)"},
    {"JSR $FFC0", " ; OPEN"}
};

void PrintComments(const char* line)
{
    char buffer[255];
    buffer[0] = '\0';

    for (int i=0;i<commentCount;i++)
    {
        if (strcmp(line, comments[i].match) == 0)
        {
            sprintf(buffer, GREEN "%s" RESET, comments[i].comment);
            break;
        }
    }

        /*
    if (strcmp(line, "JSR $E544") == 0)
    {
        sprintf(buffer, GREEN " ; clear screen" RESET);
    }
    */
    printf(buffer);
}

static int g_useColors = 1;
void EnableDisassemblerColors(int onOff)
{
    g_useColors = onOff;
}
// return # bytes used
void Disassemble(int print, uint16_t pc, uint16_t addressOfOpCode, uint8_t opcode, uint8_t* pOperands, uint16_t startOfOpCode, int count, int* pBytesUsed, int* pBytesNeeded, int cleanMode)
{
    int breakPointOpCode = is_breakpoint(addressOfOpCode);
    if (breakPointOpCode != -1)
    {
        opcode = (uint8_t)(breakPointOpCode & 0xFF);
        pOperands[startOfOpCode] = opcode;
    }
    *pBytesNeeded = 0;
	AddressingMode addressingMode = (AddressingMode)g_opcodeToAddressingMode[opcode];
	Instruction* pIns = g_opcodeToInstructionPointer[opcode];
	char buffer[128];
	int i = 0;

	if (pIns == NULL || addressingMode == 0)
	{
		sprintf(buffer, "???");
		*pBytesUsed = 1;
	}
	else 
	{

	switch(addressingMode)
	{
		case ZeroPage:
			*pBytesUsed = 2;
			sprintf(buffer, "%s $%02X", pIns->Name, pOperands[startOfOpCode + 1]);
			break;

		case ZeroPageX:
			*pBytesUsed = 2;
			sprintf(buffer, "%s $%02X,X", pIns->Name, pOperands[startOfOpCode + 1]);
			break;

		case ZeroPageY:
			*pBytesUsed = 2;
			sprintf(buffer, "%s $%02X,Y", pIns->Name, pOperands[startOfOpCode + 1]);
			break;

		case Absolute:
			*pBytesUsed = 3;
			sprintf(buffer, "%s $%02X%02X", pIns->Name, pOperands[startOfOpCode + 2], pOperands[startOfOpCode+1]);
			break;

		case AbsoluteIndirect:
			*pBytesUsed = 3;
			sprintf(buffer, "%s ($%02X%02X)", pIns->Name, pOperands[startOfOpCode + 2], pOperands[startOfOpCode+1]);
			break;

		case AbsoluteX:
			*pBytesUsed = 3;
			sprintf(buffer, "%s $%02X%02X,X", pIns->Name, pOperands[startOfOpCode + 2], pOperands[startOfOpCode+1]);
			break;

		case AbsoluteY:
			*pBytesUsed = 3;
			sprintf(buffer, "%s $%02X%02X,Y", pIns->Name, pOperands[startOfOpCode + 2], pOperands[startOfOpCode+1]);
			break;

		case IndirectX:
			*pBytesUsed = 2;
			sprintf(buffer, "%s ($%02X,X)", pIns->Name, pOperands[startOfOpCode + 1]);
			break;

		case IndirectY:
			*pBytesUsed = 2;
			sprintf(buffer, "%s ($%02X),Y", pIns->Name, pOperands[startOfOpCode + 1]);
			break;

		case Immediate:
			*pBytesUsed = 2;
			sprintf(buffer, "%s #$%02X", pIns->Name, pOperands[startOfOpCode + 1]);
			break;

		case Accumulator:
			*pBytesUsed = 1;
			sprintf(buffer, "%s", pIns->Name);
			break;

		case Relative:
			{
				int8_t offset = (int8_t)pOperands[startOfOpCode + 1];
			    *pBytesUsed = 2;
			    sprintf(buffer, "%s $%04X", pIns->Name, (uint16_t)(addressOfOpCode + 2 + offset));
			}
			break;

		case Implied:
			*pBytesUsed = 1;
			sprintf(buffer, "%s", pIns->Name);
			break;
		default:
			*pBytesUsed = 1;
			sprintf(buffer, "DC.B $%02X\n", opcode);
			break;
	}
	}


    if (print > 0)
    {
        if (!cleanMode)
        {
            if (startOfOpCode + *pBytesUsed > count)
            {
                //printf("not enough data to fully decode\n");
                *pBytesNeeded = (startOfOpCode + *pBytesUsed) - count;
                *pBytesUsed -= *pBytesNeeded;
                printf("...(%d, %d)\n", *pBytesNeeded, *pBytesUsed);
                return;
            }
            if (print == 1)
            {
                if (is_breakpoint(addressOfOpCode) != -1)
                {
                    printf("*");
                }
                else if (pc == addressOfOpCode)
                {
                    printf(">");
                }
                else
                {
                    printf(" ");
                }
            }

            if (g_useColors) {
				printf(GREEN "%04X: " RESET, addressOfOpCode);
			}
			else {
				printf("%04X: ", addressOfOpCode);
			}

            for (i=0;i<*pBytesUsed;i++)
            {
                if ((startOfOpCode+i) >= count)
                {
                    break;
                }

                if (i > 0)
                        printf(" ");

                if (g_useColors) {
					printf(BLUE "%02X" RESET, pOperands[startOfOpCode+i]);
				}
				else {
					printf("%02X", pOperands[startOfOpCode+i]);
				}
            }
            for (i=0;i<(10-(*pBytesUsed*3));i++)
            {
                printf(" ");
            }
        }
        printf(buffer);
        PrintComments(buffer);
        printf("\n");
    }
}
