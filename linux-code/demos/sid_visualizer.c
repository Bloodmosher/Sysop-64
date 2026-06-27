/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * sid_visualizer.c — Real-time SID voice waveform visualizer at 1920×1080.
 *
 * Overview
 * --------
 * A sampler thread reads all three SID voice outputs via
 * sysop_read_sid_voices_data() at 48 kHz and stores them in a 10-second
 * ring buffer.  The main render loop reads that buffer every HDMI vblank
 * and draws anti-aliased waveform traces for each voice using direct pixel
 * writes to the Sysop-64 dual framebuffer.
 *
 * An optional mouse thread reads raw evdev events from the first USB mouse
 * found in /dev/input and provides:
 *   - On-screen pointer with optional PNG cursor image
 *   - A "Settings" menu bar (toggle waveforms, debug grid, transparency,
 *     exit) that auto-hides when the mouse leaves the top strip
 *   - Mouse-wheel scrolling of the SID-Wizard pattern editor and instrument
 *     selection
 *   - Left-click mapping to SID-Wizard editor cursor positions via
 *     sidwiz_ui_mapper (enabled with --ui-map)
 *   - Mouse hotplug detection via inotify on /dev/input
 *
 * Command-line usage
 * ------------------
 *   sid_visualizer [<zoom>] [--mouse] [--ui-map] [--ui-map-debug]
 *                  [--mouse-image <path.png>]
 *
 *   zoom           Floating-point multiplier (default 1.0).  Controls how
 *                  many audio samples are visible across the screen width.
 *                  1.0 ≈ one video frame's worth of samples.  Values up to
 *                  600.0 are accepted.
 *   --mouse        Enable mouse pointer and menu overlay.
 *   --ui-map       Enable mouse + SID-Wizard UI click mapping.
 *   --ui-map-debug Enable mouse + UI mapping + visible region outlines.
 *   --mouse-image  Load a custom PNG image as the mouse pointer.
 *
 * Build dependencies: Cairo (-lcairo), pthreads (-lpthread), libsysop64.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <cairo/cairo.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <memory.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <dirent.h>
#include <limits.h>
#include <sys/inotify.h>
#include <linux/input.h>
#include "sysop64.h"
#include "sidwiz_ui_mapper.h"

/* ── Embedded resources ─────────────────────────────────────────────────── */
/* mouse_pointer.png is linked as a binary object via:                      */
/*   ld -r -b binary mouse_pointer.png -o mouse_pointer.o                   */
extern "C" {
    extern const unsigned char _binary_mouse_pointer_png_start[];
    extern const unsigned char _binary_mouse_pointer_png_end[];
}

/* ── Configuration ─────────────────────────────────────────────────────── */

#define SCREEN_WIDTH  1920
#define SCREEN_HEIGHT 1080
#define NUM_VOICES    3
#define SAMPLE_RATE   48000
#define HISTORY_SIZE  (SAMPLE_RATE * 10)   /* 10 seconds of ring buffer */
#define SID_BASE_ADDR 0xD400

/* Pre-render at least this many frames of samples before starting display */
#define MIN_BUFFER_AHEAD_FRAMES 1
#define MAX_BUFFER_AHEAD_FRAMES 5

/* Measured frame rate of the HDMI output in Hz */
#define ACTUAL_FRAME_RATE 50.124

/* Mouse tracking */
#define DEV_INPUT_DIR   "/dev/input"
#define MOUSE_POINTER_SIZE 24

/* Frame render budget (nanoseconds) — ~20 ms for 50 Hz display */
#define FRAME_BUDGET_NS (20000000L)

/* Menu bar geometry */
#define MENU_BAR_HEIGHT  32
#define MENU_ITEM_HEIGHT 24
#define MENU_ITEM_WIDTH  200

/* ── Forward declarations ───────────────────────────────────────────────── */

void sigintHandler(int signal);

/* ── Types ──────────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t freq;
    uint16_t pw;
    uint8_t  ctrl;
    uint8_t  ad;
    uint8_t  sr;
} SIDVoiceRegs;

typedef struct {
    SIDVoiceRegs v[3];
    uint16_t cutoff;
    uint8_t  res_filt;
    uint8_t  mode_vol;
} SIDRegisters;

typedef struct {
    double   phase;
    double   value;
    uint32_t lfsr;
} OscState;

typedef struct {
    float           buffer[NUM_VOICES][HISTORY_SIZE];
    int             head;
    pthread_mutex_t mutex;
    bool            sampler_running;
    int             total_samples_written;
} AudioHistory;

typedef struct {
    int             x, y;
    bool            visible;
    int             fd;
    char            path[PATH_MAX];
    pthread_mutex_t mutex;
    bool            thread_running;
    cairo_surface_t *image_surface;
    int             image_width, image_height;
    bool            left_button_down;
    bool            left_button_was_down;
} MouseState;

typedef struct {
    bool menu_bar_visible;
    bool settings_dropdown_open;
    bool visualize_waveforms;
    bool show_debug_grid;
    bool transparent_bg;
    int  hover_item;   /* -1 = none, 0+ = menu item index */
} MenuState;

/* ── Globals ────────────────────────────────────────────────────────────── */

/* Pixel colors — ARGB8888.  COLOR_BG is mutable (alpha adjusted via wheel) */
uint32_t       COLOR_BG      = 0x00000000;  /* transparent by default */
const uint32_t COLOR_GRID    = 0xFF222233;
const uint32_t COLOR_VOICE_1 = 0xFF55FFFF;  /* cyan    */
const uint32_t COLOR_VOICE_2 = 0xFFFF55FF;  /* magenta */
const uint32_t COLOR_VOICE_3 = 0xFFFFFF55;  /* yellow  */

SIDRegisters sid_regs;
OscState     osc_states[NUM_VOICES];
AudioHistory history;

uint32_t *raw_buffer_A;
uint32_t *raw_buffer_B;

MouseState mouse_state = {
    SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2,
    false, -1, "", PTHREAD_MUTEX_INITIALIZER, false,
    NULL, 0, 0, false, false
};
MenuState menu_state = { false, false, false, false, true, -1 };

bool enable_mouse      = false;
bool enable_ui_mapping = false;
bool enable_ui_debug   = false;

