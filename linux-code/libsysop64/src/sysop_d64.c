/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * sysop_d64.c — D64/D81 disk image library implementation.
 *
 * See sysop_d64.h for the full API documentation and design rationale.
 *
 * D64 disk structure summary
 * --------------------------
 * A D64 image is a flat byte stream of all sectors, ordered by track
 * then sector (track 1 sector 0, track 1 sector 1, ..., track 35 sector 16,
 * track 18 sector 0 = BAM/header, track 18 sector 1 = first directory
 * sector, ...).  Each sector is exactly 256 bytes.
 *
 * Sectors per track vary: tracks 1–17 have 21, 18–24 have 19, 25–30 have
 * 18, and 31–35 (or 40 for extended images) have 17 sectors.  The
 * d64_track_info table encodes both the sector count and the byte offset
 * of each track's first sector within the image.
 *
 * Every sector in a file chain begins with two link bytes:
 *   byte[0] = next track  (0 if this is the last sector)
 *   byte[1] = next sector (if track==0: 1-based index of last valid byte)
 * Data starts at byte[2].
 *
 * D81 disk structure summary
 * --------------------------
 * A D81 image is simpler: all 80 tracks have exactly 40 sectors each.
 * The byte offset of (track, sector) is ((track-1)*40 + sector) * 256.
 * The directory is on track 40, starting at sector 3.  BAM sectors are
 * at track 40 sectors 1 and 2.
 */

#include "sysop_d64.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Track geometry table (single authoritative copy) ──────────────────── */

/*
 * D64 per-track data: [sectors_per_track, byte_offset_of_first_sector].
 *
 * Zone layout for a standard 35-track D64:
 *   Tracks  1–17: 21 sectors/track  (speed zone 3 — fastest)
 *   Tracks 18–24: 19 sectors/track  (speed zone 2)
 *   Tracks 25–30: 18 sectors/track  (speed zone 1)
 *   Tracks 31–40: 17 sectors/track  (speed zone 0 — slowest)
 *
 * Tracks 36–40 appear only in extended D64 images; their offsets continue
 * the pattern and are valid entries in this table.
 */
const int d64_track_info[40][2] = {
    {21, 0x00000},  /* track  1 */
    {21, 0x01500},  /* track  2 */
    {21, 0x02A00},  /* track  3 */
    {21, 0x03F00},  /* track  4 */
    {21, 0x05400},  /* track  5 */
    {21, 0x06900},  /* track  6 */
    {21, 0x07E00},  /* track  7 */
    {21, 0x09300},  /* track  8 */
    {21, 0x0A800},  /* track  9 */
    {21, 0x0BD00},  /* track 10 */
    {21, 0x0D200},  /* track 11 */
    {21, 0x0E700},  /* track 12 */
    {21, 0x0FC00},  /* track 13 */
    {21, 0x11100},  /* track 14 */
    {21, 0x12600},  /* track 15 */
    {21, 0x13B00},  /* track 16 */
    {21, 0x15000},  /* track 17 */
    {19, 0x16500},  /* track 18 — directory track / BAM header */
    {19, 0x17800},  /* track 19 */
    {19, 0x18B00},  /* track 20 */
    {19, 0x19E00},  /* track 21 */
    {19, 0x1B100},  /* track 22 */
    {19, 0x1C400},  /* track 23 */
    {19, 0x1D700},  /* track 24 */
    {18, 0x1EA00},  /* track 25 */
    {18, 0x1FC00},  /* track 26 */
    {18, 0x20E00},  /* track 27 */
    {18, 0x22000},  /* track 28 */
    {18, 0x23200},  /* track 29 */
    {18, 0x24400},  /* track 30 */
    {17, 0x25600},  /* track 31 */
    {17, 0x26700},  /* track 32 */
    {17, 0x27800},  /* track 33 */
    {17, 0x28900},  /* track 34 */
    {17, 0x29A00},  /* track 35 — last track in a standard 1541 image */
    {17, 0x2AB00},  /* track 36 — extended D64 only */
    {17, 0x2BC00},  /* track 37 */
    {17, 0x2CD00},  /* track 38 */
    {17, 0x2DE00},  /* track 39 */
    {17, 0x2EF00}   /* track 40 */
};

/* ── Internal helpers ──────────────────────────────────────────────────── */

/* Case-insensitive ASCII character comparison (avoids locale dependency). */
static int ch_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/*
 * has_extension — check whether filename ends with the given extension
 * (including the leading dot) using a case-insensitive ASCII comparison.
 * e.g. has_extension("game.D81", ".d81") returns 1.
 */
static int has_extension(const char *filename, const char *ext)
{
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    while (*dot && *ext) {
        if (ch_lower((unsigned char)*dot) != ch_lower((unsigned char)*ext))
            return 0;
        dot++;
        ext++;
    }
    return *dot == '\0' && *ext == '\0';
}

