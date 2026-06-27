/*
 * Sysop-64 
 * https://github.com/Bloodmosher/Sysop-64 
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project 
 *
 * sysop64_input_mapping.cpp
 *
 * Host keyboard input translation: converts Linux evdev input_event structs
 * to ASCII/control characters for writing to the PTY master.
 *
 * Provides two translation paths:
 *   map_key_with_c64_mapping() - legacy if-chain translator used in C64
 *       console mode, where Ctrl is the C64 CTRL key behaviour.
 *   map_key() / init_key_mapping_tables() - fast lookup-table translator
 *       used for normal terminal input.
 *
 * Also contains shift_lines_up() / shift_lines_down() helpers used by the
 * terminal parser when the shell sends scroll-region commands.
 */

#include "sysop64_internal.h"
// Shifts lines[start..end-1] up by one, overwriting lines[start] with
// lines[start+1] and so on. Fills the vacated bottom line with spaces.
void shift_lines_up(std::vector<std::string>& lines, size_t start, size_t end) {
    if (lines.empty() || start >= end || end >= lines.size()) return;

    // Shift up from top to bottom
    for (size_t i = start; i < end; ++i) {
        lines[i] = lines[i + 1];
    }

    // Clear the bottom line of the range
    lines[end] = std::string(term_cols, ' ');
    //lines[end].clear();
}

// Shifts lines[start+1..end] down by one, overwriting lines[end] with
// lines[end-1] and so on. Fills the vacated top line with spaces.
void shift_lines_down(std::vector<std::string>& lines, size_t start, size_t end) {
    if (lines.empty() || start >= end || end >= lines.size()) return;

    // Shift down from bottom to top
    for (size_t i = end; i > start; --i) {
        lines[i] = lines[i - 1];
    }

    // Clear the top line of the range
    //lines[start].clear();
    lines[start] = std::string(term_cols, ' ');
}


//


CircularBuffer line_buffer;
CircularBuffer alt_line_buffer;
CircularBuffer* current_line_buffer = &line_buffer;


int c64_ctrl_pressed = 0;
int c64_shift_pressed = 0;