/* SID-Wizard C64 memory addresses used by the interaction layer */
uint16_t g_sidwiz_addr_selinst_plus_1              = 0xb6b;
uint16_t g_sidwiz_addr_instrument_refresh_needed   = 0xb3f;
uint16_t g_sidwiz_editor_pattern_lengths_table     = 0x1f83;
uint16_t g_sidwiz_editor_draw_pattern_lengths_table = 0x1fe8;

/* ── Sample conversion ──────────────────────────────────────────────────── */

static float convert_sample_to_float(uint16_t raw)
{
    int16_t s = (int16_t)raw;
    return ((float)s / 32768.0f) * 6.0f;
}

static float get_sample(uint64_t data, int voice_idx)
{
    switch (voice_idx) {
    case 0: return convert_sample_to_float((uint16_t)(data & 0xFFFF));
    case 1: return convert_sample_to_float((uint16_t)((data >> 16) & 0xFFFF));
    case 2: return convert_sample_to_float((uint16_t)((data >> 32) & 0xFFFF));
    default: return 0.0f;
    }
}

/* ── Sampler thread ─────────────────────────────────────────────────────── */

void *sampler_thread_func(void *arg)
{
    long ns_per_sample = 1000000000L / SAMPLE_RATE;

    struct timespec next_sample_time;
    clock_gettime(CLOCK_MONOTONIC, &next_sample_time);

    printf("Sampler thread started: %ld ns per sample (%.3f us)\n",
           ns_per_sample, ns_per_sample / 1000.0);

    while (true) {
        pthread_mutex_lock(&history.mutex);
        if (!history.sampler_running) {
            pthread_mutex_unlock(&history.mutex);
            break;
        }
        pthread_mutex_unlock(&history.mutex);

        uint64_t sid_voices_data = sysop_read_sid_voices_data();

        pthread_mutex_lock(&history.mutex);
        history.head = (history.head + 1) % HISTORY_SIZE;
        for (int v = 0; v < NUM_VOICES; v++)
            history.buffer[v][history.head] = get_sample(sid_voices_data, v);
        history.total_samples_written++;
        pthread_mutex_unlock(&history.mutex);

        next_sample_time.tv_nsec += ns_per_sample;
        if (next_sample_time.tv_nsec >= 1000000000L) {
            next_sample_time.tv_sec++;
            next_sample_time.tv_nsec -= 1000000000L;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_sample_time, NULL);
    }

    printf("Sampler thread stopped\n");
    return NULL;
}

static void wait_for_initial_samples(int min_samples)
{
    printf("Waiting for %d samples to be collected...\n", min_samples);
    while (true) {
        pthread_mutex_lock(&history.mutex);
        int total = history.total_samples_written;
        bool running = history.sampler_running;
        pthread_mutex_unlock(&history.mutex);

        if (total >= min_samples) {
            printf("Initial samples collected: %d\n", total);
            break;
        }
        if (!running) {
            printf("Sampler stopped before initial buffer filled\n");
            break;
        }
        struct timespec t = { 0, 10000000 };
        nanosleep(&t, NULL);
    }
}

/* ── Graphics primitives ────────────────────────────────────────────────── */

static void draw_line(uint32_t *buffer, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    while (1) {
        if (x0 >= 0 && x0 < SCREEN_WIDTH && y0 >= 0 && y0 < SCREEN_HEIGHT)
            buffer[y0 * SCREEN_WIDTH + x0] = color;
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static inline uint32_t blend_color(uint32_t bg, uint32_t fg, float alpha)
{
    if (alpha <= 0.0f) return bg;
    if (alpha >= 1.0f) return fg;

    uint8_t fa = (fg >> 24) & 0xFF, fr = (fg >> 16) & 0xFF;
    uint8_t fg_ = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    uint8_t ba = (bg >> 24) & 0xFF, br = (bg >> 16) & 0xFF;
    uint8_t bg_ = (bg >> 8) & 0xFF, bb = bg & 0xFF;

    return (uint32_t)(((uint8_t)(fa * alpha + ba * (1.0f - alpha))) << 24) |
           (uint32_t)(((uint8_t)(fr * alpha + br * (1.0f - alpha))) << 16) |
           (uint32_t)(((uint8_t)(fg_ * alpha + bg_ * (1.0f - alpha))) << 8) |
           (uint32_t)  (uint8_t)(fb  * alpha + bb  * (1.0f - alpha));
}

static inline void plot_pixel_aa(uint32_t *buffer, int x, int y,
                                  uint32_t color, float alpha)
{
    if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        int idx = y * SCREEN_WIDTH + x;
        buffer[idx] = blend_color(buffer[idx], color, alpha);
    }
}

/* Xiaolin Wu anti-aliased line */
static void draw_line_aa(uint32_t *buffer, int x0, int y0, int x1, int y1,
                          uint32_t color)
{
    auto fpart  = [](float x) -> float { return x - floorf(x); };
    auto rfpart = [&fpart](float x) -> float { return 1.0f - fpart(x); };

    float dx = (float)(x1 - x0);
    float dy = (float)(y1 - y0);

    if (fabsf(dx) < 0.5f && fabsf(dy) < 0.5f) {
        plot_pixel_aa(buffer, x0, y0, color, 1.0f);
        return;
    }

    bool steep = fabsf(dy) > fabsf(dx);
    if (steep) {
        int t; t = x0; x0 = y0; y0 = t;
               t = x1; x1 = y1; y1 = t;
        float ft; ft = dx; dx = dy; dy = ft;
    }
    if (x0 > x1) {
        int t; t = x0; x0 = x1; x1 = t;
               t = y0; y0 = y1; y1 = t;
    }

    float gradient = (x1 - x0) < 0.5f ? 1.0f : dy / dx;

    float xend = roundf((float)x0);
    float yend = (float)y0 + gradient * (xend - (float)x0);
    float xgap = rfpart((float)x0 + 0.5f);
    int xpxl1 = (int)xend, ypxl1 = (int)floorf(yend);
    if (steep) {
        plot_pixel_aa(buffer, ypxl1,     xpxl1, color, rfpart(yend) * xgap);
        plot_pixel_aa(buffer, ypxl1 + 1, xpxl1, color, fpart(yend)  * xgap);
    } else {
        plot_pixel_aa(buffer, xpxl1, ypxl1,     color, rfpart(yend) * xgap);
        plot_pixel_aa(buffer, xpxl1, ypxl1 + 1, color, fpart(yend)  * xgap);
    }
    float intery = yend + gradient;

    xend = roundf((float)x1);
    yend = (float)y1 + gradient * (xend - (float)x1);
    xgap = fpart((float)x1 + 0.5f);
    int xpxl2 = (int)xend, ypxl2 = (int)floorf(yend);
    if (steep) {
        plot_pixel_aa(buffer, ypxl2,     xpxl2, color, rfpart(yend) * xgap);
        plot_pixel_aa(buffer, ypxl2 + 1, xpxl2, color, fpart(yend)  * xgap);
    } else {
        plot_pixel_aa(buffer, xpxl2, ypxl2,     color, rfpart(yend) * xgap);
        plot_pixel_aa(buffer, xpxl2, ypxl2 + 1, color, fpart(yend)  * xgap);
    }

    if (steep) {
        for (int x = xpxl1 + 1; x < xpxl2; x++) {
            int y = (int)floorf(intery);
            plot_pixel_aa(buffer, y,     x, color, rfpart(intery));
            plot_pixel_aa(buffer, y + 1, x, color, fpart(intery));
            intery += gradient;
        }
    } else {
        for (int x = xpxl1 + 1; x < xpxl2; x++) {
            int y = (int)floorf(intery);
            plot_pixel_aa(buffer, x, y,     color, rfpart(intery));
            plot_pixel_aa(buffer, x, y + 1, color, fpart(intery));
            intery += gradient;
        }
    }
}

static void clear_buffer(uint32_t *buffer, uint32_t color)
{
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++)
        buffer[i] = color;
}