/*
 * detect_d64_max_track — infer the highest readable track from the image
 * size by walking the track table from the end.
 *
 * Handles:
 *   - Standard 35-track images (174 848 bytes)
 *   - Extended 40-track images (196 608 bytes)
 *   - Truncated images (returns the highest complete track)
 *   - Error-info-appended images (196 608 + 683 bytes, etc.) — the extra
 *     bytes are simply ignored because the track-end check is >=.
 */
static int detect_d64_max_track(uint64_t size)
{
    for (int track = D64_MAX_TRACKS; track >= 1; track--) {
        uint64_t track_end = (uint64_t)d64_track_info[track - 1][1] +
                             (uint64_t)d64_track_info[track - 1][0] * D64_SECTOR_SIZE;
        if (size >= track_end)
            return track;
    }
    return D64_STANDARD_TRACKS;
}

/*
 * sector_byte_offset — compute the byte position of a sector within the
 * flat image buffer.  Callers must have already validated (track, sector).
 */
static int sector_byte_offset(const D64Image *img, int track, int sector)
{
    if (img->type == D64_TYPE_D81)
        return ((track - 1) * D81_SECTORS_PER_TRACK + sector) * D64_SECTOR_SIZE;

    /* D64: use the precomputed per-track offset from the geometry table. */
    return d64_track_info[track - 1][1] + sector * D64_SECTOR_SIZE;
}

/* ── Public API ────────────────────────────────────────────────────────── */

int sysop_d64_sectors_per_track(const D64Image *img, int track)
{
    if (!img || track < 1 || track > img->read_max_track) return 0;
    if (img->type == D64_TYPE_D81) return D81_SECTORS_PER_TRACK;
    return d64_track_info[track - 1][0];
}

int sysop_d64_sector_is_valid(const D64Image *img, int track, int sector)
{
    int spt = sysop_d64_sectors_per_track(img, track);
    return spt > 0 && sector >= 0 && sector < spt;
}

uint8_t *sysop_d64_read_sector(const D64Image *img, int track, int sector)
{
    if (!img || !img->buffer) return NULL;
    if (!sysop_d64_sector_is_valid(img, track, sector)) return NULL;

    int offset = sector_byte_offset(img, track, sector);
    if ((uint64_t)offset + D64_SECTOR_SIZE > img->buffer_size) return NULL;

    return &img->buffer[offset];
}

bool sysop_d64_open(D64Image *img, const char *filename)
{
    if (!img || !filename) return false;
    memset(img, 0, sizeof(*img));

    FILE *f = fopen(filename, "rb");
    if (!f) {
        perror("sysop_d64_open: fopen");
        return false;
    }

    fseek(f, 0, SEEK_END);
    long raw_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (raw_size <= 0) {
        fprintf(stderr, "sysop_d64_open: empty or unreadable file '%s'\n", filename);
        fclose(f);
        return false;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)raw_size);
    if (!buf) {
        fprintf(stderr, "sysop_d64_open: out of memory loading '%s'\n", filename);
        fclose(f);
        return false;
    }

    if (fread(buf, 1, (size_t)raw_size, f) != (size_t)raw_size) {
        fprintf(stderr, "sysop_d64_open: read error on '%s'\n", filename);
        fclose(f);
        free(buf);
        return false;
    }
    fclose(f);

    img->buffer      = buf;
    img->buffer_size = (uint64_t)raw_size;

    /*
     * Detect format.  D81 images are exactly 819 200 bytes or carry a
     * ".d81" extension (case-insensitive).  Everything else is treated as
     * D64 and the exact track count is inferred from the file size.
     */
    if ((uint64_t)raw_size == D81_IMAGE_SIZE || has_extension(filename, ".d81")) {
        img->type           = D64_TYPE_D81;
        img->max_track      = D81_MAX_TRACKS;
        img->read_max_track = D81_MAX_TRACKS;
    } else {
        img->type            = D64_TYPE_D64;
        img->read_max_track  = detect_d64_max_track((uint64_t)raw_size);
        img->max_track       = (img->read_max_track < D64_STANDARD_TRACKS)
                               ? img->read_max_track : D64_STANDARD_TRACKS;
    }

    /*
     * Read the disk name and ID from the header / BAM sector.
     *
     * D64 (T18 S0):
     *   offset 0x90: disk name (16 bytes, 0xA0-padded)
     *   offset 0xA2: disk ID   (2 bytes)
     *   offset 0xA5: DOS type  (2 bytes)
     *
     * D81 (T40 S0):
     *   offset 0x04: disk name (16 bytes, 0xA0-padded)
     *   offset 0x16: disk ID   (2 bytes)
     *   offset 0x19: DOS type  (2 bytes)
     */
    {
        int hdr_track  = (img->type == D64_TYPE_D81) ? D81_HEADER_TRACK  : D64_HEADER_TRACK;
        int hdr_sector = (img->type == D64_TYPE_D81) ? D81_HEADER_SECTOR : D64_HEADER_SECTOR;
        uint8_t *hdr = sysop_d64_read_sector(img, hdr_track, hdr_sector);
        if (hdr) {
            const uint8_t *name_ptr = (img->type == D64_TYPE_D81) ? hdr + 0x04 : hdr + 0x90;
            const uint8_t *id_ptr   = (img->type == D64_TYPE_D81) ? hdr + 0x16 : hdr + 0xa2;

            memcpy(img->name, name_ptr, 16);
            img->name[16] = '\0';
            /* Strip CBM shift-space (0xA0) padding from disk name. */
            for (int i = 0; i < 16; i++) {
                if ((uint8_t)img->name[i] == 0xA0) { img->name[i] = '\0'; break; }
            }

            memcpy(img->id, id_ptr, 5);
            img->id[5] = '\0';
        }
    }

    return true;
}

