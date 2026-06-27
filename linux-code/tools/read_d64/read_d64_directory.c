/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "read_d64_private.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── DIR-mount helpers ─────────────────────────────────────────────────── */

uint8_t *cached_file_data = NULL;
int cached_file_size = 0;

void free_cached_file(void)
{
    if (cached_file_data) {
        free(cached_file_data);
        cached_file_data = NULL;
        cached_file_size = 0;
    }
}

/* Case-insensitive search for a C64 filename in a Linux directory.
   Pass 1: exact case-insensitive match against full Linux name.
   Pass 2: strip one extension, compare.
   Pass 3: compute the same 16-char uppercase C64 display name that
           dir_build_listing() would show, and compare.
   Pass 4: wildcard — if c64_name ends with '*', match the first .prg whose
           normalized display name starts with the prefix before the '*'.
           Bare "*" matches the first .prg (or first regular file if none).
   Returns 1 and sets out_path on success, 0 on failure. */
static int dir_find_file(const char *dir_path, const char *c64_name,
                         char *out_path, size_t out_size)
{
    /* Determine wildcard prefix (empty = match anything) */
    int is_wildcard = 0;
    char wc_prefix[17] = {0};
    {
        size_t nlen = strlen(c64_name);
        if (nlen > 0 && c64_name[nlen - 1] == '*') {
            is_wildcard = 1;
            size_t plen = nlen - 1;
            if (plen > 16) plen = 16;
            memcpy(wc_prefix, c64_name, plen);
            wc_prefix[plen] = '\0';
        }
    }

    DIR *d = opendir(dir_path);
    if (!d) return 0;

    struct dirent *ent;
    int found = 0;

    /* For wildcard: prefer .prg; track first match of each kind */
    char wc_prg_path[512] = {0};
    char wc_any_path[512] = {0};

    while ((ent = readdir(d)) != NULL && !found) {
        if (ent->d_name[0] == '.') continue;

        if (!is_wildcard) {
            /* Pass 1: exact match (case-insensitive) */
            if (strcasecmp(ent->d_name, c64_name) == 0) {
                snprintf(out_path, out_size, "%s/%s", dir_path, ent->d_name);
                found = 1;
                break;
            }

            /* Pass 2: strip extension and compare */
            char name_buf[256];
            strncpy(name_buf, ent->d_name, sizeof(name_buf) - 1);
            name_buf[sizeof(name_buf) - 1] = '\0';
            char *dot = strrchr(name_buf, '.');
            if (dot) {
                *dot = '\0';
                if (strcasecmp(name_buf, c64_name) == 0) {
                    snprintf(out_path, out_size, "%s/%s", dir_path, ent->d_name);
                    found = 1;
                    break;
                }
            }

            /* Pass 3: compute the C64 display name exactly as dir_build_listing()
               does (strip ext, truncate to 16, uppercase) and compare.
               This handles Linux filenames longer than 16 chars. */
            {
                const char *base = ent->d_name;
                const char *edot = strrchr(base, '.');
                int blen = edot ? (int)(edot - base) : (int)strlen(base);
                if (blen > 16) blen = 16;
                char normalized[17];
                for (int i = 0; i < blen; i++)
                    normalized[i] = (char)toupper((unsigned char)base[i]);
                normalized[blen] = '\0';
                if (strcmp(normalized, c64_name) == 0) {
                    snprintf(out_path, out_size, "%s/%s", dir_path, ent->d_name);
                    found = 1;
                    break;
                }
            }
        } else {
            /* Pass 4: wildcard — normalize name and check prefix */
            const char *base = ent->d_name;
            const char *edot = strrchr(base, '.');
            int blen = edot ? (int)(edot - base) : (int)strlen(base);
            if (blen > 16) blen = 16;
            char normalized[17];
            for (int i = 0; i < blen; i++)
                normalized[i] = (char)toupper((unsigned char)base[i]);
            normalized[blen] = '\0';

            if (strncmp(normalized, wc_prefix, strlen(wc_prefix)) == 0) {
                int is_prg = edot && (strcasecmp(edot + 1, "prg") == 0);
                if (is_prg && wc_prg_path[0] == '\0')
                    snprintf(wc_prg_path, sizeof(wc_prg_path), "%s/%s", dir_path, ent->d_name);
                else if (!is_prg && wc_any_path[0] == '\0')
                    snprintf(wc_any_path, sizeof(wc_any_path), "%s/%s", dir_path, ent->d_name);
            }
        }
    }
    closedir(d);

    if (!found && is_wildcard) {
        const char *pick = wc_prg_path[0] ? wc_prg_path : wc_any_path;
        if (pick[0]) {
            snprintf(out_path, out_size, "%s", pick);
            found = 1;
        }
    }
    return found;
}