/* ── Mouse input ─────────────────────────────────────────────────────────── */

/* Cairo read-callback for loading a PNG from an in-memory buffer */
typedef struct {
    const unsigned char *data;
    size_t               pos;
    size_t               size;
} PngMemStream;

static cairo_status_t png_mem_read(void *closure, unsigned char *data, unsigned int length)
{
    PngMemStream *s = (PngMemStream *)closure;
    if (s->pos + length > s->size) return CAIRO_STATUS_READ_ERROR;
    memcpy(data, s->data + s->pos, length);
    s->pos += length;
    return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *load_embedded_mouse_png(void)
{
    PngMemStream stream = {
        _binary_mouse_pointer_png_start,
        0,
        (size_t)(_binary_mouse_pointer_png_end - _binary_mouse_pointer_png_start)
    };
    return cairo_image_surface_create_from_png_stream(png_mem_read, &stream);
}

static int test_bit(const unsigned long *bits, int bit)
{
    return bits[bit / (8 * sizeof(unsigned long))] &
           (1UL << (bit % (8 * sizeof(unsigned long))));
}

static int looks_like_mouse(int fd, const char *debug_path)
{
    unsigned long evbits[(EV_MAX  + 1) / (8 * sizeof(unsigned long))];
    unsigned long keybits[(KEY_MAX + 1) / (8 * sizeof(unsigned long))];
    unsigned long relbits[(REL_MAX + 1) / (8 * sizeof(unsigned long))];
    unsigned long absbits[(ABS_MAX + 1) / (8 * sizeof(unsigned long))];

    memset(evbits,  0, sizeof(evbits));
    memset(keybits, 0, sizeof(keybits));
    memset(relbits, 0, sizeof(relbits));
    memset(absbits, 0, sizeof(absbits));

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
        if (enable_mouse) printf("  %s: ioctl EVIOCGBIT failed\n", debug_path);
        return 0;
    }

    int has_ev_key = test_bit(evbits, EV_KEY);
    int has_ev_rel = test_bit(evbits, EV_REL);
    int has_ev_abs = test_bit(evbits, EV_ABS);

    if (has_ev_key) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
    if (has_ev_rel) ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits);
    if (has_ev_abs) ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);

    int has_rel_x     = test_bit(relbits, REL_X);
    int has_rel_y     = test_bit(relbits, REL_Y);
    int has_btn_left  = test_bit(keybits, BTN_LEFT);
    int has_btn_mouse = test_bit(keybits, BTN_MOUSE);
    int has_gamepad   = test_bit(keybits, BTN_GAMEPAD) || test_bit(keybits, BTN_SOUTH);

    if (enable_mouse) {
        char name[256] = {0};
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        printf("  %s (%s): EV_KEY=%d EV_REL=%d EV_ABS=%d BTN_LEFT=%d\n",
               debug_path, name[0] ? name : "Unknown",
               has_ev_key, has_ev_rel, has_ev_abs, has_btn_left);
    }

    if (!has_gamepad && has_ev_rel && (has_btn_left || has_btn_mouse))
        return 1;
    if (has_rel_x && has_rel_y && !has_gamepad)
        return 1;
    return 0;
}

static int open_first_mouse(char *out_path, size_t out_sz)
{
    DIR *d = opendir(DEV_INPUT_DIR);
    if (!d) {
        if (enable_mouse) perror("opendir " DEV_INPUT_DIR);
        return -1;
    }
    if (enable_mouse) printf("Scanning for mouse devices in %s:\n", DEV_INPUT_DIR);

    int   best_fd = -1;
    char  best_path[PATH_MAX] = {0};
    struct dirent *de;

    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), DEV_INPUT_DIR "/%s", de->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        if (looks_like_mouse(fd, path)) {
            best_fd = fd;
            strncpy(best_path, path, sizeof(best_path) - 1);
            char name[256] = {0};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            printf("Found mouse: %s (%s)\n", path, name[0] ? name : "Unknown");
            break;
        }
        close(fd);
    }
    closedir(d);

    if (best_fd < 0 && enable_mouse) printf("No suitable mouse device found.\n");

    if (best_fd >= 0 && out_path) {
        strncpy(out_path, best_path, out_sz - 1);
        out_path[out_sz - 1] = '\0';
    }
    return best_fd;
}