// Translates a Linux input_event to an ASCII character using C64-style
// modifier semantics (C64 CTRL key tracked separately from normal Ctrl).
// Used when the sysop64 console is in C64 keyboard mode. Returns 0 for
// modifier-only events or unrecognised keys.
char map_key_with_c64_mapping(struct input_event *ev) {
        //printf("type: %d\n", ev->type);
        //printf("Key Pressed: %s\n", libevdev_event_code_get_name(ev->type, ev->code));
    if (ev->type == EV_KEY) {

        if (ev->code == KEY_LEFTCTRL || ev->code == KEY_RIGHTCTRL) {
            // Control key state has changed
            c64_ctrl_pressed = (ev->value == 1 || ev->value == 2);  // 1 for pressed, 0 for released - 2 is hold?
            //printf("Setting ctrl_pressed to %d\n", ctrl_pressed);
        } 
        else if (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT) {
            c64_shift_pressed = (ev->value == 1 || ev->value == 2);  // 1 for pressed, 0 for released - 2 is hold?
        } 
        else if (ev->value == 1)
        {        
            if (c64_ctrl_pressed)
            {
                if (ev->code == KEY_A) return 'a';
                else if (ev->code == KEY_L) return 'l';
                else if (ev->code == KEY_S) return 's';
                else if (ev->code == KEY_B) return 'b';
                else if (ev->code == KEY_C) return 0x3; // ^c
                else if (ev->code == KEY_D) return 'd';
                else if (ev->code == KEY_E) return 'e';
                else if (ev->code == KEY_F) return 'f';
                else if (ev->code == KEY_G) return 'g';
                else if (ev->code == KEY_H) return 'h';
                else if (ev->code == KEY_I) return 'i';
                else if (ev->code == KEY_J) return 'j';
                else if (ev->code == KEY_K) return 'k';
                else if (ev->code == KEY_L) return 'l';
                else if (ev->code == KEY_M) return 'm';
                else if (ev->code == KEY_N) return 'n';
                else if (ev->code == KEY_O) return 'o';
                else if (ev->code == KEY_P) return 'p';
                else if (ev->code == KEY_Q) return 'q';
                else if (ev->code == KEY_R) return 'r';
                else if (ev->code == KEY_S) return 's';
                else if (ev->code == KEY_T) return 't';
                else if (ev->code == KEY_U) return 'u';
                else if (ev->code == KEY_V) return 'v';
                else if (ev->code == KEY_W) return 'w';
                else if (ev->code == KEY_X) return 'x';
                else if (ev->code == KEY_Y) return 'y';
                else if (ev->code == KEY_Z) return 'z';        
                else if (ev->code == KEY_SPACE) return ' ';        
                else if (ev->code == KEY_MINUS) return '-';        
                else if (ev->code == KEY_ENTER) return '\n';
            }
            else if (c64_shift_pressed)
            {
                if (ev->code == KEY_A) return 'A';
                else if (ev->code == KEY_B) return 'B';
                else if (ev->code == KEY_C) return 'C';
                else if (ev->code == KEY_D) return 'D';
                else if (ev->code == KEY_E) return 'E';
                else if (ev->code == KEY_F) return 'F';
                else if (ev->code == KEY_G) return 'G';
                else if (ev->code == KEY_H) return 'H';
                else if (ev->code == KEY_I) return 'I';
                else if (ev->code == KEY_J) return 'J';
                else if (ev->code == KEY_K) return 'K';
                else if (ev->code == KEY_L) return 'L';
                else if (ev->code == KEY_M) return 'M';
                else if (ev->code == KEY_N) return 'N';
                else if (ev->code == KEY_O) return 'O';
                else if (ev->code == KEY_P) return 'P';
                else if (ev->code == KEY_Q) return 'Q';
                else if (ev->code == KEY_R) return 'R';
                else if (ev->code == KEY_S) return 'S';
                else if (ev->code == KEY_T) return 'T';
                else if (ev->code == KEY_U) return 'U';
                else if (ev->code == KEY_V) return 'V';
                else if (ev->code == KEY_W) return 'W';
                else if (ev->code == KEY_X) return 'X';
                else if (ev->code == KEY_Y) return 'Y';
                else if (ev->code == KEY_Z) return 'Z';        
                else if (ev->code == KEY_SPACE) return ' ';        
                else if (ev->code == KEY_MINUS) return '_';        
                else if (ev->code == KEY_ENTER) return '\n';
                else if (ev->code == KEY_DOT) return '>';
                else if (ev->code == KEY_SLASH) return '?';
                else if (ev->code == KEY_BACKSLASH) return '|';
                else if (ev->code == KEY_1) return '!';
                else if (ev->code == KEY_2) return '"';
                else if (ev->code == KEY_3) return '#';
                else if (ev->code == KEY_4) return '$';
                else if (ev->code == KEY_5) return '%';
                else if (ev->code == KEY_6) return '^';
                else if (ev->code == KEY_7) return '&';
                else if (ev->code == KEY_8) return '*';
                else if (ev->code == KEY_9) return '(';
                else if (ev->code == KEY_0) return ')';            
                else if (ev->code == KEY_APOSTROPHE) return '"';
                else if (ev->code == KEY_SEMICOLON) return ':';
                else if (ev->code == KEY_EQUAL) return '+';
                //else if (ev->code == KEY_UP) 
            }
            else
            {
                if (ev->code == KEY_A) return 'a';
                else if (ev->code == KEY_L) return 'l';
                else if (ev->code == KEY_S) return 's';
                else if (ev->code == KEY_B) return 'b';
                else if (ev->code == KEY_C) return 'c';
                else if (ev->code == KEY_D) return 'd';
                else if (ev->code == KEY_E) return 'e';
                else if (ev->code == KEY_F) return 'f';
                else if (ev->code == KEY_G) return 'g';
                else if (ev->code == KEY_H) return 'h';
                else if (ev->code == KEY_I) return 'i';
                else if (ev->code == KEY_J) return 'j';
                else if (ev->code == KEY_K) return 'k';
                else if (ev->code == KEY_L) return 'l';
                else if (ev->code == KEY_M) return 'm';
                else if (ev->code == KEY_N) return 'n';
                else if (ev->code == KEY_O) return 'o';
                else if (ev->code == KEY_P) return 'p';
                else if (ev->code == KEY_Q) return 'q';
                else if (ev->code == KEY_R) return 'r';
                else if (ev->code == KEY_S) return 's';
                else if (ev->code == KEY_T) return 't';
                else if (ev->code == KEY_U) return 'u';
                else if (ev->code == KEY_V) return 'v';
                else if (ev->code == KEY_W) return 'w';
                else if (ev->code == KEY_X) return 'x';
                else if (ev->code == KEY_Y) return 'y';
                else if (ev->code == KEY_Z) return 'z';        
                else if (ev->code == KEY_SPACE) return ' ';        
                else if (ev->code == KEY_MINUS) return '-';        
                else if (ev->code == KEY_ENTER) return '\n';
                else if (ev->code == KEY_DOT) return '.';
                else if (ev->code == KEY_SLASH) return '/';
                else if (ev->code == KEY_BACKSLASH) return '\\';
                else if (ev->code == KEY_ESC) return 27;
                else if (ev->code == KEY_TAB) return 9;
                else if (ev->code == KEY_1) return '1';
                else if (ev->code == KEY_2) return '2';
                else if (ev->code == KEY_3) return '3';
                else if (ev->code == KEY_4) return '4';
                else if (ev->code == KEY_5) return '5';
                else if (ev->code == KEY_6) return '6';
                else if (ev->code == KEY_7) return '7';
                else if (ev->code == KEY_8) return '8';
                else if (ev->code == KEY_9) return '9';
                else if (ev->code == KEY_0) return '0';
                else if (ev->code == KEY_APOSTROPHE) return '\'';
                else if (ev->code == KEY_SEMICOLON) return ';';
                else if (ev->code == KEY_EQUAL) return '=';
                //else if (ev->code == KEY_LEFTBRACE) return '[';
                //else if (ev->code == KEY_RIGHTBRACE) return ']';
                //else if (ev->code == KEY_BACKSPACE) return 8; // 127? 8?
                //else if (ev->code == KEY_BACKSPACE) return 127; // 8?;
            }
        }

    }    
    return 0;
}


