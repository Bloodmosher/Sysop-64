/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * sysop_d64.h — canonical D64/D81 disk image library for the Sysop-64 project.
 *
 * This is the single authoritative location for D64/D81 format constants,
 * the D64 track-geometry table, and disk image I/O within the Sysop-64
 * codebase.  Other modules that previously kept their own copies of the
 * track table or sector-reading logic should include this header instead.
 *
 * Design goals
 * ------------
 *  - Self-contained: no dependency on sysop64.h or the bridge layer.
 *  - In-memory model: sysop_d64_open() loads the entire image into a malloc'd
 *    buffer; all subsequent operations work from that buffer with no
 *    further I/O.  This gives O(1) random sector access and simplifies
 *    error handling.
 *  - Read-only: this library provides no write / BAM-allocation support.
 *    The full read_d64 server retains its own write layer on top.
 *  - C and C++ safe: all declarations are wrapped in extern "C" when
 *    compiled as C++ so callers in either language can include the header.
 *
 * Supported formats
 * -----------------
 *  D64  Standard 35-track Commodore 1541 image (174 848 bytes).
 *       Extended 40-track variants are also handled transparently.
 *  D81  Commodore 1581 double-sided DD image (819 200 bytes).
 *
 * Quick start
 * -----------
 *   D64Image img;
 *   if (!sysop_d64_open(&img, "game.d64")) { ... handle error ... }
 *
 *   D64DirectoryEntry dir[144];
 *   int n = sysop_d64_read_directory(&img, dir, 144);
 *
 *   uint8_t prg[65536];
 *   int bytes = sysop_d64_load_file(&img, &dir[0], prg, sizeof(prg));
 *
 *   sysop_d64_close(&img);
 */

#ifndef SYSOP_D64_H
#define SYSOP_D64_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ── Format constants ──────────────────────────────────────────────────── */

/* All sectors on both D64 and D81 are exactly 256 bytes. */
#define D64_SECTOR_SIZE         256

/* D64 — directory and BAM are both on track 18. */
#define D64_DIRECTORY_TRACK     18
#define D64_DIRECTORY_SECTOR     1
#define D64_HEADER_TRACK        18      /* BAM sector doubles as the disk header */
#define D64_HEADER_SECTOR        0
#define D64_STANDARD_TRACKS     35      /* last track in a standard 1541 image */
#define D64_MAX_TRACKS          40      /* upper limit including extended tracks */

/* D81 — directory and BAM are on track 40. */
#define D81_DIRECTORY_TRACK     40
#define D81_DIRECTORY_SECTOR     3
#define D81_HEADER_TRACK        40
#define D81_HEADER_SECTOR        0
#define D81_BAM_TRACK           40
#define D81_BAM_LOW_SECTOR       1      /* BAM for tracks 1-40 */
#define D81_BAM_HIGH_SECTOR      2      /* BAM for tracks 41-80 */
#define D81_MAX_TRACKS          80
#define D81_SECTORS_PER_TRACK   40      /* constant across all D81 tracks */
#define D81_IMAGE_SIZE          819200  /* exact byte size of a standard D81 */

/*
 * d64_track_info[track-1][0] = sectors per track
 * d64_track_info[track-1][1] = byte offset of track 0 in a D64 image
 *
 * This table is the single authoritative copy for the whole Sysop-64
 * project.  Other files (read_d64_image.c, etc.) that used to embed
 * their own copy should reference this extern instead.
 */
extern const int d64_track_info[40][2];

/* ── Image type ────────────────────────────────────────────────────────── */

typedef enum {
    D64_TYPE_UNKNOWN = 0,
    D64_TYPE_D64,
    D64_TYPE_D81
} D64ImageType;

/*
 * D64Image — a fully loaded disk image.
 *
 * All fields are populated by sysop_d64_open() and should be treated as
 * read-only by callers.  The buffer is owned by this struct and must
 * be released by calling sysop_d64_close().
 */
typedef struct {
    uint8_t     *buffer;            /* raw image data, owned by this struct */
    uint64_t     buffer_size;       /* size of buffer in bytes */
    D64ImageType type;
    int          max_track;         /* highest writable track (35 for std D64) */
    int          read_max_track;    /* highest readable track (>=max_track for extended images) */
    char         name[17];          /* disk label, null-terminated, 0xA0 padding stripped */
    char         id[6];             /* 2-char disk ID + 2-char DOS type, null-terminated */
} D64Image;