static void draw_mouse_pointer(uint32_t *buffer, int x, int y)
{
    if (!mouse_state.visible) return;

    /* Custom PNG cursor */
    if (mouse_state.image_surface) {
        unsigned char *img = cairo_image_surface_get_data(mouse_state.image_surface);
        int stride = cairo_image_surface_get_stride(mouse_state.image_surface);
        int w = mouse_state.image_width, h = mouse_state.image_height;

        for (int dy = 0; dy < h; dy++) {
            int py = y + dy;
            if (py < 0 || py >= SCREEN_HEIGHT) continue;
            uint32_t *row = (uint32_t *)(img + dy * stride);
            for (int dx = 0; dx < w; dx++) {
                int px = x + dx;
                if (px < 0 || px >= SCREEN_WIDTH) continue;
                uint32_t src = row[dx];
                uint8_t a = (src >> 24) & 0xFF;
                if (a == 0) continue;
                if (a == 255) {
                    buffer[py * SCREEN_WIDTH + px] = src;
                } else {
                    /* Unpremultiply Cairo premultiplied alpha */
                    uint8_t r = (uint8_t)(((src >> 16) & 0xFF) * 255 / a);
                    uint8_t g = (uint8_t)(((src >>  8) & 0xFF) * 255 / a);
                    uint8_t b = (uint8_t)((src & 0xFF) * 255 / a);
                    buffer[py * SCREEN_WIDTH + px] = ((uint32_t)a << 24) |
                                                     ((uint32_t)r << 16) |
                                                     ((uint32_t)g <<  8) | b;
                }
            }
        }
        return;
    }

    /* Fallback: simple arrow */
    const uint32_t C_FG = 0xFFFFFFFF, C_BG = 0xFF000000;
    int half = MOUSE_POINTER_SIZE / 2;
    for (int dy = -half; dy < half; dy++) {
        int len = half - abs(dy);
        for (int dx = 0; dx < len; dx++) {
            int px = x + dx, py = y + dy;
            if (px < 0 || px >= SCREEN_WIDTH || py < 0 || py >= SCREEN_HEIGHT) continue;
            uint32_t c = (dx == 0 || dx == len - 1 || abs(dy) == half - 1) ? C_BG : C_FG;
            buffer[py * SCREEN_WIDTH + px] = c;
        }
    }
}

/* ── UI debug overlay ───────────────────────────────────────────────────── */

static void draw_ui_debug_grid(uint32_t *buffer)
{
    if (!enable_ui_debug && !menu_state.show_debug_grid) return;

    const uint32_t gc = 0xFFFF0000;  /* solid red */

    auto draw_rect = [&](Rect r) {
        for (int x = r.x1; x <= r.x2; x++) {
            if (x >= 0 && x < SCREEN_WIDTH) {
                if (r.y1 >= 0 && r.y1 < SCREEN_HEIGHT) buffer[r.y1 * SCREEN_WIDTH + x] = gc;
                if (r.y2 >= 0 && r.y2 < SCREEN_HEIGHT) buffer[r.y2 * SCREEN_WIDTH + x] = gc;
            }
        }
        for (int y = r.y1; y <= r.y2; y++) {
            if (y >= 0 && y < SCREEN_HEIGHT) {
                if (r.x1 >= 0 && r.x1 < SCREEN_WIDTH) buffer[y * SCREEN_WIDTH + r.x1] = gc;
                if (r.x2 >= 0 && r.x2 < SCREEN_WIDTH) buffer[y * SCREEN_WIDTH + r.x2] = gc;
            }
        }
    };

    for (int t = 0; t < 3; t++) draw_rect(get_pattern_track_rect(t));
    draw_rect(get_orderlist_rect());
    draw_rect(get_instrument_main_rect());
    draw_rect(get_wfarp_rect());
    draw_rect(get_pulse_rect());
    draw_rect(get_filter_rect());
    draw_rect(get_chord_rect());
    draw_rect(get_tempo_rect());
}

/* ── Menu system ────────────────────────────────────────────────────────── */