int ctrl_pressed = 0;
int shift_pressed = 0;

static char ctrl_map[KEY_CNT];
static char shift_map[KEY_CNT];
static char normal_map[KEY_CNT];

static int key_maps_initialized = 0;
// Populates the ctrl_map[], shift_map[], and normal_map[] lookup tables.
// Call once at startup before using map_key(). Idempotent.
void init_key_mapping_tables()
{
    if (key_maps_initialized)
        return;

    key_maps_initialized = 1;

    ctrl_map[KEY_A] = 'a';
    ctrl_map[KEY_B] = 'b';
    ctrl_map[KEY_C] = 0x03;
    ctrl_map[KEY_D] = 'd';
    ctrl_map[KEY_E] = 'e';
    ctrl_map[KEY_F] = 'f';
    ctrl_map[KEY_G] = 'g';
    ctrl_map[KEY_H] = 'h';
    ctrl_map[KEY_I] = 'i';
    ctrl_map[KEY_J] = 'j';
    ctrl_map[KEY_K] = 'k';
    ctrl_map[KEY_L] = 'l';
    ctrl_map[KEY_M] = 'm';
    ctrl_map[KEY_N] = 'n';
    ctrl_map[KEY_O] = 'o';
    ctrl_map[KEY_P] = 'p';
    ctrl_map[KEY_Q] = 'q';
    ctrl_map[KEY_R] = 'r';
    ctrl_map[KEY_S] = 's';
    ctrl_map[KEY_T] = 't';
    ctrl_map[KEY_U] = 'u';
    ctrl_map[KEY_V] = 'v';
    ctrl_map[KEY_W] = 'w';
    ctrl_map[KEY_X] = 'x';
    ctrl_map[KEY_Y] = 'y';
    ctrl_map[KEY_Z] = 'z';
    ctrl_map[KEY_SPACE] = ' ';
    ctrl_map[KEY_MINUS] = '-';
    ctrl_map[KEY_ENTER] = '\n';

    shift_map[KEY_A] = 'A';
    shift_map[KEY_B] = 'B';
    shift_map[KEY_C] = 'C';
    shift_map[KEY_D] = 'D';
    shift_map[KEY_E] = 'E';
    shift_map[KEY_F] = 'F';
    shift_map[KEY_G] = 'G';
    shift_map[KEY_H] = 'H';
    shift_map[KEY_I] = 'I';
    shift_map[KEY_J] = 'J';
    shift_map[KEY_K] = 'K';
    shift_map[KEY_L] = 'L';
    shift_map[KEY_M] = 'M';
    shift_map[KEY_N] = 'N';
    shift_map[KEY_O] = 'O';
    shift_map[KEY_P] = 'P';
    shift_map[KEY_Q] = 'Q';
    shift_map[KEY_R] = 'R';
    shift_map[KEY_S] = 'S';
    shift_map[KEY_T] = 'T';
    shift_map[KEY_U] = 'U';
    shift_map[KEY_V] = 'V';
    shift_map[KEY_W] = 'W';
    shift_map[KEY_X] = 'X';
    shift_map[KEY_Y] = 'Y';
    shift_map[KEY_Z] = 'Z';

    shift_map[KEY_SPACE]      = ' ';
    shift_map[KEY_MINUS]      = '_';
    shift_map[KEY_ENTER]      = '\n';
    shift_map[KEY_DOT]        = '>';
    shift_map[KEY_SLASH]      = '?';
    shift_map[KEY_BACKSLASH]  = '|';
    shift_map[KEY_1]          = '!';
    shift_map[KEY_2]          = '@';
    shift_map[KEY_3]          = '#';
    shift_map[KEY_4]          = '$';
    shift_map[KEY_5]          = '%';
    shift_map[KEY_6]          = '^';
    shift_map[KEY_7]          = '&';
    shift_map[KEY_8]          = '*';
    shift_map[KEY_9]          = '(';
    shift_map[KEY_0]          = ')';
    shift_map[KEY_APOSTROPHE] = '"';
    shift_map[KEY_SEMICOLON]  = ':';
    shift_map[KEY_EQUAL]      = '+';
    shift_map[KEY_GRAVE]      = '~';

    normal_map[KEY_A] = 'a';
    normal_map[KEY_B] = 'b';
    normal_map[KEY_C] = 'c';
    normal_map[KEY_D] = 'd';
    normal_map[KEY_E] = 'e';
    normal_map[KEY_F] = 'f';
    normal_map[KEY_G] = 'g';
    normal_map[KEY_H] = 'h';
    normal_map[KEY_I] = 'i';
    normal_map[KEY_J] = 'j';
    normal_map[KEY_K] = 'k';
    normal_map[KEY_L] = 'l';
    normal_map[KEY_M] = 'm';
    normal_map[KEY_N] = 'n';
    normal_map[KEY_O] = 'o';
    normal_map[KEY_P] = 'p';
    normal_map[KEY_Q] = 'q';
    normal_map[KEY_R] = 'r';
    normal_map[KEY_S] = 's';
    normal_map[KEY_T] = 't';
    normal_map[KEY_U] = 'u';
    normal_map[KEY_V] = 'v';
    normal_map[KEY_W] = 'w';
    normal_map[KEY_X] = 'x';
    normal_map[KEY_Y] = 'y';
    normal_map[KEY_Z] = 'z';

    normal_map[KEY_SPACE]      = ' ';
    normal_map[KEY_MINUS]      = '-';
    normal_map[KEY_ENTER]      = '\n';
    normal_map[KEY_DOT]        = '.';
    normal_map[KEY_SLASH]      = '/';
    normal_map[KEY_BACKSLASH]  = '\\';
    normal_map[KEY_ESC]        = 27;
    normal_map[KEY_TAB]        = 9;
    normal_map[KEY_1]          = '1';
    normal_map[KEY_2]          = '2';
    normal_map[KEY_3]          = '3';
    normal_map[KEY_4]          = '4';
    normal_map[KEY_5]          = '5';
    normal_map[KEY_6]          = '6';
    normal_map[KEY_7]          = '7';
    normal_map[KEY_8]          = '8';
    normal_map[KEY_9]          = '9';
    normal_map[KEY_0]          = '0';
    normal_map[KEY_APOSTROPHE] = '\'';
    normal_map[KEY_SEMICOLON]  = ';';
    normal_map[KEY_EQUAL]      = '=';
    normal_map[KEY_GRAVE]      = '`';
}

