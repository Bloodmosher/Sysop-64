/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "read_d64_private.h"

void sigintHandler(int signal)
{
    unmount_all();
    sysop_close_bridge();
}

#define MAX_OPEN_FILES 10
char g_open_filenames[MAX_OPEN_FILES][255];
uint8_t g_chkin_logical_file = 0;
uint8_t g_chkout_logical_file = 0;
uint8_t logical_file_to_filename[255];
uint8_t logical_file_to_secondary_address[255];
int logical_file_to_size[255];
uint8_t logical_file_to_device[255];
uint8_t logical_files_open[255];

// Block read (U1) support
uint8_t channel_block_buffer[16][256];  // Buffer for each channel (0-15)
int channel_block_valid[16];             // Whether channel has valid block data
int channel_block_position[16];          // Current read position in block buffer

// Command channel (15) status buffer
char command_status[256];                // Status message for command channel
int command_status_length = 0;           // Length of status message
int command_status_position = 0;         // Read position in status message

int g_chkin_position = -1;
int g_chkin_file_length = -1;
int g_chkout_position = -1;

static int ensure_mounted_device(uint8_t device_number, const char *operation)
{
    if (select_mounted_device(device_number) == 0)
        return 1;

    dbg_printf("%s: device %u is not mounted\n", operation, (unsigned int)device_number);
    sysop_io_poke(IO_STATUS_RETURN, 0x05);
    sysop_io_poke(CMD_STATUS, 0);
    sysop_io_poke(CMD_REQUESTED, 0);
    return 0;
}

static void write_mounted_device_list(void)
{
    // Write up to 4 mounted device numbers into the stub's device_list at $DE29.
    // Unused slots are filled with $FF (no-match sentinel).
    int slot = 0;
    for (int dev = READ_D64_FIRST_DEVICE; dev <= READ_D64_LAST_DEVICE && slot < 4; dev++) {
        if (is_device_mounted(dev))
            sysop_poke(MOUNTED_DEVICE_LIST + slot++, (uint8_t)dev);
    }
    while (slot < 4)
        sysop_poke(MOUNTED_DEVICE_LIST + slot++, 0xFF);
}

void init_tables()
{
    for (int i=0;i<MAX_OPEN_FILES;i++)
    {
        g_open_filenames[i][0] = '\0';
    }
    memset(logical_file_to_filename, 0, sizeof(logical_file_to_filename));
    memset(logical_file_to_device, 0, sizeof(logical_file_to_device));
    memset(logical_files_open, 0, sizeof(logical_files_open));
    memset(logical_file_to_secondary_address, 0, sizeof(logical_file_to_secondary_address));
    memset(channel_block_valid, 0, sizeof(channel_block_valid));
    memset(channel_block_position, 0, sizeof(channel_block_position));
    memset(command_status, 0, sizeof(command_status));
    command_status_length = 0;
    command_status_position = 0;
    // Set initial "OK" status
    strcpy(command_status, "00, OK,00,00\r");
    command_status_length = strlen(command_status);
    g_chkin_logical_file = 0;
    g_chkout_logical_file = 0;
}

void handleClose()
{
    dbg_printf("ENTER: handleClose\n");
    dma_enable_wrapper();

    uint8_t open_files = sysop_peek(0x98);

    uint8_t file_a = sysop_peek(CPU_A);

    dbg_printf("Closing logical file %d\n", file_a);
    int found = 0;
    
    for (uint8_t i=0;i<open_files;i++)
    {
        uint8_t entry = sysop_peek(0x259+i);
        if (entry == file_a)
        {
            dbg_printf("Found file %d at index %d, closing\n", file_a, i);
            found = 1;
            // if the entry is the last open file, just mark as closed
            if (i == (open_files - 1))
            {
                dbg_printf("Entry was last in tables\n");
                sysop_poke(0x259+i, 0);
                int idx = logical_file_to_filename[file_a];
                g_open_filenames[idx][0] = '\0';
                logical_file_to_filename[file_a] = 0;

                sysop_poke(0x26d+i, 0); // clear secondary addr entry
                sysop_poke(0x263+i, 0); // clear device number entry
            }
            else // otherwise copy the last entry over the one we are closine
            {
                dbg_printf("Entry was not last in tables, so swapping w/ last position and marking end closed\n");
                // logical file
                uint8_t tmp = sysop_peek(0x259+(open_files-1));
                sysop_poke(0x259+i, tmp);
                sysop_poke(0x259+(open_files-1), 0);

                // secondary address
                tmp = sysop_peek(0x26d+(open_files-1));
                sysop_poke(0x26d+i, tmp);
                sysop_poke(0x26d+(open_files-1), 0);

                // device number
                tmp = sysop_peek(0x263+(open_files-1));
                sysop_poke(0x263+i, tmp);
                sysop_poke(0x263+(open_files-1), 0);
            }

            sysop_poke(0x98, open_files-1);
            break;
        }
    }
    if (!found)
    {
        dbg_printf("Warning: logical file %d not found in open files table\n", file_a);
    }
    
    // set the return IO status that will go in $90
    sysop_io_poke(IO_STATUS_RETURN, 0); // ? non-zero is errors; what do they do?

    // write IO completion signal here
    sysop_io_poke(CMD_STATUS, 0);
    // clear command byte
    sysop_io_poke(CMD_REQUESTED, 0);

    if (g_step_mode) {
        dbg_printf("HIT ENTER to complete handleClose\n");
        getchar();
    }

    dma_disable_wrapper();
}