static void draw_menu(uint32_t *buffer, int mouse_x, int mouse_y)
{
    if (!enable_mouse) return;

    /* Auto-show when mouse is near top or dropdown is open */
    menu_state.menu_bar_visible = (mouse_y < MENU_BAR_HEIGHT ||
                                   menu_state.settings_dropdown_open);
    if (!menu_state.menu_bar_visible) return;

    const uint32_t C_BAR    = 0xFF2A2A2A;
    const uint32_t C_TEXT   = 0xFFE0E0E0;
    const uint32_t C_HOVER  = 0xFF404040;
    const uint32_t C_BORDER = 0xFF505050;

    /* Menu bar background */
    for (int y = 0; y < MENU_BAR_HEIGHT; y++)
        for (int x = 0; x < SCREEN_WIDTH; x++)
            buffer[y * SCREEN_WIDTH + x] = C_BAR;

    /* "Settings" button */
    int btn_x = 10, btn_w = 80;
    bool btn_hover = (mouse_x >= btn_x && mouse_x < btn_x + btn_w && mouse_y < MENU_BAR_HEIGHT);
    if (btn_hover || menu_state.settings_dropdown_open)
        for (int y = 0; y < MENU_BAR_HEIGHT; y++)
            for (int x = btn_x; x < btn_x + btn_w; x++)
                buffer[y * SCREEN_WIDTH + x] = C_HOVER;

    /* Use Cairo to render text */
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                        SCREEN_WIDTH, SCREEN_HEIGHT);
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 0.875, 0.875, 0.875);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    cairo_move_to(cr, btn_x + 5, 20);
    cairo_show_text(cr, "Settings");

    if (menu_state.settings_dropdown_open) {
        int dx = btn_x, dy = MENU_BAR_HEIGHT;
        int dh = MENU_ITEM_HEIGHT * 4;

        /* Hover detection */
        menu_state.hover_item = -1;
        if (mouse_x >= dx && mouse_x < dx + MENU_ITEM_WIDTH &&
            mouse_y >= dy && mouse_y < dy + dh)
            menu_state.hover_item = (mouse_y - dy) / MENU_ITEM_HEIGHT;

        /* Item backgrounds */
        for (int y = dy; y < dy + dh; y++) {
            int item = (y - dy) / MENU_ITEM_HEIGHT;
            uint32_t c = (menu_state.hover_item == item) ? C_HOVER : C_BAR;
            for (int x = dx; x < dx + MENU_ITEM_WIDTH; x++)
                if (x >= 0 && x < SCREEN_WIDTH && y < SCREEN_HEIGHT)
                    buffer[y * SCREEN_WIDTH + x] = c;
        }

        /* Border */
        for (int x = dx; x < dx + MENU_ITEM_WIDTH; x++) {
            if (dy < SCREEN_HEIGHT)             buffer[dy       * SCREEN_WIDTH + x] = C_BORDER;
            if (dy + dh - 1 < SCREEN_HEIGHT)    buffer[(dy+dh-1)* SCREEN_WIDTH + x] = C_BORDER;
        }
        for (int y = dy; y < dy + dh; y++) {
            buffer[y * SCREEN_WIDTH + dx]                   = C_BORDER;
            buffer[y * SCREEN_WIDTH + dx + MENU_ITEM_WIDTH - 1] = C_BORDER;
        }

        /* Helper: draw a checkbox at (cx, cy) filled if 'checked' */
        auto draw_checkbox = [&](int cx, int cy, bool checked) {
            const int sz = 8;
            for (int i = 0; i < sz; i++) {
                if (cx+i >= 0 && cx+i < SCREEN_WIDTH) {
                    if (cy >= 0 && cy < SCREEN_HEIGHT)      buffer[cy       * SCREEN_WIDTH + cx+i] = C_TEXT;
                    if (cy+sz-1 < SCREEN_HEIGHT)            buffer[(cy+sz-1)* SCREEN_WIDTH + cx+i] = C_TEXT;
                }
                if (cy+i >= 0 && cy+i < SCREEN_HEIGHT) {
                    if (cx >= 0 && cx < SCREEN_WIDTH)        buffer[(cy+i)* SCREEN_WIDTH + cx]      = C_TEXT;
                    if (cx+sz-1 < SCREEN_WIDTH)              buffer[(cy+i)* SCREEN_WIDTH + cx+sz-1] = C_TEXT;
                }
            }
            if (checked)
                for (int i = 1; i < sz-1; i++)
                    for (int j = 1; j < sz-1; j++) {
                        int px = cx+i, py = cy+j;
                        if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT)
                            buffer[py * SCREEN_WIDTH + px] = C_TEXT;
                    }
        };

        /* The four menu items */
        struct { const char *label; bool *state; } items[3] = {
            { "Visualize waveforms", &menu_state.visualize_waveforms },
            { "Show debug grid",     &menu_state.show_debug_grid     },
            { "Transparent",         &menu_state.transparent_bg      },
        };
        for (int i = 0; i < 3; i++) {
            int cx = dx + 10, cy = dy + MENU_ITEM_HEIGHT * i + MENU_ITEM_HEIGHT/2 - 4;
            draw_checkbox(cx, cy, *items[i].state);
            cairo_move_to(cr, dx + 25, dy + MENU_ITEM_HEIGHT * i + 16);
            cairo_show_text(cr, items[i].label);
        }
        cairo_move_to(cr, dx + 10, dy + MENU_ITEM_HEIGHT * 3 + 16);
        cairo_show_text(cr, "Exit");
    }

    /* Composite Cairo text layer onto pixel buffer */
    cairo_surface_flush(surf);
    unsigned char *cdata  = cairo_image_surface_get_data(surf);
    int            stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        uint32_t *crow = (uint32_t *)(cdata + y * stride);
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            uint32_t cp = crow[x];
            uint8_t  a  = (cp >> 24) & 0xFF;
            if (a == 0) continue;
            if (a == 255) {
                buffer[y * SCREEN_WIDTH + x] = cp;
            } else {
                uint32_t bg = buffer[y * SCREEN_WIDTH + x];
                float    fa = a / 255.0f;
                uint8_t cr_ = (uint8_t)(((cp >> 16) & 0xFF) / fa);
                uint8_t cg  = (uint8_t)(((cp >>  8) & 0xFF) / fa);
                uint8_t cb  = (uint8_t)((cp & 0xFF)          / fa);
                uint8_t br  = (bg >> 16) & 0xFF;
                uint8_t bg_ = (bg >>  8) & 0xFF;
                uint8_t bb  = bg & 0xFF;
                buffer[y * SCREEN_WIDTH + x] = 0xFF000000 |
                    ((uint32_t)(uint8_t)(cr_ * fa + br * (1.0f - fa)) << 16) |
                    ((uint32_t)(uint8_t)(cg  * fa + bg_ * (1.0f - fa)) <<  8) |
                     (uint8_t)(cb  * fa + bb  * (1.0f - fa));
            }
        }
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
}

/* ── SID-Wizard interaction ─────────────────────────────────────────────── */

static void redraw_instruments(void)
{
    sysop_poke(g_sidwiz_addr_instrument_refresh_needed, 0x01);
}

/*
 * handle_mouse_wheel — responds to vertical scroll events.
 *
 *   mouse.x < 100   : adjust background overlay alpha
 *   curwind PATTERN  : scroll pattern editor first-visible row
 *   curwind INSTRUM  : change selected instrument
 */
static void handle_mouse_wheel(int delta)
{
    /* Left edge: adjust background alpha */
    if (mouse_state.x > 0 && mouse_state.x < 100) {
        uint8_t alpha = (COLOR_BG >> 24) & 0xFF;
        if (delta < 0 && alpha > 0)   alpha--;
        else if (delta > 0 && alpha < 255) alpha++;
        COLOR_BG = ((uint32_t)alpha << 24) | (COLOR_BG & 0x00FFFFFF);
        printf("Background alpha: %d\n", alpha);
        return;
    }

    uint8_t curwind = sysop_peek(0x343);

    if (curwind == CURWIND_PATTERN) {
        uint8_t track   = sysop_peek(PATTERN_EDITOR_SUBWINDOW_ADDR);
        uint8_t pattern = sysop_peek(0x340 + track);

        uint16_t length_address = g_sidwiz_editor_pattern_lengths_table + (pattern - 1);
        uint8_t  track_len      = sysop_peek(length_address) - 1;

        uint16_t first_vis_addr = 0x353 + track;
        uint8_t  first_vis_row  = sysop_peek(first_vis_addr);

        printf("Wheel: track=%d pattern=%01X len=%d first_vis=%d delta=%d\n",
               track, pattern, track_len, first_vis_row, delta);

        sysop_wait_vic2(0, 1);
        sysop_dma_enable();
        const int VISIBLE_ROWS = 24;
        if (delta < 0 && first_vis_row > 0)
            sysop_poke(first_vis_addr, first_vis_row - 1);
        else if (delta > 0 && first_vis_row < (uint8_t)(track_len - (VISIBLE_ROWS - 1)))
            sysop_poke(first_vis_addr, first_vis_row + 1);
        sysop_dma_disable();

    } else if (curwind == CURWIND_INSTRUM) {
        uint8_t inst = sysop_peek(g_sidwiz_addr_selinst_plus_1);
        printf("Wheel instrument: current=%d delta=%d\n", inst, delta);

        if (delta > 0 && inst < 0x25) inst++;
        else if (delta < 0 && inst > 1) inst--;

        sysop_wait_vic2(0, 1);
        sysop_dma_enable();
        sysop_poke(g_sidwiz_addr_selinst_plus_1, inst);
        redraw_instruments();
        sysop_dma_disable();
    }
}

