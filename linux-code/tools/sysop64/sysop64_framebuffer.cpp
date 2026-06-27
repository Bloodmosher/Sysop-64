/*
 * Sysop-64 
 * https://github.com/Bloodmosher/Sysop-64 
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project 
 *
 * sysop64_framebuffer.cpp
 *
 * Low-level framebuffer operations. Manages the FPGA-mapped framebuffer
 * memory: clearing, drawing and erasing the software mouse cursor, and
 * the rate-limited double-buffer update cycle.
 *
 * update_framebuffer() is the central render dispatch: it throttles to
 * ~60 fps, calls draw_to_context() to composite the terminal content onto
 * the inactive buffer, flips the hardware buffer pointer, waits for vblank,
 * then re-draws the new back buffer so both buffers stay in sync.
 *
 * draw_to_context() handles full-redraw vs. dirty-line-only rendering,
 * clearing the appropriate regions before delegating to render_lines().
 */

#include "sysop64_internal.h"

// Clears the active framebuffer to all zeros (transparent black).
void clear_framebuffer()
{
    //printf("clear_framebuffer pFrameBuffer %p\n", pFrameBuffer);
    memset(pFrameBuffer, 0, (g_framebuffer_width*g_framebuffer_height*4));
}


int save_mouse_x1;
int save_mouse_x2;

int save_mouse_y1;
int save_mouse_y2;

// Draws a 10x10 white rectangle at the current mouse position using cr.
// Saves the drawn position in save_mouse_x1/y1 or x2/y2 depending on which
// Cairo context is active, so erase_mouse() can clear the same region.
void draw_mouse(cairo_t* cr)
{
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, g_mouse_x, g_mouse_y, 10, 10);
    cairo_fill(cr);

    if (cr == g_cr1) {
        save_mouse_x1 = g_mouse_x;
        save_mouse_y1 = g_mouse_y;
    }
    else {
        save_mouse_x2 = g_mouse_x;
        save_mouse_y2 = g_mouse_y;
    }
}

// Erases the mouse cursor drawn by the last draw_mouse() call on this
// context by filling the saved bounding box with transparent pixels.
void erase_mouse(cairo_t* cr)
{
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 1, 1, 1, 0);
    cairo_set_line_width(cr, 1.0);
    
    if (cr == g_cr1) {
        cairo_rectangle(cr, save_mouse_x1, save_mouse_y1, 10, 10);
    }
    else {
        cairo_rectangle(cr, save_mouse_x2, save_mouse_y2, 10, 10);
    }
    cairo_fill(cr);
}

struct timespec g_last_update;

// Returns elapsed time in milliseconds between two CLOCK_MONOTONIC samples.
long diff_in_ms(struct timespec start, struct timespec end) {
    long seconds = end.tv_sec - start.tv_sec;
    long nanoseconds = end.tv_nsec - start.tv_nsec;
    return (seconds * 1000) + (nanoseconds / 1000000);
}


void draw_to_context(cairo_t* cr);

// Debug-only sanity check: verifies that pFrameBuffer points to the buffer
// that is NOT currently being displayed by the FPGA. Exits to a getchar()
// if out of sync. No-ops when the framebuffer is hidden.
void verify_framebuffer()
{
    if (framebuffer_visible == 0)
        return;

        pFrameBuffer = (pFrameBuffer == pFrameBuffer1 ? pFrameBuffer2 : pFrameBuffer1);
  uint32_t status1 = sysop_read_status_1();
  uint8_t val = (uint8_t)((status1 >> 9) & 0xFF);
  if ((val & 1) && pFrameBuffer != pFrameBuffer1) // we should be pointing at the one not currently visible
   {
        printf("Frame buffer pointer out of sync\n");
        getchar();
  }
}