void sysop_d64_close(D64Image *img)
{
    if (!img) return;
    free(img->buffer);
    memset(img, 0, sizeof(*img));
}

int sysop_d64_read_directory(const D64Image *img, D64DirectoryEntry *entries, int max_entries)
{
    if (!img || !entries || max_entries <= 0) return -1;

    int dir_track  = (img->type == D64_TYPE_D81) ? D81_DIRECTORY_TRACK  : D64_DIRECTORY_TRACK;
    int dir_sector = (img->type == D64_TYPE_D81) ? D81_DIRECTORY_SECTOR : D64_DIRECTORY_SECTOR;
    int count = 0;

    int cur_track  = dir_track;
    int cur_sector = dir_sector;

    while (cur_track != 0 && count < max_entries) {
        uint8_t *sec = sysop_d64_read_sector(img, cur_track, cur_sector);
        if (!sec) break;

        /*
         * Each directory sector holds 8 entries of 32 bytes each.
         * The first entry in the first directory sector is special: its
         * first two bytes are the T/S link to the next directory sector
         * (shared with the entry layout since offset 0x00 overlaps with
         * the link field of the sector header).  This is harmless because
         * we only use the link bytes from the raw sector pointer, not from
         * the first entry struct.
         */
        for (int i = 0; i < 8 && count < max_entries; i++) {
            uint8_t *e = sec + i * 32;

            /*
             * Directory entry byte 0x02 is the file type byte.
             * 0x00 means the entry has been scratched (deleted) — skip it.
             */
            if (e[0x02] == 0x00) continue;

            D64DirectoryEntry *de = &entries[count++];
            de->file_type       = e[0x02] & 0x0F;   /* low nibble = type */
            de->closed          = (e[0x02] & 0x80) != 0;
            de->locked          = (e[0x02] & 0x40) != 0;
            de->start_track     = e[0x03];
            de->start_sector    = e[0x04];
            de->size_in_sectors = (uint16_t)(e[0x1E] | (e[0x1F] << 8));

            /*
             * Copy and null-terminate the filename, stripping 0xA0 padding.
             * CBM filenames are always padded to 16 chars with 0xA0; after
             * stripping, the result is a standard C string.
             */
            memcpy(de->filename, e + 0x05, 16);
            de->filename[16] = '\0';
            for (int j = 0; j < 16; j++) {
                if ((uint8_t)de->filename[j] == 0xA0) { de->filename[j] = '\0'; break; }
            }
        }

        /* Advance to the next directory sector via the T/S link in bytes 0-1. */
        cur_track  = sec[0];
        cur_sector = sec[1];
    }

    return count;
}

int sysop_d64_load_file(const D64Image *img, const D64DirectoryEntry *entry,
                  uint8_t *out, int out_size)
{
    if (!img || !entry || !out || out_size <= 0) return -1;

    int cur_track  = entry->start_track;
    int cur_sector = entry->start_sector;
    int loaded     = 0;

    while (cur_track != 0) {
        uint8_t *sec = sysop_d64_read_sector(img, cur_track, cur_sector);
        if (!sec) {
            fprintf(stderr, "sysop_d64_load_file: bad sector T%d S%d in '%s'\n",
                    cur_track, cur_sector, entry->filename);
            return -1;
        }

        int next_track  = sec[0];
        int next_sector = sec[1];

        /*
         * Data bytes per sector:
         *   - Interior sector (next_track != 0): bytes [2..255] = 254 bytes.
         *   - Last sector (next_track == 0): bytes [2..next_sector-1].
         *     The link field byte[1] in the final sector holds the 1-based
         *     index of the last valid data byte, so valid count = byte[1] - 1.
         */
        int bytes = (next_track == 0) ? (next_sector - 1) : (D64_SECTOR_SIZE - 2);
        if (bytes <= 0) break;

        if (loaded + bytes > out_size) {
            fprintf(stderr, "sysop_d64_load_file: output buffer too small for '%s' "
                    "(%d bytes loaded, need %d more, buffer is %d)\n",
                    entry->filename, loaded, bytes, out_size);
            return -1;
        }

        memcpy(out + loaded, sec + 2, bytes);
        loaded     += bytes;
        cur_track   = next_track;
        cur_sector  = next_sector;
    }

    return loaded;
}