/* ── Mouse thread ───────────────────────────────────────────────────────── */

void *mouse_thread_func(void *arg)
{
    while (true) {
        pthread_mutex_lock(&mouse_state.mutex);
        if (!mouse_state.thread_running) {
            pthread_mutex_unlock(&mouse_state.mutex);
            break;
        }
        int fd = mouse_state.fd;
        pthread_mutex_unlock(&mouse_state.mutex);

        if (fd < 0) { usleep(10000); continue; }

        struct input_event ev;
        ssize_t r = read(fd, &ev, sizeof(ev));

        if (r == sizeof(ev)) {
            if (ev.type == EV_REL) {
                pthread_mutex_lock(&mouse_state.mutex);
                if (ev.code == REL_X) {
                    mouse_state.x = (int)fmaxf(0.0f, fminf((float)(SCREEN_WIDTH  - 1), mouse_state.x + ev.value));
                } else if (ev.code == REL_Y) {
                    mouse_state.y = (int)fmaxf(0.0f, fminf((float)(SCREEN_HEIGHT - 1), mouse_state.y + ev.value));
                } else if (ev.code == REL_WHEEL) {
                    handle_mouse_wheel(ev.value);
                }
                pthread_mutex_unlock(&mouse_state.mutex);

            } else if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
                pthread_mutex_lock(&mouse_state.mutex);
                bool was_down = mouse_state.left_button_down;
                mouse_state.left_button_down = (ev.value != 0);
                bool pressed = mouse_state.left_button_down && !was_down;
                int click_x = mouse_state.x, click_y = mouse_state.y;
                pthread_mutex_unlock(&mouse_state.mutex);

                if (!pressed) continue;

                bool menu_handled = false;

                /* Settings button in menu bar */
                if (menu_state.menu_bar_visible && click_y < MENU_BAR_HEIGHT) {
                    if (click_x >= 10 && click_x < 90) {
                        menu_state.settings_dropdown_open = !menu_state.settings_dropdown_open;
                        printf("Settings menu %s\n",
                               menu_state.settings_dropdown_open ? "opened" : "closed");
                        menu_handled = true;
                    }
                /* Dropdown items */
                } else if (menu_state.settings_dropdown_open) {
                    int ddx = 10, ddy = MENU_BAR_HEIGHT, ddh = MENU_ITEM_HEIGHT * 4;
                    if (click_x >= ddx && click_x < ddx + MENU_ITEM_WIDTH &&
                        click_y >= ddy && click_y < ddy + ddh) {
                        int item = (click_y - ddy) / MENU_ITEM_HEIGHT;
                        if (item == 0) {
                            menu_state.visualize_waveforms = !menu_state.visualize_waveforms;
                            printf("Visualize waveforms: %s\n",
                                   menu_state.visualize_waveforms ? "on" : "off");
                        } else if (item == 1) {
                            menu_state.show_debug_grid = !menu_state.show_debug_grid;
                            printf("Debug grid: %s\n",
                                   menu_state.show_debug_grid ? "on" : "off");
                        } else if (item == 2) {
                            menu_state.transparent_bg = !menu_state.transparent_bg;
                            COLOR_BG = menu_state.transparent_bg ? 0x00000000 : 0xFF000000;
                            printf("Transparent background: %s\n",
                                   menu_state.transparent_bg ? "on" : "off");
                        } else if (item == 3) {
                            printf("Exit requested from menu\n");
                            sigintHandler(SIGINT);
                        }
                        menu_state.settings_dropdown_open = false;
                    } else {
                        menu_state.settings_dropdown_open = false;
                    }
                    menu_handled = true;
                }

                /* SID-Wizard UI click mapping */
                if (!menu_handled && enable_ui_mapping) {
                    UIAction action = map_mouse_to_ui(click_x, click_y);

                    printf("\n=== Click (%d, %d) => %s ===\n",
                           click_x, click_y, action.description);
                    printf("  curwind=$%02X\n", action.curwind);

                    if (action.row_address != 0 || action.column_address != 0) {
                        sysop_wait_vic2(0, 1);
                        sysop_dma_enable();
                    }
                    sysop_poke(0x343, action.curwind);
                    if (action.subwindow_address) sysop_poke(action.subwindow_address, action.subwindow_value);
                    if (action.row_address)        sysop_poke(action.row_address,       action.row_value);
                    if (action.column_address)     sysop_poke(action.column_address,    action.column_value);
                    if (action.row_address != 0 || action.column_address != 0)
                        sysop_dma_disable();
                }
            }
        } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        } else {
            usleep(1000);
        }
    }

    printf("Mouse thread stopped\n");
    return NULL;
}

/* ── Renderer ───────────────────────────────────────────────────────────── */

/*
 * render_frame_with_budget — clear the pixel buffer and draw waveforms,
 * the UI debug grid, the menu bar, and the mouse pointer.
 *
 * snapshot_head is the ring-buffer read index captured before this call so
 * that the sampler thread can continue writing without locking.
 * zoom_level controls how many samples are spread across the screen width.
 */