// Schedules or performs a framebuffer redraw. Throttles to a minimum 16ms
// interval (~60 fps). If called too early, sets framebuffer_update_needed
// so the next loop iteration retries. When due:
//   1. Draws the current terminal state to the back buffer.
//   2. Flips the hardware buffer and waits for vblank.
//   3. Swaps all double-buffer pointers (pFrameBuffer, g_cr, g_layout).
//   4. Redraws the new back buffer to keep both sides in sync.
void update_framebuffer()
{
    //verify_framebuffer();

    static struct timespec last = {0, 0};

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // compute elapsed time in milliseconds
    uint64_t elapsed_ms =
        (now.tv_sec - last.tv_sec) * 1000ULL +
        (now.tv_nsec - last.tv_nsec) / 1000000ULL;

    if (elapsed_ms < 16) {
        framebuffer_update_needed = 1;
        return; // too soon, skip
    }

    last = now;  // update timestamp
    framebuffer_update_needed = 0;

    if (framebuffer_visible)
    {
        // I think if we wait here, by the time we are ready to flip the drawing code will have latched the buffer, so we can then safely flip it 
 //        printf("wait vblank...\n");
  //       sysop_wait_hdmi_vblank();
 //        printf("past wait vblank...\n");

/*        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        long elapsed_ms = diff_in_ms(g_last_update, now);

        if (elapsed_ms < 16) {
            printf("not enough time!\n");
            return;
        }

        g_last_update = now;
*/
        //printf("Rendering to frame buffer %d\n", pFrameBuffer == pFrameBuffer1 ? 1 : 2);
        //printf("Rendering to context %d\n", g_cr == g_cr1 ? 1 : 2);

        //printf("Clearing frame buffer\n");
        //clear_framebuffer();

        //printf("Drawing g_cr=%p\n", g_cr);
        cairo_t* cr = g_cr;
        int save_x = previous_cursor_x;
        int save_y = previous_cursor_y;
        int save_width = previous_cursor_width;
        int save_height = previous_cursor_height;

        draw_to_context(cr);
        // display what we just drew
     //   cairo_surface_t* surface = cairo_get_target(cr);
     //   cairo_surface_flush(surface);

        //sysop_wait_hdmi_vblank();
        sysop_framebuffer_flip();
        sysop_wait_hdmi_vblank(); // wait for tof of the buffer we just flipped to

        //printf("Flipping pointers\n");
        pFrameBuffer = (pFrameBuffer == pFrameBuffer1 ? pFrameBuffer2 : pFrameBuffer1);
        g_cr = (g_cr == g_cr1 ? g_cr2 : g_cr1);
        g_layout = (g_layout == g_layout1 ? g_layout2 : g_layout1);


        // make sure our back buffer is in sync with what is now on the screen
        previous_cursor_x = save_x;
        previous_cursor_y = save_y;
        previous_cursor_width = save_width;
        previous_cursor_height = save_height;

        draw_to_context(g_cr);

        for (int i=0;i<MAX_VISIBLE_LINES;i++) {
            current_line_buffer->redraw_needed[i] = false;
        }
        current_line_buffer->redraw_all = false;
    }
}

// Renders the current terminal line buffer to cr. On a full redraw
// (redraw_all), first clears the content region with solid black and draws
// the green border rectangle. On a partial update, only dirty lines are
// cleared and redrawn. Delegates per-line text rendering to render_lines().
void draw_to_context(cairo_t* cr)
{
/*        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_rectangle(cr, 0, 0, g_framebuffer_width, g_framebuffer_height);
        cairo_fill(cr);
  */      

    if (current_line_buffer->redraw_all)
    {
        cairo_set_source_rgba(cr, 0, 0, 0, 1);
        //cairo_set_source_rgba(cr, 1, 0, 0, .5);
        //cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        //cairo_paint(cr);
        //cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        erase_mouse(cr);

        cairo_rectangle(cr, 0, g_framebuffer_height/2, g_framebuffer_width-2, g_framebuffer_height/2);
        
        //cairo_rectangle(cr, 100, g_framebuffer_height/2, g_framebuffer_width-200, (g_framebuffer_height/2)-20);
        cairo_rectangle(cr, 100, 10, g_framebuffer_width-200, (g_framebuffer_height)-20);
        //cairo_rectangle(cr, 0, 0, g_framebuffer_width, g_framebuffer_height);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 0, 0, 0, 1);
        //cairo_set_source_rgba(cr, 0, 0, 0, .2);
//cairo_rectangle(cr, 0, 0, g_framebuffer_width-1, g_framebuffer_height-1);

        cairo_rectangle(cr, 0, 0, g_framebuffer_width, 10);
        cairo_rectangle(cr, 0, g_framebuffer_height-10, g_framebuffer_width, g_framebuffer_height);
        cairo_rectangle(cr, 0, 0, 100, g_framebuffer_height);
        cairo_rectangle(cr, g_framebuffer_width-100, 0, g_framebuffer_width, g_framebuffer_height);

        cairo_fill(cr);

        //cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

        //cairo_set_source_rgba(cr, 0, 0, 0, 1);
        //cairo_rectangle(cr, 100, g_framebuffer_height/2, g_framebuffer_width-200, (g_framebuffer_height/2)-20);
        //cairo_fill(cr);

        cairo_set_source_rgba(cr, 0, 1, 0, 1);
        cairo_set_line_width(cr, 1.0);
        //cairo_rectangle(cr, 0, g_framebuffer_height/2, g_framebuffer_width-2, g_framebuffer_height/2);
        
        
        //cairo_rectangle(cr, 100, g_framebuffer_height/2, g_framebuffer_width-200, (g_framebuffer_height/2)-20);
        //cairo_rectangle(cr, 100, 10, g_framebuffer_width-200, (g_framebuffer_height)-20);
        cairo_rectangle(cr, 98, 10, g_framebuffer_width-200, (g_framebuffer_height)-20);
        
        cairo_stroke(cr);
    }

        //printf("Calling render_lines\n");
        render_lines(cr, g_framebuffer_width, g_framebuffer_height);

//        draw_mouse(cr);

        //cairo_pop_group_to_source(cr);


        
//        printf("Drawing frame buffer now points to buffer %d\n", pFrameBuffer == pFrameBuffer1 ? 1 : 2);
//        printf("Rendering context now  %d\n", g_cr == g_cr1 ? 1 : 2);

        // this is actually "wait for buffer flip to complete", i.e. the renderer is now using the buffer we just flipped to and it is safe to start drawing on the other one
        //printf("Waiting for vblank\n");
//TODO: why did this stop working?  I think I changed debug_info..       
        //sysop_wait_hdmi_vblank();
}