uint8_t test_buffer[4096];

// on entry assume we have already checked whether
// there are too many open files
/* Execute a DOS N (format/NEW) command.
   Caller must have DMA enabled. Does not call sysop_dma_enable/disable itself. */
static void handle_n_command(const char *cmd)
{
    if (g_disk_format == DISK_FORMAT_DIR) {
        printf("FORMAT: ignored for directory mount\n");
        snprintf(command_status, sizeof(command_status), "00, OK,00,00\r");
        command_status_length = strlen(command_status);
        command_status_position = 0;
        sysop_io_poke(IO_STATUS_RETURN, 0);
        return;
    }

    if (read_only_mode) {
        printf("FORMAT rejected: image is mounted read-only\n");
        snprintf(command_status, sizeof(command_status),
                 "26,WRITE PROTECT ON,00,00\r");
        command_status_length = strlen(command_status);
        command_status_position = 0;
        sysop_io_poke(IO_STATUS_RETURN, 0x02);
        return;
    }

    uint8_t device_number = sysop_peek(0xBA);
    if (!ensure_mounted_device(device_number, "FORMAT"))
        return;

    const char *colon = strchr(cmd, ':');
    if (colon == NULL) {
        snprintf(command_status, sizeof(command_status),
                 "33,SYNTAX ERROR,00,00\r");
        command_status_length = strlen(command_status);
        command_status_position = 0;
        sysop_io_poke(IO_STATUS_RETURN, 0x02);
        return;
    }

    char disk_name[17] = {0};
    char disk_id[3]    = {0};
    const char *name_start = colon + 1;
    const char *comma      = strchr(name_start, ',');

    if (comma) {
        int nlen = (int)(comma - name_start);
        if (nlen > 16) nlen = 16;
        memcpy(disk_name, name_start, nlen);
        strncpy(disk_id, comma + 1, 2);
    } else {
        strncpy(disk_name, name_start, 16);
        disk_id[0] = 0xA0;
        disk_id[1] = 0xA0;
    }

    printf("FORMAT: name='%s' id='%s'\n", disk_name, disk_id);
    format_disk(disk_name, disk_id);

    if (flush_buffer() == 0) {
        snprintf(command_status, sizeof(command_status), "00, OK,00,00\r");
        sysop_io_poke(IO_STATUS_RETURN, 0);
    } else {
        snprintf(command_status, sizeof(command_status), "25,WRITE ERROR,00,00\r");
        sysop_io_poke(IO_STATUS_RETURN, 0x02);
    }
    command_status_length = strlen(command_status);
    command_status_position = 0;
}