void render_frame_with_budget(uint32_t *buffer, float zoom_level,
                               long /*budget_ns*/, int snapshot_head)
{
    clear_buffer(buffer, COLOR_BG);

    const uint32_t voice_colors[3] = { COLOR_VOICE_1, COLOR_VOICE_2, COLOR_VOICE_3 };
    const int      amplitude_scale = 40;
    const int      voice_centers[3] = {
        SCREEN_HEIGHT / 4,
        SCREEN_HEIGHT / 2,
        (SCREEN_HEIGHT * 3) / 4
    };

    int samples_to_show = (int)((SAMPLE_RATE / ACTUAL_FRAME_RATE) * zoom_level);
    if (samples_to_show > HISTORY_SIZE) samples_to_show = HISTORY_SIZE;
    int sample_step = samples_to_show / SCREEN_WIDTH;
    if (sample_step < 1) sample_step = 1;

    static int frame_count = 0;
    if ((frame_count % 50) == 0)
        printf("Frame %d: head=%d samples_to_show=%d step=%d\n",
               frame_count, snapshot_head, samples_to_show, sample_step);
    frame_count++;

    /* Draw anti-aliased waveforms */
    if (menu_state.visualize_waveforms) {
        for (int v = 0; v < NUM_VOICES; v++) {
            int buf_idx = snapshot_head;
            int prev_x  = SCREEN_WIDTH - 1;
            int prev_y  = voice_centers[v] -
                          (int)(history.buffer[v][buf_idx] * amplitude_scale);

            for (int x = SCREEN_WIDTH - 2; x >= 0; x--) {
                buf_idx -= sample_step;
                if (buf_idx < 0) buf_idx += HISTORY_SIZE;

                float sample = history.buffer[v][buf_idx];
                int   y      = voice_centers[v] - (int)(sample * amplitude_scale);

                draw_line_aa(buffer, prev_x, prev_y, x, y, voice_colors[v]);
                prev_x = x;
                prev_y = y;
            }
        }
    }

    if (menu_state.show_debug_grid) draw_ui_debug_grid(buffer);

    if (enable_mouse) {
        pthread_mutex_lock(&mouse_state.mutex);
        int mx = mouse_state.x, my = mouse_state.y;
        pthread_mutex_unlock(&mouse_state.mutex);
        draw_menu(buffer, mx, my);
    }

    if (enable_mouse && mouse_state.visible) {
        pthread_mutex_lock(&mouse_state.mutex);
        int mx = mouse_state.x, my = mouse_state.y;
        pthread_mutex_unlock(&mouse_state.mutex);
        draw_mouse_pointer(buffer, mx, my);
    }
}

/* ── Framebuffer initialisation ─────────────────────────────────────────── */

#define MEM_ADDRESS1 0x20000000
#define MEM_ADDRESS2 0x207e9000
#define MEM_SIZE     (16 * 1024 * 1024)

static unsigned char *pFrameBuffer1;
static unsigned char *pFrameBuffer2;

