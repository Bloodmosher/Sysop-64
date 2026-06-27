/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

int sysop_loadbin(const char *filename, uint16_t address)
{
    FILE *file = NULL;
    unsigned char *buffer = NULL;

    int result = 0;

    printf("loading %s...\n", filename);
    file = fopen(filename, "rb"); // Open the .prg file in binary read mode

    if (file == NULL)
    {
        fprintf(stderr, "Error opening file.\n");
        return 1;
    }

    fseek(file, 0, SEEK_END);    // Move the file pointer to the end of the file
    long fileSize = ftell(file); // Get the file size
    fseek(file, 0, SEEK_SET);    // Reset the file pointer to the beginning

    buffer = (unsigned char *)malloc(fileSize); // Allocate memory to store the program

    if (buffer == NULL)
    {
        fprintf(stderr, "Memory allocation failed.\n");
        fclose(file);
        return 1;
    }

    fread(buffer, sizeof(unsigned char), fileSize, file); // Read the file into the buffer
    fclose(file);                                         // Close the file

    printf("Loading at 0x%04X\n", address);

    // Load the program into memory at the correct address
    int i;
    uint8_t val;
    int wrong = 0;
    for (i = 0; i < fileSize; i++)
    {
        sysop_poke((uint16_t)(address + i), (uint8_t)buffer[i]);

        sysop_dma_wait_empty();
        sysop_dma_wait_not_busy();
        val = sysop_peek((uint16_t)(address + i));

        if (val != (uint8_t)buffer[i])
        {
            printf("Verify 1 - incorrect byte at %04X (%02X expected, got %02X)\n", (uint16_t)(address + i), (uint8_t)buffer[i], val);
            wrong++;
        }
    }
    if (wrong > 0)
    {
        printf("Verify found %d incorrect bytes\n", wrong);
    }

    free(buffer); 

    printf("Waiting for DMA queue to drain...\n");
    
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    return 0;
}

#define MEMORY_START_ADDRESS 0x0801

int sysop_load(const char *filename)
{
    FILE *file = NULL;
    unsigned char *buffer = NULL;
    uint16_t load_address = 0;

    int result = 0;

    file = fopen(filename, "rb"); // Open the .prg file in binary read mode

    if (file == NULL)
    {
        fprintf(stderr, "Error opening file.\n");
        return 1;
    }

    fseek(file, 0, SEEK_END);    // Move the file pointer to the end of the file
    long fileSize = ftell(file); // Get the file size
    fseek(file, 0, SEEK_SET);    // Reset the file pointer to the beginning

    buffer = (unsigned char *)malloc(fileSize); // Allocate memory to store the program

    if (buffer == NULL)
    {
        fprintf(stderr, "Memory allocation failed.\n");
        fclose(file);
        return 1;
    }

    fread(buffer, sizeof(unsigned char), fileSize, file); // Read the file into the buffer
    fclose(file);                                         // Close the file

    load_address = (uint16_t)((buffer[1] << 8) | buffer[0]);

    printf("Loading %s at 0x%04X\n", filename, load_address);

    // Load the program into memory at the correct address
    int i;
    uint8_t val;
    uint16_t addr;
    int wrong = 0;
    for (i = 2; i < fileSize; i++)
    {
        addr = (uint16_t)(load_address + (i - 2));
        sysop_poke(addr, (uint8_t)buffer[i]);
    }

    wrong = 0;
    printf("Verifying...\n");
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    for (i = 2; i < fileSize; i++)
    {
        addr = (uint16_t)(load_address + (i - 2));

        if (addr >= 0xa000 && addr < 0xc000)
            continue; // skip verifying in the basic area

        val = sysop_peek(addr);

        if (val != (uint8_t)buffer[i])
        {
            wrong++;
        }
    }
    printf("Verification found %d incorrect bytes\n", wrong);

    free(buffer); // Free the allocated buffer

    // make sure basic's variable start location is set; some programs may depend on this
    uint16_t variable_start = (fileSize - 2) + 0x801;
    sysop_poke(0x2D, variable_start & 0xFF);
    sysop_poke(0x2E, (variable_start & 0xFF00) >> 8);
    printf("Set BASIC variable start pointer $2D to %04X\n", variable_start);
    return 0;
}

static uint16_t byte_swap_16(uint16_t value)
{
    return (value >> 8) | (value << 8);
}