/* ── Directory entry ───────────────────────────────────────────────────── */

/* CBM DOS file type codes (low nibble of directory entry byte 0x02). */
#define D64_FILETYPE_DEL    0   /* scratched / deleted */
#define D64_FILETYPE_SEQ    1
#define D64_FILETYPE_PRG    2
#define D64_FILETYPE_USR    3
#define D64_FILETYPE_REL    4

/*
 * D64DirectoryEntry — one file as listed in the disk directory.
 *
 * The filename field is null-terminated and has the CBM 0xA0 shift-space
 * padding already stripped, making it safe to use directly with strcmp().
 */
typedef struct {
    char     filename[17];          /* up to 16 PETSCII chars + null terminator */
    uint16_t size_in_sectors;       /* block count as stored in the directory */
    uint8_t  start_track;
    uint8_t  start_sector;
    uint8_t  file_type;             /* D64_FILETYPE_* (low nibble only) */
    bool     closed;                /* bit 7: 0 = open/"splat" file, 1 = properly closed */
    bool     locked;                /* bit 6: write-protected on the disk */
} D64DirectoryEntry;

/* ── Public API ────────────────────────────────────────────────────────── */

/*
 * sysop_d64_open — load a D64 or D81 disk image file into memory.
 *
 * Allocates img->buffer and populates all D64Image fields.  The caller
 * must call sysop_d64_close() when done.
 *
 * Format detection order:
 *   1. File size == D81_IMAGE_SIZE → D81
 *   2. File has ".d81" extension   → D81
 *   3. Everything else             → D64 (35-track or extended up to 40)
 *
 * Returns true on success.  On failure, prints the reason to stderr and
 * leaves *img zeroed so sysop_d64_close() is safe to call regardless.
 */
bool sysop_d64_open(D64Image *img, const char *filename);

/*
 * sysop_d64_close — free all resources held by a D64Image.
 *
 * Safe to call on a zero-initialised struct or after a failed sysop_d64_open().
 */
void sysop_d64_close(D64Image *img);

/*
 * sysop_d64_sectors_per_track — number of sectors on the given track for the
 * image's format, or 0 if the track is out of range.
 */
int sysop_d64_sectors_per_track(const D64Image *img, int track);

/*
 * sysop_d64_sector_is_valid — 1 if (track, sector) is within bounds, 0 otherwise.
 */
int sysop_d64_sector_is_valid(const D64Image *img, int track, int sector);

/*
 * sysop_d64_read_sector — return a pointer directly into the image buffer at the
 * start of the requested 256-byte sector.  The pointer is valid for the
 * lifetime of the D64Image (until sysop_d64_close()).  No copy is made.
 *
 * Returns NULL if the track/sector is out of range (does not abort).
 */
uint8_t *sysop_d64_read_sector(const D64Image *img, int track, int sector);

/*
 * sysop_d64_read_directory — read all used directory entries from the disk.
 *
 * Populates entries[] (up to max_entries) and returns the count found.
 * Scratched entries (type byte == 0) are skipped.
 * Returns -1 if the image pointer or entries pointer is NULL.
 */
int sysop_d64_read_directory(const D64Image *img, D64DirectoryEntry *entries, int max_entries);

/*
 * sysop_d64_load_file — follow the sector chain for the file described by entry
 * and copy its data into out[out_size].
 *
 * CBM DOS sector chain convention:
 *   - Each sector begins with two link bytes: [next_track, next_sector].
 *   - If next_track == 0, this is the last sector; next_sector is then the
 *     1-based index of the last valid data byte (so valid bytes = next_sector - 1).
 *   - Otherwise all 254 bytes (offsets 2..255) are valid data.
 *
 * Returns the number of bytes loaded, or -1 on error or buffer overflow.
 */
int sysop_d64_load_file(const D64Image *img, const D64DirectoryEntry *entry,
                  uint8_t *out, int out_size);

#ifdef __cplusplus
}
#endif

#endif /* SYSOP_D64_H */