/* Load file into cached_file_data.  No-op if already cached. */
static int dir_cache_file(const char *dir_path, const char *c64_name)
{
    if (cached_file_data != NULL) return 1;

    char path[512];
    if (!dir_find_file(dir_path, c64_name, path, sizeof(path))) {
        dbg_printf("DIR: file '%s' not found in '%s'\n", c64_name, dir_path);
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { perror("DIR: fopen"); return 0; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    cached_file_data = (uint8_t*)malloc(size > 0 ? size : 1);
    if (!cached_file_data) { fclose(f); return 0; }
    if (size > 0) fread(cached_file_data, 1, (size_t)size, f);
    fclose(f);
    cached_file_size = (int)size;
    dbg_printf("DIR: cached '%s' (%d bytes)\n", path, cached_file_size);
    return 1;
}

/* Build a C64 BASIC-format directory listing from a Linux directory.
   base_addr: the C64 address where the data will be placed (e.g. $0801 for LOAD,
              $0101 for CHRIN where the caller prepends a 2-byte load-address header).
   Output contains NO load-address prefix — callers prepend that themselves. */
static uint8_t *dir_build_listing(const char *dir_path, uint16_t base_addr, int *out_size)
{
    DIR *d = opendir(dir_path);
    if (!d) return NULL;

    uint8_t *buf = (uint8_t*)malloc(DIRECTORY_LISTING_BUFFER_SIZE);
    if (!buf) { closedir(d); return NULL; }

    /* Disk label = basename of dir_path, uppercase, padded with $A0 */
    uint8_t label[16];
    memset(label, 0xa0, 16);
    const char *base = strrchr(dir_path, '/');
    base = base ? base + 1 : dir_path;
    for (int i = 0; i < 16 && base[i]; i++)
        label[i] = (uint8_t)toupper((unsigned char)base[i]);

    int i = 0;
    int save_nlp;

    /* Header line — buf[0] is the first byte of BASIC data at base_addr */
    save_nlp = i; i += 2;          /* nlp placeholder */
    buf[i++] = 0x00; buf[i++] = 0x00;  /* line number = 0 */
    buf[i++] = 0x12; buf[i++] = 0x22;  /* RVS + " */
    for (int j = 0; j < 16; j++) buf[i++] = label[j];
    buf[i++] = 0x22; buf[i++] = 0x20;
    buf[i++] = '0';  buf[i++] = '0';   /* id */
    buf[i++] = 0x20;
    buf[i++] = 'D';  buf[i++] = 'R';   /* type = DR */
    buf[i++] = 0x00;
    /* nlp = base_addr + i  (address of the first file-entry line) */
    buf[save_nlp]   = (uint8_t)((base_addr + i) & 0xFF);
    buf[save_nlp+1] = (uint8_t)((base_addr + i) >> 8);

    /* File entries */
    struct dirent *ent;
    struct stat st;
    char full_path[512];

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);
        if (stat(full_path, &st) != 0) continue;

        int is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !S_ISREG(st.st_mode)) continue;

        uint16_t blocks = is_dir ? 0 : (uint16_t)((st.st_size + 253) / 254);

        /* Display name: for dirs use name as-is (uppercase, max 16);
           for files strip extension */
        char disp[17] = {0};
        const char *dot = is_dir ? NULL : strrchr(ent->d_name, '.');
        int nlen = dot ? (int)(dot - ent->d_name) : (int)strlen(ent->d_name);
        if (nlen > 16) nlen = 16;
        for (int j = 0; j < nlen; j++)
            disp[j] = (char)toupper((unsigned char)ent->d_name[j]);

        /* File type from extension (directories always "DIR") */
        const char *ext = (!is_dir && dot) ? dot + 1 : "";
        const char *ftype = is_dir ? "DIR" : "PRG";
        if      (strcasecmp(ext, "seq") == 0) ftype = "SEQ";
        else if (strcasecmp(ext, "usr") == 0) ftype = "USR";
        else if (strcasecmp(ext, "rel") == 0) ftype = "REL";

        int start = i;
        save_nlp = i; i += 2;
        buf[i++] = blocks & 0xFF;
        buf[i++] = (blocks >> 8) & 0xFF;

        char tmp[10];
        sprintf(tmp, "%u", blocks);
        int spaces = 4 - (int)strlen(tmp);
        for (int j = 0; j < spaces; j++) buf[i++] = 0x20;

        buf[i++] = 0x22;  /* " */
        int end_quote = 0;
        for (int j = 0; j < 16; j++) {
            if (j < nlen)
                buf[i++] = (uint8_t)disp[j];
            else {
                if (!end_quote) { buf[i++] = 0x22; end_quote = 1; }
                else buf[i++] = 0x20;
            }
        }
        if (!end_quote) buf[i++] = 0x22; else buf[i++] = 0x20;
        buf[i++] = 0x20;
        buf[i++] = ftype[0]; buf[i++] = ftype[1]; buf[i++] = ftype[2];

        /* Pad entry to 32 bytes (including the trailing 0x00) */
        while ((i - start) < 31) buf[i++] = 0x20;
        buf[i++] = 0x00;

        buf[save_nlp]   = (uint8_t)((base_addr + i) & 0xFF);
        buf[save_nlp+1] = (uint8_t)((base_addr + i) >> 8);

        if (i >= DIRECTORY_LISTING_BUFFER_SIZE - 64) break;  /* safety */
    }
    closedir(d);

    /* Blocks-free footer */
    save_nlp = i; i += 2;
    buf[i++] = 0x00; buf[i++] = 0x00;
    const char *bfmsg = "BLOCKS FREE.             ";
    for (int j = 0; j < 13; j++) buf[i++] = (uint8_t)bfmsg[j];
    for (int j = 0; j < 13; j++) buf[i++] = 0x20;
    buf[i++] = 0x00; buf[i++] = 0x00; buf[i++] = 0x00;
    buf[save_nlp]   = (uint8_t)((base_addr + i) & 0xFF);
    buf[save_nlp+1] = (uint8_t)((base_addr + i) >> 8);

    *out_size = i;
    return buf;
}

/* ── end DIR-mount helpers ─────────────────────────────────────────────── */

