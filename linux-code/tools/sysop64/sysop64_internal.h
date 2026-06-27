/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#ifndef SYSOP64_INTERNAL_H
#define SYSOP64_INTERNAL_H

#include <error.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <memory.h>
#include <sys/epoll.h>
#include <linux/input.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <signal.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <list>
#include <queue>
#include <glib.h>
#include <cassert>
#include <pty.h>
#include <math.h>

#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>

#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#include <sys/inotify.h>

#include "sysop64.h"

#define KEYBOARD_EVENT_FILE "/dev/input/by-id/usb-Corsair_CORSAIR_K70_RGB_MK.2_LOW_PROFILE_Mechanical_Gaming_Keyboard_0302700AAF8D0500608A125AF5001BC2-event-kbd"
#define MOUSE_EVENT_FILE "/dev/input/mice"
#define JOYSTICK_EVENT_FILE "/dev/input/eventZ"

#define MAX_EVENTS 3

#define MAX_LINES 10000
#define MAX_VISIBLE_LINES 33
#define LINE_WIDTH 255

#define MEM_ADDRESS1 0x20000000
#define MEM_ADDRESS2 0x207e9000
#define MEM_SIZE (1024*1024*100)

#define LOG_LEVEL 0
#define LOG(...) \
    do { \
        if (LOG_LEVEL > 0) { \
            printf(__VA_ARGS__); \
        } \
    } while (0)

typedef struct {
    std::vector<std::string> lines;
    bool redraw_needed[MAX_VISIBLE_LINES];
    bool redraw_all;
} CircularBuffer;

struct QueuedMessage {
    uint8_t msg_type;
    std::string message;
    long long timeout_ms;
};

extern int framebuffer_update_needed;
extern int framebuffer_message_display_updated_needed;
extern int term_rows;
extern int term_cols;

extern int console_button_down;
extern int c64_console_active;
extern int console_yield_lock;
extern int console_reacquire_lock;
extern int console_yield_occurred;
extern int console_close_requested;
extern int message_display_close_requested;

extern int framebuffer_visible;
extern int framebuffer_message_display_visible;

extern unsigned char* pFrameBuffer;
extern unsigned char* pFrameBuffer1;
extern unsigned char* pFrameBuffer2;

extern cairo_t* g_cr;
extern cairo_t* g_cr1;
extern cairo_t* g_cr2;

extern PangoLayout* g_layout;
extern PangoLayout* g_layout1;
extern PangoLayout* g_layout2;

extern int g_framebuffer_width;
extern int g_framebuffer_height;
extern int font_size;

extern int g_mouse_x;
extern int g_mouse_y;
extern int g_prev_mouse_x;
extern int g_prev_mouse_y;

extern int g_cursor_style;

extern CircularBuffer line_buffer;
extern CircularBuffer alt_line_buffer;
extern CircularBuffer* current_line_buffer;

extern int inotify_fd;
extern int current_keyboard_fd;
extern char current_keyboard_path[256];

extern int c64_ctrl_pressed;
extern int c64_shift_pressed;
extern int ctrl_pressed;
extern int shift_pressed;

extern int message_display_show_requested;
extern std::queue<QueuedMessage> g_message_queue;
extern pthread_mutex_t g_message_queue_mutex;

extern struct timespec g_last_update;

extern int cursor_row;
extern int cursor_col;
extern int scroll_top;

extern int previous_cursor_x;
extern int previous_cursor_y;
extern int previous_cursor_width;
extern int previous_cursor_height;

void set_raw_mode(int fd);
void error_exit(const char *msg);

ushort map_c64_key(ushort c64_key);
void scan_keys_pipe(int fd);
char map_key_with_c64_mapping(struct input_event *ev);
char map_key(struct input_event *ev);
char map_key_old(struct input_event *ev);
void init_key_mapping_tables();

void shift_lines_up(std::vector<std::string>& lines, size_t start, size_t end);
void shift_lines_down(std::vector<std::string>& lines, size_t start, size_t end);

void init_buffer(CircularBuffer* buffer);
void process_buffer(const char* buffer, int n, char** ppResponseBytes, int& nResponseBytes);
void render_lines(cairo_t* cr, int width, int height);

void sendSignalToProcessesWithSameSID(pid_t pid, int signal);
void toggle_ui_visibility();
void toggle_message_display_ui_visibility();
void handleMessageDisplay();
void handle_console_button();
void handleConsoleCloseRequest();

void clear_framebuffer();
void draw_mouse(cairo_t* cr);
void erase_mouse(cairo_t* cr);
void update_framebuffer();

void init_keyboard_monitor();
void cleanup_keyboard_monitor();
void process_inotify_events();

int pty_loop();

int initialize_lock();
void console_acquire_lock();
void console_release_lock();
void *handleClient(void *arg);
void *handleAccept(void *arg);

#endif