void handleOpen()
{
    dbg_printf("ENTER: handleOpen\n");
    dma_enable_wrapper();

    uint8_t open_files = sysop_peek(0x98);
    if (open_files >= MAX_OPEN_FILES) {
        dbg_printf("ERROR: Too many open files (%d), cannot open another\n", open_files);
        sysop_io_poke(IO_STATUS_RETURN, 0x02);
        sysop_io_poke(CMD_STATUS, 0);
        sysop_io_poke(CMD_REQUESTED, 0);
        dma_disable_wrapper();
        return;
    }

    char filename[255];
    char file_type = 'P';
    getFilename(filename, &file_type);

    dbg_printf("Open %s\n", filename);

    uint8_t logical_file = sysop_peek(0xb8);
    dbg_printf("using logical file $%02X\n", logical_file);
    sysop_poke(0x259+open_files, logical_file); // save in the logical file table

    logical_files_open[logical_file] = 1;
    logical_file_to_filename[logical_file] = open_files;
    strcpy(&g_open_filenames[open_files][0], filename);
    
    uint8_t secondary_address = sysop_peek(0xb9);
    
    // TODO: is this needed?
    //secondary_address |= 0x60; // OR with OPEN CHANNEL
    
    sysop_poke(0xb9, secondary_address); // save back agin
    sysop_poke(0x26d+open_files, secondary_address); // save in secondary address table

    dbg_printf("using secondary address $%02X\n", secondary_address);
    if (secondary_address == 15)
    {
        dbg_printf("Command channel opened\n");
        
        // Check for M-E (Memory Execute) command - need to read raw bytes from C64 memory
        // because address bytes after M-E might contain null or non-printable chars
        uint8_t filenameLo = sysop_peek(0xbb);
        uint8_t filenameHi = sysop_peek(0xbc);
        uint16_t filenameAddr = filenameHi << 8 | filenameLo;
        uint8_t filenameCount = sysop_peek(0xb7);
        
        dbg_printf("Raw filename: %d bytes at $%04X\n", filenameCount, filenameAddr);
        
        /* Always dump raw hex of command channel filename for diagnostics */
        if (filenameCount > 0) {
            dbg_printf("Raw filename hex: ");
            for (int i = 0; i < filenameCount; i++)
                dbg_printf("%02X ", sysop_peek(filenameAddr + i));
            dbg_printf("\n");
        }

        if (filenameCount >= 3) {
            uint8_t byte0 = sysop_peek(filenameAddr);
            uint8_t byte1 = sysop_peek(filenameAddr + 1);
            uint8_t byte2 = sysop_peek(filenameAddr + 2);
            
            // Check for M-R (0x4D 0x2D 0x52) - Memory Read from drive address space
            if (byte0 == 0x4D && byte1 == 0x2D && byte2 == 0x52)
            {
                if (filenameCount >= 5) {
                    uint8_t addr_lo = sysop_peek(filenameAddr + 3);
                    uint8_t addr_hi = sysop_peek(filenameAddr + 4);
                    uint16_t address = (addr_hi << 8) | addr_lo;
                    uint8_t count = (filenameCount >= 6) ? sysop_peek(filenameAddr + 5) : 1;
                    if (count == 0) count = 1;
                    if (count > 255) count = 255;

                    printf("\n=== M-R (Memory Read) Command ===\n");
                    printf("Drive address: $%04X  count: %d\n", address, count);
                    printf("(Virtual drive has no ROM — returning zeros)\n");
                    printf("=================================\n\n");

                    /* Return 'count' zero bytes via the command channel */
                    memset(command_status, 0, count);
                    command_status_length = count;
                    command_status_position = 0;
                } else {
                    printf("M-R command detected but missing address bytes (need 5, have %d)\n",
                           filenameCount);
                }
            }
            // Check for M-E (0x4D 0x2D 0x45) or M-E: (0x4D 0x2D 0x45 0x3A)
            if (byte0 == 0x4D && byte1 == 0x2D && byte2 == 0x45)
            {
                int has_colon = 0;
                int addr_offset = 3;
                
                if (filenameCount >= 4 && sysop_peek(filenameAddr + 3) == 0x3A) {
                    has_colon = 1;
                    addr_offset = 4;
                }
                
                if (filenameCount >= addr_offset + 2) {
                    uint8_t addr_lo = sysop_peek(filenameAddr + addr_offset);
                    uint8_t addr_hi = sysop_peek(filenameAddr + addr_offset + 1);
                    uint16_t address = (addr_hi << 8) | addr_lo;
                    
                    printf("\n=== M-E (Memory Execute) Command ===\n");
                    printf("Command: M-E%s\n", has_colon ? ":" : "");
                    printf("Filename length: %d bytes\n", filenameCount);
                    printf("Raw filename bytes: ");
                    for (int i = 0; i < filenameCount; i++) {
                        printf("%02X ", sysop_peek(filenameAddr + i));
                    }
                    printf("\n");
                    printf("Execute address: $%04X (lo=$%02X, hi=$%02X)\n",
                           address, addr_lo, addr_hi);
                    printf("ERROR: M-E command is not supported yet\n");
                    printf("=====================================\n\n");

                    printf("Hit enter to skip unsupported M-E command\n");
                    getchar();
                    /*
                    // Set error status
                    sysop_io_poke(IO_STATUS_RETURN, 0x02);
                    sysop_io_poke(CMD_STATUS, 0);
                    sysop_io_poke(CMD_REQUESTED, 0);
                    sysop_dma_disable();
                    exit(1);
                    */
                } else {
                    printf("M-E command detected but missing address bytes (need %d, have %d)\n",
                           addr_offset + 2, filenameCount);
                }
            }
        }

        // Execute other DOS commands sent as the filename on SA=15
        // (e.g. OPEN 1,8,15,"N0:name,id")
        if (filenameCount > 0) {
            char cmd[256] = {0};
            int cmdlen = filenameCount < 255 ? (int)filenameCount : 255;
            for (int i = 0; i < cmdlen; i++)
                cmd[i] = (char)sysop_peek(filenameAddr + i);
            if (cmd[0] == 'N' || cmd[0] == 'n') {
                handle_n_command(cmd);
            } else if ((cmd[0] == 'C' || cmd[0] == 'c') && (cmd[1] == 'D' || cmd[1] == 'd')) {
                /* CD command from fb64 et al.:
                     "cd:dirname"  — enter subdirectory
                     "cd:_"        — go up one level
                     "cd//"        — go to root
                     "cd/_"        — go to root (fb64 variant)             */
                if (g_disk_format == DISK_FORMAT_DIR) {
                    int r = dir_cd(cmd);
                    if (r == 0) {
                        snprintf(command_status, sizeof(command_status), "00, OK,00,00\r");
                    } else {
                        snprintf(command_status, sizeof(command_status),
                                 "62,FILE NOT FOUND,00,00\r");
                    }
                    command_status_length = strlen(command_status);
                    command_status_position = 0;
                    sysop_io_poke(IO_STATUS_RETURN, r == 0 ? 0 : 0x02);
                } else {
                    dbg_printf("CD command on non-DIR mount ignored: %s\n", cmd);
                    snprintf(command_status, sizeof(command_status), "00, OK,00,00\r");
                    command_status_length = strlen(command_status);
                    command_status_position = 0;
                }
            }
        }
    }
    logical_file_to_secondary_address[logical_file] = secondary_address;

    uint8_t device_number = sysop_peek(0xba);
    if (!ensure_mounted_device(device_number, "OPEN")) {
        dma_disable_wrapper();
        return;
    }

    logical_file_to_device[logical_file] = device_number; // save in our address space for convenience?
    sysop_poke(0x263+open_files, device_number); // save in the device number table

    sysop_poke(0x98, open_files+1);

    // set the return IO status that will go in $90
    sysop_io_poke(IO_STATUS_RETURN, 0); // ? non-zero is errors; what do they do?

    // write IO completion signal here
    sysop_io_poke(CMD_STATUS, 0);
    // clear command byte
    sysop_io_poke(CMD_REQUESTED, 0);

    
    if (g_step_mode) {
        dbg_printf("HIT ENTER to complete handleOpen\n");
        getchar();
    }

    dma_disable_wrapper();// will do a wait
    dbg_printf("EXIT: handleOpen\n");
}