void loadDirectory(uint16_t addr)
{
    /* ── DIR mount: build listing from Linux directory ── */
    if (g_disk_format == DISK_FORMAT_DIR) {
        int dir_size = 0;
        /* base_addr = addr: nlp values correct for $0801 (or wherever BASIC loads) */
        uint8_t *dir_data = dir_build_listing(g_dir_cwd, addr, &dir_size);
        if (!dir_data) return;
        /* load_buffer_at skips the first 2 bytes (load address), so prepend them */
        uint8_t *with_hdr = (uint8_t*)malloc(dir_size + 2);
        if (!with_hdr) { free(dir_data); return; }
        with_hdr[0] = addr & 0xFF;
        with_hdr[1] = (addr >> 8) & 0xFF;
        memcpy(with_hdr + 2, dir_data, dir_size);
        free(dir_data);
        sysop_load_buffer_at(with_hdr, dir_size + 2, addr);
        free(with_hdr);
        return;
    }

    // will be written starting at 0801
    uint8_t* outBuffer = (uint8_t*)malloc(DIRECTORY_LISTING_BUFFER_SIZE);
    if (!outBuffer) {
        fprintf(stderr, "Error: Failed to allocate directory listing buffer.\n");
        return;
    }

    uint8_t disk_label[16];
    uint8_t disk_id[2];
    uint8_t disk_type[2];
    disk_get_header(disk_label, disk_id, disk_type);

    // loadbbin will look for load address here
    //outBuffer[0] = 0x01;
    //outBuffer[1] = 0x08;
    outBuffer[0] = (uint8_t)(addr & 0xFF);
    outBuffer[1] = (uint8_t)(addr>>8);
    dbg_printf("outBuffer 0: %02X, 1: %02X\n", outBuffer[0], outBuffer[1]);

    int i = 4;
    int j;
    int save_next_line_index = 2;
    outBuffer[i++] = 0x01;
    outBuffer[i++] = 0x00;
    outBuffer[i++] = 0x12; // inverse?
    outBuffer[i++] = 0x22;

    for (j=0;j<16;j++)
    {
        if (disk_label[j] == 0xa0)
        {
            outBuffer[i++] = 0x20;
        }
        else
        {
            outBuffer[i++] = disk_label[j];
        }
    }

    outBuffer[i++] = 0x22;
    outBuffer[i++] = 0x20;
    outBuffer[i++] = disk_id[0];
    outBuffer[i++] = disk_id[1];
    outBuffer[i++] = 0x20;
    outBuffer[i++] = disk_type[0];
    outBuffer[i++] = disk_type[1];
    outBuffer[i++] = 0x00;
    uint16_t next_line = 0x0801 + i - 2;
    outBuffer[save_next_line_index] = next_line & 0xFF;
    outBuffer[save_next_line_index+1] = (next_line >> 8) & 0xff;

    int k;
    int track = disk_directory_track();
    int sector = disk_directory_sector();
    uint16_t sectors_free = disk_count_free_sectors();

    dbg_printf("Total free: %d\n", sectors_free);

    int totalCount = 0;
    while(1)
    {
        uint8_t* directoryBuffer = readSector(track, sector);

        for (j=0;j<256;j++)
        {
            dbg_printf("%02X ", directoryBuffer[j]);
            if (j != 0 && j % 16 == 0)
                dbg_printf("\n");
        }


        // Iterate through the directory entries
        for (k = 0; k < 8; k++) {

            // Each entry is 32 bytes
            uint8_t* entry = directoryBuffer + k * 32;
            dbg_printf("Processing entry %d, first two bytes %02X %02X\n", k, entry[0], entry[1]);

            for (j=0;j<32;j++)
            {
                dbg_printf("%02X ", entry[j]);
                if (j != 0 && j % 16 == 0)
                    dbg_printf("\n");
            }
            dbg_printf("\n");


            int start = i;
            save_next_line_index = i;
            dbg_printf("Saving index %d to write next line\n", save_next_line_index);

            // Check if the entry is not empty
            // was [0]
            //if (entry[0] != 0 || entry[1] != 0 || entry[2] != 0 || entry[3] != 0) {
            if (entry[2] == 00)
            {
                dbg_printf("Skipping unused entry\n");
                continue;
            }
            else 
            {
                i += 2; // skip ahead
                dbg_printf("writing size at index %d of output buffer\n", i);
                outBuffer[i++] = entry[0x1e];
                outBuffer[i++] = entry[0x1f];
                
                uint16_t fileSize = (entry[0x1f] * 256) + entry[0x1e];
                char tmp[10];
                sprintf(tmp, "%d", fileSize);
                dbg_printf("File size: %d\n", fileSize);
                int fileSizeChars = strlen(tmp);
                int spaces = 4 - fileSizeChars;
                for (j=0;j<spaces;j++)
                {
                    outBuffer[i++] = 0x20;
                }
                // "
                outBuffer[i++] = 0x22;
                int endQuote = 0;
                for (j=0;j<16;j++)
                {
                    if (entry[0x05 + j] == 0xa0)
                    {
                        if (!endQuote)
                        {
                            outBuffer[i++] = 0x22;
                            endQuote = 1;
                        }
                        else
                        {
                            outBuffer[i++] = 0x20;
                        }
                    }
                    else
                    {
                        outBuffer[i++] = entry[0x05 + j];
                    }
                }
                if (!endQuote)
                    outBuffer[i++] = 0x22;
                else
                    outBuffer[i++] = 0x20;

                outBuffer[i++] = 0x20;
                switch(entry[0x02] & 0xF)
                {
                    case 0: // del
                        outBuffer[i++] = 0x44;
                        outBuffer[i++] = 0x45;
                        outBuffer[i++] = 0x4c;
                        break;

                    case 1: // seq
                        outBuffer[i++] = 0x53;
                        outBuffer[i++] = 0x45;
                        outBuffer[i++] = 0x51;
                        break;
                    case 2: // prg
                        outBuffer[i++] = 0x50;
                        outBuffer[i++] = 0x52;
                        outBuffer[i++] = 0x47;
                        break;
                    case 3: // USR
                        outBuffer[i++] = 0x55;
                        outBuffer[i++] = 0x53;
                        outBuffer[i++] = 0x52;
                        break;
                    case 4: // rel
                        outBuffer[i++] = 0x52;
                        outBuffer[i++] = 0x45;
                        outBuffer[i++] = 0x4c;
                        break;
                    default: // ??? 
                        dbg_printf("don't know about file type\n");
                        outBuffer[i++] = 0x50;
                        outBuffer[i++] = 0x52;
                        outBuffer[i++] = 0x47;
                        break;

                }

                //dbg_printf("i %02X, start %04X\n", i, start);
                dbg_printf("Entry has %d bytes\n", i-start);
                for (j=(i-start);j<31;j++)
                {
                    dbg_printf("Pad byte\n");
                    outBuffer[i++] = 0x20;
                }
                //outBuffer[i++] = 0x20;
                //outBuffer[i++] = 0x20;
                outBuffer[i++] = 0x00;

                //next_line = 0x0801 + i - 2;
                next_line = next_line + 32;
                dbg_printf("Wrote next line %04X, i is %d\n", next_line, i);
                outBuffer[save_next_line_index] = next_line & 0xFF;
                outBuffer[save_next_line_index+1] = (next_line >> 8) & 0xff;
                //outBuffer[save_next_line_index] = 0;
                //outBuffer[save_next_line_index+1] = 0;
                totalCount++;
            }
        }

        dbg_printf("Done\n");
        dbg_printf("%02X\n", directoryBuffer[0]);
        // end of directory if next track is 0
        if (directoryBuffer[0] == 0x0)
        {
            save_next_line_index = i;
            i += 2;

            dbg_printf("Starting blocks free at %d\n", i);
            // print blocks free
            outBuffer[i++] = sectors_free & 0xFF;
            outBuffer[i++] = (sectors_free & 0xFF00) >> 8;
            // BLOCKS FREE.
            outBuffer[i++] = 0x42;
            outBuffer[i++] = 0x4c;
            outBuffer[i++] = 0x4f;
            outBuffer[i++] = 0x43;
            outBuffer[i++] = 0x4b;
            outBuffer[i++] = 0x53;
            outBuffer[i++] = 0x20;
            outBuffer[i++] = 0x46;
            outBuffer[i++] = 0x52;
            outBuffer[i++] = 0x45;
            outBuffer[i++] = 0x45;
            outBuffer[i++] = 0x2E;
            for (j=0;j<13;j++)
            {
                outBuffer[i++] = 0x20;
            }
            outBuffer[i++] = 0x00;
            outBuffer[i++] = 0x00;
            outBuffer[i++] = 0x00;

            next_line = next_line + 30;
            dbg_printf("Wrote next line %04X, i is %d\n", next_line, i);
            outBuffer[save_next_line_index] = next_line & 0xFF;
            outBuffer[save_next_line_index+1] = (next_line >> 8) & 0xff;

            break;
        }
        else{
            track = directoryBuffer[0];
            sector = directoryBuffer[1];
        }
    }

    dbg_printf("output buffer\n");
    for (j=0;j<i;j++)
    {
        dbg_printf("%02X ", outBuffer[j]);
        if (j != 0 && j % 16 == 0)
            dbg_printf("\n");
    }
    dbg_printf("\n");

    sysop_load_buffer(&outBuffer[0], i);
    free(outBuffer);
}

