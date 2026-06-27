/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * Animated 3D starfield renderer using Cairo.
 *
 * Overview
 * --------
 * Eighty stars are stored in 3D space with signed 16-bit (x, y, z)
 * coordinates.  Each frame two things happen:
 *
 *   1. updateStars()  — adds the current x/y/z velocity increments to
 *                        every star's position, wrapping at the field
 *                        boundaries.
 *
 *   2. advanceStars() — advances three independent read-pointers into
 *                        g_incTable, changing the velocity slightly each
 *                        frame.  The table encodes a slow triangle wave
 *                        (0 → +8 → 0 → -8 → 0), so the stars gently
 *                        pulse and drift in a smooth oscillating pattern.
 *                        The three axes start at different phase offsets
 *                        so their motion is not in sync.
 *
 * drawStars() projects the 3D positions onto the screen using a simple
 * perspective divide (screen_x = star_x * scale / z), depth-cues each
 * star by mapping z to a grayscale colour (near = bright, far = dark),
 * and draws a small filled circle for each visible star.
 *
 * Coordinate ranges
 * -----------------
 *   x : -starfield_width  .. +starfield_width   (wraps)
 *   y : -starfield_height .. +starfield_height  (wraps)
 *   z : 64 .. 1024  (wraps; minimum 64 prevents division-by-zero)
 */

#include <stdint.h>
#include <cairo.h>
#include <math.h>

#include "stars.h"

/* Half-width and half-height of the logical star coordinate space.
 * Stars with |x| > starfield_width or |y| > starfield_height are
 * wrapped to the opposite edge so they stay inside the volume. */
int starfield_width = 384;
int starfield_height = 256;

/* Number of bytes to iterate over in g_stars when drawing.
 * Each star is 6 bytes (x_hi, x_lo, y_hi, y_lo, z_hi, z_lo).
 * Set to (num_stars * 6).  Reduce to draw fewer than all 80 stars. */
unsigned long g_drawCount = 80*6; // g_stars.length; there are 80 so 80*6

/* Current byte-offsets into g_incTable for each axis.
 * Each offset selects one signed 16-bit increment value (2 bytes,
 * big-endian) that is added to every star's coordinate that frame.
 * The three axes start at different positions so their phases are
 * offset from each other, producing non-uniform motion. */
unsigned long x_inc_offset = 980;
unsigned long y_inc_offset = 1492;
unsigned long z_inc_offset = 1748;

/* Saved initial offsets (not currently used at runtime; kept for
 * reference or future reset-to-start functionality). */
unsigned long x_inc_offset_base = 980;
unsigned long y_inc_offset_base = 1492;
unsigned long z_inc_offset_base = 1748;

/* Direction each offset advances through g_incTable: 1 = forward,
 * 0 (or any non-1) = backward.  Currently all three run forward.
 * Reversing a direction would make the table play in reverse,
 * effectively negating the sign of the speed change each frame. */
uint8_t x_inc_dir = 1;
uint8_t y_inc_dir = 1;
uint8_t z_inc_dir = 1;

/* Single-star test array (unused in normal operation).
 * Format is the same 6-byte layout as g_stars: x_hi,x_lo, y_hi,y_lo, z_hi,z_lo. */
uint8_t g_stars2[] = {0x00,0x00,0x00,0xff,0x01,0x6f};

/* Grayscale colour palette indexed by depth bucket (8 entries, R/G/B each).
 *
 * drawStars() maps a star's z coordinate to a colour index:
 *   colorIndex = 7 - floor(z / 128)
 * With z in [64, 1024]:
 *   z ≈  64  → colorIndex 7 → white  (nearest, brightest)
 *   z ≈ 128  → colorIndex 6 → light gray
 *   ...                               (fades with distance)
 *   z ≈ 896  → colorIndex 1 → very dark
 *   z ≈ 1024 → colorIndex 0 → black  (farthest, invisible)
 *
 * Index 7 (last entry) is also black — this catches any out-of-range
 * value and renders the star invisible rather than crashing.
 *
 * To change star colours, replace the R/G/B triplets.  All three
 * channels are equal here for grayscale; they can differ for coloured
 * starfields (e.g. blue-tinted distant stars). */
