/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/**
 * Reads screenshot data, decodes it using a fixed color palette,
 * and saves it as a PNG file.
 *
 * This program simulates reading 32-bit words from a device, where each
 * word contains 8 pixels encoded as 4-bit palette indices. It then maps
 * these indices to a predefined color palette and writes the resulting
 * image to a PNG file using the libpng library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <png.h>
#include "sysop64.h"

// Define screen and data properties
#define TOTAL_WORDS 16536
#define WORDS_PER_LINE 53
#define PIXELS_PER_WORD 8

// Calculate image dimensions
#define IMAGE_WIDTH (WORDS_PER_LINE * PIXELS_PER_WORD) // 53 * 8 = 424 pixels
#define IMAGE_HEIGHT (TOTAL_WORDS / WORDS_PER_LINE)   // 16536 / 53 = 312 pixels

// Structure to hold an RGB color
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} color_t;

// The color palette, as defined in the Verilog code
const color_t palette[16] = {
    {  0,   0,   0}, // PALETTE_COLOR_0_RGB
    {255, 255, 255}, // PALETTE_COLOR_1_RGB
    {190,  26,  36}, // PALETTE_COLOR_2_RGB
    { 48, 230, 198}, // PALETTE_COLOR_3_RGB
    {180,  26, 226}, // PALETTE_COLOR_4_RGB
    { 31, 210,  30}, // PALETTE_COLOR_5_RGB
    { 33,  27, 174}, // PALETTE_COLOR_6_RGB
    {223, 246,  10}, // PALETTE_COLOR_7_RGB
    {184,  65,   4}, // PALETTE_COLOR_8_RGB
    {106,  51,   4}, // PALETTE_COLOR_9_RGB
    {254,  74,  87}, // PALETTE_COLOR_10_RGB
    { 66,  69,  64}, // PALETTE_COLOR_11_RGB
    {112, 116, 111}, // PALETTE_COLOR_12_RGB
    { 89, 254,  89}, // PALETTE_COLOR_13_RGB
    { 85,  83, 254}, // PALETTE_COLOR_14_RGB
    {164, 167, 162}  // PALETTE_COLOR_15_RGB
};

/**
 * Writes image data to a PNG file.
 */
int write_png_file(const char *filename, int width, int height, const uint8_t *buffer) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "Error: Could not open file %s for writing.\n", filename);
        return 1;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        fprintf(stderr, "Error: png_create_write_struct failed.\n");
        fclose(fp);
        return 1;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        fprintf(stderr, "Error: png_create_info_struct failed.\n");
        png_destroy_write_struct(&png_ptr, NULL);
        fclose(fp);
        return 1;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        fprintf(stderr, "Error: An error occurred during PNG creation.\n");
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return 1;
    }

    png_init_io(png_ptr, fp);

    // Set PNG header information
    png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    // Create row pointers
    png_bytep *row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * height);
    for (int y = 0; y < height; y++) {
        row_pointers[y] = (png_bytep)(buffer + y * width * 4);
    }

    // Write the image data
    png_write_image(png_ptr, row_pointers);
    png_write_end(png_ptr, NULL);

    // Cleanup
    free(row_pointers);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);

    printf("Successfully created PNG: %s\n", filename);
    return 0;
}


/**
 * Performs bilinear interpolation on RGBA image data.
 */