int getFilename(char* filename, char* file_type)
{
    uint8_t filenameLo = sysop_peek(0xbb);
    uint8_t filenameHi = sysop_peek(0xbc);
    uint16_t filenameAddr = filenameHi << 8 | filenameLo;
    dbg_printf("Filename is at %04X\n", filenameAddr);
    uint8_t filenameCount = sysop_peek(0xb7);
    if (filenameCount > 254)
        filenameCount = 254;  // guard against overflow into the null terminator byte

    int i;
    for (i=0;i<filenameCount;i++)
    {
        filename[i] = sysop_peek(filenameAddr+i);
    }
    filename[filenameCount] = '\0';
    dbg_printf("Raw filename is %s\n", filename);
    
    // Parse and strip disk command prefixes like @S:, @P:, @0:, etc.
    char* src = filename;
    int save_with_replace = 0;
    *file_type = 'P';  // Default to PRG
    
    // Check for @ prefix (save-with-replace)
    if (*src == '@') {
        save_with_replace = 1;
        src++;
        dbg_printf("Found @ prefix (save-with-replace)\n");
        
        // Check for file type indicator (S:, P:, U:, R:) or drive number (0:, 1:)
        if (*src != '\0' && src[1] == ':') {
            // Check if it's a file type (not a digit)
            if (*src == 'S' || *src == 's' || *src == 'P' || *src == 'p' ||
                *src == 'U' || *src == 'u' || *src == 'R' || *src == 'r') {
                *file_type = (*src == 's') ? 'S' : (*src == 'p') ? 'P' : 
                             (*src == 'u') ? 'U' : (*src == 'r') ? 'R' : *src;
                dbg_printf("Found file type prefix '%c:'\n", *file_type);
            } else {
                dbg_printf("Found drive number prefix '%c:'\n", *src);
            }
            src += 2;  // Skip the character and the colon
        }
    } else {
        // No @ prefix, check for just file type or drive number prefix
        if (*src != '\0' && src[1] == ':') {
            if (*src == 'S' || *src == 's' || *src == 'P' || *src == 'p' ||
                *src == 'U' || *src == 'u' || *src == 'R' || *src == 'r') {
                *file_type = (*src == 's') ? 'S' : (*src == 'p') ? 'P' : 
                             (*src == 'u') ? 'U' : (*src == 'r') ? 'R' : *src;
                dbg_printf("Found file type prefix '%c:' without @\n", *file_type);
            } else {
                dbg_printf("Found drive number prefix '%c:' without @\n", *src);
            }
            src += 2;  // Skip the character and the colon
        }
    }
    
    // If we stripped anything, move the remaining string to the beginning
    if (src != filename) {
        memmove(filename, src, strlen(src) + 1);
        dbg_printf("Stripped filename is %s\n", filename);
    }
    
    return save_with_replace;
}