float g_starColors[] = {
    0.0f,    0.0f,    0.0f,    // index 0 — farthest  (black)
    0.066f,  0.066f,  0.066f, // index 1
    // 0xff,0xff,0xff,  // omitted
    0.2f,    0.2f,    0.2f,   // index 2
    0.333f,  0.333f,  0.333f, // index 3
    0.533f,  0.533f,  0.533f, // index 4
    0.667f,  0.667f,  0.667f, // index 5
    1.0f,    1.0f,    1.0f,   // index 6 — nearest    (white)
    0.0f,    0.0f,    0.0f    // index 7 — out-of-range guard (black)
};

/* Star position data — 80 stars, 6 bytes each.
 *
 * Byte layout per star (big-endian signed 16-bit integers):
 *   [0] x_hi  [1] x_lo   — x coordinate, range roughly -384..+384
 *   [2] y_hi  [3] y_lo   — y coordinate, range roughly -256..+256
 *   [4] z_hi  [5] z_lo   — z (depth),    range 64..1024
 *
 * This array is mutated in-place every frame by updateStars().
 * To change the starting configuration, replace the data or generate
 * new values at startup with random positions in the valid ranges. */
uint8_t g_stars[] =
{
	0xfe,0xa7,0x00,0xcc,0x01,0x6f,0xfe,0xd3,0xff,0x11,0x02,0x0a,0x00,0x1a,0x00,0x22
	,0x02,0xec,0x00,0x0d,0x00,0xae,0x00,0xaf,0x01,0x6e,0x00,0x30,0x03,0x4a,0x00,0x11
	,0x00,0xa2,0x01,0xf7,0x00,0xb5,0x00,0x6a,0x02,0xb8,0x01,0x22,0x00,0x4e,0x03,0x97
	,0xff,0xc6,0xff,0x3e,0x01,0x5f,0x00,0x87,0xff,0x0a,0x01,0x51,0xff,0x52,0xff,0x38
	,0x03,0xcf,0xff,0x95,0x00,0xd4,0x03,0x94,0x00,0xa0,0x00,0x89,0x02,0x4c,0xfe,0xcc
	,0x00,0xed,0x03,0x5a,0xfe,0xfd,0xff,0x3e,0x01,0x9a,0x00,0x8a,0xff,0xc4,0x02,0x58
	,0x00,0x9b,0xff,0xd8,0x00,0xc5,0xff,0x4e,0xff,0xee,0x02,0x93,0x00,0xc1,0xff,0x15
	,0x01,0xf7,0x01,0x71,0x00,0x9f,0x02,0x86,0x00,0x3d,0x00,0xfa,0x02,0x20,0x01,0x14
	,0xff,0x5d,0x02,0x35,0xff,0x9b,0x00,0x38,0x01,0xc3,0x00,0x9b,0xff,0x75,0x01,0x0f
	,0x00,0xed,0x00,0xb1,0x02,0x32,0x00,0x9a,0x00,0x26,0x01,0x7a,0xff,0xf8,0x00,0xc5
	,0x02,0x44,0x01,0x34,0x00,0x69,0x00,0xb4,0xfe,0xc6,0x00,0x52,0x03,0xf5,0xff,0xc5
	,0x00,0xaf,0x01,0xb4,0xff,0x8b,0x00,0x9c,0x01,0xe0,0x00,0xa8,0xff,0x2d,0x02,0x6b
	,0x00,0x57,0x00,0xc6,0x03,0x31,0xff,0x88,0xff,0x0c,0x01,0x0f,0x00,0x44,0xff,0x08
	,0x01,0xb0,0x00,0x52,0xff,0x41,0x02,0x65,0xfe,0xdc,0x00,0x03,0x00,0xbe,0xff,0xbc
	,0xff,0x41,0x03,0x9e,0x00,0x5b,0xff,0x53,0x02,0xb4,0xff,0x2c,0xff,0x68,0x03,0x93
	,0x00,0xb7,0x00,0x67,0x02,0x67,0x01,0x58,0x00,0x67,0x01,0xac,0x00,0xaa,0x00,0x8a
	,0x01,0xdb,0xff,0x08,0x00,0x0e,0x01,0x9d,0xff,0x15,0xff,0x3e,0x01,0x38,0x01,0x5c
	,0x00,0xed,0x01,0xe4,0x01,0x4f,0xff,0xb6,0x01,0xe4,0x00,0x35,0xff,0x94,0x02,0xf8
	,0x00,0xbe,0xff,0x1c,0x02,0xd2,0x01,0x36,0xff,0xf5,0x00,0xd4,0x01,0x43,0xff,0x0c
	,0x00,0xb7,0xff,0x19,0xff,0xd7,0x00,0x51,0x01,0x01,0x00,0x8a,0x00,0x64,0x01,0x03
	,0xff,0x89,0x01,0xa0,0x01,0x40,0x00,0x00,0x02,0xbe,0xff,0xff,0xff,0x95,0x00,0x89
	,0x01,0x75,0xff,0x62,0x01,0xff,0x00,0x5b,0x00,0xa7,0x03,0xdc,0xfe,0xe0,0xff,0x74
	,0x02,0x40,0xfe,0xc1,0xff,0x9e,0x02,0x8b,0xff,0x28,0x00,0xda,0x02,0xcb,0xff,0x88
	,0xff,0xd6,0x02,0x11,0x00,0xa0,0xff,0x4a,0x02,0x0d,0x01,0x77,0x00,0x95,0x02,0x38
	,0x00,0x8d,0xff,0x8e,0x01,0x74,0x00,0x51,0x00,0x1a,0x01,0x5b,0x00,0x0b,0x00,0xde
	,0x02,0x69,0x00,0x80,0x00,0xdb,0x00,0xb0,0x01,0x54,0xff,0xf6,0x03,0x09,0x01,0x7b
	,0x00,0x8b,0x00,0x8c,0x00,0x41,0xff,0x08,0x02,0x5a,0xff,0x14,0xff,0x93,0x01,0xde
	,0x00,0x7a,0xff,0xbc,0x02,0xd3,0xff,0x4d,0x00,0xcb,0x03,0x01,0xff,0xe1,0xff,0x85
	,0x03,0xc4,0x00,0xac,0x00,0xff,0x01,0x5a,0xff,0x38,0x00,0x1e,0x00,0xc4,0xff,0x51
    ,0xff,0xf5,0x01,0x07,0x00,0x33,0x00,0xbf,0x01,0x58,0xff,0x0f,0x00,0x9b,0x00,0x7c
};