static void init_sysop_buffers(void)
{
    sysop_init();
    if (sysop_server_connect() == -1) {
        printf("sysop_server_connect failed\n");
        exit(-1);
    }

    int fd = open("/dev/sysop-fb", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/sysop-fb"); exit(-1); }

    pFrameBuffer1 = (unsigned char *)mmap(NULL, MEM_SIZE,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, fd, 0);
    pFrameBuffer2 = (unsigned char *)mmap(NULL, MEM_SIZE,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, fd,
                                           MEM_ADDRESS2 - MEM_ADDRESS1);

    memset(pFrameBuffer1, 0, SCREEN_WIDTH * SCREEN_HEIGHT * 4);
    memset(pFrameBuffer2, 0, SCREEN_WIDTH * SCREEN_HEIGHT * 4);

    sysop_framebuffer_show();
}

/* ── Signal handler ─────────────────────────────────────────────────────── */

static pthread_t sampler_thread;
static pthread_t mouse_thread;

void sigintHandler(int sig)
{
    if (enable_mouse && mouse_state.thread_running) {
        pthread_mutex_lock(&mouse_state.mutex);
        mouse_state.thread_running = false;
        pthread_mutex_unlock(&mouse_state.mutex);
        pthread_join(mouse_thread, NULL);
    }
    if (mouse_state.fd >= 0) { close(mouse_state.fd); mouse_state.fd = -1; }
    if (mouse_state.image_surface) {
        cairo_surface_destroy(mouse_state.image_surface);
        mouse_state.image_surface = NULL;
    }

    printf("Stopping sampler thread...\n");
    pthread_mutex_lock(&history.mutex);
    history.sampler_running = false;
    pthread_mutex_unlock(&history.mutex);
    pthread_join(sampler_thread, NULL);
    pthread_mutex_destroy(&history.mutex);

    sysop_framebuffer_hide();
    sysop_framebuffer_unlock();
    sysop_uninit();
    exit(128 + sig);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    float zoom_level = 1.0f;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "--?") == 0) {
            printf(
                "Usage: sid_visualizer [options] [zoom]\n"
                "\n"
                "Real-time SID voice waveform visualizer at 1920x1080.\n"
                "\n"
                "Options:\n"
                "  --mouse          Enable mouse pointer and Settings menu overlay.\n"
                "  --ui-map         Enable mouse + SID-Wizard editor click mapping.\n"
                "  --ui-map-debug   Enable mouse + UI mapping + visible region outlines.\n"
                "  --help, --?      Show this help message and exit.\n"
                "\n"
                "Arguments:\n"
                "  zoom             Floating-point zoom multiplier (default 1.0).\n"
                "                   Controls how many audio samples span the screen width.\n"
                "                   1.0 ~ one video frame of samples; range 0.1 to 600.0.\n"
                "\n"
                "Mouse controls (when --mouse is active):\n"
                "  Move to top edge   Show Settings menu bar.\n"
                "  Left click         Toggle menu items or map cursor in SID-Wizard.\n"
                "  Scroll (x < 100)   Adjust background overlay alpha.\n"
                "  Scroll (pattern)   Scroll SID-Wizard pattern editor rows.\n"
                "  Scroll (instrument) Change selected SID-Wizard instrument.\n"
            );
            return 0;
        } else if (strcmp(argv[i], "--mouse") == 0) {
            enable_mouse = true;
            printf("Mouse support enabled\n");
        } else if (strcmp(argv[i], "--ui-map") == 0) {
            enable_mouse = enable_ui_mapping = true;
            printf("UI mapping enabled\n");
        } else if (strcmp(argv[i], "--ui-map-debug") == 0) {
            enable_mouse = enable_ui_mapping = enable_ui_debug = true;
            printf("UI mapping with debug grid enabled\n");
        } else {
            zoom_level = atof(argv[i]);
            if (zoom_level < 0.1f)  zoom_level = 0.1f;
            if (zoom_level > 600.0f) zoom_level = 600.0f;
            printf("Zoom: %.1fx (~%.2fs)\n", zoom_level, zoom_level / 60.0f);
        }
    }

    /* Without a mouse there is no Settings menu, so enable waveforms by default. */
    if (!enable_mouse)
        menu_state.visualize_waveforms = true;

    signal(SIGINT, sigintHandler);
    sysop_framebuffer_lock();
    init_sysop_buffers();

    raw_buffer_A = (uint32_t *)pFrameBuffer1;
    raw_buffer_B = (uint32_t *)pFrameBuffer2;
    if (!raw_buffer_A || !raw_buffer_B) {
        printf("Failed to map frame buffers\n");
        return 1;
    }

    pthread_mutex_init(&history.mutex, NULL);
    history.head = 0;
    history.total_samples_written = 0;
    history.sampler_running = true;
    if (pthread_create(&sampler_thread, NULL, sampler_thread_func, NULL) != 0) {
        printf("Failed to create sampler thread\n");
        return 1;
    }

    /* Mouse setup */
    int inofd = -1, wd = -1;
    if (enable_mouse) {
        inofd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inofd >= 0) {
            wd = inotify_add_watch(inofd, DEV_INPUT_DIR,
                                   IN_CREATE | IN_DELETE | IN_ATTRIB |
                                   IN_MOVED_TO | IN_MOVED_FROM);
            if (wd < 0) { close(inofd); inofd = -1; enable_mouse = false; }
        } else {
            enable_mouse = false;
        }

        if (enable_mouse) {
            mouse_state.fd = open_first_mouse(mouse_state.path, sizeof(mouse_state.path));
            if (mouse_state.fd >= 0) {
                mouse_state.visible = mouse_state.thread_running = true;

                mouse_state.image_surface = load_embedded_mouse_png();
                if (cairo_surface_status(mouse_state.image_surface) == CAIRO_STATUS_SUCCESS) {
                    mouse_state.image_width  = cairo_image_surface_get_width(mouse_state.image_surface);
                    mouse_state.image_height = cairo_image_surface_get_height(mouse_state.image_surface);
                    printf("Cursor image loaded: %dx%d\n",
                           mouse_state.image_width, mouse_state.image_height);
                } else {
                    fprintf(stderr, "Warning: failed to load embedded mouse pointer PNG\n");
                    cairo_surface_destroy(mouse_state.image_surface);
                    mouse_state.image_surface = NULL;
                }

                if (pthread_create(&mouse_thread, NULL, mouse_thread_func, NULL) != 0) {
                    perror("Failed to create mouse thread");
                    close(mouse_state.fd);
                    mouse_state.fd = -1;
                    mouse_state.visible = mouse_state.thread_running = false;
                    if (mouse_state.image_surface) {
                        cairo_surface_destroy(mouse_state.image_surface);
                        mouse_state.image_surface = NULL;
                    }
                } else {
                    printf("Mouse: %s (~1000 Hz polling)\n", mouse_state.path);
                }
            } else {
                printf("No mouse found; hotplug active.\n");
            }
        }
    }

    /* Wait for initial ring-buffer fill */
    int min_samples = MIN_BUFFER_AHEAD_FRAMES * SAMPLE_RATE / (int)ACTUAL_FRAME_RATE;
    printf("Waiting for initial buffer (%d samples)...\n", min_samples);
    wait_for_initial_samples(min_samples);
    printf("Buffer ready — starting render loop\n");

    /* Pre-render both framebuffers from the same snapshot to avoid a
     * single-frame phase discontinuity on startup. */
    pthread_mutex_lock(&history.mutex);
    int initial_head = history.head;
    pthread_mutex_unlock(&history.mutex);
    render_frame_with_budget(raw_buffer_A, zoom_level, FRAME_BUDGET_NS, initial_head);
    render_frame_with_budget(raw_buffer_B, zoom_level, FRAME_BUDGET_NS, initial_head);

    uint32_t *current_buffer = raw_buffer_B;
    uint32_t *next_buffer    = raw_buffer_A;
    int prev_snapshot_head = initial_head;
    srand((unsigned int)time(NULL));

    while (true) {
        sysop_framebuffer_flip();
        sysop_wait_hdmi_vblank();

        /* Mouse hotplug via inotify */
        if (enable_mouse && inofd >= 0) {
            struct pollfd pfd = { inofd, POLLIN, 0 };
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
                ssize_t len = read(inofd, buf, sizeof(buf));
                if (len > 0) {
                    if (mouse_state.thread_running) {
                        pthread_mutex_lock(&mouse_state.mutex);
                        mouse_state.thread_running = false;
                        pthread_mutex_unlock(&mouse_state.mutex);
                        pthread_join(mouse_thread, NULL);
                    }
                    if (mouse_state.fd >= 0) { close(mouse_state.fd); mouse_state.fd = -1; }
                    mouse_state.visible = false;

                    int newfd = open_first_mouse(mouse_state.path, sizeof(mouse_state.path));
                    if (newfd >= 0) {
                        mouse_state.fd = newfd;
                        mouse_state.visible = mouse_state.thread_running = true;
                        if (pthread_create(&mouse_thread, NULL, mouse_thread_func, NULL) == 0)
                            printf("Mouse reconnected: %s\n", mouse_state.path);
                        else {
                            close(mouse_state.fd); mouse_state.fd = -1;
                            mouse_state.visible = mouse_state.thread_running = false;
                        }
                    } else {
                        printf("Mouse removed\n");
                    }
                }
            }
        }

        /* Swap buffers */
        uint32_t *tmp = current_buffer;
        current_buffer = next_buffer;
        next_buffer    = tmp;

        /* Snapshot the ring-buffer head for this frame */
        pthread_mutex_lock(&history.mutex);
        int render_head = history.head;
        pthread_mutex_unlock(&history.mutex);

        /* Adjust effective zoom to track exactly how many samples advanced */
        float effective_zoom = zoom_level;
        if (prev_snapshot_head >= 0) {
            int adv = render_head - prev_snapshot_head;
            if (adv < 0) adv += HISTORY_SIZE;
            effective_zoom = zoom_level * (float)adv / (SAMPLE_RATE / ACTUAL_FRAME_RATE);

            static int dbg = 0;
            if ((dbg++ % 50) == 0)
                printf("samples_advanced=%d expected=%.1f eff_zoom=%.4f\n",
                       adv, SAMPLE_RATE / ACTUAL_FRAME_RATE, effective_zoom);
        }
        prev_snapshot_head = render_head;

        render_frame_with_budget(current_buffer, effective_zoom, FRAME_BUDGET_NS, render_head);

        static int fc = 0;
        if ((fc++ % 50) == 0) {
            pthread_mutex_lock(&history.mutex);
            int tw = history.total_samples_written;
            pthread_mutex_unlock(&history.mutex);
            printf("Sampler: %d total samples (%.2fs)\n",
                   tw, tw / (float)SAMPLE_RATE);
        }
    }

    return 0;
}
