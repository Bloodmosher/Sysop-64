/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#ifndef READ_D64_PRIVATE_H
#define READ_D64_PRIVATE_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sysop64.h"
#include "sysop_d64.h"     /* D64/D81 format constants and track geometry table */

#define CMD_REQUESTED 0xde00
#define CMD_STATUS 0xde01
#define IO_STATUS_RETURN 0xde03
#define CPU_A 0xDE04
#define CPU_X 0xDE05
#define CPU_Y 0xDE06
#define CPU_STATUS 0xDE07
#define MOUNTED_DEVICE_LIST 0xDE29  /* 4 bytes: raw device numbers, $FF = unused slot */

#define SECTOR_SIZE 256
#define DIRECTORY_LISTING_BUFFER_SIZE 32768

typedef enum {
    DISK_FORMAT_D64,
    DISK_FORMAT_D81,
    DISK_FORMAT_DIR   /* native ARM Linux directory */
} disk_image_format_t;

#define READ_D64_FIRST_DEVICE 8
#define READ_D64_LAST_DEVICE 11

extern int g_step_mode;
extern int g_debug_enabled;
extern int read_only_mode;
extern disk_image_format_t g_disk_format;
extern char *mounted_filename;
extern char *g_dir_cwd;          /* DIR mounts: active current working directory */
extern uint8_t *cached_dir_data;
extern int cached_dir_size;
extern uint8_t *cached_file_data;
extern int cached_file_size;
void free_cached_file(void);

void dbg_printf(const char *format, ...);

const char *disk_format_name(void);
int disk_max_track(void);
int disk_directory_track(void);
int disk_directory_sector(void);
int disk_sectors_per_track(int track);
int disk_sector_is_valid(int track, int sector);
int disk_is_reserved_track(int track);
void disk_get_header(uint8_t label[16], uint8_t id[2], uint8_t dos_type[2]);
uint16_t disk_count_free_sectors(void);
int disk_is_sector_free(int track, int sector);
void disk_mark_sector_free(int track, int sector);
void disk_mark_sector_used(int track, int sector);
uint8_t *readSector(int track, int sector);
int mount(char *filename);
void unmount(void);
int mount_device_image(int device_number, const char *filename, int read_only);
int dir_cd(const char *cmd);
int select_mounted_device(int device_number);
int is_device_mounted(int device_number);
int current_mounted_device(void);
void unmount_all(void);
int flush_buffer(void);
void format_disk(const char *name, const char *id);

void loadDirectory(uint16_t addr);
int getFilename(char *filename, char *file_type);
int directory_entry_is_used(const uint8_t *entry);
void directory_entry_name(const uint8_t *entry, char *out, size_t out_size);
int directory_entry_matches_filename(const uint8_t *entry, const char *filename, int prg_wildcard);
uint16_t locateFileAndLoad(char *filename, uint16_t load_address);
int locateFileAndGetByte(char *filename, int offset, uint8_t *output);

void dump_info(void);
void dma_enable_with_verify(void);
void dma_disable_with_verify(void);
void dma_enable_wrapper(void);
void dma_disable_wrapper(void);

int handleLoad(void);
void handleSave(void);

void sigintHandler(int signal);
void init_tables(void);
void handleClose(void);
void handleOpen(void);
void handleChkIn(void);
void handleChkOut(void);
void handleClrChn(void);
void handleChrIn(void);
void handleChrOut(void);
void handleGetIn(void);
void verifyIoVectors(void);
void load_io_routines(int device_number);
void ioServerLoop(void);

#endif /* READ_D64_PRIVATE_H */