/*
 * Velocity increment look-up table — generated at startup by initIncTable().
 * Triangle wave (0 → +8 → 0 → -8 → 0), each step held for 32 big-endian
 * int16_t pairs (64 bytes).  32 steps × 64 bytes = 2048 bytes total.
 */
uint8_t g_incTable[2048];

/* Fill g_incTable with the triangle wave at startup. */
void initIncTable(void)
{
    /* Triangle wave 0 → +8 → 0 → -8 → 0, each step held for 32 pairs
     * (64 bytes).  Entries are signed 16-bit big-endian values. */
    static const int16_t wave[] = {
         0, 1, 2, 3, 4, 5, 6, 7, 8,
         7, 6, 5, 4, 3, 2, 1,
         0,  /* zero crossing */
        -1, -2, -3, -4, -5, -6, -7, -8,
        -7, -6, -5, -4, -3, -2, -1
    };
    int out = 0;
    for (int i = 0; i < (int)(sizeof(wave) / sizeof(wave[0])); i++) {
        uint16_t uv = (uint16_t)wave[i];
        uint8_t  hi = (uint8_t)(uv >> 8);
        uint8_t  lo = (uint8_t)(uv & 0xff);
        for (int j = 0; j < 32; j++) {
            g_incTable[out++] = hi;
            g_incTable[out++] = lo;
        }
    }
}

    
/* updateStars() — move every star by the current velocity increment.
 *
 * Reads one signed 16-bit velocity for each axis from g_incTable at
 * the current offset, doubles it (so peak speed is ±16 units/frame),
 * then adds it to every star's coordinate.
 *
 * Wrapping logic:
 *   x: if x > +starfield_width  → x -= starfield_width*2
 *      if x < -starfield_width  → x += starfield_width*2
 *   y: same pattern with starfield_height
 *   z: if z > 1024 → z -= 960  (keeps z in roughly 64..1024)
 *      if z <   64 → z += 960
 *   The z wrap range (960 = 1024-64) ensures z never reaches 0,
 *   which would cause a division-by-zero in drawStars().
 *
 * Call once per frame BEFORE drawStars(). */