// Translates a Linux input_event to an ASCII character using the pre-built
// lookup tables. Tracks Ctrl and Shift state from modifier key events.
// Returns 0 for modifier-only events or unrecognised keys.
char map_key(struct input_event *ev) {

    if (ev->type == EV_KEY) {
        if (ev->code == KEY_LEFTCTRL || ev->code == KEY_RIGHTCTRL) {
            ctrl_pressed = (ev->value == 1 || ev->value == 2);
        } else if (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT) {
            shift_pressed = (ev->value == 1 || ev->value == 2);
        } else if (ev->value == 1) {
            if (ctrl_pressed) {
                if (ev->code < KEY_CNT)
                    return ctrl_map[ev->code];
            } else if (shift_pressed) {
                if (ev->code < KEY_CNT)
                    return shift_map[ev->code];
            } else {
                if (ev->code < KEY_CNT)
                    return normal_map[ev->code];
            }
        }
    }
    return 0;
}

char map_key_old(struct input_event *ev) {
        //printf("type: %d\n", ev->type);
        //printf("Key Pressed: %s\n", libevdev_event_code_get_name(ev->type, ev->code));
    if (ev->type == EV_KEY) {

        if (ev->code == KEY_LEFTCTRL || ev->code == KEY_RIGHTCTRL) {
            // Control key state has changed
            ctrl_pressed = (ev->value == 1 || ev->value == 2);  // 1 for pressed, 0 for released - 2 is hold?
            //printf("Setting ctrl_pressed to %d\n", ctrl_pressed);
        } 
        else if (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT) {
            shift_pressed = (ev->value == 1 || ev->value == 2);  // 1 for pressed, 0 for released - 2 is hold?
        } 
        else if (ev->value == 1)
        {        
            if (ctrl_pressed)
            {
                if (ev->code == KEY_A) return 'a';
                else if (ev->code == KEY_L) return 'l';
                else if (ev->code == KEY_S) return 's';
                else if (ev->code == KEY_B) return 'b';
                else if (ev->code == KEY_C) return 0x3; // ^c
                else if (ev->code == KEY_D) return 'd';
                else if (ev->code == KEY_E) return 'e';
                else if (ev->code == KEY_F) return 'f';
                else if (ev->code == KEY_G) return 'g';
                else if (ev->code == KEY_H) return 'h';
                else if (ev->code == KEY_I) return 'i';
                else if (ev->code == KEY_J) return 'j';
                else if (ev->code == KEY_K) return 'k';
                else if (ev->code == KEY_L) return 'l';
                else if (ev->code == KEY_M) return 'm';
                else if (ev->code == KEY_N) return 'n';
                else if (ev->code == KEY_O) return 'o';
                else if (ev->code == KEY_P) return 'p';
                else if (ev->code == KEY_Q) return 'q';
                else if (ev->code == KEY_R) return 'r';
                else if (ev->code == KEY_S) return 's';
                else if (ev->code == KEY_T) return 't';
                else if (ev->code == KEY_U) return 'u';
                else if (ev->code == KEY_V) return 'v';
                else if (ev->code == KEY_W) return 'w';
                else if (ev->code == KEY_X) return 'x';
                else if (ev->code == KEY_Y) return 'y';
                else if (ev->code == KEY_Z) return 'z';        
                else if (ev->code == KEY_SPACE) return ' ';        
                else if (ev->code == KEY_MINUS) return '-';        
                else if (ev->code == KEY_ENTER) return '\n';
            }
            else if (shift_pressed)
            {
                if (ev->code == KEY_A) return 'A';
                else if (ev->code == KEY_B) return 'B';
                else if (ev->code == KEY_C) return 'C';
                else if (ev->code == KEY_D) return 'D';
                else if (ev->code == KEY_E) return 'E';
                else if (ev->code == KEY_F) return 'F';
                else if (ev->code == KEY_G) return 'G';
                else if (ev->code == KEY_H) return 'H';
                else if (ev->code == KEY_I) return 'I';
                else if (ev->code == KEY_J) return 'J';
                else if (ev->code == KEY_K) return 'K';
                else if (ev->code == KEY_L) return 'L';
                else if (ev->code == KEY_M) return 'M';
                else if (ev->code == KEY_N) return 'N';
                else if (ev->code == KEY_O) return 'O';
                else if (ev->code == KEY_P) return 'P';
                else if (ev->code == KEY_Q) return 'Q';
                else if (ev->code == KEY_R) return 'R';
                else if (ev->code == KEY_S) return 'S';
                else if (ev->code == KEY_T) return 'T';
                else if (ev->code == KEY_U) return 'U';
                else if (ev->code == KEY_V) return 'V';
                else if (ev->code == KEY_W) return 'W';
                else if (ev->code == KEY_X) return 'X';
                else if (ev->code == KEY_Y) return 'Y';
                else if (ev->code == KEY_Z) return 'Z';        
                else if (ev->code == KEY_SPACE) return ' ';        
                else if (ev->code == KEY_MINUS) return '_';        
                else if (ev->code == KEY_ENTER) return '\n';
                else if (ev->code == KEY_DOT) return '>';
                else if (ev->code == KEY_SLASH) return '?';
                else if (ev->code == KEY_BACKSLASH) return '|';
                else if (ev->code == KEY_1) return '!';
                else if (ev->code == KEY_2) return '@';
                else if (ev->code == KEY_3) return '#';
                else if (ev->code == KEY_4) return '$';
                else if (ev->code == KEY_5) return '%';
                else if (ev->code == KEY_6) return '^';
                else if (ev->code == KEY_7) return '&';
                else if (ev->code == KEY_8) return '*';
                else if (ev->code == KEY_9) return '(';
                else if (ev->code == KEY_0) return ')';            
                else if (ev->code == KEY_APOSTROPHE) return '"';
                else if (ev->code == KEY_SEMICOLON) return ':';
                else if (ev->code == KEY_EQUAL) return '+';
                else if (ev->code == KEY_GRAVE) return '~';
                //else if (ev->code == KEY_UP) 
            }
            else
            {
                if (ev->code == KEY_A) return 'a';
                else if (ev->code == KEY_L) return 'l';
                else if (ev->code == KEY_S) return 's';
                else if (ev->code == KEY_B) return 'b';
                else if (ev->code == KEY_C) return 'c';
                else if (ev->code == KEY_D) return 'd';
                else if (ev->code == KEY_E) return 'e';
                else if (ev->code == KEY_F) return 'f';
                else if (ev->code == KEY_G) return 'g';
                else if (ev->code == KEY_H) return 'h';
                else if (ev->code == KEY_I) return 'i';
                else if (ev->code == KEY_J) return 'j';
                else if (ev->code == KEY_K) return 'k';
                else if (ev->code == KEY_L) return 'l';
                else if (ev->code == KEY_M) return 'm';
                else if (ev->code == KEY_N) return 'n';
                else if (ev->code == KEY_O) return 'o';
                else if (ev->code == KEY_P) return 'p';
                else if (ev->code == KEY_Q) return 'q';
                else if (ev->code == KEY_R) return 'r';
                else if (ev->code == KEY_S) return 's';
                else if (ev->code == KEY_T) return 't';
                else if (ev->code == KEY_U) return 'u';
                else if (ev->code == KEY_V) return 'v';
                else if (ev->code == KEY_W) return 'w';
                else if (ev->code == KEY_X) return 'x';
                else if (ev->code == KEY_Y) return 'y';
                else if (ev->code == KEY_Z) return 'z';        
                else if (ev->code == KEY_SPACE) return ' ';        
                else if (ev->code == KEY_MINUS) return '-';        
                else if (ev->code == KEY_ENTER) return '\n';
                else if (ev->code == KEY_DOT) return '.';
                else if (ev->code == KEY_SLASH) return '/';
                else if (ev->code == KEY_BACKSLASH) return '\\';
                else if (ev->code == KEY_ESC) return 27;
                else if (ev->code == KEY_TAB) return 9;
                else if (ev->code == KEY_1) return '1';
                else if (ev->code == KEY_2) return '2';
                else if (ev->code == KEY_3) return '3';
                else if (ev->code == KEY_4) return '4';
                else if (ev->code == KEY_5) return '5';
                else if (ev->code == KEY_6) return '6';
                else if (ev->code == KEY_7) return '7';
                else if (ev->code == KEY_8) return '8';
                else if (ev->code == KEY_9) return '9';
                else if (ev->code == KEY_0) return '0';
                else if (ev->code == KEY_APOSTROPHE) return '\'';
                else if (ev->code == KEY_SEMICOLON) return ';';
                else if (ev->code == KEY_EQUAL) return '=';
                else if (ev->code == KEY_GRAVE) return '`';
                //else if (ev->code == KEY_LEFTBRACE) return '[';
                //else if (ev->code == KEY_RIGHTBRACE) return ']';
                //else if (ev->code == KEY_BACKSPACE) return 8; // 127? 8?
                //else if (ev->code == KEY_BACKSPACE) return 127; // 8?;
            }
        }

    }    
    return 0;
}

