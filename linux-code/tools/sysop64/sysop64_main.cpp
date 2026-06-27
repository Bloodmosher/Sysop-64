/*
 * Sysop-64 
 * https://github.com/Bloodmosher/Sysop-64 
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project 
 *
 * sysop64_main.cpp
 *
 * Application entry point. Performs the full one-time initialisation
 * sequence and then runs the main pty/console loop:
 *
 *   1. Build the key-mapping lookup tables.
 *   2. Open the FPGA hardware bridge.
 *   3. Map both framebuffer pages from /dev/mem.
 *   4. Create Cairo image surfaces and Pango layouts for each buffer page.
 *   5. Start the TCP command-server accept thread.
 *   6. Loop forever calling pty_loop(); on each exit from pty_loop()
 *      (child process death), tear down the console state and restart.
 */

#include "sysop64_internal.h"
int main(int argc, char **argv) 
{
    std::string foo(term_cols, ' ');
    uint64_t a = 0;

    int fd = 0;
    uint8_t *sysop64_bridge_map = NULL;

    // Build ctrl/shift/normal key-translation lookup tables before any input.
    init_key_mapping_tables();

    // Open the low-level FPGA hardware interface.
    sysop_open_bridge();

// not connecting since we are now the server
/*    int result = sysop_server_connect();
    if (result != 0)
    {
        printf("Could not connect to sysop, error %d\n", result);
        return -1;
    }
    */

    init_buffer(&line_buffer);
    current_line_buffer = &line_buffer;

    clock_gettime(CLOCK_MONOTONIC, &g_last_update);

    //add_line(&line_buffer, "Welcome to the terminal.");
    //add_line(&line_buffer, "Greetings profressor Falken.");

    fd = open("/dev/mem", O_RDWR | O_SYNC);

    if (fd < 0) {
        perror("Couldn't open /dev/mem\n");
        return -2;
    }

    printf("requesting framebuffer lock\n");
    sysop_framebuffer_lock();

    // Map both FPGA framebuffer pages as write-combined ARGB surfaces.
    pFrameBuffer1 = (unsigned char*)mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MEM_ADDRESS1);
    pFrameBuffer2 = (unsigned char*)mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MEM_ADDRESS2);

    // Zero both buffers so no stale FPGA content is visible on first show.
    memset(pFrameBuffer1, 0, (g_framebuffer_width*g_framebuffer_height*4));
    memset(pFrameBuffer2, 0, (g_framebuffer_width*g_framebuffer_height*4));

    printf("Mapped frame buffers at %p and %p\n", pFrameBuffer1, pFrameBuffer2);

    if (sysop64_bridge_map == MAP_FAILED) {
        perror("mmap failed.");
        close(fd);
        return -3;
    }

    sysop_framebuffer_hide();

    pFrameBuffer = pFrameBuffer1;
    clear_framebuffer();
    
    cairo_surface_t* surface1 = cairo_image_surface_create_for_data(pFrameBuffer1, CAIRO_FORMAT_ARGB32, g_framebuffer_width, g_framebuffer_height, g_framebuffer_width * 4);
    g_cr1 = cairo_create(surface1);

    cairo_surface_t* surface2 = cairo_image_surface_create_for_data(pFrameBuffer2, CAIRO_FORMAT_ARGB32, g_framebuffer_width, g_framebuffer_height, g_framebuffer_width * 4);
    g_cr2 = cairo_create(surface2);

    printf("Cairo contexts at %p and %p\n", g_cr1, g_cr2);


    g_layout1 = pango_cairo_create_layout(g_cr1);
    PangoFontDescription* desc1 = pango_font_description_from_string("monospace 20");
    //PangoFontDescription* desc1 = pango_font_description_from_string("ROG Fonts 20");
    pango_layout_set_font_description(g_layout1, desc1);
    pango_font_description_free(desc1);

    g_layout2 = pango_cairo_create_layout(g_cr2);
    PangoFontDescription* desc2 = pango_font_description_from_string("monospace 20");
    //PangoFontDescription* desc2 = pango_font_description_from_string("ROG Fonts 20");
    pango_layout_set_font_description(g_layout2, desc2);
    pango_font_description_free(desc2);


    //cairo_select_font_face(g_cr1, "Impact", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    //cairo_select_font_face(g_cr1, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_select_font_face(g_cr1, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(g_cr1, font_size);
    
    //cairo_select_font_face(g_cr2, "Impact", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    //cairo_select_font_face(g_cr2, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_select_font_face(g_cr2, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(g_cr2, font_size);

    sysop_framebuffer_unlock();

    g_cr = g_cr1;
    g_layout = g_layout1;
    //g_cr = g_cr2;

    //update_framebuffer();
    //cairo_set_source_rgba(cr, 0, 0, 0, 0);
   // cairo_paint(cr);

    fflush(stdout);

    // Start the command server on a detached thread; it handles DMA lock
    // requests and display messages from libsysop64 clients.
    initialize_lock();

    pthread_t tid;
    if (pthread_create(&tid, NULL, handleAccept, NULL) != 0) {
        perror("Error creating thread");
    } else {
        // Detach the thread to allow it to run independently
        pthread_detach(tid);
    }

    // Main loop: pty_loop() runs until the child shell exits. Each iteration
    // tears down the console overlay (if active) and restarts the login shell.
    while(1) {
        pty_loop();

        if (framebuffer_visible) {
            toggle_ui_visibility();
        }
        if (c64_console_active) {
            console_release_lock();
            c64_console_active = 0;
        }
        
/*        current_line_buffer->lines.clear();
        init_buffer(&line_buffer);
        current_line_buffer = &line_buffer;

        cursor_position = 0;
        cursor_row = 1;
        cursor_col = 1;
*/

        printf("\n\n\nStarting new pty_loop\n");
    }

    cairo_destroy(g_cr1);
    cairo_surface_destroy(surface1);

    cairo_destroy(g_cr2);
    cairo_surface_destroy(surface2);

    munmap(pFrameBuffer1, MEM_SIZE);
    munmap(pFrameBuffer2, MEM_SIZE);
    close(fd);

    sysop_close_bridge();
    return 0;
}



