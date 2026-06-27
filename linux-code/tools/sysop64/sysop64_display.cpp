/*
 * Sysop-64 
 * https://github.com/Bloodmosher/Sysop-64 
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project 
 *
 * sysop64_display.cpp
 *
 * High-level display and UI visibility management. Controls switching
 * between the two display modes: the interactive C64 console (terminal
 * framebuffer overlay) and the message-display panel (animated logo +
 * typing-effect text). Handles the framebuffer show/hide lifecycle,
 * Cairo/Pango context setup, double-buffer selection, and the per-frame
 * update scheduling for both modes.
 *
 * Also drives the physical console button (sysop_is_button_pressed) and the
 * message queue: messages received from clients are queued, then dequeued
 * one at a time with a typewriter animation and configurable display timeout.
 */

#include "sysop64_internal.h"
// Sends signal to all processes in the same session as pid, excluding pid
// itself. Used to propagate signals to sibling processes on console events.
void sendSignalToProcessesWithSameSID(pid_t pid, int signal) {
    printf("Looking for processes sharing sid with pid %d\n", pid);
    pid_t current_sid = getsid(pid);

    DIR *proc_dir = opendir("/proc");
    if (proc_dir == NULL) {
        perror("opendir");
        exit(EXIT_FAILURE);
    }

    struct dirent *entry;
    while ((entry = readdir(proc_dir)) != NULL) {
        if (entry->d_type == DT_DIR && atoi(entry->d_name) > 0) {
            pid_t proc_pid = atoi(entry->d_name);
            pid_t sid = getsid(proc_pid);

            if (sid == current_sid && proc_pid != pid) {
                printf("PID: %d, SID: %d\n", proc_pid, sid);
                kill(proc_pid, signal);
            }
        }
    }

    closedir(proc_dir);
}

// Fills both framebuffer Cairo surfaces with fully transparent black,
// effectively clearing any previous content from both double-buffer pages.
void clear_both_frame_buffers()
{
    cairo_set_source_rgba(g_cr1, 0, 0, 0, 0);
    cairo_set_operator(g_cr1, CAIRO_OPERATOR_SOURCE);
    cairo_rectangle(g_cr1, 0, 0, g_framebuffer_width, g_framebuffer_height);
    cairo_fill(g_cr1);

    cairo_set_source_rgba(g_cr2, 0, 0, 0, 0);
    cairo_set_operator(g_cr2, CAIRO_OPERATOR_SOURCE);
    cairo_rectangle(g_cr2, 0, 0, g_framebuffer_width, g_framebuffer_height);
    cairo_fill(g_cr2);
}

// Applies the monospace terminal font to both Pango layouts (g_layout1 and
// g_layout2). Called when switching into console mode.
void set_console_font()
{
    PangoFontDescription* desc1 = pango_font_description_from_string("monospace 20");
    pango_layout_set_font_description(g_layout1, desc1);
    pango_font_description_free(desc1);

    PangoFontDescription* desc2 = pango_font_description_from_string("monospace 20");
    pango_layout_set_font_description(g_layout2, desc2);
    pango_font_description_free(desc2);
}

// Toggles the C64 console framebuffer overlay on or off. When enabling,
// acquires the framebuffer lock, clears both buffers, sets the console font,
// and switches the active draw context to the back buffer. When disabling,
// hides the framebuffer and releases the lock.
void toggle_ui_visibility()
{
    if (framebuffer_visible == 0)
    {
        // show will default to drawing the 1st buffer, so set our draw buffer to the second
        printf("requesting framebuffer lock\n");
        sysop_framebuffer_lock();
        printf("frame buffer switched on\n");

        sysop_framebuffer_hide();
        clear_both_frame_buffers();
        set_console_font();
        framebuffer_message_display_visible = 0;
        
        sysop_framebuffer_show(); 
        
        pFrameBuffer = pFrameBuffer2;
        g_cr = g_cr2;
        //g_layout = g_layout1;
        g_layout = g_layout2;
        //pFrameBuffer = pFrameBuffer1;
        //g_cr = g_cr1;

        //update_framebuffer();
        //update_framebuffer();

        //pFrameBuffer = pFrameBuffer2;
        //pFrameBuffer = pFrameBuffer1; // debugging
        current_line_buffer->redraw_all = true;
        framebuffer_visible = 1;
        update_framebuffer();
    }
    else
    {
        printf("frame buffer switched off\n");
        sysop_framebuffer_hide();
        framebuffer_visible = 0;
        sysop_framebuffer_unlock();
    }
}

