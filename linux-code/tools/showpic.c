/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cairo.h>
#include <time.h>
#include "sysop64.h"

cairo_surface_t *g_image = NULL;
int g_image_width = 0;

// 512MB is the start
#define MEM_ADDRESS1 0x20000000
#define MEM_ADDRESS2 0x207e9000

int framebuffer_visible = 0;
unsigned char *pFrameBuffer = NULL;
unsigned char *pFrameBuffer1 = NULL;
unsigned char *pFrameBuffer2 = NULL;

cairo_t *g_cr = NULL;
cairo_t *g_cr1 = NULL;
cairo_t *g_cr2 = NULL;

int g_framebuffer_width = 1920;
int g_framebuffer_height = 1080;

// assume 100MB for now
#define MEM_SIZE (1024 * 1024 * 100)

void clear_framebuffer()
{
    memset(pFrameBuffer, 0, (g_framebuffer_width * g_framebuffer_height * 4));
}

void update_framebuffer()
{
    if (framebuffer_visible)
    {
        printf("Rendering to frame buffer %d\n", pFrameBuffer == pFrameBuffer1 ? 1 : 2);
        printf("Rendering to context %d\n", g_cr == g_cr1 ? 1 : 2);

        cairo_t *cr = g_cr;

        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_rectangle(cr, 0, 0, g_framebuffer_width - 1, g_framebuffer_height - 1);
        cairo_fill(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

        cairo_set_source_rgba(cr, 0, 1, 0, 1);
        cairo_set_line_width(cr, 1.0);

        int x = (g_framebuffer_width / 2) - (g_image_width / 2);
        cairo_set_source_surface(cr, g_image, x, 0);
        cairo_paint(cr);

        printf("Calling sysop_framebuffer_flip\n");
        sysop_framebuffer_flip();

        pFrameBuffer = (pFrameBuffer == pFrameBuffer1 ? pFrameBuffer2 : pFrameBuffer1);
        g_cr = (g_cr == g_cr1 ? g_cr2 : g_cr1);

        printf("Drawing frame buffer now points to buffer %d\n", pFrameBuffer == pFrameBuffer1 ? 1 : 2);
        printf("Rendering context now  %d\n", g_cr == g_cr1 ? 1 : 2);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("Expected arguments: <path to image file> [seconds]\n");
        return -1;
    }

    sysop_init();

    sysop_framebuffer_lock();

    int fd = open("/dev/mem", O_RDWR | O_SYNC);

    if (fd < 0)
    {
        perror("Couldn't open /dev/mem\n");
        return -2;
    }

    pFrameBuffer1 = (unsigned char *)mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MEM_ADDRESS1);
    pFrameBuffer2 = (unsigned char *)mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MEM_ADDRESS2);

    memset(pFrameBuffer1, 0, (g_framebuffer_width * g_framebuffer_height * 4));
    memset(pFrameBuffer2, 0, (g_framebuffer_width * g_framebuffer_height * 4));

    printf("Mapped frame buffers at %p and %p\n", pFrameBuffer1, pFrameBuffer2);

    int duration = -1;
    if (argc > 2)
    {
        duration = (int)strtol(argv[2], NULL, 10);
    }

    g_image = cairo_image_surface_create_from_png(argv[1]);
    g_image_width = cairo_image_surface_get_width(g_image);

    sysop_framebuffer_hide();

    pFrameBuffer = pFrameBuffer1;
    clear_framebuffer();

    cairo_surface_t *surface1 = cairo_image_surface_create_for_data(pFrameBuffer1, CAIRO_FORMAT_ARGB32, g_framebuffer_width, g_framebuffer_height, g_framebuffer_width * 4);
    g_cr1 = cairo_create(surface1);

    cairo_surface_t *surface2 = cairo_image_surface_create_for_data(pFrameBuffer2, CAIRO_FORMAT_ARGB32, g_framebuffer_width, g_framebuffer_height, g_framebuffer_width * 4);

    g_cr2 = cairo_create(surface2);

    printf("Cairo contexts at %p and %p\n", g_cr1, g_cr2);

    g_cr = g_cr1;

    sysop_framebuffer_show();
    pFrameBuffer = pFrameBuffer2;
    g_cr = g_cr2;
    framebuffer_visible = 1;
    update_framebuffer();

    if (duration != -1)
    {
        struct timespec ts = {duration, 0}; // 2 sec, 0 nsec
        nanosleep(&ts, NULL);
        if (duration != 0)
        {
            sysop_framebuffer_hide();
        }
    }
    else
    {
        printf("Hit enter to exit\n");
        getchar();
        sysop_framebuffer_hide();
    }

    cairo_destroy(g_cr1);
    cairo_surface_destroy(surface1);

    cairo_destroy(g_cr2);
    cairo_surface_destroy(surface2);

    cairo_surface_destroy(g_image);

    munmap(pFrameBuffer1, MEM_SIZE);
    munmap(pFrameBuffer2, MEM_SIZE);
    close(fd);

    sysop_framebuffer_unlock();
    sysop_uninit();
    return 0;
}