void updateStars()
{
    /* Read current per-axis velocity from the wave table (big-endian 16-bit). */
    int16_t x_inc = (int16_t)(g_incTable[x_inc_offset] << 8 | g_incTable[x_inc_offset+1]);
    int16_t y_inc = (int16_t)(g_incTable[y_inc_offset] << 8 | g_incTable[y_inc_offset+1]);
    int16_t z_inc = (int16_t)(g_incTable[z_inc_offset] << 8 | g_incTable[z_inc_offset+1]);

    /* Double the increment so the effective speed range is ±16 rather than ±8. */
    x_inc *= 2;
    y_inc *= 2;
    z_inc *= 2;
    
    int i;
    for (i=0;i<sizeof(g_stars);i+=6) 
    {
        int16_t x = (int16_t)(g_stars[i] << 8 | g_stars[i+1]);
        int16_t y = (int16_t)(g_stars[i+2] << 8 | g_stars[i+3]);
        int16_t z = (int16_t)(g_stars[i+4] << 8 | g_stars[i+5]);

        int16_t oldx = x;
        int16_t oldy = y;
        int16_t oldz = z;

        x += x_inc;
        if (x > starfield_width) 
        {
            x = x - (starfield_width*2);
        }
        else if (x < -starfield_width) 
        {
            x = x + (starfield_width*2);
        }
        y += y_inc;
        if (y > starfield_height) 
        {
            y = y - (starfield_height*2);
        }
        else if (y < -starfield_height) 
        {
            y = y + (starfield_height*2);
        }
        z += z_inc;
        if (z > 1024) {
            z = z - 960;
        }
        else if (z < 64) {
            z = z + 960;
        }

        g_stars[i] = (x >> 8) & 0xff;
        g_stars[i+1] = x & 0xff;

        g_stars[i+2] = (y >> 8) & 0xff;
        g_stars[i+3] = y & 0xff;

        g_stars[i+4] = (z >> 8) & 0xff;
        g_stars[i+5] = z & 0xff;
    }
}


/* advanceStars() — step the velocity wave table pointers forward.
 *
 * Each axis's offset advances by 2 bytes (one 16-bit entry) per call,
 * wrapping at the end of g_incTable.  This must be called once per
 * frame (typically after updateStars()) so the velocity slowly changes
 * each frame, producing the smooth oscillating motion.
 *
 * x/y/z_inc_dir controls the direction: 1 = forward through the table,
 * other values = backward (currently all three are 1). */
void advanceStars()
{
    x_inc_offset += (2*x_inc_dir);
    if (x_inc_offset >= sizeof(g_incTable)) {
        x_inc_offset = 0;
    }
    y_inc_offset += (2*y_inc_dir);
    if (y_inc_offset >= sizeof(g_incTable)) {
        y_inc_offset = 0;
    }
    z_inc_offset += (2*z_inc_dir);
    if (z_inc_offset >= sizeof(g_incTable)) {
        z_inc_offset = 0;
    }
}