void bilinear_sample(const uint8_t *src, int src_width, int src_height, double x, double y, uint8_t *out) {
    // Clamp coordinates to valid range
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= src_width - 1) x = src_width - 1.001;
    if (y >= src_height - 1) y = src_height - 1.001;

    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    // Ensure we don't go out of bounds
    if (x1 >= src_width) x1 = src_width - 1;
    if (y1 >= src_height) y1 = src_height - 1;

    double fx = x - x0;
    double fy = y - y0;

    // Get the four surrounding pixels
    const uint8_t *p00 = src + (y0 * src_width + x0) * 4;
    const uint8_t *p10 = src + (y0 * src_width + x1) * 4;
    const uint8_t *p01 = src + (y1 * src_width + x0) * 4;
    const uint8_t *p11 = src + (y1 * src_width + x1) * 4;

    // Interpolate each channel
    for (int c = 0; c < 4; c++) {
        double top = p00[c] * (1 - fx) + p10[c] * fx;
        double bottom = p01[c] * (1 - fx) + p11[c] * fx;
        out[c] = (uint8_t)(top * (1 - fy) + bottom * fy);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Expected argument <filename> [start-line] [width] [height]\n");
        printf("  If only width is specified, height is calculated to preserve aspect ratio.\n");
        return 1;
    }

    int start_line = 0;
    if (argc > 2) {
        start_line = atoi(argv[2]);
        if (start_line < 0 || start_line >= IMAGE_HEIGHT) {
            fprintf(stderr, "Warning: Start line %d is out of bounds [0, %d]. Defaulting to 0.\n", start_line, IMAGE_HEIGHT - 1);
            start_line = 0;
        }
    }
    printf("Using starting line: %d\n", start_line);

    int scaled_width = IMAGE_WIDTH;
    int scaled_height = IMAGE_HEIGHT;
    
    if (argc > 3) {
        scaled_width = atoi(argv[3]);
        if (scaled_width <= 0 || scaled_width > 10000) {
            fprintf(stderr, "Warning: Width %d is out of bounds (0, 10000]. Using original width %d.\n", scaled_width, IMAGE_WIDTH);
            scaled_width = IMAGE_WIDTH;
        }
        
        if (argc > 4) {
            // Both width and height specified
            scaled_height = atoi(argv[4]);
            if (scaled_height <= 0 || scaled_height > 10000) {
                fprintf(stderr, "Warning: Height %d is out of bounds (0, 10000]. Calculating from aspect ratio.\n", scaled_height);
                scaled_height = (int)((double)scaled_width * IMAGE_HEIGHT / IMAGE_WIDTH);
            }
        } else {
            // Only width specified, preserve aspect ratio
            scaled_height = (int)((double)scaled_width * IMAGE_HEIGHT / IMAGE_WIDTH);
        }
    }
    
    printf("Output dimensions: %dx%d\n", scaled_width, scaled_height);

    // 1. Allocate a buffer to hold all the raw 32-bit words from the device
    uint32_t *raw_buffer = (uint32_t *)malloc(TOTAL_WORDS * sizeof(uint32_t));
    if (!raw_buffer) {
        fprintf(stderr, "Error: Failed to allocate memory for raw data buffer.\n");
        return 1;
    }

    sysop_init();
    printf("Requesting screenshot\n");
    sysop_screenshot_request();
    while (sysop_screenshot_status())
    {
        printf("Waiting...\n");
        usleep(10000);
    }

    // 2. Read all data into the buffer by repeatedly calling the read function
    printf("Reading screenshot data...\n");
    for (uint16_t addr = 0; addr < TOTAL_WORDS; addr++) {
        raw_buffer[addr] = sysop_screenshot_read(addr);
        //if (addr != 0 && addr % 16 == 0) printf("\n");
    }
    printf("Data reading complete.\n");

    // 3. First create an unscaled RGBA image buffer
    uint8_t *unscaled_buffer = (uint8_t *)malloc((size_t)IMAGE_WIDTH * IMAGE_HEIGHT * 4 * sizeof(uint8_t));
    if (!unscaled_buffer) {
        fprintf(stderr, "Error: Failed to allocate memory for unscaled RGBA buffer.\n");
        free(raw_buffer);
        return 1;
    }

    // 4. Process the raw data and convert it to RGBA format (unscaled)
    printf("Converting data to image format...\n");
    for (int y = 0; y < IMAGE_HEIGHT; y++) {
        // Calculate the source line number, wrapping around if necessary
        int source_y = (start_line + y) % IMAGE_HEIGHT;

        for (int x_word = 0; x_word < WORDS_PER_LINE; x_word++) {
            // Get the current 32-bit word from the calculated source line
            int word_index = source_y * WORDS_PER_LINE + x_word;
            uint32_t current_word = raw_buffer[word_index];

            // Extract 8 pixels from this word
            for (int p = 0; p < PIXELS_PER_WORD; p++) {
                uint8_t palette_index = (current_word >> (4 * p)) & 0x0F;
                color_t color = palette[palette_index];
                
                int pixel_x = x_word * PIXELS_PER_WORD + p;
                int pixel_index = (y * IMAGE_WIDTH + pixel_x) * 4;

                unscaled_buffer[pixel_index + 0] = color.r;
                unscaled_buffer[pixel_index + 1] = color.g;
                unscaled_buffer[pixel_index + 2] = color.b;
                unscaled_buffer[pixel_index + 3] = 255;
            }
        }
    }
    printf("Conversion complete.\n");

    // 5. Allocate buffer for scaled image
    uint8_t *rgba_buffer = (uint8_t *)malloc((size_t)scaled_width * scaled_height * 4 * sizeof(uint8_t));
    if (!rgba_buffer) {
        fprintf(stderr, "Error: Failed to allocate memory for scaled RGBA buffer.\n");
        free(raw_buffer);
        free(unscaled_buffer);
        return 1;
    }

    // 6. Perform scaling with bilinear interpolation
    printf("Scaling image to %dx%d...\n", scaled_width, scaled_height);
    
    double scale_x = (double)IMAGE_WIDTH / scaled_width;
    double scale_y = (double)IMAGE_HEIGHT / scaled_height;
    
    for (int dest_y = 0; dest_y < scaled_height; dest_y++) {
        for (int dest_x = 0; dest_x < scaled_width; dest_x++) {
            // Map destination coordinates back to source coordinates
            double src_x = (dest_x + 0.5) * scale_x - 0.5;
            double src_y = (dest_y + 0.5) * scale_y - 0.5;
            
            int dest_pixel_index = (dest_y * scaled_width + dest_x) * 4;
            bilinear_sample(unscaled_buffer, IMAGE_WIDTH, IMAGE_HEIGHT, src_x, src_y, 
                          &rgba_buffer[dest_pixel_index]);
        }
    }
    printf("Scaling complete.\n");


    // 7. Write the RGBA buffer to a PNG file
    if (write_png_file(argv[1], scaled_width, scaled_height, rgba_buffer) != 0) {
        fprintf(stderr, "Error: Failed to write PNG file.\n");
    }

    // 8. Clean up allocated memory
    free(raw_buffer);
    free(unscaled_buffer);
    free(rgba_buffer);

    return 0;
}
