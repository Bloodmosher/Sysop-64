/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <cairo.h>
#include <pango/pangocairo.h>

/* Framebuffer dimensions and font size */
extern int g_framebuffer_width;
extern int g_framebuffer_height;
extern int font_size;

/* Raw framebuffer pointers (double-buffered) */
extern unsigned char *pFrameBuffer;
extern unsigned char *pFrameBuffer1;
extern unsigned char *pFrameBuffer2;

/* Cairo rendering contexts */
extern cairo_t *g_cr;
extern cairo_t *g_cr1;
extern cairo_t *g_cr2;

/* Cairo surfaces (owned by main, used for cleanup) */
extern cairo_surface_t *g_surface1;
extern cairo_surface_t *g_surface2;

/* Pango text layout objects (one per buffer) */
extern PangoLayout *g_layout;
extern PangoLayout *g_layout1;
extern PangoLayout *g_layout2;

/* Dirty flags: set when the file list display needs to be redrawn */
extern int layout1_redraw_needed;
extern int layout2_redraw_needed;
extern int *pRedrawNeeded;

/* Whether the framebuffer output is currently active */
extern int framebuffer_visible;

/* Logo image loaded from PNG */
extern cairo_surface_t *g_image;
extern int g_image_width;
extern int g_image_height;

void clear_framebuffer(void);
void flip_buffers(void);
void update_framebuffer(void);
void update(void);
void set_redraw_needed(void);

#endif /* DISPLAY_H */