// Applies the message-display (ROG Fonts) font to both Pango layouts.
// Called when switching into message-display mode.
void set_message_display_font()
{
    PangoFontDescription* desc1 = pango_font_description_from_string("ROG Fonts 20");
    //PangoFontDescription* desc1 = pango_font_description_from_string("Asus Rog 20");
    pango_layout_set_font_description(g_layout1, desc1);
    pango_font_description_free(desc1);

    PangoFontDescription* desc2 = pango_font_description_from_string("ROG Fonts 20");
    //PangoFontDescription* desc2 = pango_font_description_from_string("Asus Rog 20");
    pango_layout_set_font_description(g_layout2, desc2);
    pango_font_description_free(desc2);
}

// Toggles the message-display panel on or off. Similar to
// toggle_ui_visibility() but targets the message-display mode and uses the
// ROG Fonts layout instead of the monospace console layout.
void toggle_message_display_ui_visibility()
{
    if (framebuffer_message_display_visible == 0)
    {
        printf("requesting framebuffer lock\n");
        sysop_framebuffer_lock();
        printf("message display frame buffer switched on\n");

        clear_both_frame_buffers();
        set_message_display_font();
        sysop_framebuffer_show(); 
        
        pFrameBuffer = pFrameBuffer2;
        g_cr = g_cr2;
        g_layout = g_layout2;
        framebuffer_message_display_visible = 1;
    }
    else
    {
        printf("message display frame buffer switched off\n");
        sysop_framebuffer_hide();
        framebuffer_message_display_visible = 0;
        sysop_framebuffer_unlock();
    }
}


////////////////////////////////
// message display handling
////////////////////////////////

//logo
cairo_surface_t* g_image = NULL;
int g_image_width = 0;
int g_image_height = 0;

float eye_color = 1.0;
float eye_change = 0.05;

// Advances the logo eye brightness animation. The eye colour oscillates
// between 0 and 1 in steps of 0.05, reversing at each extreme.
void updateEyes()
{
    eye_color += eye_change;
    if (eye_color > 1.0) {
        eye_change = -0.05;
        eye_color = 1.0;
    }
    if (eye_color < 0) {
        eye_change = 0.05;
        eye_color = 0;
    }
}

// Draws the two animated logo eye dots at their hard-coded screen positions
// using the current eye_color value.
void drawEyes(cairo_t* cr)
{
    cairo_set_source_rgba(cr, 0, 0, eye_color, 1);
    cairo_set_line_width(cr, 1.0);
    double radius = 2;

    int x1 = 285;
    int x2 = x1 + 20;
    int y = 965;

    cairo_arc(cr, x1, y, radius, 0, 2*M_PI);
    cairo_fill(cr);
    cairo_arc(cr, x2, y, radius, 0, 2*M_PI);
    cairo_fill(cr);
}


int message_display_show_requested = 0;

std::string g_display_msg;
std::queue<QueuedMessage> g_message_queue;
pthread_mutex_t g_message_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

int g_typing_char_index = 0;
bool g_typing_complete = false;
long long g_last_char_time = 0;
//long long g_typing_speed_ns = 50000000; // 50ms per character (configurable)
long long g_typing_speed_ns = 1000000; // 1ms per character (configurable)
long long g_typing_start_time = 0;
long long g_message_display_timeout_ms = 5000; // 5000ms (5 seconds) timeout before accepting new message (configurable)
long long prev_time = 0;

// Cursor blinking variables
bool g_cursor_visible = true;
long long g_last_cursor_blink_time = 0;
long long g_cursor_blink_speed_ns = 500000000; // 500ms blink rate


