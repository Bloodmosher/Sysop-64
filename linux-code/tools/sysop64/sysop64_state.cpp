/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 *
 * sysop64_state.cpp
 *
 * Global state definitions for the sysop64 application. Declares and
 * initializes all shared variables used across the module: double-buffered
 * framebuffer pointers, Cairo rendering contexts, Pango layouts, cursor
 * position, terminal dimensions, console/message-display flags, and mouse
 * coordinates. Also provides two small utility functions: set_raw_mode()
 * and error_exit().
 */

#include "sysop64_internal.h"

int framebuffer_update_needed = 0;
int framebuffer_message_display_updated_needed = 0;

int term_rows = MAX_VISIBLE_LINES-1;
int term_cols = 106;

int console_button_down = 0;
int c64_console_active = 0;
int console_yield_lock = 0;
int console_reacquire_lock = 0;
int console_yield_occurred = 0;
int console_close_requested = 0;
int message_display_close_requested = 0;

int framebuffer_visible = 0;
int framebuffer_message_display_visible = 0;

unsigned char* pFrameBuffer = NULL;
unsigned char* pFrameBuffer1 = NULL;
unsigned char* pFrameBuffer2 = NULL;

cairo_t* g_cr = NULL;
cairo_t* g_cr1 = NULL;
cairo_t* g_cr2 = NULL;

PangoLayout* g_layout = NULL;
PangoLayout* g_layout1 = NULL;
PangoLayout* g_layout2 = NULL;

int g_framebuffer_width = 1920;
int g_framebuffer_height = 1080;
int font_size = 30;

int g_mouse_x = 0;
int g_mouse_y = 0;
int g_prev_mouse_x = 0;
int g_prev_mouse_y = 0;

int g_cursor_style = 1;

// Sets a terminal file descriptor to raw (non-canonical, no-echo) mode so
// that input is forwarded byte-for-byte without line buffering or processing.
// Sets a terminal file descriptor to raw (non-canonical, no-echo) mode so
// that input is forwarded byte-for-byte without line buffering or processing.
void set_raw_mode(int fd)
{
    struct termios tty;
    if (tcgetattr(fd, &tty) == -1) {
        perror("tcgetattr");
        exit(EXIT_FAILURE);
    }

    tty.c_lflag &= ~(ICANON | ECHO | ISIG);
    tty.c_iflag &= ~(IXON | ICRNL);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) == -1) {
        perror("tcsetattr");
        exit(EXIT_FAILURE);
    }
}

// Prints a perror message for msg then terminates the process. Used as a
// simple fatal-error handler throughout the application.
// Prints a perror message for msg then terminates the process. Used as a
// simple fatal-error handler throughout the application.
void error_exit(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}
