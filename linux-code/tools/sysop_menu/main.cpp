/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * main.cpp — hardware initialisation, main event loop, and signal
 * handling for the sysop_menu program.
 */

#include <error.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <signal.h>
#include <time.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <string>
#include <list>
#include <algorithm>
#include <iterator>

#include "sysop_defines.h"
#include "sysop64.h"
#include "c64keys.h"
#include "keyboard.h"
#include "stars.h"
#include "display.h"
#include "file_browser.h"

/* Provided by disk.cpp */
extern int d64_load(const char *d64filename, const char *filename);

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/* Physical framebuffer base addresses */
#define MEM_ADDRESS1 0x20000000
#define MEM_ADDRESS2 0x207e9000

/* Size to mmap per framebuffer */
#define MEM_SIZE (1024 * 1024 * 100)

/* C64 screen RAM base address used for screen_clear() calls */
static const uint16_t g_scr = 0x400;

/* ------------------------------------------------------------------ */
/* Timing helper                                                       */
/* ------------------------------------------------------------------ */

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* C64 keyboard buffer "RUN" injection                                 */
/* ------------------------------------------------------------------ */

static void inject_run(void)
{
    sysop_poke(0x800, 0x00);
    uint16_t addr = 0x277;
    sysop_poke(addr++, 0x52);  /* R */
    sysop_poke(addr++, 0x55);  /* U */
    sysop_poke(addr++, 0x4e);  /* N */
    sysop_poke(addr++, 0x0d);  /* CR */
    sysop_poke(0x00c6, 0x04);
}

/* ------------------------------------------------------------------ */
/* Signal handler — clean up and exit                                  */
/* ------------------------------------------------------------------ */