// Renders the current message string up to the typing-animation index into
// the active Pango layout, appending a blinking cursor underscore when
// g_cursor_visible is true.
void drawMessage(cairo_t* cr)
{
    std::string markup_builder;
    
    // Only display characters up to the current typing index
    std::string str = g_display_msg.substr(0, g_typing_char_index);
    
    // Add blinking cursor if visible
    if (g_cursor_visible) {
        str += "_";
    }
    
    markup_builder += str;
    
    // Always set the markup (even if empty, to show just the cursor)
    pango_layout_set_markup(g_layout, markup_builder.c_str(), -1);

    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_move_to(cr, 370, 913);

    if (cr == g_cr1 && g_layout == g_layout2) {
        printf("wrong layout1\n");
        getchar();
    }
    if (cr == g_cr2 && g_layout == g_layout1) {
        printf("wrong layout2\n");
        getchar();
    }
    pango_layout_set_width(g_layout, 1250 * PANGO_SCALE);
    pango_layout_set_wrap(g_layout, PANGO_WRAP_WORD);
    pango_cairo_show_layout(cr, g_layout);
}

// Rate-limited redraw for the message-display panel. Throttles to ~60 fps
// (16 ms minimum between frames). When due, composites the logo image, eye
// animation, and message text onto the active buffer, then flips and waits
// for vblank before swapping the double-buffer pointers.
void draw_message_display()
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
        framebuffer_message_display_updated_needed = 1;
        return; // too soon, skip
    }

    last = now;  // update timestamp
    framebuffer_message_display_updated_needed = 0;

    if (framebuffer_message_display_visible)
    {
        cairo_t* cr = g_cr;

/*        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        //cairo_set_source_rgba(cr, 0, 0, 0, 1);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_rectangle(cr, 0, 0, g_framebuffer_width, g_framebuffer_height);
        cairo_fill(cr);
        */

        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);


        cairo_set_source_rgba(cr, 0, 1, 0, 1);
        cairo_set_line_width(cr, 1.0);

        //drawStars(cr, g_framebuffer_width, g_framebuffer_height, 1);
        

        int x = (g_framebuffer_width / 2) - (g_image_width / 2)-3;
        //printf("showing at %d\n", x);
        cairo_set_source_surface(cr, g_image, x, 905);
        cairo_paint(cr);

        drawEyes(cr);
        
        drawMessage(cr);

        //flip_buffers();
        sysop_framebuffer_flip();
        sysop_wait_hdmi_vblank(); // tof

        pFrameBuffer = (pFrameBuffer == pFrameBuffer1 ? pFrameBuffer2 : pFrameBuffer1);
        g_cr = (g_cr == g_cr1 ? g_cr2 : g_cr1);
        g_layout = (g_layout == g_layout1 ? g_layout2 : g_layout1);
    }
}