static int ascii_lower(int ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return ch + ('a' - 'A');
    return ch;
}

static const char* normalize_requested_filename(const char* filename, char* buffer, size_t buffer_size)
{
    const char* start = filename;
    const char* end;
    size_t length;

    while (*start == ' ' || *start == '\t')
        start++;

    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;

    if (end - start >= 2 && start[0] == '"' && end[-1] == '"') {
        start++;
        end--;
    }

    if (end - start >= 2 && start[1] == ':' &&
        ((start[0] >= '0' && start[0] <= '9') || start[0] == '*')) {
        start += 2;
    }

    length = (size_t)(end - start);
    if (length >= buffer_size)
        length = buffer_size - 1;

    memcpy(buffer, start, length);
    buffer[length] = '\0';

    return buffer;
}

static int filename_equals(const char* left, const char* right)
{
    while (*left && *right) {
        if (*left != *right)
            return 0;
        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

static int filename_equals_ignore_case(const char* left, const char* right)
{
    while (*left && *right) {
        if (ascii_lower((unsigned char)*left) != ascii_lower((unsigned char)*right))
            return 0;
        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

int directory_entry_is_used(const uint8_t* entry)
{
    return entry[2] != 0;
}

void directory_entry_name(const uint8_t* entry, char* out, size_t out_size)
{
    size_t length = 16;

    if (out_size == 0)
        return;

    while (length > 0 && (entry[0x05 + length - 1] == 0xa0 ||
                          entry[0x05 + length - 1] == 0x00)) {
        length--;
    }

    if (length >= out_size)
        length = out_size - 1;

    for (size_t i = 0; i < length; i++) {
        uint8_t ch = entry[0x05 + i];
        out[i] = (ch == 0xa0) ? ' ' : (char)ch;
    }
    out[length] = '\0';
}

int directory_entry_matches_filename(const uint8_t* entry, const char* filename, int prg_wildcard)
{
    char entry_name[17];
    char requested_name[256];

    if (!directory_entry_is_used(entry))
        return 0;

    normalize_requested_filename(filename, requested_name, sizeof(requested_name));

    if (prg_wildcard && strcmp(requested_name, "*") == 0 && ((entry[2] & 0x0f) == 0x2))
        return 1;

    directory_entry_name(entry, entry_name, sizeof(entry_name));

    return filename_equals(requested_name, entry_name) ||
           filename_equals_ignore_case(requested_name, entry_name);
}

uint16_t locateFileAndLoad(char* filename, uint16_t load_address)
{
    /* ── DIR mount: load straight from Linux FS ── */
    if (g_disk_format == DISK_FORMAT_DIR) {
        char path[512];
        if (!dir_find_file(g_dir_cwd, filename, path, sizeof(path))) {
            printf("DIR: file '%s' not found in '%s'\n", filename, g_dir_cwd);
            return 0;
        }
        FILE *f = fopen(path, "rb");
        if (!f) { perror("DIR: fopen"); return 0; }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *data = (uint8_t*)malloc(size > 0 ? (size_t)size : 1);
        if (!data) { fclose(f); return 0; }
        if (size > 0) fread(data, 1, (size_t)size, f);
        fclose(f);
        printf("DIR: loading '%s' (%ld bytes)\n", path, size);
        uint16_t end_addr;
        if (load_address == 0) {
            uint16_t file_addr = (uint16_t)(data[0] | (data[1] << 8));
            sysop_load_buffer(data, (int)size);
            end_addr = (uint16_t)(file_addr + size - 2);
        } else {
            sysop_load_buffer_at(data, (int)size, load_address);
            end_addr = (uint16_t)(load_address + size - 2);
        }
        free(data);
        return end_addr;
    }

    int track = disk_directory_track();
    int sector = disk_directory_sector();
    int i, k;
    int use_file_address = (load_address == 0);

    while(1) 
    {        
        // load other file
        uint8_t* directoryBuffer = readSector(track, sector);

        // Iterate through the directory entries
        for (i = 0; i < 8; i++) {
            // Each entry is 32 bytes
            uint8_t* entry = directoryBuffer + i * 32;
            dbg_printf("offset %08X\n", (i*32));

            if (directory_entry_is_used(entry)) {

                uint16_t fileSize = (entry[0x1f] * 256) + entry[0x1e];

                char entryName[17];
                for (k=0;k<16;k++) {
                    dbg_printf("%hx ", *((char*)(entry+0x05+k)));
                }
                directory_entry_name(entry, entryName, sizeof(entryName));
                dbg_printf("\n");
                dbg_printf("entry name: %s\n", entryName);

                if (directory_entry_matches_filename(entry, filename, 1))
                {
                    dbg_printf("found matching file, loading...\n");
                    uint8_t file_type_byte = entry[2] & 0x0F;  // Get file type
                    uint8_t fileTrack = entry[0x03];
                    uint8_t fileSector = entry[0x04];

                    // If fileSize is 0, manually compute it by traversing the sector chain
                    if (fileSize == 0) {
                        dbg_printf("Directory entry shows fileSize=0, computing actual size...\n");
                        uint8_t tempTrack = fileTrack;
                        uint8_t tempSector = fileSector;
                        int totalBytes = 0;
                        int blocks = 0;
                        
                        while (1) {
                            uint8_t* tempBuffer = readSector(tempTrack, tempSector);
                            uint8_t nextTrack = tempBuffer[0];
                            uint8_t nextSector = tempBuffer[1];
                            
                            if (nextTrack == 0) {
                                // Last sector
                                int lastSectorBytes = nextSector - 1;
                                totalBytes += lastSectorBytes;
                                blocks++;
                                dbg_printf("Last sector: %d bytes\n", lastSectorBytes);
                                break;
                            } else {
                                // Full sector
                                totalBytes += 254;
                                blocks++;
                                tempTrack = nextTrack;
                                tempSector = nextSector;
                            }
                        }
                        
                        dbg_printf("Computed file size: %d bytes in %d blocks\n", totalBytes, blocks);
                        fileSize = blocks;  // Update fileSize to match computed blocks
                    }

                    uint8_t* data = malloc(fileSize*254);
                    if (!data)
                    {
                        dbg_printf("Memory allocation failed\n");
                        return 0;
                    }
                    int dataIndex = 0;
                    uint8_t* fileBuffer;
                    int blocksRead = 0;
                    while(1)
                    {
                        fileBuffer = readSector(fileTrack, fileSector);
                        uint8_t nextTrack = fileBuffer[0];
                        uint8_t nextSector = fileBuffer[1];
                        
                        // Determine how many bytes to copy from this sector
                        int bytesToCopy;
                        if (nextTrack == 0) {
                            // Last sector: nextSector contains byte count (1-254)
                            bytesToCopy = nextSector - 1;
                            dbg_printf("Last sector: copying %d bytes\n", bytesToCopy);
                        } else {
                            // Not last sector: copy full 254 bytes
                            bytesToCopy = 254;
                        }
                        
                        if (dataIndex + bytesToCopy > (fileSize*254)) {
                            dbg_printf("Copying beyond bounds of alloc\n");
                            exit(-1);
                        }
                        
                        memcpy(&data[dataIndex], &fileBuffer[2], bytesToCopy);
                        dataIndex += bytesToCopy;
                        blocksRead++;
                        
                        if (nextTrack == 0)
                            break;
                            
                        fileTrack = nextTrack;
                        fileSector = nextSector;
                    }
                    
                    // PRG files (type 2) have load address in first 2 bytes
                    // SEQ files (type 1) and others don't have a load address header
                    dbg_printf("Loaded %d bytes total\n", dataIndex);
                    uint16_t end_address = 0;
                    
                    if (use_file_address && fileSize > 0 && file_type_byte == 0x2) {
                        dbg_printf("PRG file: using load address from first 2 bytes\n");
                        // load_buffer expects first 2 bytes to contain the load address
                        uint16_t start = data[0] | (data[1] << 8);
                        end_address = start + (dataIndex - 2);
                        sysop_load_buffer(data, dataIndex);
                    } else if (use_file_address && file_type_byte != 0x2) {
                        dbg_printf("Non-PRG file: no load address header, loading to default address\n");
                        // For SEQ and other file types, use the provided load_address
                        // If none was provided (load_address is still 0), we should error or use a default
                        if (load_address == 0) {
                            dbg_printf("WARNING: No load address specified for non-PRG file, using default 0x0801\n");
                            load_address = 0x0801;
                        }
                        end_address = load_address + dataIndex;
                        //sysop_load_buffer_at(data, dataIndex, load_address);
                        printf("Not implemented yet\n");
                        exit(-1);
                    } else {
                        // Explicit load address provided - skip first 2 bytes if PRG file
                        if (file_type_byte == 0x2 && fileSize > 0) {
                            dbg_printf("PRG file with specified load address %04X: skipping first 2 bytes\n", load_address);
                            //load_buffer_at(&data[2], dataIndex - 2, load_address);
                            // load_buffer_at still expects first 2 bytes to contain the load address and it will skip them
                            end_address = load_address + (dataIndex - 2);
                            sysop_load_buffer_at(data, dataIndex, load_address);
                        } else {
                            dbg_printf("Using specified load address: %04X\n", load_address);
                            end_address = load_address + dataIndex;
                            sysop_load_buffer_at(data, dataIndex, load_address);
                        }
                    }
                    dbg_printf("Done, end address: %04X\n", end_address);
                    free(data);
//    dbg_printf("hit enter to continue\n");
//    getchar();
                    return end_address;
                }
            }
        }

        // end of directory if next track is 0
        if (directoryBuffer[0] == 0x0)
        {
            break;
        }
        else {
            track = directoryBuffer[0];
            sector = directoryBuffer[1];
        }
    }


    return 0;
}


uint8_t* cached_dir_data = NULL;
int cached_dir_size = 0;

int locateFileAndGetByte(char* filename, int offset, uint8_t* output)
{
    /* ── DIR mount ── */
    if (g_disk_format == DISK_FORMAT_DIR) {
        if (strcmp(filename, "$") == 0) {
            if (cached_dir_data == NULL) {
                dbg_printf("DIR: building directory listing\n");
                /* base_addr=$0101 matches the CHRIN convention used by disk images.
                   We then prepend 0x01,0x01 as the 2-byte load-address header so
                   the reader knows where to anchor the BASIC link pointers. */
                int raw_size = 0;
                uint8_t *raw = dir_build_listing(g_dir_cwd, 0x0101, &raw_size);
                if (!raw) return -1;
                cached_dir_data = (uint8_t*)malloc(raw_size + 2);
                if (!cached_dir_data) { free(raw); return -1; }
                cached_dir_data[0] = 0x01; cached_dir_data[1] = 0x01;
                memcpy(cached_dir_data + 2, raw, raw_size);
                cached_dir_size = raw_size + 2;
                free(raw);
                printf("DIR: directory listing %d bytes\n", cached_dir_size);
            }
            if (offset < cached_dir_size) {
                *output = cached_dir_data[offset];
                return 1;
            }
            return 0;  /* EOF */
        } else {
            if (!dir_cache_file(g_dir_cwd, filename)) return -1;
            if (offset < cached_file_size) {
                *output = cached_file_data[offset];
                return 1;
            }
            return 0;  /* EOF */
        }
    }

    // Handle directory listing request
    if (strcmp(filename, "$") == 0) {
        // Use static buffer to cache directory data across calls

        // Generate directory if not already cached
        if (cached_dir_data == NULL) {
            dbg_printf("Generating directory for byte-by-byte reading\n");

            uint8_t* dirBuffer = (uint8_t*)malloc(DIRECTORY_LISTING_BUFFER_SIZE);
            if (!dirBuffer) {
                fprintf(stderr, "Error: Failed to allocate directory listing buffer.\n");
                return -1;
            }

            uint8_t disk_label[16];
            uint8_t disk_id[2];
            uint8_t disk_type[2];
            disk_get_header(disk_label, disk_id, disk_type);

            // Start building directory like loadDirectory but without load address
            int i = 0;
            int j;
            int save_next_line_index = 0;

            // First line: disk header
            dirBuffer[i++] = 0x01;
            dirBuffer[i++] = 0x01;
            save_next_line_index = i;
            i += 2;  // Space for next line pointer
            
            dirBuffer[i++] = 0x00;
            dirBuffer[i++] = 0x00;
            dirBuffer[i++] = 0x12; // inverse
            dirBuffer[i++] = 0x22; // "

            for (j = 0; j < 16; j++) {
                if (disk_label[j] == 0xa0)
                    dirBuffer[i++] = 0x20;
                else
                    dirBuffer[i++] = disk_label[j];
            }

            dirBuffer[i++] = 0x22;
            dirBuffer[i++] = 0x20;
            dirBuffer[i++] = disk_id[0];
            dirBuffer[i++] = disk_id[1];
            dirBuffer[i++] = 0x20;
            dirBuffer[i++] = disk_type[0];
            dirBuffer[i++] = disk_type[1];
            dirBuffer[i++] = 0x00;

            uint16_t next_line = 0x0101 + i;
            dirBuffer[save_next_line_index] = next_line & 0xFF;
            dirBuffer[save_next_line_index + 1] = (next_line >> 8) & 0xff;
            
            // Calculate free sectors
            int track = disk_directory_track();
            int sector = disk_directory_sector();
            uint16_t sectors_free = disk_count_free_sectors();

            int k = 0;

            // Process directory entries
            while (1) {
                uint8_t* directoryBuffer = readSector(track, sector);

                for (k = 0; k < 8; k++) {
                    uint8_t* entry = directoryBuffer + k * 32;
                    
                    if (entry[2] == 0) continue;  // Skip unused entries
                    
                    int start = i;
                    save_next_line_index = i;
                    i += 2;  // Space for next line pointer
                    
                    // File size in blocks
                    dirBuffer[i++] = entry[0x1e];
                    dirBuffer[i++] = entry[0x1f];
                    
                    uint16_t fileSize = (entry[0x1f] * 256) + entry[0x1e];
                    char tmp[10];
                    sprintf(tmp, "%d", fileSize);
                    int fileSizeChars = strlen(tmp);
                    int spaces = 4 - fileSizeChars;
                    for (j = 0; j < spaces; j++)
                        dirBuffer[i++] = 0x20;
                    
                    // Filename
                    dirBuffer[i++] = 0x22;
                    int endQuote = 0;
                    for (j = 0; j < 16; j++) {
                        if (entry[0x05 + j] == 0xa0) {
                            if (!endQuote) {
                                dirBuffer[i++] = 0x22;
                                endQuote = 1;
                            } else {
                                dirBuffer[i++] = 0x20;
                            }
                        } else {
                            dirBuffer[i++] = entry[0x05 + j];
                        }
                    }
                    if (!endQuote)
                        dirBuffer[i++] = 0x22;
                    else
                        dirBuffer[i++] = 0x20;
                    
                    dirBuffer[i++] = 0x20;
                    
                    // File type
                    switch (entry[0x02] & 0xF) {
                        case 0: dirBuffer[i++] = 0x44; dirBuffer[i++] = 0x45; dirBuffer[i++] = 0x4c; break;
                        case 1: dirBuffer[i++] = 0x53; dirBuffer[i++] = 0x45; dirBuffer[i++] = 0x51; break;
                        case 2: dirBuffer[i++] = 0x50; dirBuffer[i++] = 0x52; dirBuffer[i++] = 0x47; break;
                        case 3: dirBuffer[i++] = 0x55; dirBuffer[i++] = 0x53; dirBuffer[i++] = 0x52; break;
                        case 4: dirBuffer[i++] = 0x52; dirBuffer[i++] = 0x45; dirBuffer[i++] = 0x4c; break;
                        default: dirBuffer[i++] = 0x50; dirBuffer[i++] = 0x52; dirBuffer[i++] = 0x47; break;
                    }
                    
                    // Pad to 32 bytes total
                    for (j = (i - start); j < 31; j++)
                        dirBuffer[i++] = 0x20;
                    dirBuffer[i++] = 0x00;
                    
                    next_line = next_line + 32;
                    dirBuffer[save_next_line_index] = next_line & 0xFF;
                    dirBuffer[save_next_line_index + 1] = (next_line >> 8) & 0xff;
                }
                
                if (directoryBuffer[0] == 0x0) break;
                track = directoryBuffer[0];
                sector = directoryBuffer[1];
            }
            
            // Blocks free line
            save_next_line_index = i;
            i += 2;
            dirBuffer[i++] = sectors_free & 0xFF;
            dirBuffer[i++] = (sectors_free & 0xFF00) >> 8;
            
            const char* blocks_free_msg = "BLOCKS FREE.             ";
            for (j = 0; j < 13; j++)
                dirBuffer[i++] = blocks_free_msg[j];
            for (j = 0; j < 13; j++)
                dirBuffer[i++] = 0x20;
            dirBuffer[i++] = 0x00;
            dirBuffer[i++] = 0x00;
            dirBuffer[i++] = 0x00;
            
            next_line = next_line + 30;
            dirBuffer[save_next_line_index] = next_line & 0xFF;
            dirBuffer[save_next_line_index + 1] = (next_line >> 8) & 0xff;
            
            // Cache the generated directory
            cached_dir_size = i;
            cached_dir_data = (uint8_t*)malloc(cached_dir_size);
            if (cached_dir_data) {
                memcpy(cached_dir_data, dirBuffer, cached_dir_size);
                //dbg_printf("Cached directory: %d bytes\n", cached_dir_size);
                printf("Cached directory: %d bytes\n", cached_dir_size);

                // do a hex dump for debugging
                dbg_printf("Directory dump:\n");
                for (j = 0; j < cached_dir_size; j++) {
                    dbg_printf("%02X ", cached_dir_data[j]);
                    if ((j + 1) % 16 == 0)
                        dbg_printf("\n");
                }
                dbg_printf("\n");
                //getchar();
            }
            free(dirBuffer);
        }
        
        // Return byte at offset
        if (cached_dir_data && offset < cached_dir_size) {
            *output = cached_dir_data[offset];
            dbg_printf("Directory read at offset %d: %02X\n", offset, *output);
            return 1;
        } else {
            dbg_printf("Directory EOF at offset %d (size %d)\n", offset, cached_dir_size);
            return 0;  // EOF
        }
    }

    int track = disk_directory_track();
    int sector = disk_directory_sector();
    int i, k;

    while(1) 
    {        
        // load other file
        uint8_t* directoryBuffer = readSector(track, sector);

        // Iterate through the directory entries
        for (i = 0; i < 8; i++) {
            // Each entry is 32 bytes
            uint8_t* entry = directoryBuffer + i * 32;
            dbg_printf("offset %08X\n", (i*32));

            if (directory_entry_is_used(entry)) {

                uint16_t fileSize = (entry[0x1f] * 256) + entry[0x1e];

                char entryName[17];
                for (k=0;k<16;k++) {
                    dbg_printf("%hx ", *((char*)(entry+0x05+k)));
                }
                directory_entry_name(entry, entryName, sizeof(entryName));
                dbg_printf("\n");
                dbg_printf("entry name: %s\n", entryName);

                if (directory_entry_matches_filename(entry, filename, 1))
                {
                    dbg_printf("found matching file, loading...\n");
                    uint8_t fileTrack = entry[0x03];
                    uint8_t fileSector = entry[0x04];

                    uint8_t* data = malloc(fileSize*254);
                    if (!data)
                    {
                        dbg_printf("Memory allocation failed\n");
                        return -1;
                    }
                    int dataIndex = 0;
                    uint8_t* fileBuffer;
                    int blocksRead = 0;
                    while(1)
                    {
                        fileBuffer = readSector(fileTrack, fileSector);
                        uint8_t nextTrack = fileBuffer[0];
                        uint8_t nextSector = fileBuffer[1];
                        
                        // Determine how many bytes to copy from this sector
                        int bytesToCopy;
                        if (nextTrack == 0) {
                            // Last sector: nextSector contains byte count (1-254)
                            bytesToCopy = nextSector - 1;
                        } else {
                            // Not last sector: copy full 254 bytes
                            bytesToCopy = 254;
                        }
                        
                        memcpy(&data[dataIndex], &fileBuffer[2], bytesToCopy);
                        dataIndex += bytesToCopy;
                        blocksRead++;
                        
                        if (nextTrack == 0)
                            break;
                            
                        fileTrack = nextTrack;
                        fileSector = nextSector;
                    }
                    if (offset < (fileSize*254))
                    {
                        //dbg_printf("Returning byte at offset %d: %02X\n", offset, data[offset]);
                        printf("Returning byte at offset %d: %02X\n", offset, data[offset]);
                        *output = data[offset];
                        free(data);
                        return 1;
                    }
                    else 
                    {
                        dbg_printf("Asked for byte beyond end of file, returning EOF\n");
                        free(data);
                        return 0;
                    }
                }
            }
        }

        // end of directory if next track is 0
        if (directoryBuffer[0] == 0x0)
        {
            break;
        }
        else {
            track = directoryBuffer[0];
            sector = directoryBuffer[1];
        }
    }

    return 0;
}