void handleChkIn()
{
    dbg_printf("ENTER: handleChkIn\n");
    uint8_t x = sysop_io_peek(CPU_X);
    uint8_t y = sysop_io_peek(CPU_Y);
    uint8_t a = sysop_io_peek(CPU_A);
    dbg_printf("CPU_A: $%02X CPU_X: $%02X CPU_Y: $%02X\n", a, x, y);
    dbg_printf("Using logical file $%02X\n", x);

    uint8_t device_number = logical_file_to_device[x];
    //if (g_open_filenames[logical_file_to_filename[x]] == '\0') {
    if (device_number == 0) {
        dbg_printf("We don't have file open, let kernal handle\n");
        // write IO completion signal here
        sysop_io_poke(CMD_STATUS, 2);
        // clear command byte
        sysop_io_poke(CMD_REQUESTED, 0);
        return;
    }
    if (!ensure_mounted_device(device_number, "CHKIN"))
        return;

    // required so that subsequent CHRIN knows whether to come back here
    dbg_printf("Setting current input device at $99 to $%02X\n", device_number);
    dma_enable_wrapper(); 
    sysop_poke(0x0099, device_number);
    dma_disable_wrapper();

    dbg_printf("CHKIN: Set current logical file input channel to %d\n", x);
    g_chkin_logical_file = x;
    g_chkin_position = 0;

    // Get the channel number from secondary address
    uint8_t secondary_addr = logical_file_to_secondary_address[x];
    uint8_t channel = secondary_addr & 0x0F;  // Lower 4 bits
    
    // If this is the command channel (15), reset status read position
    if (channel == 15) {
        command_status_position = 0;
        dbg_printf("Command channel 15: reset status read position\n");
    }
    
    // If this channel has a valid block buffer, reset position to start
    if (channel_block_valid[channel]) {
        channel_block_position[channel] = 0;
        dbg_printf("Channel %d has valid block buffer, reset position to 0\n", channel);
    }

    // TODO: this is temporary for testing
    g_chkin_file_length = 10;

    if (g_step_mode) {
        dbg_printf("HIT ENTER to complete handleChkIn\n");
        getchar();
    }

    // write IO completion signal here
    sysop_io_poke(CMD_STATUS, 1);
    // clear command byte
    sysop_io_poke(CMD_REQUESTED, 0);

    sysop_io_poke(IO_STATUS_RETURN, 0);
    //sysop_poke(0x90, 0);

    dbg_printf("EXIT: handleChkIn\n");
}

