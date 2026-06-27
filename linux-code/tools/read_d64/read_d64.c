/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "read_d64_private.h"
#include <sys/stat.h>
#include <sys/types.h>

static int is_mount_option(const char *arg)
{
    return strcmp(arg, "step") == 0 || strcmp(arg, "debug") == 0;
}

int main(int argc, char** argv) 
{
    if (argc < 2) {
        printf("Expected arguments: <path to d64/d81 file> <cmd> <options>\n");
        printf("cmd is one of:\n");
        printf("dir  - Show directory\n");
        printf("load - load followed by <filename> or just use '*'\n");
        printf("dump - dump <filename> - Display all sectors of a file in hex\n");
        printf("extract <output_dir> - extract all files from image into a directory\n");
        printf("mount <device#> <r|rw> [<path> <device#> <r|rw> ...] [step] [debug] - mount images for devices 8-11 and run IO server\n");
        return -1;
    }

    init_tables();

    char* command = NULL;
    char* filename = NULL;
    char* output = NULL;

    if (argc > 2)
    {
        command = argv[2];
    }
    if (argc > 3)
    {
        filename = argv[3];
    }
    if (command == NULL)
    {
        printf("Missing command. Expected: dir, load, dump, or mount\n");
        unmount();
        return 1;
    }

    if (strcmp(command, "mount") != 0) {
        if (mount(argv[1]) != 0)
        {
            return 1;
        }
    }

    if (strcmp(command, "dump")==0)
    {
        if (filename == NULL)
        {
            printf("dump command requires a filename\n");
            unmount();
            return 1;
        }
        
        // Search for the file in the directory
        int track = disk_directory_track();
        int sector = disk_directory_sector();
        int found = 0;
        
        while(1) 
        {        
            uint8_t* directoryBuffer = readSector(track, sector);

            // Iterate through the directory entries
            for (int i = 0; i < 8; i++) {
                uint8_t* entry = directoryBuffer + i * 32;

                if (directory_entry_is_used(entry)) {
                    uint16_t fileSize = (entry[0x1f] * 256) + entry[0x1e];

                    // Extract and null-terminate filename
                    char entryName[17];
                    directory_entry_name(entry, entryName, sizeof(entryName));

                    if (directory_entry_matches_filename(entry, filename, 1))
                    {
                        found = 1;
                        printf("=== Directory Entry Details ===\n");
                        printf("Filename: %s\n", entryName);
                        
                        // File type details
                        uint8_t fileTypeByte = entry[0x02];
                        uint8_t fileType = fileTypeByte & 0x0F;
                        const char* typeStr = "???";
                        switch(fileType) {
                            case 0: typeStr = "DEL"; break;
                            case 1: typeStr = "SEQ"; break;
                            case 2: typeStr = "PRG"; break;
                            case 3: typeStr = "USR"; break;
                            case 4: typeStr = "REL"; break;
                        }
                        printf("File Type: %s (0x%02X)\n", typeStr, fileTypeByte);
                        printf("  - Type code: %d\n", fileType);
                        printf("  - Closed flag: %s\n", (fileTypeByte & 0x80) ? "Yes" : "No");
                        printf("  - Locked flag: %s\n", (fileTypeByte & 0x40) ? "Yes" : "No");
                        
                        printf("First Track: %d (0x%02X)\n", entry[0x03], entry[0x03]);
                        printf("First Sector: %d (0x%02X)\n", entry[0x04], entry[0x04]);
                        printf("File size: %d blocks (%d bytes)\n", fileSize, fileSize * 254);
                        printf("File size bytes: Lo=0x%02X Hi=0x%02X\n", entry[0x1e], entry[0x1f]);
                        
                        // Show raw directory entry in hex
                        printf("\nRaw Directory Entry (32 bytes):\n");
                        for (int j = 0; j < 32; j++) {
                            printf("%02X ", entry[j]);
                            if ((j + 1) % 16 == 0) printf("\n");
                        }
                        printf("\n");
                        
                        uint8_t fileTrack = entry[0x03];
                        uint8_t fileSector = entry[0x04];
                        
                        printf("\n=== Sector Data Dump ===\n\n");
                        
                        int blockNum = 0;
                        while(1)
                        {
                            uint8_t* fileBuffer = readSector(fileTrack, fileSector);
                            
                            printf("=== Block %d (Track %d, Sector %d) ===\n", 
                                   blockNum, fileTrack, fileSector);
                            
                            // Interpret first two bytes
                            uint8_t byte0 = fileBuffer[0];
                            uint8_t byte1 = fileBuffer[1];
                            if (byte0 == 0) {
                                printf("Last block: %d bytes used (bytes 2-%d contain data)\n", 
                                       byte1 - 1, byte1);
                            } else {
                                printf("Next block: Track %d, Sector %d\n", byte0, byte1);
                            }
                            
                            // Dump the entire 256-byte sector in hex
                            for (int j = 0; j < 256; j++) {
                                printf("%02X ", fileBuffer[j]);
                                if ((j + 1) % 16 == 0) {
                                    printf("\n");
                                }
                            }
                            printf("\n");
                            
                            blockNum++;
                            
                            // Check if this is the last block
                            if (byte0 == 0) {
                                printf("End of file (last block)\n");
                                break;
                            }
                                
                            fileTrack = byte0;
                            fileSector = byte1;
                        }
                        
                        printf("Total blocks dumped: %d\n", blockNum);
                        break;
                    }
                }
            }
            
            if (found)
                break;

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
        
        if (!found) {
            printf("File '%s' not found\n", filename);
            unmount();
            return 1;
        }
        
        unmount();
        return 0;
    }
    else if (strcmp(command, "extract") == 0)
    {
        const char *out_dir = (argc >= 4) ? argv[3] : NULL;
        if (out_dir == NULL) {
            printf("extract command requires an output directory\n");
            printf("Usage: %s <image> extract <output_dir>\n", argv[0]);
            unmount();
            return 1;
        }

        /* Create output directory if it doesn't exist */
        struct stat dstat;
        if (stat(out_dir, &dstat) != 0) {
            if (mkdir(out_dir, 0755) != 0) {
                perror("extract: mkdir");
                unmount();
                return 1;
            }
            printf("Created directory: %s\n", out_dir);
        } else if (!S_ISDIR(dstat.st_mode)) {
            printf("extract: '%s' exists but is not a directory\n", out_dir);
            unmount();
            return 1;
        }

        int track = disk_directory_track();
        int sector = disk_directory_sector();
        int extracted = 0;
        int errors = 0;

        while (1) {
            uint8_t *dirBuf = readSector(track, sector);

            for (int i = 0; i < 8; i++) {
                uint8_t *entry = dirBuf + i * 32;
                if (!directory_entry_is_used(entry)) continue;

                char c64name[17];
                directory_entry_name(entry, c64name, sizeof(c64name));

                uint8_t file_type_byte = entry[0x02] & 0x0F;
                const char *ext;
                switch (file_type_byte) {
                    case 1:  ext = "seq"; break;
                    case 2:  ext = "prg"; break;
                    case 3:  ext = "usr"; break;
                    case 4:  ext = "rel"; break;
                    default: ext = "prg"; break;
                }

                /* Sanitize filename: replace chars invalid on Linux */
                char safe[17];
                for (int j = 0; j < 16 && c64name[j]; j++) {
                    unsigned char ch = (unsigned char)c64name[j];
                    safe[j] = (ch == '/' || ch == '\0') ? '_' : (char)ch;
                }
                safe[strlen(c64name)] = '\0';

                char out_path[768];
                snprintf(out_path, sizeof(out_path), "%s/%s.%s", out_dir, safe, ext);

                FILE *f = fopen(out_path, "wb");
                if (!f) {
                    perror(out_path);
                    errors++;
                    continue;
                }

                /* Read sector chain and write file data.
                   For PRG files the first 2 data bytes are already the
                   load address — no special prefix needed. */
                uint8_t ft = entry[0x03];
                uint8_t fs = entry[0x04];
                int ok = 1;
                while (1) {
                    uint8_t *sec = readSector(ft, fs);
                    uint8_t nt = sec[0];
                    uint8_t ns = sec[1];
                    int nbytes = (nt == 0) ? (ns - 1) : 254;

                    fwrite(&sec[2], 1, (size_t)nbytes, f);

                    if (nt == 0) break;
                    ft = nt;
                    fs = ns;
                }
                fclose(f);

                if (ok) {
                    printf("  %s\n", out_path);
                    extracted++;
                } else {
                    errors++;
                }
            }

            if (dirBuf[0] == 0) break;
            track = dirBuf[0];
            sector = dirBuf[1];
        }

        printf("Extracted %d file(s) to '%s'", extracted, out_dir);
        if (errors) printf(", %d error(s)", errors);
        printf("\n");
        unmount();
        return errors ? 1 : 0;
    }
    else if (strcmp(command, "mount")==0)
    {
        if (argc < 5) {
            printf("mount command requires at least one device number (8-11) and mode (r or rw)\n");
            printf("Usage: %s <disk-image> mount <device#> <r|rw> [<disk-image> <device#> <r|rw> ...] [step] [debug]\n", argv[0]);
            unmount_all();
            return 1;
        }

        const char *image_path = argv[1];
        int arg_index = 3;
        int first_mount = 1;
        while (arg_index < argc && !is_mount_option(argv[arg_index]))
        {
            if (arg_index + 1 >= argc) {
                printf("Incomplete mount specification for image %s\n", image_path);
                unmount_all();
                return 1;
            }

            int device_number = atoi(argv[arg_index]);
            if (device_number < READ_D64_FIRST_DEVICE || device_number > READ_D64_LAST_DEVICE) {
                printf("Device number must be between %d and %d\n", READ_D64_FIRST_DEVICE, READ_D64_LAST_DEVICE);
                unmount_all();
                return 1;
            }

            char* mode = argv[arg_index + 1];
            int mount_read_only;
            if (strcmp(mode, "r") == 0) {
                mount_read_only = 1;
                printf("Mounting %s as device %d (read-only)\n", image_path, device_number);
            } else if (strcmp(mode, "rw") == 0) {
                mount_read_only = 0;
                printf("Mounting %s as device %d (read-write)\n", image_path, device_number);
            } else {
                printf("Mode must be 'r' (read-only) or 'rw' (read-write)\n");
                unmount_all();
                return 1;
            }

            if (mount_device_image(device_number, image_path, mount_read_only) != 0) {
                unmount_all();
                return 1;
            }

            arg_index += 2;
            first_mount = 0;
            if (arg_index >= argc || is_mount_option(argv[arg_index]))
                break;

            image_path = argv[arg_index++];
        }

        if (first_mount) {
            printf("mount command requires at least one device number and mode\n");
            unmount_all();
            return 1;
        }

        for (int i = arg_index; i < argc; i++)
        {
            if (strcmp(argv[i], "step") == 0)
            {
                g_step_mode = 1;
                g_debug_enabled = 1;
                printf("Step mode enabled\n");
            }
            else if (strcmp(argv[i], "debug") == 0)
            {
                g_debug_enabled = 1;
                printf("Debug mode enabled\n");
            }
            else {
                printf("Unknown mount option: %s\n", argv[i]);
                unmount_all();
                return 1;
            }
        }
        sysop_init();
        int res = sysop_server_connect();
        if (res == -1)
        {
            printf("sysop_server_connect() failed\n");
            return 1;
        }
        load_io_routines(READ_D64_FIRST_DEVICE);
        ioServerLoop();
    }
    else if (strcmp(command, "ioreq")==0)
    {
        sysop_open_bridge();
        dbg_printf("TODO: finish this\n");
        sysop_close_bridge();
        unmount();
        return 0;
    }
    else if (strcmp(command, "load") == 0)
    {
        if (filename != NULL && strcmp(filename, "$") == 0)
        {
            printf("Loading directory\n");
            
            sysop_open_bridge();
            dma_enable_wrapper();
            loadDirectory(0x0801);
            dma_disable_wrapper();
            sysop_close_bridge();
            
            return -1;
        }
        if (filename != NULL)
        {
            printf("Loading file %s\n", filename);
        }
        else
        {
            printf("Missing filename\n");
            return 1;
        }
    }
    else if (strcmp(command, "write") == 0)
    {
        if (argc < 5) {
            printf("write command requires a filename argument\n");
            unmount();
            return 1;
        }
        output = argv[4];
        dbg_printf("Writing file %s\n", output);
    }

    
    g_debug_enabled = 1; // so printing works
    
    // Traverse all directory sectors
    int dir_track = disk_directory_track();
    int dir_sector = disk_directory_sector();
    
    while (1) {
        // Read the directory sector
        uint8_t* directoryBuffer = readSector(dir_track, dir_sector);

        // Iterate through the directory entries
        for (int i = 0; i < 8; i++) {
            // Each entry is 32 bytes
            uint8_t* entry = directoryBuffer + i * 32;
            dbg_printf("offset %08X\n", (i*32));

            if (directory_entry_is_used(entry)) {
                char entryName[17];
                dbg_printf("Filename in hex: ");
                for (int a=0x05;a<0x05+16;a++)
                {
                    dbg_printf("%02X ", entry[a]);
                }
                dbg_printf("\n");

                directory_entry_name(entry, entryName, sizeof(entryName));
                dbg_printf("Filename: %s\n", entryName);


                uint16_t fileSize = (entry[0x1f] * 256) + entry[0x1e];
                dbg_printf("File size %02X%02X (%d blocks)\n", entry[0x1e], entry[0x1f], fileSize);

                // Other information can be extracted from the entry
                // For example, the file type, starting track, and starting sector
                dbg_printf("File Type: %d\n", entry[0x02] & 0xF);
                dbg_printf("Start Track: %d\n", entry[0x03]);
                dbg_printf("Start Sector: %d\n", entry[0x04]);

            if (filename != NULL && strcmp(command, "write") == 0 &&
                directory_entry_matches_filename(entry, filename, 0))
            {
                dbg_printf("found matching file, writing to disk...\n");

                FILE* outFile = fopen(output, "wb");
                if (outFile == NULL) {
                    perror("Error opening output file");
                    unmount();
                    return 1;
                }
                fseek(outFile, 0, SEEK_SET);
                uint8_t track = entry[0x03];
                uint8_t sector = entry[0x04];

                uint8_t* fileBuffer;
                while(1)
                {
                    fileBuffer = readSector(track, sector);
                    uint8_t nextTrack = fileBuffer[0];
                    uint8_t nextSector = fileBuffer[1];
                    
                    // Determine how many bytes to write
                    int bytesToWrite;
                    if (nextTrack == 0) {
                        // Last sector: nextSector contains byte count (1-254)
                        bytesToWrite = nextSector - 1;
                        dbg_printf("Last sector: writing %d bytes\n", bytesToWrite);
                    } else {
                        // Not last sector: write full 254 bytes
                        bytesToWrite = 254;
                    }
                    
                    fwrite(&fileBuffer[2], 1, bytesToWrite, outFile);
                    
                    if (nextTrack == 0)
                        break;
                        
                    track = nextTrack;
                    sector = nextSector;
                    dbg_printf("next track %d, sector %d\n", (int)track, (int)sector);
                }
                fclose(outFile);
            }
            // assume we're going to load
            else if (filename != NULL && strcmp(command, "load") == 0 &&
                     directory_entry_matches_filename(entry, filename, 1))
            {
                dbg_printf("found matching file, loading...\n");

                uint8_t track = entry[0x03];
                uint8_t sector = entry[0x04];

                uint8_t* data = malloc(fileSize*254);
                if (!data)
                {
                    dbg_printf("Memory allocation failed\n");
                    return 1;
                }
                int dataIndex = 0;
                uint8_t* fileBuffer;
                while(1)
                {
                    fileBuffer = readSector(track, sector);
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
                    
                    memcpy(&data[dataIndex], &fileBuffer[2], bytesToCopy);
                    dataIndex += bytesToCopy;
                    
                    if (nextTrack == 0)
                        break;
                        
                    track = nextTrack;
                    sector = nextSector;
                    dbg_printf("next track %d, sector %d\n", (int)track, (int)sector);
                }
                sysop_open_bridge();
                dma_enable_wrapper();
                sysop_load_buffer(data, dataIndex);
                dma_disable_wrapper();
                sysop_close_bridge();
                free(data);
                return 0;
            }

            dbg_printf("\n\n");
            }  // Close if (entry[0] != 0 ...)
        }  // Close for (int i = 0; i < 8; i++)
        
        // Move to next directory sector
        if (directoryBuffer[0] == 0) {
            break;  // End of directory chain
        }
        dir_track = directoryBuffer[0];
        dir_sector = directoryBuffer[1];
    }  // Close while (1)

    unmount();
    return 0;
}