static int handle_cartridge_data(uint8_t *buffer, size_t length, uint16_t bank_number, uint16_t chip_load_address, int verifyOnly)
{
    if (bank_number != 0)
    {
        printf("Supposed to load bank %d at address %04x, but banking not yet supported\n", bank_number, chip_load_address);
        return -1;
    }

    int result = 0;

    // Load the program into memory at the correct address
    int i;
    uint8_t val;
    uint16_t addr;
    int wrong = 0;
    if (verifyOnly != 1)
    {
        printf("Loading at 0x%04X (cart space)\n", chip_load_address);

        for (i = 0; i < length; i++)
        {
            addr = (uint16_t)(chip_load_address + i);
            sysop_cartridge_poke(addr, (uint8_t)buffer[i]);
            sysop_cartridge_poke(addr, (uint8_t)buffer[i]);

            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();
        }
    }

    // TODO: verification will need an internal read capability since we cannot
    // guarantee that the c64 has enabled the cartridge
    if (chip_load_address == 0xe000)
    {
        sysop_dma_wait_empty();
        sysop_dma_wait_not_busy();
        for (i = 0; i < 0x1000; i++)
        {
            sysop_poke(0x3000 + i, buffer[0x1000 + i]);
        }
        sysop_dma_wait_empty();
        sysop_dma_wait_not_busy();
        sysop_cartridge_enable_ultimax();
    }

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    // for now assuming it is enabled...
    for (i = 0; i < length; i++)
    {
        addr = (uint16_t)(chip_load_address + i);
        val = sysop_peek(addr);
        if (val != buffer[i])
        {
            printf("Verify failed at 0x%04X, expected %hx, got %hx\n", addr, buffer[i], val);
            wrong++;
        }
    }

    printf("Wrong bytes: %d\n", wrong);
    printf("Finished loading cartridge data\n");

    return 0;
}

static int cartridge_load_raw(const char *filename, int verifyOnly);

int sysop_cartridge_load(const char *filename, int verifyOnly)
{
    if (strstr(filename, ".bin") != 0)
    {
        return cartridge_load_raw(filename, verifyOnly);
    }

    // Open the CRT file
    FILE *file = fopen(filename, "rb");
    if (!file)
    {
        perror("Error opening file");
        return 1;
    }

    // Read and verify the header
    CRTHeader header;
    if (fread(&header, sizeof(CRTHeader), 1, file) != 1)
    {
        fprintf(stderr, "Error reading CRT header\n");
        fclose(file);
        return 1;
    }

    // Check the signature
    if (strncmp(header.signature, "C64 CARTRIDGE   ", 16) != 0)
    {
        fprintf(stderr, "Invalid CRT file signature\n");
        fclose(file);
        return 1;
    }

    uint16_t rom_size;
    int ultimax = 0;
    // Process chip packets
    while (!feof(file))
    {
        CRTChip chip;

        // Read the CHIP header
        if (fread(&chip, sizeof(CRTChip), 1, file) != 1)
        {
            if (feof(file))
                break; // End of file
            fprintf(stderr, "Error reading CHIP header\n");
            fclose(file);
            return 1;
        }

        // Verify "CHIP" signature
        if (strncmp(chip.chip_type, "CHIP", 4) != 0)
        {
            fprintf(stderr, "Invalid CHIP packet\n");
            fclose(file);
            return 1;
        }

        // Read the ROM data
        rom_size = byte_swap_16(chip.rom_size);
        uint8_t *rom_data = (uint8_t *)malloc(rom_size);
        if (!rom_data)
        {
            fprintf(stderr, "Memory allocation failed\n");
            fclose(file);
            return 1;
        }

        if (fread(rom_data, rom_size, 1, file) != 1)
        {
            fprintf(stderr, "Error reading ROM data\n");
            free(rom_data);
            fclose(file);
            return 1;
        }

        uint16_t load_address = byte_swap_16(chip.load_address);
        printf("Cartridge load address %04X\n", load_address);
        if (load_address == 0xe000)
            ultimax = 1;

        uint16_t bank_number = byte_swap_16(chip.bank_number);

        // Determine if it's an 8K or 16K cartridge
        if (rom_size == 0x2000 || rom_size == 0x4000)
        {
            printf("Detected %s cartridge\n", rom_size == 0x2000 ? "8K" : "16K");
            handle_cartridge_data(rom_data, rom_size, bank_number, load_address, verifyOnly);
        }
        else
        {
            printf("Skipping unsupported ROM size: %u bytes\n", rom_size);
        }

        free(rom_data);
    }

    fclose(file);
    if (!verifyOnly)
    {
        if (ultimax)
        {
            printf("Enabling ultimax\n");
            sysop_cartridge_enable_ultimax();
        }
        else
        {
            sysop_cartridge_enable(rom_size);
        }
        //    sysop_c64_reset();
    }
    return 0;
}

static int cartridge_load_raw(const char *filename, int verifyOnly)
{
    FILE *file = NULL;
    unsigned char *buffer = NULL;
    uint16_t load_address = 0;

    int result = 0;

    printf("loading cartridge data from %s...\n", filename);
    file = fopen(filename, "rb");

    if (file == NULL)
    {
        fprintf(stderr, "Error opening file.\n");
        return 1;
    }

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    buffer = (unsigned char *)malloc(fileSize);

    if (buffer == NULL)
    {
        fprintf(stderr, "Memory allocation failed.\n");
        fclose(file);
        return 1;
    }

    fread(buffer, sizeof(unsigned char), fileSize, file);
    fclose(file);

    // load_address = 0x8000;
    load_address = 0;

    // Load the program into memory at the correct address
    int i;
    uint8_t val;
    uint16_t addr;
    int wrong = 0;
    if (verifyOnly != 1)
    {
        printf("Loading at 0x%04X (cart space)\n", load_address);

        for (i = 0; i < fileSize; i++)
        {
            addr = (uint16_t)(load_address + i);
            sysop_cartridge_poke(addr, (uint8_t)buffer[i]);

            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();

            val = sysop_peek(0x8000 + i);
            if (val != buffer[i])
            {
                printf("Verify failed at 0x%04X, expected %hx, got %hx\n", addr, buffer[i], val);
                wrong++;
            }
        }
    }

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    // for now assuming it is enabled...
    for (i = 0; i < fileSize; i++)
    {
        addr = (uint16_t)(0x8000 + i);
        val = sysop_peek(addr);
        if (val != buffer[i])
        {
            printf("Verify failed at 0x%04X, expected %hx, got %hx\n", addr, buffer[i], val);
            wrong++;
        }
    }

    printf("Wrong bytes: %d\n", wrong);

    printf("Finished loading cartridge data\n");
    free(buffer);

    return 0;
}