static void sigintHandler(int sig)
{
    printf("SIGINT handler\n");
    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_close_bridge();

    cairo_destroy(g_cr1);
    cairo_surface_destroy(g_surface1);

    cairo_destroy(g_cr2);
    cairo_surface_destroy(g_surface2);

    g_object_unref(g_layout1);
    g_object_unref(g_layout2);

    exit(sig);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    signal(SIGINT,  sigintHandler);
    signal(SIGTERM, sigintHandler);

    initIncTable();
    sysop_init();

    int res = sysop_server_connect();
    if (res != 0) {
        printf("Could not connect to sysop\n");
        return res;
    }

    int fd = open("/dev/sysop-fb", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("Couldn't open /dev/sysop-fb");
        return -2;
    }

    pFrameBuffer1 = (unsigned char *)mmap(NULL, MEM_SIZE,
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    pFrameBuffer2 = (unsigned char *)mmap(NULL, MEM_SIZE,
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                        MEM_ADDRESS2 - MEM_ADDRESS1);

    sysop_framebuffer_lock();
    memset(pFrameBuffer1, 0, (size_t)(g_framebuffer_width * g_framebuffer_height * 4));
    memset(pFrameBuffer2, 0, (size_t)(g_framebuffer_width * g_framebuffer_height * 4));
    printf("Mapped frame buffers at %p and %p\n", pFrameBuffer1, pFrameBuffer2);

    sysop_framebuffer_hide();
    pFrameBuffer = pFrameBuffer1;
    clear_framebuffer();

    /* Create Cairo surfaces and rendering contexts for both buffers */
    cairo_surface_t *surface1 = cairo_image_surface_create_for_data(
        pFrameBuffer1, CAIRO_FORMAT_ARGB32,
        g_framebuffer_width, g_framebuffer_height, g_framebuffer_width * 4);
    g_cr1 = cairo_create(surface1);

    cairo_surface_t *surface2 = cairo_image_surface_create_for_data(
        pFrameBuffer2, CAIRO_FORMAT_ARGB32,
        g_framebuffer_width, g_framebuffer_height, g_framebuffer_width * 4);
    g_cr2 = cairo_create(surface2);

    printf("Cairo contexts at %p and %p\n", g_cr1, g_cr2);
    g_surface1 = surface1;
    g_surface2 = surface2;

    /* Pango layouts — one per buffer so both can be dirty independently */
    g_layout1 = pango_cairo_create_layout(g_cr1);
    PangoFontDescription *desc1 = pango_font_description_from_string("monospace 20");
    pango_layout_set_font_description(g_layout1, desc1);
    pango_font_description_free(desc1);

    g_layout2 = pango_cairo_create_layout(g_cr2);
    PangoFontDescription *desc2 = pango_font_description_from_string("monospace 20");
    pango_layout_set_font_description(g_layout2, desc2);
    pango_font_description_free(desc2);

    cairo_select_font_face(g_cr1, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(g_cr1, font_size);
    cairo_select_font_face(g_cr2, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(g_cr2, font_size);

    g_image = cairo_image_surface_create_from_png("/usr/local/bin/sysop-logo-480x196.png");
    printf("created image %p\n", g_image);
    g_image_width  = cairo_image_surface_get_width(g_image);
    g_image_height = cairo_image_surface_get_height(g_image);

    sysop_framebuffer_unlock();

    /* ----------------------------------------------------------------
     * Outer loop: each iteration is one press-and-release of button 1,
     * which activates the file browser.
     * -------------------------------------------------------------- */
    while (1) {
        printf("Waiting for press...\n");
        while (!sysop_is_button_pressed(1))
            usleep(10000);
        printf("pressed\n");
        while (sysop_is_button_pressed(1))
            usleep(10000);
        printf("released\n");

        sysop_framebuffer_lock();

        /* Show buffer 1, start drawing into buffer 2 */
        sysop_framebuffer_show();
        pFrameBuffer  = pFrameBuffer2;
        g_cr          = g_cr2;
        g_layout      = g_layout2;
        framebuffer_visible = 1;
        pRedrawNeeded = &layout2_redraw_needed;
        set_redraw_needed();

        sysop_server_dma_lock();
        sysop_poke(0xd418, 0);  /* silence SID */

        /* Load initial file list */
        std::string path = g_root;
        g_file_list.clear();
        get_items(path, g_file_list);
        g_file_list.sort(fs_item_sort_name);
        for (auto &it : g_file_list)
            printf("%s\n", it.name.c_str());

        int parent_position = 0;
        int position        = 0;
        show_items(g_file_list, 0);
        update_position(position);
        top_position = 0;

        int reset_c64_needed              = 0;
        int timeBetweenKeyPressMs         = 120;
        long long prev_time               = now_ns();

        /* ----------------------------------------------------------------
         * Inner loop: runs while the file browser is on-screen.
         * -------------------------------------------------------------- */
        while (1) {
            long long now = now_ns();
            if (now - prev_time >= 20000000LL) {
                update();
                prev_time = now;
            }
            update_framebuffer();
            sysop_scan_keys();

            /* ESC — leave file browser */
            if (sysop_is_key_down(C64_KEY_ESC)) {
                usleep(400000);
                printf("exit 1\n");
                break;
            }

            /* Hardware button — also leave file browser */
            if (sysop_is_button_pressed(1)) {
                while (sysop_is_button_pressed(1))
                    usleep(10000);
                printf("exit (button pressed)\n");
                break;
            }

            /* F7 / shift-F7 — page / jump to end */
            if (sysop_is_key_down(C64_KEY_F7)) {
                if (sysop_is_shift_key_down())
                    position = (int)g_file_list.size() - 1;
                else {
                    position += 5;
                    if (position >= (int)g_file_list.size())
                        position = (int)g_file_list.size() - 1;
                }
                int newTop = (position >= MAX_DRAW_ITEMS) ? position - MAX_DRAW_ITEMS : 0;
                if (newTop != top_position) {
                    show_items(g_file_list, newTop);
                    top_position = newTop;
                }
                update_position(position);
            }

            /* F1 / shift-F1 — page / jump to top */
            if (sysop_is_key_down(C64_KEY_F1)) {
                if (sysop_is_shift_key_down())
                    position = 0;
                else {
                    position -= 5;
                    if (position < 0)
                        position = 0;
                }
                int newTop = (position >= MAX_DRAW_ITEMS) ? position - MAX_DRAW_ITEMS : 0;
                if (newTop != top_position) {
                    show_items(g_file_list, newTop);
                    top_position = newTop;
                }
                update_position(position);
            }

            /* Cursor down / shift = cursor up */
            if (sysop_is_key_down(C64_KEY_CRSR_DOWN)) {
                static struct timespec last_time = {0, 0};
                struct timespec current_time;
                clock_gettime(CLOCK_MONOTONIC, &current_time);
                long elapsed_ms = (current_time.tv_sec  - last_time.tv_sec)  * 1000 +
                                  (current_time.tv_nsec - last_time.tv_nsec) / 1000000;
                if (elapsed_ms >= timeBetweenKeyPressMs) {
                    last_time = current_time;
                    if (sysop_is_shift_key_down()) {
                        position--;
                        if (position == -1)
                            position = (int)g_file_list.size() - 1;
                    } else {
                        position++;
                        if (position == (int)g_file_list.size())
                            position = 0;
                    }
                    int newTop = (position >= MAX_DRAW_ITEMS) ? position - MAX_DRAW_ITEMS : 0;
                    if (newTop != top_position) {
                        show_items(g_file_list, newTop);
                        top_position = newTop;
                    }
                    update_position(position);
                }
            }

            /* Return — activate selected item */
            if (sysop_is_key_down(C64_KEY_RETURN)) {
                static struct timespec last_time = {0, 0};
                struct timespec current_time;
                clock_gettime(CLOCK_MONOTONIC, &current_time);
                long elapsed_ms = (current_time.tv_sec  - last_time.tv_sec)  * 1000 +
                                  (current_time.tv_nsec - last_time.tv_nsec) / 1000000;
                if (elapsed_ms >= timeBetweenKeyPressMs) {
                    last_time = current_time;
                    fs_item item = *(std::next(g_file_list.begin(), position));
                    printf("hit return on '%s', parent: %s\n",
                           item.name.c_str(), item.parent.c_str());
                    printf("fullpath '%s'\n", item.fullpath.c_str());

                    if (endsWithCaseInsensitive(item.name, ".d64") ||
                        endsWithCaseInsensitive(item.name, ".d81")) {
                        /* Enter D64/D81 image as a virtual directory */
                        getd64_items(g_current_folder, item.fullpath, g_file_list);
                        g_file_list.sort(fs_item_sort_name);
                        parent_position = position;
                        position = 0;
                        show_items(g_file_list, 0);
                        update_position(position);

                    } else if (item.locationType == LocationType::D64 && item.name != "..") {
                        /* Load a file from inside a D64 image */
                        sysop_cartridge_disable();
                        sysop_server_dma_unlock();
                        sysop_framebuffer_hide();
                        sysop_framebuffer_unlock();
                        sysop_c64_reset();
                        printf("Waiting for c64 reset...\n");
                        usleep(5000000);
                        sysop_server_dma_lock();
                        sysop_screen_clear(g_scr);
                        d64_load(item.parent.c_str(), item.name.c_str());
                        sysop_dma_wait_empty();
                        sysop_dma_wait_not_busy();
                        if (!sysop_is_shift_key_down()) {
                            inject_run();
                            sysop_dma_wait_empty();
                            sysop_dma_wait_not_busy();
                        }
                        printf("exit 2\n");
                        break;

                    } else if (item.d_type != DT_DIR) {
                        /* Load a native file from the host filesystem */
                        sysop_cartridge_disable();
                        sysop_server_dma_unlock();
                        sysop_framebuffer_hide();
                        sysop_framebuffer_unlock();
                        sysop_c64_reset();
                        printf("Waiting for c64 reset (2)\n");
                        usleep(5000000);
                        sysop_server_dma_lock();
                        sysop_screen_clear(g_scr);
                        if (strstr(item.fullpath.c_str(), ".crt") != NULL) {
                            printf("%s\n", item.fullpath.c_str());
                            sysop_cartridge_load(item.fullpath.c_str(), 0);
                            reset_c64_needed = 1;
                        } else {
                            sysop_load(item.fullpath.c_str());
                            inject_run();
                        }
                        printf("exit 3\n");
                        break;

                    } else {
                        /* Navigate into a directory (or back with "..") */
                        std::string next_folder;
                        if (item.name == "..") {
                            if (endsWithCaseInsensitive(g_current_folder, ".d64") ||
                                endsWithCaseInsensitive(g_current_folder, ".d81"))
                                next_folder = item.parent;
                            else
                                next_folder = g_current_folder.substr(
                                    0, g_current_folder.find_last_of("/"));
                        } else if (item.d_type == DT_DIR) {
                            next_folder = item.fullpath;
                        }
                        if (!next_folder.empty()) {
                            get_items(next_folder, g_file_list);
                            printf("Sorting...\n");
                            g_file_list.sort(fs_item_sort_name);
                            printf("Finished sorting.\n");
                            if (item.name == "..") {
                                position = parent_position;
                            } else {
                                parent_position = position;
                                position = 0;
                            }
                            show_items(g_file_list, 0);
                            if (position >= (int)g_file_list.size())
                                position = 0;
                            update_position(position);
                            printf("Done here\n");
                        }
                    }
                }
            }
        } /* inner loop */

        sysop_framebuffer_hide();
        sysop_framebuffer_unlock();
        framebuffer_visible = 0;
        sysop_server_dma_unlock();

        if (reset_c64_needed) {
            printf("resetting...\n");
            sysop_c64_reset();
            usleep(3500000);
            reset_c64_needed = 0;
        }
    } /* outer loop */

    /* Cleanup — only reached if the outer loop is broken */
    printf("About to exit\n");

    g_object_unref(g_layout1);
    printf("cairo_surface_destroy surface1\n");
    cairo_surface_destroy(surface1);
    printf("cairo_destroy g_cr1\n");
    cairo_destroy(g_cr1);

    g_object_unref(g_layout2);
    printf("cairo_surface_destroy surface2\n");
    cairo_surface_destroy(surface2);
    printf("cairo_destroy g_cr2\n");
    cairo_destroy(g_cr2);

    printf("unmap pFrameBuffer1 %p\n", pFrameBuffer1);
    munmap(pFrameBuffer1, MEM_SIZE);
    printf("unmap pFrameBuffer2 %p\n", pFrameBuffer2);
    munmap(pFrameBuffer2, MEM_SIZE);

    printf("close fd %d\n", fd);
    close(fd);

    sysop_server_dma_unlock();
    sysop_server_disconnect();
    printf("sysop_close_bridge\n");
    sysop_close_bridge();
    return 0;
}
