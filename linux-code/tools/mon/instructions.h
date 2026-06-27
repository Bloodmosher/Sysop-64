/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#ifndef INSTRUCTIONS_H
#define INSTRUCTIONS_H

//ANSI color code definitions
#define RED     "\x1b[31m"
#define GREEN   "\x1b[32m"
#define YELLOW  "\x1b[33m"
#define BLUE    "\x1b[34m"
#define LIGHT_GRAY "\x1b[37m"
#define RESET   "\x1b[0m"

typedef enum tagAddressingMode
{
    Invalid = 0,
	ZeroPage = 1,
    ZeroPageX = 2,
	ZeroPageY = 3,
	Absolute = 4,
	AbsoluteX = 5,
	AbsoluteY = 6,
	IndirectX = 7,
	IndirectY = 8,
	Implied = 9,
	Immediate = 10,
	Accumulator = 11,
	Relative = 12,
	AbsoluteIndirect = 13
} AddressingMode;

typedef struct tagOpCode 
{
	uint8_t  value;
	AddressingMode addrMode;
} OpCode;

typedef struct tagInstruction
{
	const char* Name;
	OpCode opcodes[10]; // max
	uint8_t opcode_count;
} Instruction;

#define INSTRUCTION_COUNT 57
extern Instruction instructions[];

extern uint8_t g_opcodeToAddressingMode[255];
extern Instruction* g_opcodeToInstructionPointer[255];


typedef struct tagCommentMatch
{
    const char* match;
    const char* comment;
} CommentMatch;


void Disassemble(int print, uint16_t pc, uint16_t addressOfOpCode, uint8_t opcode, uint8_t* pOperands, uint16_t startOfOpCode, int count, int* pBytesUsed, int* pBytesNeeded, int cleanMode);


int GetOpCodeForInstruction(const char* input, AddressingMode addrMode);

void InitInstructionTables();

#endif /* INSTRUCTIONS_H */