int sysop_kernal_load(const char *filename, int verifyOnly)
{
    FILE *file = NULL;
    unsigned char *buffer = NULL;
    uint16_t load_address = 0;

    int result = 0;

    printf("loading kernal data from %s...\n", filename);
    file = fopen(filename, "rb");

    if (file == NULL)
    {
        fprintf(stderr, "Error opening file.\n");
        return 1;
    }

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    buffer = (unsigned char *)malloc(fileSize);

    if (buffer == NULL)
    {
        fprintf(stderr, "Memory allocation failed.\n");
        fclose(file);
        return 1;
    }

    fread(buffer, sizeof(unsigned char), fileSize, file);
    fclose(file);

    // load_address = 0x8000;
    load_address = 0;

    // TODO: this needs to enforce kernal size of 8192 bytes

    // Load the program into memory at the correct address
    int i;
    uint8_t val;
    uint16_t addr;
    int wrong = 0;
    if (verifyOnly != 1)
    {
        printf("Loading at 0x%04X (kernal space)\n", load_address);

        for (i = 0; i < fileSize; i++)
        {
            addr = (uint16_t)(load_address + i);
            sysop_kernal_poke(addr, (uint8_t)buffer[i]);

            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();

            val = sysop_peek(0xE000 + i);
            if (val != buffer[i])
            {
                printf("Verify failed at 0x%04X, expected %hx, got %hx\n", addr, buffer[i], val);
                wrong++;
            }
        }
    }

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    // for now assuming it is enabled...
    for (i = 0; i < fileSize; i++)
    {
        addr = (uint16_t)(0xE000 + i);
        val = sysop_peek(addr);
        if (val != buffer[i])
        {
            printf("Verify failed at 0x%04X, expected %hx, got %hx\n", addr, buffer[i], val);
            wrong++;
        }
    }

    printf("Wrong bytes: %d\n", wrong);

    printf("Finished loading kernal data\n");
    free(buffer); // Free the allocated buffer

    return 0;
}

// TODO: make this use SEQ and REL properly
// Right now this assumes PRG which means the first 2 bytes of the file contain
// a sysop_load address.
int sysop_load_buffer_at(uint8_t *buffer, int count, uint16_t load_address)
{
    int result = 0;
    printf("Loading at 0x%04X\n", load_address);

    // Load the program into memory at the correct address
    int i;
    for (i = 2; i < count; i++)
    {
        sysop_dma_wait_empty();
        sysop_dma_wait_not_busy();
        sysop_poke((uint16_t)(load_address + (i - 2)), (uint8_t)buffer[i]);
    }
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    int wrong = 0;
    uint16_t addr = 0;
    uint8_t val = 0;
    printf("Verifying...\n");
    for (i = 2; i < count; i++)
    {
        addr = (uint16_t)(load_address + (i - 2));
        sysop_dma_wait_empty();
        sysop_dma_wait_not_busy();
        if (addr < 0xa000 || addr >= 0xc000)
        {
            val = sysop_peek(addr);
            if (val != (uint8_t)buffer[i])
            {
                printf("Verify 2 - incorrect byte at %04X (%02X expected, got %02X)\n", addr, (uint8_t)buffer[i], val);
                wrong++;
            }
        }
    }
    printf("Verification found %d incorrect bytes\n", wrong);

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    // make sure basic's variable start location is set; some programs may depend on this
    uint16_t variable_start = (count - 2) + 0x801;
    sysop_poke(0x2D, variable_start & 0xFF);
    sysop_poke(0x2E, (variable_start & 0xFF00) >> 8);
    printf("Set BASIC variable start pointer $2D to %04X\n", variable_start);

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    printf("Waiting for DMA queue to drain...\n");
    get_library_lock();
    while (1)
    {
        uint64_t status = *((uint64_t *)sysop64_fpga_status_map);
        if ((status & 0x8000000000000000) != 0)
            break;
    }
    release_library_lock();
    return 0;
}

int sysop_load_buffer(uint8_t *buffer, int count)
{
    uint16_t load_address = 0;

    load_address = (uint16_t)((buffer[1] << 8) | buffer[0]);

    return sysop_load_buffer_at(buffer, count, load_address);
}