void handleChkOut()
{
    dbg_printf("ENTER: handleChkOut\n");
    //usleep(50000);
    uint8_t x = sysop_io_peek(CPU_X);
    uint8_t y = sysop_io_peek(CPU_Y);
    uint8_t a = sysop_io_peek(CPU_A);
    dbg_printf("CPU_A: $%02X CPU_X: $%02X CPU_Y: $%02X\n", a, x, y);
    dbg_printf("Using logical file $%02X\n", x);

    uint8_t device_number = logical_file_to_device[x];
    //if (g_open_filenames[logical_file_to_filename[x]] == '\0') {
    if (device_number == 0) {
        dbg_printf("We don't have file open, let kernal handle\n");
        // write IO completion signal here
        sysop_io_poke(CMD_STATUS, 2);
        // clear command byte
        sysop_io_poke(CMD_REQUESTED, 0);
        return;
    }
    if (!ensure_mounted_device(device_number, "CHKOUT"))
        return;


    // required so that subsequent CHROUT knows whether to come back here
    dbg_printf("Setting current output device at $9A to $%02X\n", device_number);
    dma_enable_wrapper(); 
    sysop_poke(0x009A, device_number);
    dma_disable_wrapper();

    dbg_printf("CHKOUT: Set current logical file output channel to %d\n", x);
    g_chkout_logical_file = x;
    g_chkout_position = 0;

    if (g_step_mode) {
        dbg_printf("HIT ENTER to complete handleChkOut\n");
        getchar();
    }

    // write IO completion signal here
    sysop_io_poke(CMD_STATUS, 1);
    // clear command byte
    sysop_io_poke(CMD_REQUESTED, 0);

    sysop_io_poke(IO_STATUS_RETURN, 0);
    //sysop_poke(0x90, 0);

    dbg_printf("EXIT: handleChkOut\n");
}

void handleClrChn()
{
    dbg_printf("ENTER: handleClrChn\n");
    //init_tables();

    dma_enable_wrapper();

    if (g_chkout_logical_file != 0) {
        dbg_printf("Clearing active output file\n");
        g_chkout_logical_file = 0;

        //sysop_dma_enable();
        dbg_printf("Restoring default output device at $9A to $03 (screen)\n");
        sysop_poke(0x009A, 0x03);
        //sysop_dma_disable();
    }
    if (g_chkin_logical_file != 0) {
        dbg_printf("Clearing active input file\n");
        g_chkin_logical_file = 0;
        g_chkin_position = 0;
        if (cached_dir_data != NULL) {
            free(cached_dir_data);
            cached_dir_data = NULL;
            cached_dir_size = 0;
        }
        free_cached_file();

        //sysop_dma_enable();
        dbg_printf("Restoring default input device at $99 to $00 (keyboard)\n");
        sysop_poke(0x0099, 0x00);
        //sysop_dma_disable();
    }

    if (g_step_mode) {
        dbg_printf("HIT ENTER to complete handleClrChn\n");
        getchar();
    }

    // TODO: not sure we should be updated $90 on return here
    sysop_io_poke(IO_STATUS_RETURN, 0);

    // write IO completion signal here
    sysop_io_poke(CMD_STATUS, 0);
    // clear command byte
    sysop_io_poke(CMD_REQUESTED, 0);

    dbg_printf("EXIT: handleClrChn\n");
    dma_disable_wrapper();
}

