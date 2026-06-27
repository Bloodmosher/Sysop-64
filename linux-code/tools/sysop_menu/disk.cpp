/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * disk.cpp — D64 disk image support for sysop_menu.
 *
 * This file provides two entry points used by the rest of sysop_menu:
 *
 *   d64_load(d64filename, filename)
 *       Opens a D64/D81 image, finds the named file in the directory
 *       (or the first PRG if filename is "*"), loads its sector chain
 *       into memory, and hands it to the C64 via sysop_load_buffer() + DMA.
 *
 *   get_items_from_d64(path, list)
 *       Opens a D64/D81 image and returns the directory contents as a
 *       list of filename strings for display in the file browser.
 *
 * All disk format details (track table, sector geometry, directory
 * layout, sector chain following) are handled by the d64 library in
 * libsysop64 (include/sysop_d64.h, src/sysop_d64.c).  This file contains no
 * format-specific constants or byte-offset magic.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <list>
#include "sysop64.h"
#include "sysop_d64.h"

/*
 * d64_load — load a named file from a D64/D81 image and DMA it to the C64.
 *
 * d64filename : path to the .d64 or .d81 image file on the host.
 * filename    : CBM filename to look up in the directory, or "*" to load
 *               the first PRG file found.
 *
 * Matching is case-sensitive (CBM filenames are PETSCII uppercase).
 * The loaded data is passed to sysop_load_buffer() which streams it to the C64
 * via DMA.  Returns 0 on success, non-zero on error.
 */
int d64_load(const char *d64filename, const char *filename)
{
    printf("d64_load: opening '%s', looking for '%s'\n", d64filename, filename);

    D64Image img;
    if (!sysop_d64_open(&img, d64filename)) {
        fprintf(stderr, "d64_load: failed to open '%s'\n", d64filename);
        return 1;
    }

    /* Read the full directory (up to 144 entries — the maximum for a D64). */
    D64DirectoryEntry dir[144];
    int num_entries = sysop_d64_read_directory(&img, dir, 144);
    if (num_entries < 0) {
        fprintf(stderr, "d64_load: failed to read directory of '%s'\n", d64filename);
        sysop_d64_close(&img);
        return 1;
    }

    /* Find the requested file. "*" matches the first PRG in the directory. */
    const D64DirectoryEntry *found = NULL;
    int wildcard = (strcmp(filename, "*") == 0);

    for (int i = 0; i < num_entries && !found; i++) {
        if (wildcard) {
            if (dir[i].file_type == D64_FILETYPE_PRG && dir[i].closed)
                found = &dir[i];
        } else {
            if (strcmp(filename, dir[i].filename) == 0)
                found = &dir[i];
        }
    }

    if (!found) {
        fprintf(stderr, "d64_load: '%s' not found in '%s'\n", filename, d64filename);
        sysop_d64_close(&img);
        return 1;
    }

    printf("d64_load: found '%s' (%u blocks), loading...\n",
           found->filename, found->size_in_sectors);

    /*
     * Allocate a buffer large enough for the worst case (all sectors full).
     * The actual byte count returned by sysop_d64_load_file() may be smaller
     * because the last sector is not always completely full.
     */
    int max_bytes = found->size_in_sectors * (D64_SECTOR_SIZE - 2);
    uint8_t *data = (uint8_t *)malloc(max_bytes);
    if (!data) {
        fprintf(stderr, "d64_load: out of memory (%d bytes)\n", max_bytes);
        sysop_d64_close(&img);
        return 1;
    }

    int bytes = sysop_d64_load_file(&img, found, data, max_bytes);
    if (bytes < 0) {
        fprintf(stderr, "d64_load: error reading '%s'\n", found->filename);
        free(data);
        sysop_d64_close(&img);
        return 1;
    }

    printf("d64_load: loaded %d bytes, sending to C64...\n", bytes);
    sysop_load_buffer(data, bytes);

    free(data);
    sysop_d64_close(&img);

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    printf("d64_load: done\n");
    return 0;
}

/*
 * get_items_from_d64 — return the directory contents of a D64/D81 image
 * as a list of filename strings.
 *
 * Used by the file browser (file_browser.cpp) to populate the on-screen
 * directory listing when the user browses into a .d64 file.
 *
 * Returns 0 on success, -1 if the image could not be opened.
 */
int get_items_from_d64(const std::string &path, std::list<std::string> &list)
{
    D64Image img;
    if (!sysop_d64_open(&img, path.c_str())) {
        fprintf(stderr, "get_items_from_d64: failed to open '%s'\n", path.c_str());
        return -1;
    }

    D64DirectoryEntry dir[144];
    int num_entries = sysop_d64_read_directory(&img, dir, 144);

    if (num_entries < 0) {
        fprintf(stderr, "get_items_from_d64: failed to read directory of '%s'\n",
                path.c_str());
        sysop_d64_close(&img);
        return -1;
    }

    for (int i = 0; i < num_entries; i++) {
        printf("get_items_from_d64: '%s' (%u blocks)\n",
               dir[i].filename, dir[i].size_in_sectors);
        list.push_back(dir[i].filename);
    }

    sysop_d64_close(&img);
    return 0;
}
