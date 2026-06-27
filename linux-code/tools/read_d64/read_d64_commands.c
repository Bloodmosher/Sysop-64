/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "read_d64_private.h"
#include <errno.h>

int handleLoad()
{
    dma_enable_wrapper();

    uint8_t device_number = sysop_peek(0xBA);
    if (select_mounted_device(device_number) != 0) {
        dbg_printf("LOAD: device %u is not mounted\n", (unsigned int)device_number);
        sysop_io_poke(IO_STATUS_RETURN, 0x05);
        sysop_io_poke(CMD_STATUS, 0);
        sysop_io_poke(CMD_REQUESTED, 0);
        dma_disable_wrapper();
        return 1;
    }

    uint8_t cpu_a = sysop_io_peek(CPU_A);
    uint8_t cpu_x = sysop_io_peek(CPU_X);
    uint8_t cpu_y = sysop_io_peek(CPU_Y);

    dbg_printf("handleLoad CPU_A $%02X CPU_X $%02X CPU_Y $%02X\n", cpu_a, cpu_x, cpu_y);

    // sa of 0 means the header info is ignored and x y is used to form load address
    uint16_t load_address = 0x0801;
    //uint8_t secondary_addr = sysop_peek(0xb9);
    uint8_t secondary_addr = cpu_a;
    if (secondary_addr == 0)
    {
        //uint8_t b_lo = sysop_peek(0x2b);
        //uint8_t b_hi = sysop_peek(0x2c);
        uint8_t b_lo = sysop_peek(0xc3);
        uint8_t b_hi = sysop_peek(0xc4);
        load_address = (uint16_t)((b_hi<<8) | b_lo);
        dbg_printf("Secondary address was 0, using %04X as load address\n", load_address);
        //exit(-1);
    }

    char filename[255];
    char file_type = 'P';
    getFilename(filename, &file_type);

    int i, k;
    if (strcmp(filename, "$") == 0)
    {
        dbg_printf("Got $ request\n");
        loadDirectory(load_address);
    }
    else
    {
        printf("Loading file '%s' of type '%c'\n", filename, file_type);
        printf("Load address: %04X\n", load_address);
        printf("Secondary address: %02X\n", secondary_addr);
        //printf("hit enter\n");
        //getchar();
        
        // If secondary_addr is 0, use the specified load_address
        // If secondary_addr is 1 (or non-zero), pass 0 to use file's stored address
        uint16_t end_addr = locateFileAndLoad(filename, (secondary_addr == 0) ? load_address : 0);
        if (end_addr > 0) {
            printf("File loaded, end address: %04X\n", end_addr);
            sysop_poke(0xae, end_addr & 0xFF);
            sysop_poke(0xaf, (end_addr >> 8) & 0xFF);
            //sysop_io_poke(CPU_X, end_addr & 0xFF);
            //sysop_io_poke(CPU_Y, (end_addr >> 8) & 0xFF);

            // add hex dump of loaded data for debugging
            dbg_printf("Loaded data dump:\n");
            for (uint16_t addr = load_address; addr < end_addr; addr++) {
                uint8_t value = sysop_peek(addr);
                dbg_printf("%02X ", value);
                if ((addr - load_address + 1) % 16 == 0) {
                    dbg_printf("\n");
                }
            }
            dbg_printf("\n");
        } else {
            dbg_printf("File load failed\n");
        }
    }

    // TODO: need to convey errors properly
    // maybe we deal with $90 here?
    
    // set the return IO status that will go in $90
    sysop_io_poke(IO_STATUS_RETURN, 0);

    // write IO completion signal here
    sysop_io_poke(CMD_STATUS, 0);

    // clear command byte
    sysop_io_poke(CMD_REQUESTED, 0);

    if (g_step_mode) {
        dbg_printf("HIT ENTER to complete handleLoad\n");
        getchar();
    }

    dma_disable_wrapper();
    return 0;
}

