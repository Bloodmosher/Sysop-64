/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * display.cpp — double-buffered framebuffer management, Cairo/Pango
 * context state, logo eye animation, and per-frame rendering.
 */

#include <string.h>
#include <math.h>
#include <cairo.h>
#include <pango/pangocairo.h>

#include "sysop64.h"
#include "display.h"
#include "file_browser.h"
#include "stars.h"

/* ------------------------------------------------------------------ */
/* Global state declared in display.h                                  */
/* ------------------------------------------------------------------ */

int g_framebuffer_width  = 1920;
int g_framebuffer_height = 1080;
int font_size = 30;

unsigned char *pFrameBuffer  = NULL;
unsigned char *pFrameBuffer1 = NULL;
unsigned char *pFrameBuffer2 = NULL;

cairo_t *g_cr  = NULL;
cairo_t *g_cr1 = NULL;
cairo_t *g_cr2 = NULL;

cairo_surface_t *g_surface1 = NULL;
cairo_surface_t *g_surface2 = NULL;

PangoLayout *g_layout  = NULL;
PangoLayout *g_layout1 = NULL;
PangoLayout *g_layout2 = NULL;

int  layout1_redraw_needed = 0;
int  layout2_redraw_needed = 0;
int *pRedrawNeeded = &layout1_redraw_needed;

int framebuffer_visible = 0;

cairo_surface_t *g_image = NULL;
int g_image_width  = 0;
int g_image_height = 0;

/* ------------------------------------------------------------------ */
/* Module-private state                                                */
/* ------------------------------------------------------------------ */

static float eye_color  = 1.0f;
static float eye_change = 0.05f;

/* ------------------------------------------------------------------ */
/* Public helpers                                                      */
/* ------------------------------------------------------------------ */

void clear_framebuffer(void)
{
    memset(pFrameBuffer, 0, (size_t)(g_framebuffer_width * g_framebuffer_height * 4));
}

void set_redraw_needed(void)
{
    layout1_redraw_needed = 1;
    layout2_redraw_needed = 1;
}

/* ------------------------------------------------------------------ */
/* Eye animation                                                       */
/* ------------------------------------------------------------------ */

static void updateEyes(void)
{
    eye_color += eye_change;
    if (eye_color > 1.0f) {
        eye_change = -0.05f;
        eye_color  = 1.0f;
    }
    if (eye_color < 0.0f) {
        eye_change = 0.05f;
        eye_color  = 0.0f;
    }
}

static void drawEyes(cairo_t *cr)
{
    cairo_set_source_rgba(cr, 0, 0, eye_color, 1);
    cairo_set_line_width(cr, 1.0);
    double radius = 2;

    int x1 = (g_framebuffer_width / 2) - 22;
    int x2 = (g_framebuffer_width / 2) + 10;
    int y  = 75;

    cairo_arc(cr, x1, y, radius, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_arc(cr, x2, y, radius, 0, 2 * M_PI);
    cairo_fill(cr);
}

/* ------------------------------------------------------------------ */
/* Double-buffer flip                                                  */
/* ------------------------------------------------------------------ */

void flip_buffers(void)
{
    sysop_framebuffer_flip();

    pFrameBuffer = (pFrameBuffer == pFrameBuffer1) ? pFrameBuffer2 : pFrameBuffer1;
    g_cr         = (g_cr         == g_cr1)         ? g_cr2         : g_cr1;
    g_layout     = (g_layout     == g_layout1)     ? g_layout2     : g_layout1;
    pRedrawNeeded = (pRedrawNeeded == &layout1_redraw_needed)
                        ? &layout2_redraw_needed
                        : &layout1_redraw_needed;
}

/* ------------------------------------------------------------------ */
/* Per-frame render                                                    */
/* ------------------------------------------------------------------ */

void update_framebuffer(void)
{
    if (!framebuffer_visible)
        return;

    cairo_t *cr = g_cr;

    /* Clear to black */
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_rectangle(cr, 0, 0, g_framebuffer_width, g_framebuffer_height);
    cairo_fill(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_source_rgba(cr, 0, 1, 0, 1);
    cairo_set_line_width(cr, 1.0);

    drawStars(cr, g_framebuffer_width, g_framebuffer_height, 1);

    int x = (g_framebuffer_width / 2) - (g_image_width / 2);
    cairo_set_source_surface(cr, g_image, x, 0);
    cairo_paint(cr);

    drawEyes(cr);
    drawFiles(cr, *pRedrawNeeded);

    flip_buffers();
    sysop_wait_hdmi_vblank();
}

/* ------------------------------------------------------------------ */
/* Animation state advance (call once per logical frame)              */
/* ------------------------------------------------------------------ */

void update(void)
{
    updateStars();
    advanceStars();
    updateEyes();
}
