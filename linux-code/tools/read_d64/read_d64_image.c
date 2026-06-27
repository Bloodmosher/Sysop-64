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

/* All D64/D81 format constants and the d64_track_info table are provided
 * by sysop_d64.h (included via read_d64_private.h).  No local copies needed. */

int g_step_mode = 0;
int g_debug_enabled = 0;
disk_image_format_t g_disk_format = DISK_FORMAT_D64;
static int g_disk_max_track = 35;
static int g_disk_read_max_track = 35;

typedef struct {
    uint8_t *buffer;
    uint64_t buffer_size;
    char *mounted_filename;
    char *dir_cwd;            /* DIR mounts only: current working directory */
    int read_only_mode;
    disk_image_format_t disk_format;
    int disk_max_track;
    int disk_read_max_track;
    int mounted;
} mounted_image_t;

static mounted_image_t g_mounted_images[READ_D64_LAST_DEVICE - READ_D64_FIRST_DEVICE + 1];
static int g_current_device = -1;

void dbg_printf(const char* format, ...) {
    if (!g_debug_enabled) return;
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

/* d64_track_info[] is defined in libsysop64/src/sysop_d64.c and declared in sysop_d64.h.
 * This module uses it directly via the extern — no local copy needed. */
uint8_t* buffer = NULL;
uint64_t buffer_size = 0;
char* mounted_filename = NULL;
char* g_dir_cwd = NULL;   /* DIR mounts: active current working directory */
int read_only_mode = 0;

static mounted_image_t *mounted_slot_for_device(int device_number)
{
    if (device_number < READ_D64_FIRST_DEVICE || device_number > READ_D64_LAST_DEVICE)
        return NULL;

    return &g_mounted_images[device_number - READ_D64_FIRST_DEVICE];
}

static void reset_active_globals(void)
{
    buffer = NULL;
    buffer_size = 0;
    mounted_filename = NULL;
    g_dir_cwd = NULL;
    read_only_mode = 0;
    g_disk_format = DISK_FORMAT_D64;
    g_disk_max_track = 35;
    g_disk_read_max_track = 35;
}

static void sync_globals_from_slot(mounted_image_t *slot)
{
    if (slot == NULL || !slot->mounted) {
        reset_active_globals();
        return;
    }

    buffer = slot->buffer;
    buffer_size = slot->buffer_size;
    mounted_filename = slot->mounted_filename;
    g_dir_cwd = slot->dir_cwd;
    read_only_mode = slot->read_only_mode;
    g_disk_format = slot->disk_format;
    g_disk_max_track = slot->disk_max_track;
    g_disk_read_max_track = slot->disk_read_max_track;
}

static void sync_slot_from_globals(mounted_image_t *slot)
{
    if (slot == NULL || !slot->mounted)
        return;

    slot->buffer = buffer;
    slot->buffer_size = buffer_size;
    slot->mounted_filename = mounted_filename;
    slot->read_only_mode = read_only_mode;
    slot->disk_format = g_disk_format;
    slot->disk_max_track = g_disk_max_track;
    slot->disk_read_max_track = g_disk_read_max_track;
}

static void unmount_slot(mounted_image_t *slot)
{
    if (slot == NULL)
        return;

    if (slot->buffer != NULL) {
        free(slot->buffer);
        slot->buffer = NULL;
    }
    if (slot->mounted_filename != NULL) {
        free(slot->mounted_filename);
        slot->mounted_filename = NULL;
    }
    if (slot->dir_cwd != NULL) {
        free(slot->dir_cwd);
        slot->dir_cwd = NULL;
    }

    slot->buffer_size = 0;
    slot->read_only_mode = 0;
    slot->disk_format = DISK_FORMAT_D64;
    slot->disk_max_track = 35;
    slot->disk_read_max_track = 35;
    slot->mounted = 0;
}

int current_mounted_device(void)
{
    return g_current_device;
}

int is_device_mounted(int device_number)
{
    mounted_image_t *slot = mounted_slot_for_device(device_number);
    return slot != NULL && slot->mounted;
}

int select_mounted_device(int device_number)
{
    mounted_image_t *slot = mounted_slot_for_device(device_number);
    if (slot == NULL || !slot->mounted)
        return 1;

    if (g_current_device >= READ_D64_FIRST_DEVICE && g_current_device <= READ_D64_LAST_DEVICE) {
        mounted_image_t *current_slot = mounted_slot_for_device(g_current_device);
        sync_slot_from_globals(current_slot);
    }

    g_current_device = device_number;
    sync_globals_from_slot(slot);
    return 0;
}

static int ascii_tolower(int ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return ch + ('a' - 'A');
    return ch;
}

static int has_extension(const char* filename, const char* extension)
{
    const char* dot = strrchr(filename, '.');
    if (!dot)
        return 0;

    while (*dot && *extension) {
        if (ascii_tolower((unsigned char)*dot) != ascii_tolower((unsigned char)*extension))
            return 0;
        dot++;
        extension++;
    }

    return *dot == '\0' && *extension == '\0';
}

static int detect_d64_max_track(uint64_t size)
{
    for (int track = 40; track >= 1; track--) {
        uint64_t track_end = (uint64_t)d64_track_info[track - 1][1] +
                             (uint64_t)d64_track_info[track - 1][0] * SECTOR_SIZE;
        if (size >= track_end)
            return track;
    }

    return 35;
}

const char* disk_format_name(void)
{
    if (g_disk_format == DISK_FORMAT_D81) return "D81";
    if (g_disk_format == DISK_FORMAT_DIR) return "DIR";
    return "D64";
}

int disk_max_track(void)
{
    return g_disk_max_track;
}

int disk_directory_track(void)
{
    if (g_disk_format == DISK_FORMAT_DIR) return 0;
    return (g_disk_format == DISK_FORMAT_D81) ? D81_DIRECTORY_TRACK : D64_DIRECTORY_TRACK;
}

int disk_directory_sector(void)
{
    if (g_disk_format == DISK_FORMAT_DIR) return 0;
    return (g_disk_format == DISK_FORMAT_D81) ? D81_DIRECTORY_SECTOR : D64_DIRECTORY_SECTOR;
}

static int disk_header_track(void)
{
    return (g_disk_format == DISK_FORMAT_D81) ? D81_HEADER_TRACK : D64_HEADER_TRACK;
}

static int disk_header_sector(void)
{
    return (g_disk_format == DISK_FORMAT_D81) ? D81_HEADER_SECTOR : D64_HEADER_SECTOR;
}

int disk_sectors_per_track(int track)
{
    if (track < 1 || track > g_disk_read_max_track)
        return 0;

    if (g_disk_format == DISK_FORMAT_D81)
        return D81_SECTORS_PER_TRACK;

    return d64_track_info[track - 1][0];
}

int disk_sector_is_valid(int track, int sector)
{
    int sectors_per_track = disk_sectors_per_track(track);
    return sectors_per_track > 0 && sector >= 0 && sector < sectors_per_track;
}

int disk_is_reserved_track(int track)
{
    return track == disk_directory_track();
}

static int disk_sector_offset(int track, int sector)
{
    if (g_disk_format == DISK_FORMAT_D81)
        return ((track - 1) * D81_SECTORS_PER_TRACK + sector) * SECTOR_SIZE;

    return d64_track_info[track - 1][1] + sector * SECTOR_SIZE;
}

static uint8_t* disk_bam_sector_for_track(int track)
{
    if (g_disk_format == DISK_FORMAT_D81)
        return readSector(D81_BAM_TRACK, (track <= 40) ? D81_BAM_LOW_SECTOR : D81_BAM_HIGH_SECTOR);

    return readSector(D64_HEADER_TRACK, D64_HEADER_SECTOR);
}

static int disk_bam_entry_offset(int track)
{
    if (g_disk_format == DISK_FORMAT_D81)
        return 0x10 + ((track - 1) % 40) * 6;

    return track * 4;
}

static int disk_track_has_bam_entry(int track)
{
    if (g_disk_format == DISK_FORMAT_D81)
        return track >= 1 && track <= D81_MAX_TRACKS;

    return track >= 1 && track <= 35;
}

void disk_get_header(uint8_t label[16], uint8_t id[2], uint8_t dos_type[2])
{
    if (g_disk_format == DISK_FORMAT_DIR) {
        memset(label, 0xa0, 16);
        const char *base = strrchr(mounted_filename, '/');
        base = base ? base + 1 : mounted_filename;
        for (int i = 0; i < 16 && base[i]; i++)
            label[i] = (uint8_t)toupper((unsigned char)base[i]);
        id[0] = '0'; id[1] = '0';
        dos_type[0] = 'D'; dos_type[1] = 'R';
        return;
    }

    uint8_t* header = readSector(disk_header_track(), disk_header_sector());

    if (g_disk_format == DISK_FORMAT_D81) {
        memcpy(label, &header[0x04], 16);
        memcpy(id, &header[0x16], 2);
        memcpy(dos_type, &header[0x19], 2);
    } else {
        memcpy(label, &header[0x90], 16);
        memcpy(id, &header[0xa2], 2);
        memcpy(dos_type, &header[0xa5], 2);
    }
}

static uint8_t disk_track_free_count(int track)
{
    if (!disk_track_has_bam_entry(track))
        return 0;

    uint8_t* bam = disk_bam_sector_for_track(track);
    return bam[disk_bam_entry_offset(track)];
}

uint16_t disk_count_free_sectors(void)
{
    if (g_disk_format == DISK_FORMAT_DIR) return 0;

    uint16_t sectors_free = 0;

    for (int track = 1; track <= disk_max_track(); track++) {
        if (disk_is_reserved_track(track))
            continue;

        sectors_free += disk_track_free_count(track);
    }

    return sectors_free;
}

int disk_is_sector_free(int track, int sector)
{
    if (!disk_sector_is_valid(track, sector))
        return 0;
    if (!disk_track_has_bam_entry(track))
        return 0;

    uint8_t* bam = disk_bam_sector_for_track(track);
    int offset = disk_bam_entry_offset(track) + 1 + (sector / 8);
    int bit = sector % 8;
    return (bam[offset] & (1 << bit)) != 0;
}

void disk_mark_sector_free(int track, int sector)
{
    if (!disk_sector_is_valid(track, sector))
        return;
    if (!disk_track_has_bam_entry(track))
        return;

    uint8_t* bam = disk_bam_sector_for_track(track);
    int entry_offset = disk_bam_entry_offset(track);
    int bitmap_offset = entry_offset + 1 + (sector / 8);
    int bit = sector % 8;

    if ((bam[bitmap_offset] & (1 << bit)) == 0) {
        bam[bitmap_offset] |= (1 << bit);
        bam[entry_offset]++;
    }
}

void disk_mark_sector_used(int track, int sector)
{
    if (!disk_sector_is_valid(track, sector))
        return;
    if (!disk_track_has_bam_entry(track))
        return;

    uint8_t* bam = disk_bam_sector_for_track(track);
    int entry_offset = disk_bam_entry_offset(track);
    int bitmap_offset = entry_offset + 1 + (sector / 8);
    int bit = sector % 8;

    if (bam[bitmap_offset] & (1 << bit)) {
        bam[bitmap_offset] &= ~(1 << bit);
        bam[entry_offset]--;
    }
}

// Function to read a sector from the mounted disk image
uint8_t* readSector(int track, int sector) {
    if (g_disk_format == DISK_FORMAT_DIR) {
        fprintf(stderr, "readSector called on directory mount (track %d sector %d)\n", track, sector);
        exit(1);
    }
    if (!buffer || !disk_sector_is_valid(track, sector)) {
        fprintf(stderr, "Invalid %s sector request: track %d, sector %d\n",
                disk_format_name(), track, sector);
        exit(1);
    }

    int offset = disk_sector_offset(track, sector);
    if ((uint64_t)offset + SECTOR_SIZE > buffer_size) {
        fprintf(stderr, "Sector request beyond %s image size: track %d, sector %d, offset %d, size %llu\n",
                disk_format_name(), track, sector, offset, (unsigned long long)buffer_size);
        exit(1);
    }

    dbg_printf("Seeking to offset %08X\n", offset);
    return &buffer[offset];
}


int mount_device_image(int device_number, const char *filename, int read_only)
{
    mounted_image_t *slot = mounted_slot_for_device(device_number);
    uint8_t *new_buffer;
    char *new_filename;
    disk_image_format_t new_disk_format;
    int new_disk_max_track;
    int new_disk_read_max_track;

    if (slot == NULL) {
        fprintf(stderr, "Unsupported device number %d (expected %d-%d)\n",
                device_number, READ_D64_FIRST_DEVICE, READ_D64_LAST_DEVICE);
        return 1;
    }

    /* Check if the path is a directory — if so, mount as DISK_FORMAT_DIR */
    {
        struct stat st;
        if (stat(filename, &st) == 0 && S_ISDIR(st.st_mode)) {
            char *dir_path = (char*)malloc(strlen(filename) + 1);
            if (!dir_path) { perror("Out of memory"); return 2; }
            strcpy(dir_path, filename);

            unmount_slot(slot);
            slot->buffer = NULL;
            slot->buffer_size = 0;
            slot->mounted_filename = dir_path;
            slot->dir_cwd = strdup(dir_path);   /* CWD starts at mount root */
            slot->read_only_mode = read_only;
            slot->disk_format = DISK_FORMAT_DIR;
            slot->disk_max_track = 0;
            slot->disk_read_max_track = 0;
            slot->mounted = 1;

            printf("Mounted directory '%s' as device %d (%s)\n",
                   filename, device_number, read_only ? "read-only" : "read-write");

            if (g_current_device == device_number || g_current_device == -1)
                select_mounted_device(device_number);
            return 0;
        }
    }

    FILE* file = fopen(filename, "rb");
    if (file == NULL) {
        perror("Error opening file");
        return 1;
    }
    fseek(file, 0, SEEK_END);
    uint64_t size = ftell(file);
    dbg_printf("file size %llu bytes\n", size);

    new_buffer = (uint8_t*)malloc(size);
    if (!new_buffer)
    {
        perror("Out of memory\n");
        fclose(file);
        return 2;
    }
    fseek(file, 0, SEEK_SET);
    fread(new_buffer, 1, size, file);
    fclose(file);

    if (size == D81_IMAGE_SIZE || has_extension(filename, ".d81")) {
        new_disk_format = DISK_FORMAT_D81;
        new_disk_max_track = D81_MAX_TRACKS;
        new_disk_read_max_track = D81_MAX_TRACKS;
    } else {
        new_disk_format = DISK_FORMAT_D64;
        new_disk_read_max_track = detect_d64_max_track(size);
        new_disk_max_track = (new_disk_read_max_track < 35) ? new_disk_read_max_track : 35;
    }

    dbg_printf("Mounted %s image for device %d with %d usable tracks, %d readable tracks (%llu bytes)\n",
               (new_disk_format == DISK_FORMAT_D81) ? "D81" : "D64",
               device_number,
               new_disk_max_track,
               new_disk_read_max_track,
               (unsigned long long)size);

    new_filename = (char*)malloc(strlen(filename) + 1);
    if (new_filename == NULL) {
        free(new_buffer);
        return 2;
    }
    strcpy(new_filename, filename);

    unmount_slot(slot);
    slot->buffer = new_buffer;
    slot->buffer_size = size;
    slot->mounted_filename = new_filename;
    slot->read_only_mode = read_only;
    slot->disk_format = new_disk_format;
    slot->disk_max_track = new_disk_max_track;
    slot->disk_read_max_track = new_disk_read_max_track;
    slot->mounted = 1;

    if (g_current_device == device_number || g_current_device == -1)
        select_mounted_device(device_number);

    return 0;
}

int mount(char* filename)
{
    return mount_device_image(READ_D64_FIRST_DEVICE, filename, read_only_mode);
}

void unmount_all()
{
    for (int device_number = READ_D64_FIRST_DEVICE; device_number <= READ_D64_LAST_DEVICE; device_number++)
        unmount_slot(mounted_slot_for_device(device_number));

    g_current_device = -1;
    reset_active_globals();
}

void unmount()
{
    int device_number = g_current_device;
    mounted_image_t *slot;

    if (device_number < READ_D64_FIRST_DEVICE || device_number > READ_D64_LAST_DEVICE)
        device_number = READ_D64_FIRST_DEVICE;

    slot = mounted_slot_for_device(device_number);
    unmount_slot(slot);

    if (g_current_device == device_number)
        g_current_device = -1;

    reset_active_globals();

    for (int probe_device = READ_D64_FIRST_DEVICE; probe_device <= READ_D64_LAST_DEVICE; probe_device++) {
        if (is_device_mounted(probe_device)) {
            select_mounted_device(probe_device);
            break;
        }
    }
}

/* Handle a "cd" DOS command for DIR mounts.
   cmd: full raw command string, e.g. "cd:dirname", "cd:_", "cd//", "cd/_".
   Sets g_dir_cwd and updates the active slot.  Returns 0 on success. */
int dir_cd(const char *cmd)
{
    if (g_disk_format != DISK_FORMAT_DIR) return -1;
    if (g_current_device < READ_D64_FIRST_DEVICE) return -1;

    mounted_image_t *slot = mounted_slot_for_device(g_current_device);
    if (!slot || !slot->dir_cwd) return -1;

    const char *root = slot->mounted_filename;
    const char *cwd  = slot->dir_cwd;

    /* cd// or cd/_ : go to root */
    if (strncasecmp(cmd, "cd//", 4) == 0 || strncasecmp(cmd, "cd/_", 4) == 0) {
        char *new_cwd = strdup(root);
        if (!new_cwd) return -1;
        free(slot->dir_cwd);
        slot->dir_cwd = new_cwd;
        g_dir_cwd = new_cwd;
        printf("DIR: cd to root -> '%s'\n", g_dir_cwd);
        return 0;
    }

    /* Must start with "cd:" */
    if (strncasecmp(cmd, "cd:", 3) != 0) return -1;
    const char *target = cmd + 3;

    /* cd:_ : go up one level, never above mount root */
    if (strcmp(target, "_") == 0) {
        if (strcmp(cwd, root) == 0) {
            printf("DIR: cd:_ at root — already at top\n");
            return 0;
        }
        char *new_cwd = strdup(cwd);
        if (!new_cwd) return -1;
        char *slash = strrchr(new_cwd, '/');
        if (slash) *slash = '\0';
        /* Safety: don't escape mount root */
        if (strlen(new_cwd) < strlen(root))
            strcpy(new_cwd, root);
        free(slot->dir_cwd);
        slot->dir_cwd = new_cwd;
        g_dir_cwd = new_cwd;
        printf("DIR: cd:_ -> '%s'\n", g_dir_cwd);
        return 0;
    }

    /* cd:dirname : find a case-insensitive matching subdirectory */
    DIR *d = opendir(cwd);
    if (!d) { perror("dir_cd: opendir"); return -1; }

    struct dirent *ent;
    char found_name[512] = {0};
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strcasecmp(ent->d_name, target) == 0) {
            struct stat st;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", cwd, ent->d_name);
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
                strncpy(found_name, ent->d_name, sizeof(found_name) - 1);
                break;
            }
        }
    }
    closedir(d);

    if (!found_name[0]) {
        printf("DIR: cd:'%s' not found in '%s'\n", target, cwd);
        return -1;
    }

    char *new_cwd = (char *)malloc(strlen(cwd) + strlen(found_name) + 2);
    if (!new_cwd) return -1;
    sprintf(new_cwd, "%s/%s", cwd, found_name);
    free(slot->dir_cwd);
    slot->dir_cwd = new_cwd;
    g_dir_cwd = new_cwd;
    printf("DIR: cd:'%s' -> '%s'\n", target, g_dir_cwd);
    return 0;
}