// Returns the current monotonic time in nanoseconds.
long long now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Drives the message-display lifecycle from the main loop. Shows the panel
// when a message is pending, hides it while the console is active, advances
// the typewriter animation and cursor blink, dequeues the next message when
// the current one's timeout expires, and closes the panel on close request.
void handleMessageDisplay()
{
    // don't show message display if console is active
    if (!framebuffer_visible)
    {
        if (message_display_show_requested) {
            if (!framebuffer_message_display_visible)
                toggle_message_display_ui_visibility();

            message_display_show_requested = 0;
        }
    }
    // turn off if console becomes active
    else if (framebuffer_message_display_visible) {
        toggle_message_display_ui_visibility();
    }

    // if it is visible, update it
    if (framebuffer_message_display_visible)
    {
        if (g_image == NULL) {
            g_image = cairo_image_surface_create_from_png("/usr/local/bin/sysop-msg-1.png");
            printf("created image %p\n", g_image);
            g_image_width = cairo_image_surface_get_width(g_image);
            g_image_height = cairo_image_surface_get_height(g_image);
        }

        // Check if we should dequeue a new message
        bool can_update = g_display_msg.empty();
        if (!can_update && g_typing_complete) {
            long long elapsed_ms = (now_ns() - g_typing_start_time) / 1000000;
            if (elapsed_ms >= g_message_display_timeout_ms) {
                can_update = true;
            }
        }
        
        if (can_update) {
            pthread_mutex_lock(&g_message_queue_mutex);
            if (!g_message_queue.empty()) {
                QueuedMessage queued_msg = g_message_queue.front();
                g_message_queue.pop();
                
                // Check message type
                if (queued_msg.msg_type == 1) {
                    // Hide message command - only close if no more messages
                    if (g_message_queue.empty()) {
                        pthread_mutex_unlock(&g_message_queue_mutex);
                        message_display_close_requested = 1;
                    } else {
                        // More messages available, clear display and continue
                        pthread_mutex_unlock(&g_message_queue_mutex);
                        g_display_msg = "";
                        g_typing_char_index = 0;
                        g_typing_complete = true;
                        g_cursor_visible = true;
                        g_last_cursor_blink_time = now_ns();
                    }
                } else {
                    pthread_mutex_unlock(&g_message_queue_mutex);
                    
                    // Display message (type 0)
                    g_display_msg = queued_msg.message;
                    g_message_display_timeout_ms = queued_msg.timeout_ms;
                    g_typing_char_index = 0;
                    g_typing_complete = false;
                    g_last_char_time = now_ns();
                    g_cursor_visible = true;
                    g_last_cursor_blink_time = now_ns();
                }
            } else {
                // No more messages in queue
                pthread_mutex_unlock(&g_message_queue_mutex);
                
                // Only clear the display if it's not already empty
                if (!g_display_msg.empty()) {
                    g_display_msg = "";
                    g_typing_char_index = 0;
                    g_typing_complete = true;
                    g_cursor_visible = true;
                    g_last_cursor_blink_time = now_ns();
                }
            }
        }
        
        updateEyes();
        
        long long current_time = now_ns();
        
        // Update typing animation
        if (!g_typing_complete && g_typing_char_index < g_display_msg.length()) {
            if (current_time - g_last_char_time >= g_typing_speed_ns) {
                g_typing_char_index++;
                g_last_char_time = current_time;
                framebuffer_message_display_updated_needed = 1;
                
                if (g_typing_char_index >= g_display_msg.length()) {
                    g_typing_complete = true;
                    // Start the timeout countdown now that typing is complete
                    g_typing_start_time = now_ns();
                }
            }
        }
        
        // Update cursor blinking
        if (current_time - g_last_cursor_blink_time >= g_cursor_blink_speed_ns) {
            g_cursor_visible = !g_cursor_visible;
            g_last_cursor_blink_time = current_time;
            framebuffer_message_display_updated_needed = 1;
        }
        
        draw_message_display();
    }

    // handle any request to close it
    if (message_display_close_requested && framebuffer_message_display_visible) {
        printf("Closing message display due to message_display_close_requested\n");
        
        if (framebuffer_message_display_visible) {
            toggle_message_display_ui_visibility();
        }
        
        message_display_close_requested = 0;
    }
}


// Polls the physical console button (button 2). On press: if the console is
// inactive, shows the overlay and acquires the DMA lock; if already active,
// hides the overlay and releases the lock. Debounces the release with a 250ms
// hold time to avoid spurious toggles.
void handle_console_button()
{
    static struct timespec time_console_active_changed;
    
    if (console_yield_lock)
        return;

    if (console_button_down == 0 && sysop_is_button_pressed(2)) {
        printf("here 1\n");
        console_button_down = 1;
        if (!c64_console_active) {
            printf("here 2\n");
            toggle_ui_visibility();
            //sysop_dma_enable();
//            sysop_server_dma_lock();
            console_acquire_lock();
            c64_console_active = 1;
            clock_gettime(CLOCK_MONOTONIC, &time_console_active_changed);
        }
        else {
            printf("here 5\n");
            if (framebuffer_visible) {
                toggle_ui_visibility();
            }
            //sysop_dma_disable();
            //sysop_server_dma_unlock();
            console_release_lock();
            c64_console_active = 0;
            clock_gettime(CLOCK_MONOTONIC, &time_console_active_changed);
        }

    }
    else if (console_button_down == 1 && !sysop_is_button_pressed(2)) {
        printf("here 3\n");
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        long elapsed_ms = (now.tv_sec - time_console_active_changed.tv_sec) * 1000 + (now.tv_nsec - time_console_active_changed.tv_nsec) / 1000000;

        if (elapsed_ms > 250) {
            printf("here 4\n");
            console_button_down = 0;
        }
    }
}

// Handles a pending close request from a remote client (SYSOP_SERVER_CMD_CONSOLE_CLOSE).
// If the console is currently active, hides the overlay, releases the DMA
// lock, and clears the request flag.
void handleConsoleCloseRequest()
{
    if (console_close_requested && c64_console_active) {
        printf("Closing console due to console_close_requested\n");
        if (framebuffer_visible) {
            toggle_ui_visibility();
        }
        console_release_lock();
        c64_console_active = 0;
        console_close_requested = 0;
    }
}