void handleChrIn()
{
    dbg_printf("ENTER: handleChrIn\n");
    dma_enable_wrapper();

    int idx = logical_file_to_filename[g_chkin_logical_file];
    char* filename = &g_open_filenames[idx][0];
    dbg_printf("ChrIn on file %s, current position %d, length %d\n", filename, g_chkin_position, g_chkin_file_length);

    if (g_chkin_logical_file != 0) {
        // Get the channel number from secondary address
        uint8_t secondary_addr = logical_file_to_secondary_address[g_chkin_logical_file];
        uint8_t channel = secondary_addr & 0x0F;  // Lower 4 bits
        
        // Check if this is the command channel (15) - return status
        if (channel == 15) {
            dbg_printf("Reading status from command channel 15, position %d of %d\n", 
                   command_status_position, command_status_length);
            
            if (command_status_position < command_status_length) {
                uint8_t data = (uint8_t)command_status[command_status_position];
                sysop_io_poke(CPU_A, data);
                dbg_printf("Read status byte %02X ('%c') from position %d\n", 
                       data, (data >= 32 && data < 127) ? data : '.', command_status_position);
                command_status_position++;
                
                // Check if we've read entire status message
                if (command_status_position >= command_status_length) {
                    dbg_printf("Reached end of status message\n");
                    sysop_io_poke(IO_STATUS_RETURN, 0x40);  // EOF
                } else {
                    sysop_io_poke(IO_STATUS_RETURN, 0);
                }
            } else {
                dbg_printf("Already at end of status message\n");
                sysop_io_poke(IO_STATUS_RETURN, 0x40);  // EOF
            }
        }
        // Check if this channel has a valid block buffer
        else if (channel_block_valid[channel]) {
            dbg_printf("Reading from block buffer for channel %d, position %d\n", 
                   channel, channel_block_position[channel]);
            
            if (channel_block_position[channel] < 256) {
                uint8_t data = channel_block_buffer[channel][channel_block_position[channel]];
                sysop_io_poke(CPU_A, data);
                dbg_printf("Read byte %02X from block buffer position %d\n", 
                       data, channel_block_position[channel]);
                channel_block_position[channel]++;
                
                // Check if we've read all 256 bytes
                if (channel_block_position[channel] >= 256) {
                    dbg_printf("Reached end of block buffer (256 bytes)\n");
                    sysop_io_poke(IO_STATUS_RETURN, 0x40);  // EOF
                } else {
                    sysop_io_poke(IO_STATUS_RETURN, 0);
                }
            } else {
                dbg_printf("Already at end of block buffer\n");
                sysop_io_poke(IO_STATUS_RETURN, 0x40);  // EOF
            }
        } else {
            // Original file reading logic
            uint8_t output = 0;
            int bytesRead = locateFileAndGetByte(filename, g_chkin_position, &output);
            if (bytesRead > 0) {
                sysop_io_poke(CPU_A, output);
                uint8_t val = sysop_io_peek(CPU_A);
                if (output != val) {
                    dbg_printf("WARNING: Mismatch between expected %02X and actual %02X in CPU_A\n", output, val);
                    //getchar();
                }
                dbg_printf("Read byte %02X from file at position %d\n", output, g_chkin_position);
                g_chkin_position++;
                sysop_io_poke(IO_STATUS_RETURN, 0);
            }
            else {
                // Don't reset position or free cache — keep them at end so
                // repeated CHRIN calls after EOF keep returning EOF, not restarting.
                // Cache is freed in handleClose / handleClrChn.
                dbg_printf("Signaling end of file\n");
                sysop_io_poke(CPU_A, 0x0d); // first noticed this in SID-Wizard code; Kernal routine also does this
                uint8_t val = sysop_io_peek(CPU_A);
                if (val != 0x0d) {
                    dbg_printf("WARNING: Mismatch between expected %02X and actual %02X in CPU_A\n", output, val);
                    //getchar();
                }

                sysop_io_poke(IO_STATUS_RETURN, 0x40);
            }
        }
    }
    else {
        dbg_printf("ERROR: CHRIN but CHKIN not called first\n");
        sysop_io_poke(IO_STATUS_RETURN, 0x80);
    }

    sysop_io_poke(CMD_STATUS, 0);
    // clear command byte
    sysop_io_poke(CMD_REQUESTED, 0);

    if (g_step_mode) {
        dbg_printf("HIT ENTER to complete handleChrIn\n");
        getchar();
    }

    dma_disable_wrapper();
    dbg_printf("g_chkin_position is now %d\n", g_chkin_position);
    dbg_printf("EXIT: handleChrIn\n");
}