void handleSave()
{
    dbg_printf("ENTER: handleSave\n");
    dma_enable_wrapper();

    uint8_t device_number = sysop_peek(0xBA);
    if (select_mounted_device(device_number) != 0) {
        dbg_printf("SAVE: device %u is not mounted\n", (unsigned int)device_number);
        sysop_io_poke(IO_STATUS_RETURN, 0x05);
        sysop_io_poke(CMD_STATUS, 0);
        sysop_io_poke(CMD_REQUESTED, 0);
        dma_disable_wrapper();
        return;
    }

    //uint16_t start_addr = sysop_peek(0xAC) | (sysop_peek(0xAD) << 8);
    uint16_t start_addr = sysop_peek(0xC1) | (sysop_peek(0xC2) << 8);
    uint16_t end_addr = sysop_peek(0xAE) | (sysop_peek(0xAF) << 8);

    char filename[255];
    char file_type = 'P';
    int save_with_replace = getFilename(filename, &file_type);

    if (filename[0] == '\0') {
        dbg_printf("SAVE: empty filename\n");
        sysop_io_poke(IO_STATUS_RETURN, 0x05);
        sysop_io_poke(CMD_STATUS, 0);
        sysop_io_poke(CMD_REQUESTED, 0);
        dma_disable_wrapper();
        return;
    }

    dbg_printf("filename = '%s' file_type = '%c' save_with_replace = %d start_addr = %04X, end_addr = %04X\n", 
               filename, file_type, save_with_replace, start_addr, end_addr);

    /* ── DIR mount: write directly to Linux filesystem ── */
    if (g_disk_format == DISK_FORMAT_DIR) {
        if (read_only_mode) {
            printf("SAVE rejected: directory mount is read-only\n");
            sysop_io_poke(IO_STATUS_RETURN, 0x05);
            sysop_io_poke(CMD_STATUS, 0);
            sysop_io_poke(CMD_REQUESTED, 0);
            dma_disable_wrapper();
            return;
        }

        const char *ext = (file_type == 'S') ? ".seq" : ".prg";
        char out_path[512];
        snprintf(out_path, sizeof(out_path), "%s/%s%s", g_dir_cwd, filename, ext);

        /* @-prefix means overwrite allowed; otherwise fail if exists */
        const char *fmode = save_with_replace ? "wb" : "wbx";
        FILE *fout = fopen(out_path, fmode);
        if (!fout) {
            if (!save_with_replace && errno == EEXIST)
                printf("SAVE: '%s' already exists (use @0: prefix to overwrite)\n", out_path);
            else
                perror("SAVE: fopen");
            sysop_io_poke(IO_STATUS_RETURN, 0x63);
            sysop_io_poke(CMD_STATUS, 0);
            sysop_io_poke(CMD_REQUESTED, 0);
            dma_disable_wrapper();
            return;
        }

        /* PRG: prepend 2-byte load address */
        if (file_type != 'S') {
            uint8_t lo = start_addr & 0xFF;
            uint8_t hi = (start_addr >> 8) & 0xFF;
            fwrite(&lo, 1, 1, fout);
            fwrite(&hi, 1, 1, fout);
        }

        for (uint16_t addr = start_addr; addr < end_addr; addr++) {
            uint8_t byte = sysop_peek(addr);
            fwrite(&byte, 1, 1, fout);
        }
        fclose(fout);

        printf("SAVE: wrote '%s' ($%04X-$%04X)\n", out_path, start_addr, end_addr);
        sysop_io_poke(IO_STATUS_RETURN, 0);
        sysop_io_poke(CMD_STATUS, 0);
        sysop_io_poke(CMD_REQUESTED, 0);
        dma_disable_wrapper();
        return;
    }

    // dump the content in hex first
    for (uint16_t addr = start_addr; addr < end_addr; addr++) {
        uint8_t value = sysop_peek(addr);
        printf("%02X ", value);
        if ((addr - start_addr + 1) % 16 == 0) {
            printf("\n");
        }
    }
    printf("\n");
    //printf("Hit enter to save the above bytes\n");
    //getchar();

    // Calculate file size in bytes and blocks
    // Note: The range from start_addr to end_addr is inclusive on start, exclusive on end
    // So end_addr points to the byte AFTER the last byte to save
    uint16_t file_size_bytes = end_addr - start_addr;
    int prepend_load_addr = (file_type == 'P');
    dbg_printf("prepend_load_addr = %d (file_type = '%c', comparison result = %d)\n", 
               prepend_load_addr, file_type, file_type == 'P');
    if (prepend_load_addr) file_size_bytes += 2; // Add 2 bytes for load address
    uint16_t file_size_blocks = (file_size_bytes + 253) / 254;  // Round up, 254 bytes per block
    
    dbg_printf("File size: %d bytes (%04X - %04X = %d + %d for load addr), %d blocks\n", 
               file_size_bytes, start_addr, end_addr, end_addr - start_addr, prepend_load_addr ? 2 : 0, file_size_blocks);
    
    // Allocate array to hold track/sector chain
    uint8_t* track_chain = (uint8_t*)malloc(file_size_blocks);
    uint8_t* sector_chain = (uint8_t*)malloc(file_size_blocks);
    
    if (!track_chain || !sector_chain) {
        dbg_printf("Memory allocation failed for sector chain\n");
        sysop_io_poke(IO_STATUS_RETURN, 0x02);  // Error
        dma_disable_wrapper();
        return;
    }
    
    int blocks_allocated = 0;
    
    // Check if file already exists and reuse its sectors if possible
    int existing_dir_track = disk_directory_track();
    int existing_dir_sector = disk_directory_sector();
    int file_exists = 0;
    uint8_t* existing_entry = NULL;
    int reused_blocks = 0;
    
    while (1) {
        uint8_t* dirBuffer = readSector(existing_dir_track, existing_dir_sector);
        
        for (int i = 0; i < 8; i++) {
            uint8_t* entry = dirBuffer + i * 32;
            if (directory_entry_matches_filename(entry, filename, 0)) {
                // File exists - preserve its type
                file_exists = 1;
                existing_entry = entry;

                // Override file_type with the existing file's type
                uint8_t existing_type = entry[2] & 0x0F;
                char old_file_type = file_type;
                if (existing_type == 0x01) file_type = 'S';      // SEQ
                else if (existing_type == 0x02) file_type = 'P'; // PRG
                else if (existing_type == 0x03) file_type = 'U'; // USR
                else if (existing_type == 0x04) file_type = 'R'; // REL

                if (old_file_type != file_type) {
                    dbg_printf("File exists with type '%c', overriding requested type '%c'\n",
                               file_type, old_file_type);
                    // Recalculate file size with correct type
                    file_size_bytes = end_addr - start_addr;
                    prepend_load_addr = (file_type == 'P');
                    if (prepend_load_addr) file_size_bytes += 2;
                    file_size_blocks = (file_size_bytes + 253) / 254;
                    dbg_printf("Recalculated: file_size_bytes=%d, prepend_load_addr=%d, file_size_blocks=%d\n",
                               file_size_bytes, prepend_load_addr, file_size_blocks);
                }

                // Check if we're allowed to replace it
                if (!save_with_replace) {
                    dbg_printf("ERROR: File '%s' already exists (FILE EXISTS error)\n", filename);
                    free(track_chain);
                    free(sector_chain);
                    sysop_io_poke(IO_STATUS_RETURN, 0x3F);  // Error code 63 (FILE EXISTS)
                    sysop_io_poke(CMD_STATUS, 0);
                    sysop_io_poke(CMD_REQUESTED, 0);
                    dma_disable_wrapper();
                    return;
                }

                uint8_t old_track = entry[3];
                uint8_t old_sector = entry[4];

                dbg_printf("File '%s' already exists, save-with-replace enabled, reusing sectors\n", filename);

                // Collect existing sectors into track_chain/sector_chain
                while (old_track != 0 && reused_blocks < file_size_blocks) {
                    // Reuse this existing sector
                    track_chain[reused_blocks] = old_track;
                    sector_chain[reused_blocks] = old_sector;
                    reused_blocks++;
                    blocks_allocated++;

                    // Get next sector in chain
                    uint8_t* oldSectorData = readSector(old_track, old_sector);
                    old_track = oldSectorData[0];
                    old_sector = oldSectorData[1];
                }

                // If old file had extra sectors, free them
                while (old_track != 0) {
                    uint8_t* oldSectorData = readSector(old_track, old_sector);
                    uint8_t next_track = oldSectorData[0];
                    uint8_t next_sector = oldSectorData[1];

                    disk_mark_sector_free(old_track, old_sector);

                    dbg_printf("Freed excess sector: track %d, sector %d\n", old_track, old_sector);

                    old_track = next_track;
                    old_sector = next_sector;
                }

                dbg_printf("Reused %d existing sectors\n", reused_blocks);
                break;
            }
        }
        
        if (file_exists) break;
        
        // Move to next directory sector
        if (dirBuffer[0] == 0) break;
        existing_dir_track = dirBuffer[0];
        existing_dir_sector = dirBuffer[1];
    }
    
    // Allocate additional sectors if needed (new file or old file was smaller)
    if (blocks_allocated < file_size_blocks) {
        dbg_printf("Need %d more sectors, allocating...\n", file_size_blocks - blocks_allocated);

        for (int track = 1; track <= disk_max_track() && blocks_allocated < file_size_blocks; track++) {
            if (disk_is_reserved_track(track)) continue;

            int sectors_per_track = disk_sectors_per_track(track);

            for (int sector = 0; sector < sectors_per_track && blocks_allocated < file_size_blocks; sector++) {
                if (disk_is_sector_free(track, sector)) {
                    // Sector is free, allocate it
                    track_chain[blocks_allocated] = track;
                    sector_chain[blocks_allocated] = sector;
                    blocks_allocated++;

                    disk_mark_sector_used(track, sector);

                    dbg_printf("Allocated new sector: track %d, sector %d\n", track, sector);
                }
            }
        }
        
        if (blocks_allocated < file_size_blocks) {
            dbg_printf("ERROR: Not enough free sectors (need %d, found %d)\n", 
                       file_size_blocks, blocks_allocated);
            free(track_chain);
            free(sector_chain);
            sysop_io_poke(IO_STATUS_RETURN, 0x02);  // Disk full error
            dma_disable_wrapper();
            return;
        }
    }
    
    dbg_printf("Total sectors available: %d\n", blocks_allocated);
    
    // Write file data to allocated sectors
    uint16_t data_offset = 0;  // Tracks position in file_size_bytes (including load address if PRG)
    uint16_t mem_offset = 0;   // Tracks position in C64 memory (start_addr to end_addr)
    for (int block = 0; block < file_size_blocks; block++) {
        uint8_t* sectorData = readSector(track_chain[block], sector_chain[block]);

        // Determine how many data bytes to write in this block
        int bytes_remaining = file_size_bytes - data_offset;
        int bytes_in_this_block = (bytes_remaining > 254) ? 254 : bytes_remaining;
        
        dbg_printf("Block %d: data_offset=%d, bytes_remaining=%d, bytes_in_this_block=%d\n",
                   block, data_offset, bytes_remaining, bytes_in_this_block);
        
        // Set next track/sector pointer or last block byte count
        if (block < file_size_blocks - 1) {
            sectorData[0] = track_chain[block + 1];
            sectorData[1] = sector_chain[block + 1];
        } else {
            // Last block: track=0, sector=number of bytes used + 1 (range 2-255)
            sectorData[0] = 0;
            sectorData[1] = bytes_in_this_block + 1;
            dbg_printf("Last block: setting sector header to [0x00, 0x%02X] (bytes_in_this_block=%d + 1)\n",
                       bytes_in_this_block + 1, bytes_in_this_block);
        }

        // Copy data from C64 memory, prepend load address for PRG
        if (prepend_load_addr && data_offset == 0) {
            // First sector, first two bytes are load address
            sectorData[2] = start_addr & 0xFF;
            sectorData[3] = (start_addr >> 8) & 0xFF;
            int mem_bytes = bytes_in_this_block - 2;
            for (int i = 0; i < mem_bytes; i++) {
                sectorData[4 + i] = sysop_peek(start_addr + mem_offset + i);
            }
            mem_offset += mem_bytes;
            data_offset += bytes_in_this_block;  // Includes the 2-byte load address
            dbg_printf("First block with load addr: wrote %d bytes (%d mem bytes + 2 addr bytes)\n",
                       bytes_in_this_block, mem_bytes);
        } else {
            for (int i = 0; i < bytes_in_this_block; i++) {
                sectorData[2 + i] = sysop_peek(start_addr + mem_offset + i);
            }
            mem_offset += bytes_in_this_block;
            data_offset += bytes_in_this_block;
        }
        dbg_printf("Wrote block %d: track %d, sector %d, %d bytes (track/sector: %d/%d)\n", 
                   block, track_chain[block], sector_chain[block], bytes_in_this_block,
                   sectorData[0], sectorData[1]);
    }
    
    // Update or create directory entry
    if (file_exists && existing_entry != NULL) {
        // Update existing entry
        existing_entry[3] = track_chain[0];  // First track
        existing_entry[4] = sector_chain[0];  // First sector
        existing_entry[0x1e] = file_size_blocks & 0xFF;
        existing_entry[0x1f] = (file_size_blocks >> 8) & 0xFF;
        dbg_printf("Updated existing directory entry\n");
    } else {
        // Find free directory entry (start fresh from beginning)
        int dir_track = disk_directory_track();
        int dir_sector = disk_directory_sector();
        int entry_found = 0;
        int entry_index = 0;
        
        dbg_printf("Searching for free directory entry starting at track %d, sector %d\n", dir_track, dir_sector);
        
        while (!entry_found) {
            uint8_t* dirBuffer = readSector(dir_track, dir_sector);
            
            dbg_printf("Checking directory sector at track %d, sector %d (next: %d,%d)\n", 
                       dir_track, dir_sector, dirBuffer[0], dirBuffer[1]);
            
            for (int i = 0; i < 8; i++) {
                uint8_t* entry = dirBuffer + i * 32;
                dbg_printf("Entry %d: type=%02X\n", i, entry[2]);
                if (entry[2] == 0) {  // Empty entry
                    entry_found = 1;
                    entry_index = i;
                    
                    // Entry 0 shares bytes 0-1 with the directory sector chain pointer.
                    if (i != 0) {
                        entry[0] = 0;
                        entry[1] = 0;
                    }
                    // Set file type: SEQ=0x81, PRG=0x82, USR=0x83, REL=0x84 (all with closed bit 0x80)
                    uint8_t type_byte = 0x82;  // Default PRG
                    if (file_type == 'S') type_byte = 0x81;      // SEQ
                    else if (file_type == 'P') type_byte = 0x82; // PRG
                    else if (file_type == 'U') type_byte = 0x83; // USR
                    else if (file_type == 'R') type_byte = 0x84; // REL
                    entry[2] = type_byte;
                    entry[3] = track_chain[0];  // First track
                    entry[4] = sector_chain[0];  // First sector
                    
                    // Copy filename (pad with 0xA0)
                    int name_len = strlen(filename);
                    for (int j = 0; j < 16; j++) {
                        if (j < name_len) {
                            entry[0x05 + j] = filename[j];
                        } else {
                            entry[0x05 + j] = 0xA0;
                        }
                    }
                    
                    // File size in blocks
                    entry[0x1e] = file_size_blocks & 0xFF;
                    entry[0x1f] = (file_size_blocks >> 8) & 0xFF;
                    
                    dbg_printf("Created directory entry at track %d, sector %d, entry %d\n",
                               dir_track, dir_sector, entry_index);
                    break;
                }
            }
            
            if (!entry_found) {
                // Move to next directory sector
                uint8_t next_track = dirBuffer[0];
                if (next_track == 0) {
                    dbg_printf("ERROR: Directory full\n");
                    free(track_chain);
                    free(sector_chain);
                    sysop_io_poke(IO_STATUS_RETURN, 0x02);
                    dma_disable_wrapper();
                    return;
                }
                dir_track = next_track;
                dir_sector = dirBuffer[1];
            }
        }
    }
    
    dbg_printf("File saved successfully\n");
    
    // Dump sector data before flushing
    printf("\n=== SECTORS TO BE WRITTEN ===\n");
    for (int block = 0; block < file_size_blocks; block++) {
        uint8_t* sectorData = readSector(track_chain[block], sector_chain[block]);
        printf("Track %d, Sector %d:\n", track_chain[block], sector_chain[block]);
        for (int i = 0; i < 256; i++) {
            printf("%02X ", sectorData[i]);
            if ((i + 1) % 16 == 0) {
                printf("\n");
            }
        }
        printf("\n");
    }
    printf("=== END SECTOR DUMP ===\n\n");
    free(track_chain);
    free(sector_chain);
    
    // Write the modified buffer back to the disk file
    if (flush_buffer() != 0) {
        dbg_printf("ERROR: Failed to flush buffer to disk\n");
        sysop_io_poke(IO_STATUS_RETURN, 0x02);  // Write error
    } else {
        sysop_io_poke(IO_STATUS_RETURN, 0);
    }

    if (g_step_mode) {
        dbg_printf("HIT ENTER to complete handleSave\n");
        getchar();
    }

    sysop_io_poke(CMD_STATUS, 0);
    sysop_io_poke(CMD_REQUESTED, 0);

    dma_disable_wrapper();
    printf("EXIT: handleSave\n");   
}