void format_disk(const char *name, const char *id)
{
    memset(buffer, 0, buffer_size);

    if (g_disk_format == DISK_FORMAT_D64) {
        /* ---- Header / BAM sector: T18 S0 ---- */
        uint8_t *hdr = readSector(18, 0);

        hdr[0x00] = 18;    /* track link → directory track */
        hdr[0x01] = 0x01;  /* sector link */
        hdr[0x02] = 0x41;  /* DOS version 'A' */

        /* Disk name padded with $A0 */
        memset(&hdr[0x90], 0xA0, 16);
        int nlen = (int)strlen(name);
        if (nlen > 16) nlen = 16;
        memcpy(&hdr[0x90], name, nlen);

        hdr[0xA0] = 0xA0;
        hdr[0xA1] = 0xA0;
        hdr[0xA2] = (id[0]) ? (uint8_t)id[0] : 0xA0;
        hdr[0xA3] = (id[0] && id[1]) ? (uint8_t)id[1] : 0xA0;
        hdr[0xA4] = 0xA0;
        hdr[0xA5] = '2';
        hdr[0xA6] = 'A';
        hdr[0xA7] = 0xA0;
        hdr[0xA8] = 0xA0;
        hdr[0xA9] = 0xA0;
        hdr[0xAA] = 0xA0;

        /* Mark all sectors free except the directory track (18).
           After the memset, all BAM bits are 0 (= used), so calling
           disk_mark_sector_free will set each bit and update the count. */
        for (int t = 1; t <= 35; t++) {
            if (t == 18) continue;   /* leave track 18 all-used */
            int n = disk_sectors_per_track(t);
            for (int s = 0; s < n; s++)
                disk_mark_sector_free(t, s);
        }

        /* ---- First directory sector: T18 S1 ---- */
        uint8_t *dir = readSector(18, 1);
        dir[0x00] = 0x00;  /* end of chain */
        dir[0x01] = 0xFF;

    } else {
        /* ---- D81 ---- */

        /* Header sector: T40 S0 */
        uint8_t *hdr = readSector(40, 0);
        hdr[0x00] = 40;    /* track link → directory */
        hdr[0x01] = 0x03;  /* sector link */
        hdr[0x02] = 0x44;  /* 'D' */
        hdr[0x03] = 0xBB;  /* check byte */

        memset(&hdr[0x04], 0xA0, 16);
        int nlen = (int)strlen(name);
        if (nlen > 16) nlen = 16;
        memcpy(&hdr[0x04], name, nlen);

        hdr[0x14] = 0xA0;
        hdr[0x15] = 0xA0;
        hdr[0x16] = (id[0]) ? (uint8_t)id[0] : 0xA0;
        hdr[0x17] = (id[0] && id[1]) ? (uint8_t)id[1] : 0xA0;
        hdr[0x18] = 0xA0;
        hdr[0x19] = '3';
        hdr[0x1A] = 'D';

        /* BAM low sector (tracks 1–40): T40 S1 */
        uint8_t *bam_lo = readSector(40, 1);
        bam_lo[0x00] = 40;
        bam_lo[0x01] = 0x02;
        bam_lo[0x02] = 0x44;
        bam_lo[0x03] = 0xBB;

        /* BAM high sector (tracks 41–80): T40 S2 */
        uint8_t *bam_hi = readSector(40, 2);
        bam_hi[0x00] = 0x00;
        bam_hi[0x01] = 0xFF;
        bam_hi[0x02] = 0x44;
        bam_hi[0x03] = 0xBB;

        /* Mark all sectors free except track 40 (system track) */
        for (int t = 1; t <= 80; t++) {
            if (t == 40) continue;
            int n = disk_sectors_per_track(t);
            for (int s = 0; s < n; s++)
                disk_mark_sector_free(t, s);
        }

        /* ---- First directory sector: T40 S3 ---- */
        uint8_t *dir = readSector(40, 3);
        dir[0x00] = 0x00;
        dir[0x01] = 0xFF;
    }

    printf("Formatted %s image: name='%s' id='%s'\n", disk_format_name(), name, id);
}

int flush_buffer()
{
    if (read_only_mode) {
        fprintf(stderr, "Cannot flush: image is mounted read-only\n");
        return 1;
    }
    if (!buffer || !mounted_filename) {
        fprintf(stderr, "Cannot flush: no image mounted\n");
        return 1;
    }

    FILE* file = fopen(mounted_filename, "wb");
    if (file == NULL) {
        perror("Error opening image file for write");
        return 1;
    }

    size_t written = fwrite(buffer, 1, buffer_size, file);
    fclose(file);

    if (written != (size_t)buffer_size) {
        fprintf(stderr, "Error: wrote %llu bytes but expected %llu\n",
                (unsigned long long)written, (unsigned long long)buffer_size);
        return 1;
    }

    dbg_printf("Flushed %llu bytes to %s\n", (unsigned long long)buffer_size, mounted_filename);
    return 0;
}