void handleChrOut()
{
    dbg_printf("ENTER: handleChrOut\n");
    dma_enable_wrapper();
    uint8_t val = sysop_peek(0x009a);
    dbg_printf("Current output device at $9A is %02X\n", val);

    /*
    if (g_chkout_logical_file == 0) {
        dbg_printf("We do not have a channel set for output, pass back to kernal\n");
        // write IO completion signal here
        sysop_io_poke(CMD_STATUS, 0);
        // clear command byte
        sysop_io_poke(CMD_REQUESTED, 0);
        goto exit;
    }
    */

    if (g_chkout_logical_file != 0) {
        // TODO: write this to the actual file we have open...
        uint8_t data = sysop_io_peek(CPU_A);
        test_buffer[g_chkout_position] = data;
        dbg_printf("Wrote %02X at position %d\n", data, g_chkout_position);
        g_chkout_position++;
        // check for command
        if (data == 0x0d)  {
            test_buffer[g_chkout_position] = '\0';
            dbg_printf("DISK COMMAND: %s\n", test_buffer);
            
            // Parse U+ soft reset command
            // Format: "U+" - resets drive, clears buffers, closes channels
            if ((test_buffer[0] == 'U' || test_buffer[0] == 'u') && 
                (test_buffer[1] == '+')) {
                
                dbg_printf("U+ Soft Reset command received\n");
                
                // Clear all channel buffers
                memset(channel_block_valid, 0, sizeof(channel_block_valid));
                memset(channel_block_position, 0, sizeof(channel_block_position));
                
                // Reset input/output channels
                g_chkin_logical_file = 0;
                g_chkout_logical_file = 0;
                g_chkin_position = -1;
                g_chkin_file_length = -1;
                g_chkout_position = -1;
                
                // Reset command status to OK
                strcpy(command_status, "00, OK,00,00\r");
                command_status_length = strlen(command_status);
                command_status_position = 0;
                
                dbg_printf("Drive reset complete\n");
                sysop_io_poke(IO_STATUS_RETURN, 0);  // Success
            }
            // Parse U1 block read command
            // Format: "U1" channel drive track sector or "U1:" channel drive track sector
            else if ((test_buffer[0] == 'U' || test_buffer[0] == 'u') && 
                (test_buffer[1] == '1')) {
                
                int channel, drive, track, sector;
                int parsed = 0;
                
                // Try format with colon: "U1:channel drive track sector" (most common)
                if (test_buffer[2] == ':') {
                    // Try with spaces (default format)
                    parsed = sscanf((char*)&test_buffer[3], "%d %d %d %d", 
                                   &channel, &drive, &track, &sector);
                    if (parsed != 4) {
                        // Try with commas
                        parsed = sscanf((char*)&test_buffer[3], "%d,%d,%d,%d", 
                                       &channel, &drive, &track, &sector);
                    }
                    if (parsed != 4) {
                        // Try with semicolons
                        parsed = sscanf((char*)&test_buffer[3], "%d;%d;%d;%d", 
                                       &channel, &drive, &track, &sector);
                    }
                } else if (test_buffer[2] == ' ') {
                    // Format without colon but with space: "U1 channel drive track sector"
                    parsed = sscanf((char*)&test_buffer[3], "%d %d %d %d", 
                                   &channel, &drive, &track, &sector);
                    if (parsed != 4) {
                        parsed = sscanf((char*)&test_buffer[3], "%d,%d,%d,%d", 
                                       &channel, &drive, &track, &sector);
                    }
                    if (parsed != 4) {
                        parsed = sscanf((char*)&test_buffer[3], "%d;%d;%d;%d", 
                                       &channel, &drive, &track, &sector);
                    }
                } else {
                    // Try format without colon or space
                    parsed = sscanf((char*)&test_buffer[2], "%d %d %d %d", 
                                   &channel, &drive, &track, &sector);
                    if (parsed != 4) {
                        parsed = sscanf((char*)&test_buffer[2], "%d,%d,%d,%d", 
                                       &channel, &drive, &track, &sector);
                    }
                    if (parsed != 4) {
                        parsed = sscanf((char*)&test_buffer[2], "%d;%d;%d;%d", 
                                       &channel, &drive, &track, &sector);
                    }
                }
                
                if (parsed == 4) {
                    dbg_printf("U1 Block Read: channel=%d, drive=%d, track=%d, sector=%d\n",
                           channel, drive, track, sector);
                    
                    // Validate parameters
                    if (channel >= 0 && channel <= 15 && drive == 0 &&
                        disk_sector_is_valid(track, sector)) {
                        
                        // Read the block from disk
                        uint8_t* blockData = readSector(track, sector);
                        
                        // Copy to channel buffer
                        memcpy(channel_block_buffer[channel], blockData, 256);
                        channel_block_valid[channel] = 1;
                        channel_block_position[channel] = 0;  // Start at beginning
                        
                        dbg_printf("Block read successful, 256 bytes loaded to channel %d buffer\n", channel);
                        
                        // Set success status message
                        snprintf(command_status, sizeof(command_status), 
                                "00, OK,00,00\r");
                        command_status_length = strlen(command_status);
                        command_status_position = 0;
                        
                        sysop_io_poke(IO_STATUS_RETURN, 0);  // Success
                    } else {
                        dbg_printf("U1 Block Read: Invalid parameters\n");
                        
                        // Set error status message
                        snprintf(command_status, sizeof(command_status), 
                                "24,READ ERROR,00,00\r");
                        command_status_length = strlen(command_status);
                        command_status_position = 0;
                        
                        sysop_io_poke(IO_STATUS_RETURN, 0x02);  // Error
                    }
                } else {
                    dbg_printf("U1 Block Read: Failed to parse command\n");
                    
                    // Set syntax error status message
                    snprintf(command_status, sizeof(command_status), 
                            "30,SYNTAX ERROR,00,00\r");
                    command_status_length = strlen(command_status);
                    command_status_position = 0;
                    
                    sysop_io_poke(IO_STATUS_RETURN, 0x02);  // Error
                }
            }
            // N command: format disk  "N0:name,id" or "N:name,id"
            else if (test_buffer[0] == 'N' || test_buffer[0] == 'n') {
                handle_n_command((char*)test_buffer);
            }
            else {
                // Unknown/unsupported disk command
                printf("\n=== UNSUPPORTED DISK COMMAND ===\n");
                printf("Command received: %s\n", test_buffer);
                printf("Hex dump:\n");
                for (int i = 0; i < g_chkout_position && i < 256; i++) {
                    printf("%02X ", test_buffer[i]);
                    if ((i + 1) % 16 == 0) printf("\n");
                }
                printf("\n");
                printf("Length: %d bytes\n", g_chkout_position);
                printf("================================\n");

                // Set error status
                snprintf(command_status, sizeof(command_status),
                        "31,SYNTAX ERROR,00,00\r");
                command_status_length = strlen(command_status);
                command_status_position = 0;
                sysop_io_poke(IO_STATUS_RETURN, 0x02);
                
                // Exit to allow user to see the command
                dma_disable_wrapper();
                unmount_all();
                sysop_close_bridge();
                exit(1);
            }
            
            // Reset buffer position for next command
            g_chkout_position = 0;
        }
    }
    else {
        dbg_printf("ERROR: CHROUT but CHKOUT not called first\n");
    }

    sysop_io_poke(CMD_STATUS, 0);
    // clear command byte
    sysop_io_poke(CMD_REQUESTED, 0);

    sysop_io_poke(IO_STATUS_RETURN, 0);
    //sysop_poke(0x90, 0x00); 

    uint8_t border = sysop_peek(0xd020);
    dbg_printf("BORDER: $%02X\n", border);
    dbg_printf("EXIT: handleChrOut\n");

    if (g_step_mode) {
        dbg_printf("HIT ENTER to complete handleChrOut\n");
        getchar();
    }

    dma_disable_wrapper();
}

