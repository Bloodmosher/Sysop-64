/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * mon_input.h — line editor with command history and terminal management.
 */

#pragma once

#include <termios.h>

// Saved terminal state (used by reset_term and sigintHandler)
extern struct termios oldt;
extern int old_flags;

// Configure stdin for raw, non-blocking, no-echo input.
void setup_raw_terminal(void);

// Restore the terminal to the state saved by setup_raw_terminal.
void reset_term(void);

// SIGINT handler: restores terminal and exits.
void sigintHandler(int signal);

// Read one line of input into buf (at most len-1 characters, always NUL-terminated).
// Supports arrow-key navigation through command history and backspace editing.
// Returns buf on success, or NULL if the user submitted an empty line (just Enter).
char *get_line(char *buf, int len);

// Block until the user presses Enter, then return.  Safe to call while stdin
// is in non-blocking raw mode (temporarily restores blocking for the duration).
void wait_for_enter(void);