/* drawStars() — project and render all stars using Cairo.
 *
 * Parameters:
 *   cr               — Cairo drawing context (caller manages state)
 *   framebuffer_width/height — pixel dimensions of the render target;
 *                              used to centre the projection and clip
 *   setOrClear       — reserved / unused; intended for a future mode
 *                      that erases old star positions before drawing
 *
 * Projection
 * ----------
 * A simple perspective (pinhole camera) projection:
 *   screen_x = (star_x * x_scale) / z      (x_scale = 128)
 *   screen_y = (star_y * y_scale) / z      (y_scale = 127)
 * The result is then multiplied by 3 to spread stars across the screen,
 * then offset by (center_x, center_y) to put the vanishing point at
 * the centre of the framebuffer.
 *
 * Depth cueing
 * ------------
 * colorIndex = 7 - floor(z / 128)
 *   z ≈  64 → index 7 → brightest (white)
 *   z ≈ 1024 → index ≤ 0 → darkest (black, invisible)
 * Index is clamped to [0, 7] and looked up in g_starColors.
 *
 * Stars that project outside the framebuffer bounds are skipped.
 * Each visible star is drawn as a stroked circle of radius 1.5 px. */
void drawStars(cairo_t* cr, int framebuffer_width, int framebuffer_height, int setOrClear)
{
    int center_x = framebuffer_width / 2;
    int center_y = framebuffer_height / 2;

    /* viewer_distance is defined but not used; the perspective divide
     * uses z directly, scaled by x_scale/y_scale below. */
    float viewer_distance = 1.0f;
    (void)viewer_distance;

    /* Scale factors applied before the perspective divide.
     * x_scale (128) and y_scale (127) are intentionally slightly
     * different to avoid a perfectly square projection. */
    int x_scale = 0x80;  // 128
    int y_scale = 0x7f;  // 127
    int i;

    for (i=0;i<g_drawCount;i+=6)
    {
        /* Unpack big-endian signed 16-bit coordinates. */
        int16_t x = (int16_t)(g_stars[i] << 8 | g_stars[i+1]);
        int16_t y = (int16_t)(g_stars[i+2] << 8 | g_stars[i+3]);
        int16_t z = (int16_t)(g_stars[i+4] << 8 | g_stars[i+5]);

        /* Scale then perspective-divide.  z >= 64 is guaranteed by
         * updateStars() wrapping logic, so no divide-by-zero here. */
        x *= x_scale;
        y *= y_scale;

        x /= z;
        y /= z;

        x = (int16_t)floor(x);
        y = (int16_t)floor(y);

        /* Magnify 3× to spread stars across the full display area. */
        x *= 3;
        y *= 3;

        /* Translate to screen space (origin at top-left). */
        x += center_x;
        y += center_y;

        /* Map depth to colour: near stars are bright, far are dark.
         * floor(z >> 7) == floor(z / 128), giving buckets of 128 z-units. */
        int colorIndex = floor(z>>7); // divide by 128
        colorIndex = 7-colorIndex;

        if (colorIndex < 0)
        {
            colorIndex = 0;
        }
        if (colorIndex > 7)
        {
            colorIndex = 7;
        }

        float r = g_starColors[colorIndex*3];
        float g = g_starColors[colorIndex*3+1];
        float b = g_starColors[colorIndex*3+2];

        cairo_set_source_rgba(cr, r, g, b, 1);
        cairo_set_line_width(cr, 1.0);
        double radius = 1.5;

        /* Clip: skip stars that projected outside the framebuffer. */
        if (x < 0 || x >= framebuffer_width || y < 0 || y >= framebuffer_height) {
            continue;
        }
        cairo_arc(cr, x, y, radius, 0, 2*M_PI);
        cairo_stroke(cr);
    }
}