void handleGetIn()
{
    dbg_printf("handleGetIn(), deferring to handleChrIn\n");
    handleChrIn();
}
uint8_t expected_open_lo = 0;
uint8_t expected_open_hi = 0;

/* Load a binary file by name, searching the current directory first,
   then falling back to /usr/local/bin. */
static void loadbin_search(const char *filename, uint32_t address)
{
    if (access(filename, R_OK) == 0) {
        dbg_printf("loadbin_search: using ./%s\n", filename);
        sysop_loadbin(filename, address);
    } else {
        char path[512];
        snprintf(path, sizeof(path), "/usr/local/bin/%s", filename);
        dbg_printf("loadbin_search: falling back to %s\n", path);
        sysop_loadbin(path, address);
    }
}

void verifyIoVectors()
{
    // only do this every 100 ms
    static struct timespec last_check_time = {0, 0};
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    
    uint64_t last_ms = last_check_time.tv_sec * 1000 + last_check_time.tv_nsec / 1000000;
    uint64_t current_ms = current_time.tv_sec * 1000 + current_time.tv_nsec / 1000000;
    
    if (current_ms - last_ms < 100) {
        return;
    }
    last_check_time = current_time;

    uint8_t vec1 = sysop_internal_peek(0x031a);
    uint8_t vec2 = sysop_internal_peek(0x031b);
    if (vec1 != expected_open_lo || vec2 != expected_open_hi)
    {
        dbg_printf("IO Vectors not set correctly! $031A=%02X %02X\n", vec1, vec2);
        dbg_printf("Restoring...\n");
        sysop_server_dma_lock();
        loadbin_search("vectors.o", 0x31a);
        sysop_server_dma_unlock();
    }
}

void load_io_routines(int device_number)
{   
    sysop_enable_io();
    
    sysop_server_dma_lock();
    loadbin_search("diskio.o", 0xde00);
    loadbin_search("vectors.o", 0x31a);

    expected_open_lo = sysop_peek(0x31a);
    expected_open_hi = sysop_peek(0x31b);
    
    // Write mounted device numbers into stub's 4-slot device_list at $DE29.
    // NOTE: must use direct dma_wait_* (not sysop_dma_wait_*) inside sysop_server_dma_lock().
    write_mounted_device_list();
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    dbg_printf("Set mounted device list at $%04x (requested primary device %d)\n",
               MOUNTED_DEVICE_LIST, device_number);
    
    sysop_server_dma_unlock();
}

void ioServerLoop()
{
    dbg_printf("Starting IO Server\n");
    while(1)
    {
        //verifyIoVectors();

        uint8_t val = sysop_io_peek(CMD_REQUESTED);
        if (val != 0) {
            verifyIoVectors();
            dbg_printf("CMD_REQUESTED: %d\n", val);
            dump_info();
        }

        switch(val)
        {
            case 0x01: handleOpen();    break;
            case 0x02: handleClose();   break;
            case 0x03: handleChkIn();   break;
            case 0x04: handleChkOut();  break;
            case 0x05: handleClrChn();  break;
            case 0x06: handleChrIn();   break;
            case 0x07: handleChrOut();   break;
            case 0x08: handleGetIn();   break;
            case 0x0b: handleLoad();    break;
            case 0x0c: handleSave();    break;
            default:
            { 
                if (val != 0) {
                    dbg_printf("Unknown IO request %hx\n", val);
                    dbg_printf("hit enter\n");
                    getchar();
                }
            }
            break;
        }
        //usleep(50000);
        usleep(1000);
    }
}
