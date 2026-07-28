// Sysop-64
// https://github.com/Bloodmosher/Sysop-64
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sysop-64 Project

/*
 * sidwiz is a host-side control tool for a running SID-Wizard editor on the
 * C64.  It talks through libsysop64, asserts DMA when it needs a stable view
 * of C64 memory, and then patches SID-Wizard's in-memory song, pattern,
 * order-list, and instrument structures directly.
 *
 * The tool exists for two main workflows:
 *
 *   1. Interactive/developer utility work from the HPS command line, such as
 *      loading an instrument, dumping a pattern, changing an order list, or
 *      starting/stopping SID-Wizard playback.
 *
 *   2. Automation behind the MCP server and comparison scripts.  Those scripts
 *      use this tool to create tiny reproducible SID-Wizard songs, capture the
 *      resulting SID register writes, and compare them with sidplaydma output.
 *
 * Most addresses below are not generic C64 addresses.  They are specific to the
 * SID-Wizard 1.8 editor binary that we run on the C64.  When SID-Wizard changes,
 * re-check these addresses against the SID-Wizard source and live memory before
 * changing behavior here.  The code intentionally uses the public sysop_* APIs
 * directly rather than local aliases, so it is obvious which operations are C64
 * peeks/pokes, DMA lock transitions, or bridge lifecycle calls.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "sysop64.h"
#include "sysop_d64.h"


int g_step_mode = 0;
int g_debug_enabled = 1;

/* Debug output is intentionally runtime-gated because this program is also
 * called by scripts that parse stdout. Keep normal command output stable.
 */

void dbg_printf(const char* format, ...) {
    if (!g_debug_enabled) return;
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

/* D64 helpers are used by the legacy `dump` path and by instrument loading
 * from disk images. Parsing itself lives in libsysop64; this file only picks
 * entries and prints tool-friendly diagnostics.
 */
static const char* d64_file_type_name(uint8_t file_type)
{
    switch (file_type) {
        case D64_FILETYPE_DEL: return "DEL";
        case D64_FILETYPE_SEQ: return "SEQ";
        case D64_FILETYPE_PRG: return "PRG";
        case D64_FILETYPE_USR: return "USR";
        case D64_FILETYPE_REL: return "REL";
        default: return "???";
    }
}

static const D64DirectoryEntry* find_d64_entry(const D64DirectoryEntry* entries, int count, const char* filename)
{
    if (!entries || !filename)
        return NULL;

    for (int i = 0; i < count; i++) {
        if ((strcmp(filename, "*") == 0 && entries[i].file_type == D64_FILETYPE_PRG) ||
            strcmp(filename, entries[i].filename) == 0) {
            return &entries[i];
        }
    }

    return NULL;
}

static int dump_d64_file_sectors(const D64Image* img, const D64DirectoryEntry* entry)
{
    int track = entry->start_track;
    int sector = entry->start_sector;
    int block_num = 0;

    printf("\n=== Sector Data Dump ===\n\n");

    while (track != 0) {
        uint8_t* file_buffer = sysop_d64_read_sector(img, track, sector);
        if (!file_buffer) {
            printf("Bad sector in file chain: track %d sector %d\n", track, sector);
            return 1;
        }

        printf("=== Block %d (Track %d, Sector %d) ===\n", block_num, track, sector);

        uint8_t next_track = file_buffer[0];
        uint8_t next_sector = file_buffer[1];
        if (next_track == 0) {
            printf("Last block: %d bytes used (bytes 2-%d contain data)\n", next_sector - 1, next_sector);
        } else {
            printf("Next block: Track %d, Sector %d\n", next_track, next_sector);
        }

        for (int j = 0; j < D64_SECTOR_SIZE; j++) {
            printf("%02X ", file_buffer[j]);
            if ((j + 1) % 16 == 0)
                printf("\n");
        }
        printf("\n");

        block_num++;
        track = next_track;
        sector = next_sector;
    }

    printf("End of file (last block)\n");
    printf("Total blocks dumped: %d\n", block_num);
    return 0;
}
void sigintHandler(int signal)
{
    sysop_uninit();
}

/* Minimal IEC-style state used by older SID-Wizard/disk experiments. The
 * current high-value paths mostly use direct memory patching, but keeping this
 * state documented makes it clear why the tool still knows about logical files,
 * command channel status, and U1 block reads.
 */
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

/* SID-Wizard keeps each instrument in a fixed 128-byte slot. Exported .SWI
 * files can be either that raw slot format, a PRG with a two-byte load address,
 * or SID-Wizard's compact packed export. normalize_sidwizard_instrument_bytes()
 * converts every supported form back to this in-memory slot layout.
 */
const int MAX_INSTRUMENT_SIZE = 0x80;
const int MAX_INST_NAME_LENGTH = 8;
const int MAX_SIDWIZARD_INSTRUMENTS = 62;
const int MAX_INSTRUMENT_FILE_SIZE = 0x82;
const int MAX_PATTERN_SIZE = 256;


/*
 * SID-Wizard address map notes. These came from a mix of SID-Wizard source
 * inspection, monitor hunts in live C64 memory, and repeated capture tests.
 * Keep these notes near the globals because the magic addresses are the riskiest
 * part of the tool: if an editor build moves code/data, commands may appear to
 * work while patching the wrong byte.
 *
 h 0 ffff a9 0 8d * * a9 1 20 * *
Hunt memory 0000-FFFF (65535)...
0B65: A9 00 8D 3F 0B A9 01 20 4A AA

> a b65
a b65
 0B65:
> d b65
d b65
 0B65: A9 00    LDA #$00
 0B67: 8D 3F 0B STA $0B3F
 0B6A: A9 01    LDA #$01
 0B6C: 20 4A AA JSR $AA4A
 0B6F: BC 54 AA LDY $AA54,X
 0B72: 8C 1F E0 STY $E01F
 0B75: AA       TAX
 0B76: BD 54 AA LDA $AA54,X
 0B79: 8D 1E E0 STA $E01E
 0B7C: 20 6F A9 JSR $A96F
 0B7F: 20 07 AB JSR $AB07
 0B82: AD 6D 03 LDA $036D

h 0 ffff a9 * f0 * 20 65 0b
Hunt memory 0000-FFFF (65535)...
0B3E: A9 00 F0 06 20 65 0B

d b38
 0B38: 20 5D AD JSR $AD5D
 0B3B: 20 91 AD JSR $AD91
 0B3E: A9 00    LDA #$00 ; <--------- here b3f
 0B40: F0 06    BEQ $0B48
 0B42: 20 65 0B JSR $0B65

 */
 
 uint16_t g_sidwiz_addr_instrument_refresh_needed = 0xb3f; // insref+1 in the code


/*d b65
 0B65: A9 00    LDA #$00
 0B67: 8D 3F 0B STA $0B3F
 0B6A: A9 01    LDA #$01 ;<-- here
 0B6C: 20 4A AA JSR $AA4A
 */

 uint16_t g_sidwiz_addr_selinst_plus_1 = 0xb6b; // selinst+1 in the code

 uint16_t g_sidwiz_pattern_data  = 0x2d00; // pattern data start

 // SID-Wizard playback control bytes.  These are self-modified/data bytes in
 // the loaded SID-Wizard 1.8 editor, verified from the source and live memory:
 //
 //   editor.asm:
 //     inirequ lda #1       ; operand is inirequ+1
 //
 //   live memory:
 //     082a: A9 00 F0 08 ... A9 00 8D 2B 08
 //
 //   playadapter.inc:
 //     capstore .byte ...
 //     playmod  .byte $0    ; 0=stopped/jam, 1=song, 2=pattern
 //     mul7chn  .byte 0,7,14
 //
 //   live memory:
 //     c660: 08 00 00 07 0e ...
 //             ^^ playmod
 uint16_t g_sidwiz_addr_init_request = 0x082b; // inirequ+1
 uint16_t g_sidwiz_addr_playmode = 0xc661;     // playadapter.playmod

 // Editor-visible row count for each pattern.
 uint16_t g_sidwiz_editor_pattern_lengths_table = 0x1f73; // ptnlength
 // Packed byte count for each pattern, including the terminating $FF.
 uint16_t g_sidwiz_editor_pattern_sizes_table = 0x1fd8; // ptnsize

 uint16_t g_sidwiz_order_list_voice1 = 0x207d; // SEQUENCES, order list start for subtune 0 voice 1
 
 uint16_t g_sidwiz_instrument_table_address = 0x9100;
 int g_sidwiz_voice_count = 3;

 const int MAX_ORDER_LIST_SIZE = 128;

static void trim_ascii(char* s)
{
    char* start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);

    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
}

static int parse_sidwizard_label_value(const char* token, uint16_t* value)
{
    if (token == NULL || value == NULL)
        return 0;

    if (token[0] == '$')
    {
        char* end = NULL;
        unsigned long parsed = strtoul(token + 1, &end, 16);
        if (end == token + 1 || parsed > 0xFFFF)
            return 0;
        *value = (uint16_t)parsed;
        return 1;
    }

    char* end = NULL;
    unsigned long parsed = strtoul(token, &end, 0);
    if (end == token || parsed > 0xFFFF)
        return 0;
    *value = (uint16_t)parsed;
    return 1;
}

static int sidwizard_label_line_value(const char* line, const char* symbol, uint16_t* value)
{
    char label[128];
    char token[128];
    unsigned int vice_addr = 0;

    if (sscanf(line, " al C:%x %127s", &vice_addr, label) == 2 ||
        sscanf(line, "al C:%x %127s", &vice_addr, label) == 2 ||
        sscanf(line, " al %x %127s", &vice_addr, label) == 2 ||
        sscanf(line, "al %x %127s", &vice_addr, label) == 2)
    {
        char* normalized = label;
        while (*normalized == '.' || *normalized == '_')
            normalized++;
        if (strcmp(normalized, symbol) == 0 && vice_addr <= 0xFFFF)
        {
            *value = (uint16_t)vice_addr;
            return 1;
        }
    }

    const char* equals = strchr(line, '=');
    if (equals == NULL)
        return 0;

    size_t label_len = (size_t)(equals - line);
    if (label_len >= sizeof(label))
        label_len = sizeof(label) - 1;
    memcpy(label, line, label_len);
    label[label_len] = '\0';
    trim_ascii(label);

    if (strcmp(label, symbol) != 0)
        return 0;

    snprintf(token, sizeof(token), "%s", equals + 1);
    trim_ascii(token);
    char* end = token;
    while (*end != '\0' && *end != ' ' && *end != '\t' && *end != ';')
        end++;
    *end = '\0';

    return parse_sidwizard_label_value(token, value);
}

static int read_sidwizard_label(const char* path, const char* symbol, uint16_t* value)
{
    FILE* f = fopen(path, "r");
    if (f == NULL)
        return 0;

    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (sidwizard_label_line_value(line, symbol, value))
        {
            found = 1;
            break;
        }
    }

    fclose(f);
    return found;
}

static int sidwizard_sibling_lst_path(const char* label_path, char* out, size_t out_size)
{
    if (label_path == NULL || out == NULL || out_size == 0)
        return 0;

    snprintf(out, out_size, "%s", label_path);

    char* suffix = strstr(out, ".vice.lbl");
    if (suffix != NULL)
    {
        strcpy(suffix, ".lst");
        return 1;
    }

    suffix = strstr(out, ".labels");
    if (suffix != NULL)
    {
        strcpy(suffix, ".lst");
        return 1;
    }

    char* dot = strrchr(out, '.');
    if (dot != NULL)
    {
        snprintf(dot, out_size - (size_t)(dot - out), ".lst");
        return 1;
    }

    return 0;
}

static int read_sidwizard_listing_constant(const char* path, const char* symbol, uint16_t* value)
{
    FILE* f = fopen(path, "r");
    if (f == NULL)
        return 0;

    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char* symbol_pos = strstr(line, symbol);
        if (symbol_pos == NULL || symbol_pos[strlen(symbol)] != '=')
            continue;

        char* equals = strchr(line, '=');
        if (equals == NULL || equals == symbol_pos)
            continue;

        char* end = NULL;
        unsigned long parsed = strtoul(equals + 1, &end, 10);
        if (end == equals + 1 || parsed > 0xFFFF)
            continue;

        *value = (uint16_t)parsed;
        found = 1;
        break;
    }

    fclose(f);
    return found;
}

static int read_sidwizard_listing_code_label(const char* path, const char* symbol, uint16_t* value)
{
    FILE* f = fopen(path, "r");
    if (f == NULL)
        return 0;

    char line[1024];
    int found = 0;
    size_t symbol_len = strlen(symbol);
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char* symbol_pos = line;
        int definition_like = 0;
        while ((symbol_pos = strstr(symbol_pos, symbol)) != NULL)
        {
            char before = (symbol_pos == line) ? ' ' : symbol_pos[-1];
            char after = symbol_pos[symbol_len];

            if ((before >= '0' && before <= '9') ||
                (before >= 'A' && before <= 'Z') ||
                (before >= 'a' && before <= 'z') ||
                before == '_' || before == '.')
            {
                symbol_pos += symbol_len;
                continue;
            }

            if ((after >= '0' && after <= '9') ||
                (after >= 'A' && after <= 'Z') ||
                (after >= 'a' && after <= 'z') ||
                after == '_' || after == '.')
            {
                symbol_pos += symbol_len;
                continue;
            }

            char* tail = symbol_pos + symbol_len;
            while (*tail == ' ' || *tail == '\t')
                tail++;

            // In the 64tass listing, a label definition has source text after
            // the label, e.g. "playmod .byte" or "insrefr lda #1". Plain
            // references such as "beq insrefr" have no useful tail here.
            if (*tail != '\0' && *tail != '\r' && *tail != '\n' && *tail != ';' && *tail != '+')
            {
                definition_like = 1;
                break;
            }

            symbol_pos += symbol_len;
        }

        if (!definition_like)
            continue;

        char* p = line;
        while (*p == ' ' || *p == '\t')
            p++;

        if (*p != '.' && *p != '>')
            continue;

        char* end = NULL;
        unsigned long parsed = strtoul(p + 1, &end, 16);
        if (end == p + 1 || parsed > 0xFFFF)
            continue;

        *value = (uint16_t)parsed;
        found = 1;
        break;
    }

    fclose(f);
    return found;
}

static uint16_t sidwizard_rest_pattern_from_ptn0(uint16_t ptn0)
{
    return (uint16_t)((ptn0 + 2 + 0xff) & 0xff00);
}

static int load_sidwizard_labels(const char* path, int quiet)
{
    uint16_t value = 0;
    int loaded = 0;
    char listing_path[512];
    int have_listing = sidwizard_sibling_lst_path(path, listing_path, sizeof(listing_path));

    if (read_sidwizard_label(path, "insref", &value))
    {
        g_sidwiz_addr_instrument_refresh_needed = value + 1;
        loaded++;
    }
    else if (have_listing && read_sidwizard_listing_code_label(listing_path, "insrefr", &value))
    {
        g_sidwiz_addr_instrument_refresh_needed = value + 1;
        loaded++;
    }
    else if (have_listing && read_sidwizard_listing_code_label(listing_path, "insref", &value))
    {
        g_sidwiz_addr_instrument_refresh_needed = value + 1;
        loaded++;
    }
    if (read_sidwizard_label(path, "selinst", &value))
    {
        g_sidwiz_addr_selinst_plus_1 = value + 1;
        loaded++;
    }
    else if (have_listing && read_sidwizard_listing_code_label(listing_path, "selinst", &value))
    {
        g_sidwiz_addr_selinst_plus_1 = value + 1;
        loaded++;
    }
    if (read_sidwizard_label(path, "RESTPTN", &value) ||
        (have_listing && read_sidwizard_listing_constant(listing_path, "RESTPTN", &value)))
    {
        g_sidwiz_pattern_data = value;
        loaded++;
    }
    else if (read_sidwizard_label(path, "PTN0", &value) ||
        (have_listing && read_sidwizard_listing_constant(listing_path, "PTN0", &value)))
    {
        g_sidwiz_pattern_data = sidwizard_rest_pattern_from_ptn0(value);
        loaded++;
    }
    if (read_sidwizard_label(path, "inirequ", &value))
    {
        g_sidwiz_addr_init_request = value + 1;
        loaded++;
    }
    else if (have_listing && read_sidwizard_listing_code_label(listing_path, "inirequ", &value))
    {
        g_sidwiz_addr_init_request = value + 1;
        loaded++;
    }
    if (read_sidwizard_label(path, "playmod", &value))
    {
        g_sidwiz_addr_playmode = value;
        loaded++;
    }
    else if (have_listing && read_sidwizard_listing_code_label(listing_path, "playmod", &value))
    {
        g_sidwiz_addr_playmode = value;
        loaded++;
    }
    if (read_sidwizard_label(path, "ptnlength", &value))
    {
        g_sidwiz_editor_pattern_lengths_table = value;
        loaded++;
    }
    if (read_sidwizard_label(path, "ptnsize", &value))
    {
        g_sidwiz_editor_pattern_sizes_table = value;
        loaded++;
    }
    if (read_sidwizard_label(path, "SEQUENCES", &value))
    {
        g_sidwiz_order_list_voice1 = value;
        loaded++;
    }
    if (read_sidwizard_label(path, "INSTRUMENTS", &value))
    {
        g_sidwiz_instrument_table_address = value;
        loaded++;
    }
    else if (have_listing && read_sidwizard_listing_constant(listing_path, "INSTRUMENTS", &value))
    {
        g_sidwiz_instrument_table_address = value;
        loaded++;
    }
    if (read_sidwizard_label(path, "SID_AMOUNT", &value) && value >= 1 && value <= 4)
    {
        g_sidwiz_voice_count = value * 3;
        loaded++;
    }
    else if (have_listing && read_sidwizard_listing_constant(listing_path, "SID_AMOUNT", &value) && value >= 1 && value <= 4)
    {
        g_sidwiz_voice_count = value * 3;
        loaded++;
    }
    else if (have_listing && read_sidwizard_listing_constant(listing_path, "CHN_AMOUNT", &value) && value >= 3 && value <= 12)
    {
        g_sidwiz_voice_count = value;
        loaded++;
    }
    else if (read_sidwizard_label(path, "p_seqt10", &value))
    {
        g_sidwiz_voice_count = 12;
        loaded++;
    }
    else if (read_sidwizard_label(path, "p_seqt7", &value))
    {
        g_sidwiz_voice_count = 9;
        loaded++;
    }
    else if (read_sidwizard_label(path, "p_seqt4", &value))
    {
        g_sidwiz_voice_count = 6;
        loaded++;
    }

    if (loaded == 0)
    {
        if (!quiet)
            printf("No SID-Wizard labels recognized in %s\n", path);
        return -1;
    }

    if (g_debug_enabled && !quiet)
    {
        printf("Loaded %d SID-Wizard labels from %s\n", loaded, path);
        printf("SID-Wizard map: voices=%d sequences=$%04X patterns=$%04X instruments=$%04X ptnlength=$%04X ptnsize=$%04X playmod=$%04X inirequ+1=$%04X insref+1=$%04X selinst+1=$%04X\n",
            g_sidwiz_voice_count,
            g_sidwiz_order_list_voice1,
            g_sidwiz_pattern_data,
            g_sidwiz_instrument_table_address,
            g_sidwiz_editor_pattern_lengths_table,
            g_sidwiz_editor_pattern_sizes_table,
            g_sidwiz_addr_playmode,
            g_sidwiz_addr_init_request,
            g_sidwiz_addr_instrument_refresh_needed,
            g_sidwiz_addr_selinst_plus_1);
    }

    return 0;
}

static void load_sidwizard_labels_from_environment(void)
{
    const char* path = getenv("SIDWIZ_LABELS");
    if (path == NULL || path[0] == '\0')
        path = getenv("SIDWIZARD_LABELS");
    if (path == NULL || path[0] == '\0')
        return;

    // Environment-provided labels are a convenience for swapping between
    // SID-Wizard builds. If the file is missing or stale, keep the built-in
    // 1-SID defaults so existing one-off commands continue to behave as before.
    load_sidwizard_labels(path, 1);
}
/* Ask SID-Wizard's editor UI to redraw the instrument panel after direct
 * memory edits. This is cosmetic for playback, but very useful when using the
 * MCP screenshot tools to verify that a loaded instrument decoded correctly.
 */
void redraw_instruments()
{
    sysop_poke(g_sidwiz_addr_instrument_refresh_needed, 0x01);
}

void sidwizard_play_song()
{
    // Mirrors F1 enough for automated test setup: request the editor main loop
    // to call playadapter.inisubb, then let IRQ playback run in song mode.
    sysop_poke(g_sidwiz_addr_playmode, 0x01);
    sysop_poke(g_sidwiz_addr_init_request, 0x01);
}

void sidwizard_stop_playback()
{
    // This is the core stop state used by SID-Wizard's F4 handler.  The full
    // keyboard path also stores paused instruments, but for automated
    // verification we want a deterministic stop before a fresh --play-song.
    sysop_poke(g_sidwiz_addr_playmode, 0x00);
}

/* Read the two bytes we patch for playback control. This gives scripts a cheap
 * sanity check without needing to infer state from the screen.
 */
void sidwizard_print_playback_status()
{
    uint8_t playmode = sysop_peek(g_sidwiz_addr_playmode);
    uint8_t init_request = sysop_peek(g_sidwiz_addr_init_request);
    printf("playmode=%u init_request=%u\n", playmode, init_request);
}


int set_current_instrument(uint8_t instrument_index)
{
    if (instrument_index < 1 || instrument_index > MAX_SIDWIZARD_INSTRUMENTS)
        return -1;

    sysop_poke(g_sidwiz_addr_selinst_plus_1, instrument_index);
    redraw_instruments();
    return 0;
}

int set_instrument_name(uint8_t instrument_index, const char* name)
{
    if (instrument_index < 1 || instrument_index > MAX_SIDWIZARD_INSTRUMENTS)
        return -1;

    uint16_t instrument_address = g_sidwiz_instrument_table_address +
        ((instrument_index - 1) * MAX_INSTRUMENT_SIZE);
    uint16_t name_address = instrument_address + MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH;

    for (int i = 0; i < MAX_INST_NAME_LENGTH; i++)
    {
        uint8_t ch = ' ';
        if (name != NULL && name[i] != '\0')
            ch = (uint8_t)name[i];
        sysop_poke(name_address + i, ch);
    }

    redraw_instruments();
    return 0;
}

int set_instrument_adsr(uint8_t instrument_index, uint8_t attack, uint8_t decay, uint8_t sustain, uint8_t release)
{
    if (instrument_index < 1 || instrument_index > MAX_SIDWIZARD_INSTRUMENTS)
        return -1;
    if (attack > 0x0F || decay > 0x0F || sustain > 0x0F || release > 0x0F)
        return -2;

    uint16_t instrument_address = g_sidwiz_instrument_table_address +
        ((instrument_index - 1) * MAX_INSTRUMENT_SIZE);

    sysop_poke(instrument_address + 0x03, (uint8_t)((attack << 4) | decay));
    sysop_poke(instrument_address + 0x04, (uint8_t)((sustain << 4) | release));

    redraw_instruments();
    return 0;
}

void debug_dma_enabled()
{
    uint32_t dmainfo = sysop_get_dma_info();
    if ((dmainfo & 0x80000000) != 0) { // high bit should be a zero
        printf("assert_dma_enabled failed (%08X), hit enter\n", dmainfo);
        getchar();
    }
}
/* Load the older packed/file-oriented instrument format. New automation should
 * prefer load_instrument_bin() after normalize_sidwizard_instrument_bytes(), but
 * this remains for compatibility with earlier command-line flows.
 */
void load_instrument(int instrument_index, uint8_t* data, int size)
{
    debug_dma_enabled();

    dbg_printf("Loading instrument %d of size %d bytes\n", instrument_index, size);
    
    // Calculate instrument address using the instrument table base
    uint16_t instrument_address = g_sidwiz_instrument_table_address + ((instrument_index - 1) * MAX_INSTRUMENT_SIZE);
    dbg_printf("Loading instrument data to address: %04X\n", instrument_address);

    // Extract instrument name from the last MAX_INST_NAME_LENGTH bytes
    int name_offset = size - MAX_INST_NAME_LENGTH;
    uint8_t* instrument_name = &data[name_offset];
    
    // Get the instrument data length from the byte just before the name
    uint8_t instrument_data_length = data[name_offset - 1];
    dbg_printf("Instrument data length: %d bytes\n", instrument_data_length);
    
    // Copy the instrument data to the instrument slot
    for (int i = 0; i < instrument_data_length; i++) {
        sysop_poke(instrument_address + i, data[i+2]);
        //printf("Wrote byte %02X to offset %d at %04X\n", data[i+2], i, instrument_address + i);
    }
    
    // Write terminator byte after the instrument data
    sysop_poke(instrument_address + instrument_data_length, 0xFF);
    dbg_printf("Wrote terminator at offset %d\n", instrument_data_length);
    
    // Backward compatibility: ensure first waveform byte at offset 0x0F is not zero
    uint8_t waveform_byte = sysop_peek(instrument_address + 0x0F);
    if (waveform_byte == 0) {
        sysop_poke(instrument_address + 0x0F, 0x09);
        dbg_printf("Fixed zero waveform byte at offset 0x0F to 0x09 for compatibility\n");
    }
    
    // Copy the instrument name to the end of the instrument slot
    int name_position = instrument_address + MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH;
    for (int i = 0; i < MAX_INST_NAME_LENGTH; i++) {
        sysop_poke(name_position + i, instrument_name[i]);
    }
    
    dbg_printf("Instrument loaded successfully\n");
    dbg_printf("Name at offset %d: ", MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH);
    for (int i = 0; i < MAX_INST_NAME_LENGTH; i++) {
        dbg_printf("%c", instrument_name[i]);
    }
    dbg_printf("\n");

    redraw_instruments();
}

/* Copy a normalized 128-byte SID-Wizard instrument slot into the editor. The
 * MCP server sends this form so the conversion rules live at one boundary.
 */
void load_instrument_bin(int instrument_index, uint8_t* data, int size)
{
    dbg_printf("Loading raw binary instrument %d of size %d bytes\n", instrument_index, size);
    
    // Calculate instrument address using the instrument table base
    uint16_t instrument_address = g_sidwiz_instrument_table_address + ((instrument_index - 1) * MAX_INSTRUMENT_SIZE);
    dbg_printf("Loading instrument data to address: %04X\n", instrument_address);

    // Copy the instrument data to the instrument slot (raw binary starts at offset 0)
    for (int i = 0; i < size; i++) {
        sysop_poke(instrument_address + i, data[i]);
    }
    
    dbg_printf("Instrument loaded successfully\n");
    dbg_printf("Name at offset %d: ", MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH);
    for (int i = 0; i < MAX_INST_NAME_LENGTH; i++) {
        uint8_t name_char = sysop_peek(instrument_address + MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH + i);
        dbg_printf("%c", name_char);
    }
    dbg_printf("\n");
    redraw_instruments();
}

int expand_sidwizard_packed_instrument(const uint8_t* data, int size, uint8_t* expanded)
{
    if (size < 10 || size > 127)
        return 0;

    int size_offset = size - MAX_INST_NAME_LENGTH - 1;
    uint8_t effective_size = data[size_offset];

    /*
     * SID-Wizard's packed .SWI export replaces the final $FF table
     * delimiter with the effective payload size, then appends the 8-byte
     * instrument name. Expand that packed export back into the 128-byte slot
     * format SID-Wizard keeps in memory.
     */
    if (effective_size != size_offset)
        return 0;
    if (effective_size < 0x0f || effective_size >= MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH)
        return 0;

    memset(expanded, 0, MAX_INSTRUMENT_SIZE);
    memcpy(expanded, data, effective_size);
    expanded[effective_size] = 0xff;
    memcpy(expanded + MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH,
        data + effective_size + 1,
        MAX_INST_NAME_LENGTH);

    if (expanded[0x0f] == 0)
        expanded[0x0f] = 0x09;

    return 1;
}

/* Accept every instrument representation we commonly encounter and return the
 * exact 128-byte layout that SID-Wizard stores at $9100 + slot * $80. This keeps
 * callers simple and prevents the MCP server from needing SID-Wizard-specific
 * packing knowledge.
 */
int normalize_sidwizard_instrument_bytes(const uint8_t* data, int size, uint8_t* normalized, const char** format)
{
    if (size == MAX_INSTRUMENT_SIZE)
    {
        memcpy(normalized, data, MAX_INSTRUMENT_SIZE);
        *format = "raw128";
        return 1;
    }

    if (size == MAX_INSTRUMENT_FILE_SIZE)
    {
        memcpy(normalized, data + 2, MAX_INSTRUMENT_SIZE);
        *format = "raw128_prg";
        return 1;
    }

    if (expand_sidwizard_packed_instrument(data, size, normalized))
    {
        *format = "packed_swi";
        return 1;
    }

    if (size > 2 && expand_sidwizard_packed_instrument(data + 2, size - 2, normalized))
    {
        *format = "packed_swi_prg";
        return 1;
    }

    return 0;
}

static void instrument_name_from_slot(const uint8_t data[MAX_INSTRUMENT_SIZE], char *out, size_t out_size);

/* Extract packed instruments from a SID-Wizard SWM1 workfile. SWM stores the
 * tune as a packed stream: header, sequences, patterns, instruments, chords,
 * tempo programs, and subtune tempos. The header gives the sizes of the data
 * after the instrument block, so we can find the end of the packed instruments
 * without depacking patterns. Instruments are packed in the same compact format
 * used by .SWI exports, so each record can be written back out directly.
 */
#define SWM_TUNE_HEADER_SIZE 64
#define SWM_SEQ_AMOUNT_POS 12
#define SWM_INST_AMOUNT_POS 14
#define SWM_CHORD_LENGTH_POS 15
#define SWM_TEMPO_LENGTH_POS 16
#define SWM_CHANNELS 3

static int read_binary_file(const char *path, uint8_t **out_data, size_t *out_size)
{
    FILE *fp = fopen(path, "rb");
    long len;
    uint8_t *data;

    if (!fp)
        return -1;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    data = (uint8_t *)malloc((size_t)len);
    if (!data) {
        fclose(fp);
        return -1;
    }
    if (len > 0 && fread(data, 1, (size_t)len, fp) != (size_t)len) {
        free(data);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *out_data = data;
    *out_size = (size_t)len;
    return 0;
}

static int write_binary_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;
    if (size > 0 && fwrite(data, 1, size, fp) != size) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static void sanitize_filename_part(const char *in, char *out, size_t out_size)
{
    size_t n = 0;
    if (out_size == 0)
        return;
    for (int i = 0; in[i] && n + 1 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_') {
            out[n++] = (char)c;
        } else if (c == ' ' || c == '.') {
            out[n++] = '_';
        }
    }
    if (n == 0 && out_size > 1)
        out[n++] = 'I';
    out[n] = '\0';
}

static int swm_payload_offset(const uint8_t *data, size_t size)
{
    if (size >= SWM_TUNE_HEADER_SIZE && memcmp(data, "SWM1", 4) == 0)
        return 0;
    if (size >= SWM_TUNE_HEADER_SIZE + 2 && memcmp(data + 2, "SWM1", 4) == 0)
        return 2;
    return -1;
}

static int extract_swm_instruments(const char *swm_path, const char *out_dir)
{
    uint8_t *file_data = NULL;
    size_t file_size = 0;
    int payload_offset;
    const uint8_t *swm;
    size_t swm_size;
    int seq_count;
    int instrument_count;
    int chord_len;
    int tempo_len;
    int subtune_count;
    size_t tail_size;
    size_t record_end;
    int written = 0;

    if (read_binary_file(swm_path, &file_data, &file_size) != 0) {
        printf("Failed to read SWM file '%s'\n", swm_path);
        return 1;
    }

    payload_offset = swm_payload_offset(file_data, file_size);
    if (payload_offset < 0) {
        printf("'%s' is not an SWM1 file\n", swm_path);
        free(file_data);
        return 1;
    }

    swm = file_data + payload_offset;
    swm_size = file_size - (size_t)payload_offset;
    seq_count = swm[SWM_SEQ_AMOUNT_POS];
    instrument_count = swm[SWM_INST_AMOUNT_POS];
    chord_len = swm[SWM_CHORD_LENGTH_POS];
    tempo_len = swm[SWM_TEMPO_LENGTH_POS];
    subtune_count = seq_count == 0 ? 1 : ((seq_count - 1) / SWM_CHANNELS) + 1;
    tail_size = (size_t)chord_len + (size_t)tempo_len + (size_t)subtune_count * 2;

    if (instrument_count < 0 || instrument_count > MAX_SIDWIZARD_INSTRUMENTS ||
        swm_size < SWM_TUNE_HEADER_SIZE || tail_size > swm_size - SWM_TUNE_HEADER_SIZE) {
        printf("SWM header looks invalid: seq=%d instruments=%d chord=%d tempo=%d size=%zu\n",
            seq_count, instrument_count, chord_len, tempo_len, swm_size);
        free(file_data);
        return 1;
    }

    if (mkdir(out_dir, 0775) != 0 && errno != EEXIST) {
        printf("Failed to create output folder '%s'\n", out_dir);
        free(file_data);
        return 1;
    }

    record_end = swm_size - tail_size;
    for (int slot = instrument_count; slot >= 1; slot--) {
        uint8_t effective_size;
        size_t record_size;
        size_t record_start;
        uint8_t normalized[MAX_INSTRUMENT_SIZE];
        char name[MAX_INST_NAME_LENGTH + 1];
        char safe_name[64];
        char out_path[1024];

        if (record_end < (size_t)MAX_INST_NAME_LENGTH + 1) {
            printf("Packed instrument stream ended early at slot %d\n", slot);
            free(file_data);
            return 1;
        }

        effective_size = swm[record_end - MAX_INST_NAME_LENGTH - 1];
        record_size = (size_t)effective_size + 1 + MAX_INST_NAME_LENGTH;
        if (effective_size < 0x0f || effective_size >= MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH ||
            record_size > record_end) {
            printf("Bad packed instrument record for slot %d: effective_size=%u record_end=%zu\n",
                slot, effective_size, record_end);
            free(file_data);
            return 1;
        }

        record_start = record_end - record_size;
        if (!expand_sidwizard_packed_instrument(swm + record_start, (int)record_size, normalized)) {
            printf("Failed to depack instrument slot %d\n", slot);
            free(file_data);
            return 1;
        }

        instrument_name_from_slot(normalized, name, sizeof(name));
        sanitize_filename_part(name, safe_name, sizeof(safe_name));
        snprintf(out_path, sizeof(out_path), "%s/%02d-%s.swi", out_dir, slot, safe_name);

        if (write_binary_file(out_path, swm + record_start, record_size) != 0) {
            printf("Failed to write '%s'\n", out_path);
            free(file_data);
            return 1;
        }

        printf("wrote %s (%zu bytes)\n", out_path, record_size);
        written++;
        record_end = record_start;
    }

    printf("Extracted %d instrument%s from %s\n", written, written == 1 ? "" : "s", swm_path);
    free(file_data);
    return 0;
}



static char *sidwiz_html = NULL;
static char sidwiz_html_path[512];
static char sidwiz_instrument_folder[512];

static int parse_u8_text(const char *text, uint8_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || errno != 0 || value > 255)
        return -1;
    *out = (uint8_t)value;
    return 0;
}

static int parse_slot_text(const char *text, uint8_t *slot)
{
    uint8_t value;
    if (parse_u8_text(text, &value) != 0 || value < 1 || value > MAX_SIDWIZARD_INSTRUMENTS)
        return -1;
    *slot = value;
    return 0;
}

static void read_instrument_slot(uint8_t slot, uint8_t data[MAX_INSTRUMENT_SIZE])
{
    uint16_t address = g_sidwiz_instrument_table_address + ((slot - 1) * MAX_INSTRUMENT_SIZE);
    for (int i = 0; i < MAX_INSTRUMENT_SIZE; i++)
        data[i] = sysop_peek(address + i);
}

static int write_instrument_byte(uint8_t slot, uint8_t offset, uint8_t value)
{
    if (slot < 1 || slot > MAX_SIDWIZARD_INSTRUMENTS || offset >= MAX_INSTRUMENT_SIZE)
        return -1;
    uint16_t address = g_sidwiz_instrument_table_address + ((slot - 1) * MAX_INSTRUMENT_SIZE);
    sysop_poke(address + offset, value);
    redraw_instruments();
    return 0;
}

static char printable_instrument_char(uint8_t c)
{
    if (c >= 0x20 && c <= 0x7e)
        return (char)c;
    if (c == 0xa0)
        return ' ';
    return '.';
}

static void instrument_name_from_slot(const uint8_t data[MAX_INSTRUMENT_SIZE], char *out, size_t out_size)
{
    size_t n = 0;
    if (out_size == 0)
        return;
    for (int i = 0; i < MAX_INST_NAME_LENGTH && n + 1 < out_size; i++)
        out[n++] = printable_instrument_char(data[MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH + i]);
    out[n] = '\0';
    trim_ascii(out);
}

static int json_escape_append(char *out, size_t out_size, size_t *used, const char *text)
{
    while (*text && *used + 2 < out_size) {
        unsigned char c = (unsigned char)*text++;
        if (c == '"' || c == '\\') {
            out[(*used)++] = '\\';
            out[(*used)++] = (char)c;
        } else if (c >= 0x20 && c <= 0x7e) {
            out[(*used)++] = (char)c;
        }
    }
    out[*used] = '\0';
    return 0;
}

static char *read_text_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    char *data = NULL;
    long len;
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    data = malloc((size_t)len + 1);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    if (fread(data, 1, (size_t)len, fp) != (size_t)len) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    data[len] = '\0';
    if (out_len)
        *out_len = (size_t)len;
    return data;
}

static int try_load_sidwiz_html(const char *path)
{
    size_t len = 0;
    char *loaded = read_text_file(path, &len);
    if (!loaded)
        return -1;
    free(sidwiz_html);
    sidwiz_html = loaded;
    snprintf(sidwiz_html_path, sizeof(sidwiz_html_path), "%s", path);
    printf("sidwiz: loaded web app %s (%zu bytes)\n", sidwiz_html_path, len);
    return 0;
}

static int load_sidwiz_html(const char *argv0, const char *requested_path)
{
    const char *fallbacks[] = {
        "sidwiz.html",
        "../tools/sidwiz.html",
        "../../tools/sidwiz.html",
        "/usr/local/bin/sidwiz.html",
        "/usr/local/share/sysop64/sidwiz.html"
    };
    char beside_exe[512];
    const char *slash;

    if (requested_path && requested_path[0]) {
        if (try_load_sidwiz_html(requested_path) == 0)
            return 0;
        fprintf(stderr, "sidwiz: could not load %s\n", requested_path);
        return -1;
    }

    slash = strrchr(argv0, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - argv0);
        if (dir_len + sizeof("/sidwiz.html") < sizeof(beside_exe)) {
            memcpy(beside_exe, argv0, dir_len);
            strcpy(beside_exe + dir_len, "/sidwiz.html");
            if (try_load_sidwiz_html(beside_exe) == 0)
                return 0;
        }
    }

    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        if (try_load_sidwiz_html(fallbacks[i]) == 0)
            return 0;
    }

    fprintf(stderr, "sidwiz: could not find sidwiz.html; pass an explicit path\n");
    return -1;
}

static int send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t sent = send(fd, data, len, 0);
        if (sent <= 0)
            return -1;
        data += sent;
        len -= (size_t)sent;
    }
    return 0;
}

static void http_send(int fd, const char *status, const char *type, const char *body)
{
    char header[256];
    size_t body_len = strlen(body);
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
        status, type, body_len);
    if (header_len > 0) {
        send_all(fd, header, (size_t)header_len);
        send_all(fd, body, body_len);
    }
}

static const char *query_value(const char *path, const char *key, char *out, size_t out_len)
{
    const char *q = strchr(path, '?');
    size_t key_len = strlen(key);
    if (!q)
        return NULL;
    q++;
    while (*q) {
        const char *next = strchr(q, '&');
        size_t part_len = next ? (size_t)(next - q) : strlen(q);
        if (part_len > key_len + 1 && strncmp(q, key, key_len) == 0 && q[key_len] == '=') {
            size_t value_len = part_len - key_len - 1;
            if (value_len >= out_len)
                value_len = out_len - 1;
            memcpy(out, q + key_len + 1, value_len);
            out[value_len] = '\0';
            char *dst = out;
            for (char *src = out; *src; src++) {
                if (*src == '+') {
                    *dst++ = ' ';
                } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
                    char hex[3] = { src[1], src[2], '\0' };
                    *dst++ = (char)strtoul(hex, NULL, 16);
                    src += 2;
                } else {
                    *dst++ = *src;
                }
            }
            *dst = '\0';
            return out;
        }
        if (!next)
            break;
        q = next + 1;
    }
    return NULL;
}

static size_t http_content_length(const char *req, const char *headers_end)
{
    const char *line = req;
    while (line && line < headers_end) {
        const char *line_end = strstr(line, "\r\n");
        if (!line_end || line_end > headers_end)
            line_end = headers_end;
        if ((size_t)(line_end - line) >= 15 && strncasecmp(line, "Content-Length:", 15) == 0)
            return (size_t)strtoul(line + 15, NULL, 10);
        if (line_end == headers_end)
            break;
        line = line_end + 2;
    }
    return 0;
}

static void strip_instrument_extensions(char *name)
{
    size_t len = strlen(name);
    if (len >= 4 && strcasecmp(name + len - 4, ".prg") == 0) {
        name[len - 4] = '\0';
        len -= 4;
    }
    if (len >= 4 && strcasecmp(name + len - 4, ".swi") == 0)
        name[len - 4] = '\0';
}

static int is_instrument_file_name(const char *name)
{
    size_t len = strlen(name);
    if (len >= 4 && strcasecmp(name + len - 4, ".swi") == 0)
        return 1;
    if (len >= 8 && strcasecmp(name + len - 8, ".swi.prg") == 0)
        return 1;
    if (len >= 4 && strcasecmp(name + len - 4, ".prg") == 0)
        return 1;
    return 0;
}

static void handle_http_list(int fd)
{
    char body[65536];
    size_t used = 0;
    int first;

    used += snprintf(body + used, sizeof(body) - used, "{\"maxSlots\":%d,\"voiceCount\":%d,\"slots\":[", MAX_SIDWIZARD_INSTRUMENTS, g_sidwiz_voice_count);
    sysop_server_dma_lock();
    for (int slot = 1; slot <= MAX_SIDWIZARD_INSTRUMENTS; slot++) {
        uint8_t data[MAX_INSTRUMENT_SIZE];
        char name[16];
        read_instrument_slot((uint8_t)slot, data);
        instrument_name_from_slot(data, name, sizeof(name));
        used += snprintf(body + used, sizeof(body) - used, "%s{\"slot\":%d,\"name\":\"", slot == 1 ? "" : ",", slot);
        json_escape_append(body, sizeof(body), &used, name);
        used += snprintf(body + used, sizeof(body) - used, "\"}");
    }
    sysop_server_dma_unlock();

    used += snprintf(body + used, sizeof(body) - used, "],\"files\":[");
    first = 1;
    if (sidwiz_instrument_folder[0]) {
        DIR *dir = opendir(sidwiz_instrument_folder);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL && used + 512 < sizeof(body)) {
                if (entry->d_name[0] == '.' || !is_instrument_file_name(entry->d_name))
                    continue;
                char display[256];
                snprintf(display, sizeof(display), "%s", entry->d_name);
                strip_instrument_extensions(display);
                used += snprintf(body + used, sizeof(body) - used, "%s{\"file\":\"", first ? "" : ",");
                json_escape_append(body, sizeof(body), &used, entry->d_name);
                used += snprintf(body + used, sizeof(body) - used, "\",\"display\":\"");
                json_escape_append(body, sizeof(body), &used, display);
                used += snprintf(body + used, sizeof(body) - used, "\"}");
                first = 0;
            }
            closedir(dir);
        }
    }
    used += snprintf(body + used, sizeof(body) - used, "],\"folder\":\"");
    json_escape_append(body, sizeof(body), &used, sidwiz_instrument_folder);
    used += snprintf(body + used, sizeof(body) - used, "\"}\n");
    http_send(fd, "200 OK", "application/json", body);
}

static void http_send_instrument_json(int fd, uint8_t slot)
{
    uint8_t data[MAX_INSTRUMENT_SIZE];
    char name[16];
    char body[8192];
    size_t used = 0;

    sysop_server_dma_lock();
    read_instrument_slot(slot, data);
    sysop_server_dma_unlock();
    instrument_name_from_slot(data, name, sizeof(name));

    used += snprintf(body + used, sizeof(body) - used, "{\"slot\":%u,\"name\":\"", slot);
    json_escape_append(body, sizeof(body), &used, name);
    used += snprintf(body + used, sizeof(body) - used, "\",\"bytes\":[");
    for (int i = 0; i < MAX_INSTRUMENT_SIZE; i++)
        used += snprintf(body + used, sizeof(body) - used, "%s%u", i == 0 ? "" : ",", data[i]);
    used += snprintf(body + used, sizeof(body) - used, "]}\n");
    http_send(fd, "200 OK", "application/json", body);
}

static int load_instrument_file_from_folder(uint8_t slot, const char *file_name)
{
    char path[768];
    FILE *file;
    uint8_t data[MAX_INSTRUMENT_FILE_SIZE];
    uint8_t normalized[MAX_INSTRUMENT_SIZE];
    const char *format = NULL;
    size_t bytes_read;

    if (!sidwiz_instrument_folder[0] || strstr(file_name, "..") != NULL || strchr(file_name, '/') != NULL)
        return -1;
    snprintf(path, sizeof(path), "%s/%s", sidwiz_instrument_folder, file_name);
    file = fopen(path, "rb");
    if (!file)
        return -1;
    bytes_read = fread(data, 1, sizeof(data), file);
    fclose(file);
    if (bytes_read == 0 || !normalize_sidwizard_instrument_bytes(data, (int)bytes_read, normalized, &format))
        return -1;

    sysop_server_dma_lock();
    set_current_instrument(slot);
    load_instrument_bin(slot, normalized, MAX_INSTRUMENT_SIZE);
    sysop_server_dma_unlock();
    return 0;
}

static int parse_instrument_body(char *body, uint8_t data[MAX_INSTRUMENT_SIZE])
{
    int count = 0;
    char *token = strtok(body, " \t\r\n,");
    while (token != NULL && count < MAX_INSTRUMENT_SIZE) {
        unsigned int value;
        if (sscanf(token, "%x", &value) != 1 || value > 0xff)
            return -1;
        data[count++] = (uint8_t)value;
        token = strtok(NULL, " \t\r\n,");
    }
    return count == MAX_INSTRUMENT_SIZE ? 0 : -1;
}

static void handle_sidwiz_http_client(int fd)
{
    enum { REQ_MAX = 32768 };
    char req[REQ_MAX + 1];
    char method[8], path[512];
    ssize_t n;
    size_t used = 0;
    char *headers_end;
    char *body;

    while (used < REQ_MAX) {
        n = recv(fd, req + used, REQ_MAX - used, 0);
        if (n <= 0)
            break;
        used += (size_t)n;
        req[used] = '\0';
        headers_end = strstr(req, "\r\n\r\n");
        if (headers_end) {
            size_t header_bytes = (size_t)(headers_end + 4 - req);
            size_t content_length = http_content_length(req, headers_end);
            if (used >= header_bytes + content_length)
                break;
        }
    }

    headers_end = strstr(req, "\r\n\r\n");
    body = headers_end ? headers_end + 4 : req + used;
    if (sscanf(req, "%7s %511s", method, path) != 2) {
        http_send(fd, "400 Bad Request", "text/plain", "bad request\n");
        return;
    }

    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        http_send(fd, "200 OK", "text/html", sidwiz_html);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/list") == 0) {
        handle_http_list(fd);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/instrument?", 16) == 0) {
        char slot_text[16];
        uint8_t slot;
        if (!query_value(path, "slot", slot_text, sizeof(slot_text)) || parse_slot_text(slot_text, &slot) != 0) {
            http_send(fd, "400 Bad Request", "text/plain", "bad slot\n");
            return;
        }
        http_send_instrument_json(fd, slot);
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/api/select?", 12) == 0) {
        char slot_text[16];
        uint8_t slot;
        if (!query_value(path, "slot", slot_text, sizeof(slot_text)) || parse_slot_text(slot_text, &slot) != 0) {
            http_send(fd, "400 Bad Request", "text/plain", "bad slot\n");
            return;
        }
        sysop_server_dma_lock();
        set_current_instrument(slot);
        sysop_server_dma_unlock();
        http_send(fd, "200 OK", "text/plain", "ok\n");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/api/load-file?", 15) == 0) {
        char slot_text[16], file_name[256];
        uint8_t slot;
        if (!query_value(path, "slot", slot_text, sizeof(slot_text)) || parse_slot_text(slot_text, &slot) != 0 ||
            !query_value(path, "file", file_name, sizeof(file_name)) || load_instrument_file_from_folder(slot, file_name) != 0) {
            http_send(fd, "400 Bad Request", "text/plain", "bad load\n");
            return;
        }
        http_send(fd, "200 OK", "text/plain", "ok\n");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/api/byte?", 10) == 0) {
        char slot_text[16], offset_text[16], value_text[16];
        uint8_t slot, offset, value;
        if (!query_value(path, "slot", slot_text, sizeof(slot_text)) || parse_slot_text(slot_text, &slot) != 0 ||
            !query_value(path, "offset", offset_text, sizeof(offset_text)) || parse_u8_text(offset_text, &offset) != 0 || offset >= MAX_INSTRUMENT_SIZE ||
            !query_value(path, "value", value_text, sizeof(value_text)) || parse_u8_text(value_text, &value) != 0 ||
            write_instrument_byte(slot, offset, value) != 0) {
            http_send(fd, "400 Bad Request", "text/plain", "bad byte\n");
            return;
        }
        http_send(fd, "200 OK", "text/plain", "ok\n");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/api/instrument?", 16) == 0) {
        char slot_text[16];
        uint8_t slot;
        uint8_t data[MAX_INSTRUMENT_SIZE];
        if (!query_value(path, "slot", slot_text, sizeof(slot_text)) || parse_slot_text(slot_text, &slot) != 0 ||
            parse_instrument_body(body, data) != 0) {
            http_send(fd, "400 Bad Request", "text/plain", "bad instrument\n");
            return;
        }
        sysop_server_dma_lock();
        set_current_instrument(slot);
        load_instrument_bin(slot, data, MAX_INSTRUMENT_SIZE);
        sysop_server_dma_unlock();
        http_send(fd, "200 OK", "text/plain", "ok\n");
    } else {
        http_send(fd, "404 Not Found", "text/plain", "not found\n");
    }
}

static int run_sidwiz_http_server(uint16_t port)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    int one = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }
    if (listen(server_fd, 8) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    printf("sidwiz: listening on http://0.0.0.0:%u/", (unsigned)port);
    if (sidwiz_instrument_folder[0])
        printf(" instruments=%s", sidwiz_instrument_folder);
    printf("\n");
    fflush(stdout);

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            close(server_fd);
            return -1;
        }
        handle_sidwiz_http_client(client_fd);
        close(client_fd);
    }
}
/* Pattern bytes are packed: high bits indicate that additional bytes follow for
 * instrument/effect data. These helpers keep SID-Wizard's editor metadata in
 * sync after we overwrite pattern memory directly.
 */
int sidwizard_pattern_row_count(const uint8_t* data, int size)
{
    int rows = 0;
    for (int i = 0; i < size; i++)
    {
        uint8_t byte = data[i];
        if (byte == 0xff)
            break;

        rows++;

        if (byte & 0x80)
        {
            i++;
            if (i >= size || data[i] == 0xff)
                break;

            if (data[i] & 0x80)
            {
                i++;
                if (i >= size || data[i] == 0xff)
                    break;

                /* Column 3 is the effect column. Small effects are complete in
                 * this byte, but BigFX values $01..$1F always consume one
                 * additional parameter byte. The old code looked for bit 7 here,
                 * which made BigFX parameters look like extra pattern rows and
                 * confused SID-Wizard's editor metadata after --set-pattern-data.
                 */
                if ((data[i] & 0xe0) == 0)
                    i++;
            }
        }
    }

    return rows;
}

int sidwizard_pattern_packed_size(const uint8_t* data, int size)
{
    for (int i = 0; i < size; i++)
    {
        if (data[i] == 0xff)
            return i + 1;
    }

    return size + 1;
}

/* Replace one packed pattern body. We clear the full 256-byte slot first so
 * stale bytes from a longer previous pattern cannot leak into editor views or
 * later reads.
 */
void set_pattern_bytes(int pattern, uint8_t* data, int size)
{
    if (size > 256)
        size = 256;
    
    
    uint16_t pattern_address = ((pattern - 1) * MAX_PATTERN_SIZE) + g_sidwiz_pattern_data;
    dbg_printf("Setting pattern %d at address %04X\n", pattern, pattern_address);
    for (int i = 0; i < MAX_PATTERN_SIZE; i++)
        sysop_poke(pattern_address + i, 0x00);

    int saw_terminator = 0;
    for (int i = 0; i < size && i < MAX_PATTERN_SIZE; i++)
    {
        sysop_poke(pattern_address + i, data[i]);
        if (data[i] == 0xff)
            saw_terminator = 1;
    }
    if (!saw_terminator && size < MAX_PATTERN_SIZE)
        sysop_poke(pattern_address + size, 0xff);

    dbg_printf("Setting pattern bytes of size %d bytes\n", size);
    dbg_printf("Pattern bytes set successfully\n");
}

void set_pattern_metadata(int pattern, uint8_t row_count, uint8_t packed_size)
{
    uint16_t length_address = g_sidwiz_editor_pattern_lengths_table + (pattern - 1);
    sysop_poke(length_address, row_count);
    dbg_printf("Set pattern %d row count to %d at address %04X\n", pattern, row_count, length_address);

    uint16_t size_address = g_sidwiz_editor_pattern_sizes_table + (pattern - 1);
    sysop_poke(size_address, packed_size);
    dbg_printf("Set pattern %d packed size to %d at address %04X\n", pattern, packed_size, size_address);

    redraw_instruments();
}

void set_pattern_length(int pattern, uint8_t length)
{
    set_pattern_metadata(pattern, length, length + 1);
    uint16_t pattern_address = ((pattern - 1) * MAX_PATTERN_SIZE) + g_sidwiz_pattern_data;
    sysop_poke(pattern_address + length, 0xff);
}

void get_pattern_data(int pattern)
{
    uint16_t pattern_address = ((pattern - 1) * MAX_PATTERN_SIZE) + g_sidwiz_pattern_data;
    dbg_printf("Getting pattern %d data from address %04X\n", pattern, pattern_address);
    
    for (int i = 0; i < MAX_PATTERN_SIZE; i++)
    {
        uint8_t byte = sysop_peek(pattern_address + i);
        printf("%02X", byte);
        if (i < MAX_PATTERN_SIZE - 1)
        {
            printf(",");
        }
    }
    printf("\n");
}

void get_order_list(int voice)
{
    if (voice < 1 || voice > g_sidwiz_voice_count)
    {
        printf("Voice must be between 1 and %d\n", g_sidwiz_voice_count);
        return;
    }
    
    uint16_t order_list_address = g_sidwiz_order_list_voice1 + ((voice - 1) * MAX_ORDER_LIST_SIZE);
    dbg_printf("Getting order list for voice %d from address %04X\n", voice, order_list_address);
    
    int count = 0;
    for (int i = 0; i < MAX_ORDER_LIST_SIZE; i++)
    {
        uint8_t byte = sysop_peek(order_list_address + i);
        printf("%02X", byte);
        count++;
        
        if (byte == 0xFE)
        {
            // End of song marker.
            break;
        }

        if (byte == 0xFF)
        {
            // Loop marker. SID-Wizard stores the loop target in the next byte.
            if (i + 1 < MAX_ORDER_LIST_SIZE)
            {
                uint8_t loop_pos = sysop_peek(order_list_address + i + 1);
                printf(",%02X", loop_pos);
                count++;
            }
            break;
        }
        
        if (i < MAX_ORDER_LIST_SIZE - 1)
        {
            printf(",");
        }
    }
    printf("\n");
    dbg_printf("Order list has %d entries\n", count);
}

/* Write a SID-Wizard order list for one voice. $FE ends the song; $FF followed
 * by a byte loops to that order position. The comparison scripts rely on this
 * to make tiny one-pattern songs without clearing the whole editor state.
 */
void set_order_list(int voice, uint8_t* data, int size)
{
    if (voice < 1 || voice > g_sidwiz_voice_count)
    {
        printf("Voice must be between 1 and %d\n", g_sidwiz_voice_count);
        return;
    }
    
    if (size > MAX_ORDER_LIST_SIZE)
    {
        size = MAX_ORDER_LIST_SIZE;
    }
    
    uint16_t order_list_address = g_sidwiz_order_list_voice1 + ((voice - 1) * MAX_ORDER_LIST_SIZE);
    dbg_printf("Setting order list for voice %d at address %04X with %d bytes\n", voice, order_list_address, size);
    
    // Write the provided data
    for (int i = 0; i < size; i++)
    {
        sysop_poke(order_list_address + i, data[i]);
    }
    
    // SID-Wizard order lists terminate either with $FE (end song) or
    // with $FF followed by a loop-position byte. Preserve an explicit
    // loop marker instead of silently appending $FE after it.
    int has_terminator = 0;
    if (size > 0 && data[size - 1] == 0xFE)
    {
        has_terminator = 1;
    }
    else if (size >= 2 && data[size - 2] == 0xFF)
    {
        has_terminator = 1;
    }
    
    if (!has_terminator && size < MAX_ORDER_LIST_SIZE)
    {
        sysop_poke(order_list_address + size, 0xFE);
        dbg_printf("Added end-song terminator at offset %d\n", size);
        size++; // Include the terminator in the count
    }
    
    // Zero out remaining bytes
    for (int i = size; i < MAX_ORDER_LIST_SIZE; i++)
    {
        sysop_poke(order_list_address + i, 0x00);
    }
    
    dbg_printf("Order list set successfully, cleared %d bytes\n", MAX_ORDER_LIST_SIZE - size);
}


void assert_dma_enabled()
{
    uint32_t dmainfo = sysop_get_dma_info();
    if ((dmainfo & 0x80000000) != 0) {
        printf("assert_dma_enabled failed\n");
        exit(-1);
    }
}

void assert_dma_disabled()
{
    uint32_t dmainfo = sysop_get_dma_info();
    if ((dmainfo & 0x80000000) == 0) {
        printf("assert_dma_disabled failed\n");
        exit(-1);
    }
}

/* Dump an instrument slot in forms useful to humans and automation. The JSON
 * and info modes are intentionally descriptive rather than a complete
 * SID-Wizard emulator; sidplaydma owns the exact playback interpretation.
 */
void get_instrument(int instrument_index, const char* format)
{
    //assert_dma_disabled();

    if (instrument_index < 1 || instrument_index > MAX_SIDWIZARD_INSTRUMENTS)
    {
        printf("Instrument index must be between 1 and %d\n", MAX_SIDWIZARD_INSTRUMENTS);
        return;
    }
    
    // Calculate instrument address (instruments are stored at $3000 + (index-1) * $80)
    uint16_t instrument_base = g_sidwiz_instrument_table_address;
    uint16_t instrument_address = instrument_base + ((instrument_index - 1) * MAX_INSTRUMENT_SIZE);
    
    dbg_printf("Reading instrument %d from address %04X\n", instrument_index, instrument_address);
    
    // Read all 128 bytes
    uint8_t data[MAX_INSTRUMENT_SIZE];
    for (int i = 0; i < MAX_INSTRUMENT_SIZE; i++)
    {
        data[i] = sysop_peek(instrument_address + i);
        //data[i] = internal_peek(instrument_address + i);
    }
    
    if (strcmp(format, "hex") == 0)
    {
        // Output as comma-delimited hex bytes in quotes
        printf("\"");
        for (int i = 0; i < MAX_INSTRUMENT_SIZE; i++)
        {
            printf("%02X", data[i]);
            if (i < MAX_INSTRUMENT_SIZE - 1)
            {
                printf(",");
            }
        }
        printf("\"\n");
    }
    else if (strcmp(format, "info") == 0)
    {
        // Output detailed information table
        printf("=== Instrument %d Information ===\n\n", instrument_index);
        
        // Name (last 8 bytes)
        printf("Name: ");
        for (int i = 0; i < MAX_INST_NAME_LENGTH; i++)
        {
            uint8_t c = data[MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH + i];
            if (c >= 0x20 && c <= 0x7E)
                printf("%c", c);
            else
                printf(" ");
        }
        printf("\n\n");
        
        // Control bytes
        printf("Control Register ($00): $%02X\n", data[0x00]);
        printf("  Vibrato/Restart/Tied settings\n\n");
        
        printf("Hard Restart ADSR/Timer ($01-$02): $%02X%02X\n", data[0x01], data[0x02]);
        printf("Attack/Decay ($03): $%02X (A=%d, D=%d)\n", data[0x03], (data[0x03] >> 4) & 0x0F, data[0x03] & 0x0F);
        printf("Sustain/Release ($04): $%02X (S=%d, R=%d)\n", data[0x04], (data[0x04] >> 4) & 0x0F, data[0x04] & 0x0F);
        printf("Vibrato Freq+Amp ($05): $%02X\n", data[0x05]);
        printf("Vibrato Delay/Speed ($06): $%02X\n", data[0x06]);
        printf("Arpeggio/Chord Speed ($07): $%02X\n", data[0x07]);
        printf("Default Chord ($08): $%02X\n", data[0x08]);
        printf("Octave Shift ($09): $%02X (%d semitones)\n", data[0x09], (int8_t)data[0x09]);
        printf("PW-table Pointer ($0A): $%02X (offset %d)\n", data[0x0A], data[0x0A]);
        printf("Filter-table Pointer ($0B): $%02X (offset %d)\n", data[0x0B], data[0x0B]);
        printf("Gate-off WF ($0C): $%02X\n", data[0x0C]);
        printf("Gate-off PW ($0D): $%02X\n", data[0x0D]);
        printf("Gate-off Filter ($0E): $%02X\n", data[0x0E]);
        printf("1st Frame Waveform ($0F): $%02X ", data[0x0F]);
        
        // Decode waveform
        uint8_t wf = data[0x0F];
        if (wf & 0x01) printf("[GATE] ");
        if (wf & 0x02) printf("[SYNC] ");
        if (wf & 0x04) printf("[RING] ");
        if (wf & 0x08) printf("[TEST] ");
        if (wf & 0x10) printf("[TRI] ");
        if (wf & 0x20) printf("[SAW] ");
        if (wf & 0x40) printf("[PULSE] ");
        if (wf & 0x80) printf("[NOISE] ");
        printf("\n\n");
        
        // Waveform program (starts at $10)
        printf("--- Waveform Program (starts at $10) ---\n");
        printf("Offset  WF   Arp  Det  Description\n");
        printf("------  ---  ---  ---  -----------\n");
        int offset = 0x10;
        while (offset < MAX_INSTRUMENT_SIZE && data[offset] != 0xFF)
        {
            uint8_t wf_byte = data[offset];
            uint8_t arp_byte = (offset + 1 < MAX_INSTRUMENT_SIZE) ? data[offset + 1] : 0;
            uint8_t det_byte = (offset + 2 < MAX_INSTRUMENT_SIZE) ? data[offset + 2] : 0;
            
            printf("  $%02X   $%02X  $%02X  $%02X   ", offset, wf_byte, arp_byte, det_byte);
            
            // Decode waveform
            if (wf_byte & 0x10) printf("TRI ");
            if (wf_byte & 0x20) printf("SAW ");
            if (wf_byte & 0x40) printf("PUL ");
            if (wf_byte & 0x80) printf("NOI ");
            if (wf_byte & 0x01) printf("GATE ");
            if (wf_byte & 0x02) printf("SYNC ");
            if (wf_byte & 0x04) printf("RING ");
            if (wf_byte & 0x08) printf("TEST ");
            
            // Decode arpeggio
            if (arp_byte == 0x00) printf("| No shift");
            else if (arp_byte == 0x7F) printf("| Jump chord");
            else if (arp_byte == 0x80) printf("| No process");
            else if (arp_byte >= 0x01 && arp_byte <= 0x5F) printf("| +%d semi", arp_byte);
            else if (arp_byte >= 0x81 && arp_byte <= 0xDF) printf("| Abs pitch");
            else if (arp_byte >= 0xE0) printf("| -%d semi", 256 - arp_byte);
            
            printf("\n");
            offset += 3;
        }
        printf("  $%02X   $FF  (end)\n\n", offset);
        
        // Pulse width program
        if (data[0x0A] > 0 && data[0x0A] < MAX_INSTRUMENT_SIZE)
        {
            printf("--- Pulse Width Program (starts at $%02X) ---\n", data[0x0A]);
            printf("Offset  PW1  PW2  KT   Description\n");
            printf("------  ---  ---  ---  -----------\n");
            offset = data[0x0A];
            while (offset < MAX_INSTRUMENT_SIZE && data[offset] != 0xFF)
            {
                uint8_t pw1 = data[offset];
                uint8_t pw2 = (offset + 1 < MAX_INSTRUMENT_SIZE) ? data[offset + 1] : 0;
                uint8_t kt = (offset + 2 < MAX_INSTRUMENT_SIZE) ? data[offset + 2] : 0;
                
                printf("  $%02X   $%02X  $%02X  $%02X   ", offset, pw1, pw2, kt);
                
                if (pw1 >= 0x80)
                {
                    printf("Set PW=$%01X%02X", pw1 & 0x0F, pw2);
                }
                else if (pw1 == 0xFE)
                {
                    printf("Jump to $%02X", pw2);
                }
                else
                {
                    printf("Animate %d iters, delta=$%02X", pw1 & 0x7F, pw2);
                }
                printf(" | KT=%d", kt);
                
                printf("\n");
                offset += 3;
            }
            printf("  $%02X   $FF  (end)\n\n", offset);
        }
        
        // Filter program
        if (data[0x0B] > 0 && data[0x0B] < MAX_INSTRUMENT_SIZE)
        {
            printf("--- Filter Program (starts at $%02X) ---\n", data[0x0B]);
            printf("Offset  FC1  FC2  KT   Description\n");
            printf("------  ---  ---  ---  -----------\n");
            offset = data[0x0B];
            while (offset < MAX_INSTRUMENT_SIZE && data[offset] != 0xFF)
            {
                uint8_t fc1 = data[offset];
                uint8_t fc2 = (offset + 1 < MAX_INSTRUMENT_SIZE) ? data[offset + 1] : 0;
                uint8_t kt = (offset + 2 < MAX_INSTRUMENT_SIZE) ? data[offset + 2] : 0;
                
                printf("  $%02X   $%02X  $%02X  $%02X   ", offset, fc1, fc2, kt);
                
                if (fc1 >= 0x80)
                {
                    uint8_t mode = (fc1 >> 4) & 0x0F;
                    uint8_t res = fc1 & 0x0F;
                    printf("Mode=");
                    if (mode == 0x8) printf("OFF");
                    else if (mode == 0x9) printf("LP");
                    else if (mode == 0xB) printf("LP+BP");
                    else if (mode == 0xC) printf("HP");
                    else if (mode == 0xD) printf("LP+HP");
                    else if (mode == 0xE) printf("BP+HP");
                    else if (mode == 0xF) printf("ALL");
                    else printf("$%X", mode);
                    printf(" Res=%d Cutoff=$%02X", res, fc2);
                }
                else if (fc1 == 0xFE)
                {
                    printf("Jump to $%02X", fc2);
                }
                else
                {
                    printf("Sweep %d iters, delta=$%02X", fc1 & 0x7F, fc2);
                }
                printf(" | KT=%d", kt);
                
                printf("\n");
                offset += 3;
            }
            printf("  $%02X   $FF  (end)\n\n", offset);
        }
    }
    else if (strcmp(format, "json") == 0)
    {
        // Output as JSON
        printf("{\n");
        printf("  \"index\": %d,\n", instrument_index);
        
        // Name (last 8 bytes)
        printf("  \"name\": \"");
        for (int i = 0; i < MAX_INST_NAME_LENGTH; i++)
        {
            uint8_t c = data[MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH + i];
            if (c >= 0x20 && c <= 0x7E)
                printf("%c", c);
            else
                printf(" ");
        }
        printf("\",\n");
        
        // Control bytes
        printf("  \"control_register\": %d,\n", data[0x00]);
        printf("  \"hard_restart_timer\": %d,\n", (data[0x01] << 8) | data[0x02]);
        printf("  \"attack\": %d,\n", (data[0x03] >> 4) & 0x0F);
        printf("  \"decay\": %d,\n", data[0x03] & 0x0F);
        printf("  \"sustain\": %d,\n", (data[0x04] >> 4) & 0x0F);
        printf("  \"release\": %d,\n", data[0x04] & 0x0F);
        printf("  \"vibrato_freq_amp\": %d,\n", data[0x05]);
        printf("  \"vibrato_delay\": %d,\n", data[0x06]);
        printf("  \"arpeggio_speed\": %d,\n", data[0x07]);
        printf("  \"default_chord\": %d,\n", data[0x08]);
        printf("  \"octave_shift\": %d,\n", (int8_t)data[0x09]);
        printf("  \"pw_table_offset\": %d,\n", data[0x0A]);
        printf("  \"filter_table_offset\": %d,\n", data[0x0B]);
        printf("  \"gate_off_wf\": %d,\n", data[0x0C]);
        printf("  \"gate_off_pw\": %d,\n", data[0x0D]);
        printf("  \"gate_off_filter\": %d,\n", data[0x0E]);
        printf("  \"first_waveform\": %d,\n", data[0x0F]);
        
        // Waveform program
        printf("  \"waveform_program\": [\n");
        int offset = 0x10;
        int first_wf = 1;
        while (offset < MAX_INSTRUMENT_SIZE && data[offset] != 0xFF)
        {
            if (!first_wf) printf(",\n");
            first_wf = 0;
            
            uint8_t wf_byte = data[offset];
            uint8_t arp_byte = (offset + 1 < MAX_INSTRUMENT_SIZE) ? data[offset + 1] : 0;
            uint8_t det_byte = (offset + 2 < MAX_INSTRUMENT_SIZE) ? data[offset + 2] : 0;
            
            printf("    {\"offset\": %d, \"waveform\": %d, \"arpeggio\": %d, \"detune\": %d}", 
                   offset, wf_byte, arp_byte, det_byte);
            offset += 3;
        }
        printf("\n  ],\n");
        
        // Pulse width program
        printf("  \"pulse_width_program\": [\n");
        if (data[0x0A] > 0 && data[0x0A] < MAX_INSTRUMENT_SIZE)
        {
            offset = data[0x0A];
            int first_pw = 1;
            while (offset < MAX_INSTRUMENT_SIZE && data[offset] != 0xFF)
            {
                if (!first_pw) printf(",\n");
                first_pw = 0;
                
                uint8_t pw1 = data[offset];
                uint8_t pw2 = (offset + 1 < MAX_INSTRUMENT_SIZE) ? data[offset + 1] : 0;
                uint8_t kt = (offset + 2 < MAX_INSTRUMENT_SIZE) ? data[offset + 2] : 0;
                
                printf("    {\"offset\": %d, \"command\": %d, \"value\": %d, \"kt\": %d}", 
                       offset, pw1, pw2, kt);
                offset += 3;
            }
            printf("\n");
        }
        printf("  ],\n");
        
        // Filter program
        printf("  \"filter_program\": [\n");
        if (data[0x0B] > 0 && data[0x0B] < MAX_INSTRUMENT_SIZE)
        {
            offset = data[0x0B];
            int first_fc = 1;
            while (offset < MAX_INSTRUMENT_SIZE && data[offset] != 0xFF)
            {
                if (!first_fc) printf(",\n");
                first_fc = 0;
                
                uint8_t fc1 = data[offset];
                uint8_t fc2 = (offset + 1 < MAX_INSTRUMENT_SIZE) ? data[offset + 1] : 0;
                uint8_t kt = (offset + 2 < MAX_INSTRUMENT_SIZE) ? data[offset + 2] : 0;
                
                printf("    {\"offset\": %d, \"command\": %d, \"value\": %d, \"kt\": %d}", 
                       offset, fc1, fc2, kt);
                offset += 3;
            }
            printf("\n");
        }
        printf("  ]\n");
        printf("}\n");
    }
    else
    {
        printf("Unknown format '%s'. Use 'hex', 'info', or 'json'\n", format);
    }
}

void voice_shift_octave(int voice, int octave_shift)
{
    if (voice < 1 || voice > g_sidwiz_voice_count)
    {
        printf("Voice must be between 1 and %d\n", g_sidwiz_voice_count);
        return;
    }
    
    dbg_printf("Shifting voice %d by %d octaves\n", voice, octave_shift);
    
    // Read the order list for this voice
    uint16_t order_list_address = g_sidwiz_order_list_voice1 + ((voice - 1) * MAX_ORDER_LIST_SIZE);
    uint8_t order_list[MAX_ORDER_LIST_SIZE];
    int order_count = 0;
    
    for (int i = 0; i < MAX_ORDER_LIST_SIZE; i++)
    {
        order_list[i] = sysop_peek(order_list_address + i);
        if (order_list[i] == 0xFE)
        {
            break;
        }
        order_count++;
    }
    
    dbg_printf("Found %d patterns in order list\n", order_count);
    
    // Process each pattern in the order list
    for (int order_idx = 0; order_idx < order_count; order_idx++)
    {
        int pattern = order_list[order_idx];
        if (pattern == 0xFE || pattern == 0xFF)
        {
            continue;
        }
        
        dbg_printf("Processing pattern %d\n", pattern);
        
        uint16_t pattern_address = ((pattern - 1) * MAX_PATTERN_SIZE) + g_sidwiz_pattern_data;
        
        // Read pattern data
        for (int i = 0; i < MAX_PATTERN_SIZE; i++)
        {
            uint8_t byte = sysop_peek(pattern_address + i);
            
            // Check if this is a note byte (not 0x00, not 0xFF, and represents a valid note)
            // In SID Wizard, notes typically range from 0x01 to around 0x60 or so
            // 0xFF is the pattern end marker, 0x00 is rest/empty
            if (byte == 0xFF)
            {
                // End of pattern
                break;
            }
            
            if (byte > 0x00 && byte < 0xFE)
            {
                // This appears to be a note value
                // Mask off bit 7 (0x80) which may be used for other purposes
                uint8_t top_bit = byte & 0x80;
                uint8_t note_value = byte & 0x7F;
                
                // Each octave is typically 12 semitones
                int new_note = (int)note_value + (octave_shift * 12);
                
                // Only shift if the new note is within valid range (1 to 96, which is 8 octaves)
                // If it would go out of range, don't shift this note at all
                if (new_note >= 1 && new_note <= 96)
                {
                    // Restore the top bit
                    uint8_t final_value = ((uint8_t)new_note) | top_bit;
                    
                    sysop_poke(pattern_address + i, final_value);
                    dbg_printf("Shifted note at offset %d from %02X to %02X (note: %d -> %d, top bit: %s)\n", 
                        i, byte, final_value, note_value, new_note, top_bit ? "set" : "clear");
                }
                else
                {
                    dbg_printf("Skipping note at offset %d (value %d, shift would be %d, out of range)\n", 
                        i, note_value, new_note);
                }
                
                // If the top bit was set, the next byte is an instrument number - skip it
                if (top_bit)
                {
                    i++; // Skip the next byte (instrument number)
                    dbg_printf("Skipping instrument byte at offset %d\n", i);
                }
            }
        }
    }
    
    dbg_printf("Voice octave shift completed\n");
}

/* Heavyweight reset of SID-Wizard song data. Most automated tests now avoid
 * this and overwrite only the patterns/order lists they need, because clearing
 * everything can perturb a live editor session more than necessary.
 */
void clear_song()
{
    dbg_printf("Clearing all patterns\n");
    
    // Clear all 100 patterns
    for (int pattern = 1; pattern <= 100; pattern++)
    {
        uint16_t pattern_address = ((pattern - 1) * MAX_PATTERN_SIZE) + g_sidwiz_pattern_data;
        
        // Zero out all pattern data
        for (int i = 0; i < MAX_PATTERN_SIZE; i++)
        {
            sysop_poke(pattern_address + i, 0x00);
        }
        
        // Set pattern length to 0x20 (32 rows)
        set_pattern_length(pattern, 0x20);
        
        dbg_printf("Cleared pattern %d\n", pattern);
    }

    uint8_t order_end[] = { 0xFE };
    for (int voice = 1; voice <= g_sidwiz_voice_count; voice++)
    {
        set_order_list(voice, order_end, 1);
    }
    
    dbg_printf("Song cleared successfully\n");
}

/* Command dispatcher. Each mutating command connects to sysop_server, locks DMA,
 * patches SID-Wizard memory, then unlocks DMA before returning. Keep new commands
 * following that pattern unless they explicitly need the C64 CPU running.
 */
int main(int argc, char** argv) 
{
    if (argc < 2) {
        printf("Expected arguments: <cmd> <options>\n");
        printf("cmd is one of:\n");
        printf("--load-inst - load instrument: <inst #> <source> [options]\n");
        printf("           <inst #> <path to d64> <filename> - load from d64\n");
        printf("           <inst #> <path to binary file> - load raw binary\n");
        printf("           <inst #> hex <hex bytes...> - load from hex (128 bytes)\n");
        printf("--load-inst-json - load instrument from JSON: <inst #> <json string>\n");
        printf("--set-inst - set current instrument followed by <index> (1-62 decimal)\n");
        printf("--set-inst-name - set instrument slot name only: <index 1-62> <name up to 8 chars>\n");
        printf("--labels - load SID-Wizard label file before running command: --labels <file> <cmd> ...\n");
        printf("           env fallback: SIDWIZ_LABELS or SIDWIZARD_LABELS, otherwise built-in 1-SID map\n");
        printf("--set-inst-adsr - set instrument ADSR: <index 1-62> <ADSR hex, e.g. 40F8> or <A> <D> <S> <R>\n");
        printf("--get-inst - get instrument data: <index> <format>\n");
        printf("           format: 'hex' for space-delimited hex, 'info' for detailed table, 'json' for JSON\n");
        printf("--set-pattern-data - set pattern data: <pattern index 1-100> \"<hex bytes>\"\n");
        printf("--set-pattern-length - set pattern length: <pattern index 1-100> <length>\n");
        printf("--get-pattern-data - get pattern data: <pattern index 1-100>\n");
        printf("--get-order-list - get order list: <voice> (1-3 by default, 1-6 with 2SID labels)\n");
        printf("--set-order-list - set order list: <voice> \"<hex bytes ending FE, or FF <loop_pos>>\"\n");
        printf("--voice-shift-octave - shift voice octave: <voice> <+/-octaves>\n");
        printf("--clear-song - clear all patterns (set to zeros, length 0x20)\n");
        printf("--play-song - start SID-Wizard song playback from the beginning\n");
        printf("--stop - stop SID-Wizard playback\n");
        printf("--play-status - print SID-Wizard playback state\n");
        printf("--http - run browser UI: [port] [instrument-folder] [html-path]\n");
        printf("--extract-swm-inst - extract packed instruments from SWM1: <swm-file> <output-folder>\n");
        printf("dump - dump <path to d64 file> <filename> - Display all sectors of a file in hex\n");
        return -1;
    }

    char* command = NULL;
    char* output = NULL;
    char* d64filename = NULL;

    if (argc > 1)
    {
        command = argv[1];
    }

    load_sidwizard_labels_from_environment();

    if (strcmp(command, "--labels") == 0)
    {
        if (argc < 4)
        {
            printf("--labels requires <label file> <cmd> [options]\n");
            return 1;
        }
        if (load_sidwizard_labels(argv[2], 0) != 0)
            return 1;
        argv += 2;
        argc -= 2;
        command = argv[1];
    }

    if (strcmp(command, "--extract-swm-inst") == 0)
    {
        if (argc < 4)
        {
            printf("--extract-swm-inst requires <swm-file> <output-folder>\n");
            return 1;
        }
        return extract_swm_instruments(argv[2], argv[3]);
    }

    if (strcmp(command, "--http") == 0)
    {
        uint16_t port = 8081;
        const char *folder = NULL;
        const char *html_path = NULL;
        if (argc >= 3)
            port = (uint16_t)strtoul(argv[2], NULL, 0);
        if (argc >= 4)
            folder = argv[3];
        if (argc >= 5)
            html_path = argv[4];
        if (argc > 5)
        {
            printf("--http usage: --http [port] [instrument-folder] [html-path]\n");
            return 1;
        }
        if (folder)
            snprintf(sidwiz_instrument_folder, sizeof(sidwiz_instrument_folder), "%s", folder);
        if (load_sidwiz_html(argv[0], html_path) != 0)
            return 1;
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            sysop_uninit();
            return 1;
        }
        int rc = run_sidwiz_http_server(port) == 0 ? 0 : 1;
        sysop_server_disconnect();
        sysop_uninit();
        return rc;
    }
    if (strcmp(command, "--play-song")==0 || strcmp(command, "--stop")==0 || strcmp(command, "--play-status")==0)
    {
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            sysop_uninit();
            return 1;
        }

        sysop_server_dma_lock();

        if (strcmp(command, "--play-song")==0)
        {
            sidwizard_play_song();
            printf("SID-Wizard song playback requested\n");
        }
        else if (strcmp(command, "--stop")==0)
        {
            sidwizard_stop_playback();
            printf("SID-Wizard playback stopped\n");
        }
        else
        {
            sidwizard_print_playback_status();
        }

        sysop_server_dma_unlock();
        sysop_server_disconnect();
        sysop_uninit();
        return 0;
    }
    if (strcmp(command, "--set-inst")==0)
    {
        int instrument_index = atoi(argv[2]);
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            return 1;
        }
        sysop_server_dma_lock();
        if (set_current_instrument((uint8_t)instrument_index) != 0)
        {
            printf("Invalid instrument index %d\n", instrument_index);
            sysop_server_dma_unlock();
            return 1;
        }
        printf("Set current instrument to index %d\n", instrument_index);
        sysop_server_dma_unlock();
        return 0;
    }

    if (strcmp(command, "--set-inst-name")==0)
    {
        if (argc < 4)
        {
            printf("--set-inst-name requires <instrument index> <name>\n");
            return 1;
        }

        int instrument_index = atoi(argv[2]);
        const char* name = argv[3];
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            return 1;
        }
        sysop_server_dma_lock();
        if (set_instrument_name((uint8_t)instrument_index, name) != 0)
        {
            printf("Invalid instrument index %d\n", instrument_index);
            sysop_server_dma_unlock();
            sysop_server_disconnect();
            sysop_uninit();
            return 1;
        }
        printf("Set instrument %d name to %.8s\n", instrument_index, name);
        sysop_server_dma_unlock();
        sysop_server_disconnect();
        sysop_uninit();
        return 0;
    }

    if (strcmp(command, "--set-inst-adsr")==0)
    {
        if (argc != 4 && argc != 7)
        {
            printf("--set-inst-adsr requires <instrument index> <ADSR hex, e.g. 40F8> or <A> <D> <S> <R>\n");
            return 1;
        }

        int instrument_index = atoi(argv[2]);
        uint8_t attack = 0;
        uint8_t decay = 0;
        uint8_t sustain = 0;
        uint8_t release = 0;

        if (argc == 4)
        {
            char* end = NULL;
            unsigned long adsr = strtoul(argv[3], &end, 16);
            if (end == argv[3] || *end != '\0' || adsr > 0xFFFF)
            {
                printf("Invalid ADSR hex value '%s'\n", argv[3]);
                return 1;
            }
            attack = (uint8_t)((adsr >> 12) & 0x0F);
            decay = (uint8_t)((adsr >> 8) & 0x0F);
            sustain = (uint8_t)((adsr >> 4) & 0x0F);
            release = (uint8_t)(adsr & 0x0F);
        }
        else
        {
            for (int i = 3; i < 7; i++)
            {
                char* end = NULL;
                unsigned long value = strtoul(argv[i], &end, 16);
                if (end == argv[i] || *end != '\0' || value > 0x0F)
                {
                    printf("Invalid ADSR nibble '%s'\n", argv[i]);
                    return 1;
                }

                if (i == 3) attack = (uint8_t)value;
                else if (i == 4) decay = (uint8_t)value;
                else if (i == 5) sustain = (uint8_t)value;
                else release = (uint8_t)value;
            }
        }

        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            sysop_uninit();
            return 1;
        }
        sysop_server_dma_lock();
        int set_result = set_instrument_adsr((uint8_t)instrument_index, attack, decay, sustain, release);
        if (set_result != 0)
        {
            printf("Invalid instrument index or ADSR value\n");
            sysop_server_dma_unlock();
            sysop_server_disconnect();
            sysop_uninit();
            return 1;
        }
        printf("Set instrument %d ADSR to A=%X D=%X S=%X R=%X ($%02X $%02X)\n",
            instrument_index, attack, decay, sustain, release,
            (attack << 4) | decay, (sustain << 4) | release);
        sysop_server_dma_unlock();
        sysop_server_disconnect();
        sysop_uninit();
        return 0;
    }

    if (strcmp(command, "--get-inst")==0)
    {
        if (argc < 4)
        {
            printf("--get-inst requires <instrument index> <format>\n");
            printf("Format: 'hex' for space-delimited hex bytes, 'info' for detailed table, 'json' for JSON\n");
            return 1;
        }
        
        int instrument_index = atoi(argv[2]);
        char* format = argv[3];
        
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            return 1;
        }
        sysop_server_dma_lock();
        get_instrument(instrument_index, format);
        sysop_server_dma_unlock();
        return 0;
    }

    if (strcmp(command, "--set-pattern-data")==0)
    {
        if (argc < 4)
        {
            printf("--set-pattern-data requires <pattern index> <hex bytes>\n");
            return 1;
        }
        
        int pattern_index = atoi(argv[2]);
        if (pattern_index < 1 || pattern_index > 100)
        {
            printf("Pattern index must be between 1 and 100\n");
            return 1;
        }
        
        char* hex_string = argv[3];
        
        // Parse hex bytes from the string
        uint8_t data[MAX_PATTERN_SIZE];
        int byte_count = 0;
        char* token = strtok(hex_string, " ,");
        
        while (token != NULL && byte_count < MAX_PATTERN_SIZE)
        {
            unsigned int byte_val;
            if (sscanf(token, "%x", &byte_val) == 1 && byte_val <= 0xFF)
            {
                data[byte_count++] = (uint8_t)byte_val;
            }
            else
            {
                printf("Invalid hex byte: '%s'\n", token);
                return 1;
            }
            token = strtok(NULL, " ,");
        }
        
        if (byte_count == 0)
        {
            printf("No hex bytes provided\n");
            return 1;
        }
        
        printf("Setting pattern %d with %d bytes\n", pattern_index, byte_count);
        
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            return 1;
        }
        sysop_server_dma_lock();
        int packed_size = sidwizard_pattern_packed_size(data, byte_count);
        int row_count = sidwizard_pattern_row_count(data, byte_count);
        if (packed_size > MAX_PATTERN_SIZE)
            packed_size = MAX_PATTERN_SIZE;
        if (row_count > 0xff)
            row_count = 0xff;

        set_pattern_bytes(pattern_index, data, byte_count);
        set_pattern_metadata(pattern_index, (uint8_t)row_count, (uint8_t)packed_size);
        sysop_server_dma_unlock();
        printf("Pattern data set successfully (rows=%d packed_size=%d)\n", row_count, packed_size);
        return 0;
    }

    if (strcmp(command, "--set-pattern-length")==0)
    {
        if (argc < 4)
        {
            printf("--set-pattern-length requires <pattern index> <length>\n");
            return 1;
        }
        
        int pattern_index = atoi(argv[2]);
        if (pattern_index < 1 || pattern_index > 100)
        {
            printf("Pattern index must be between 1 and 100\n");
            return 1;
        }
        
        int length = atoi(argv[3]);
        if (length < 1 || length > 0xf8)
        {
            printf("Pattern length must be between 1 and %d (0xF8)\n", 0xf8);
            return 1;
        }
        
        printf("Setting pattern %d length to %d\n", pattern_index, length);
        
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            return 1;
        }
        sysop_server_dma_lock();
        set_pattern_length(pattern_index, (uint8_t)length);
        sysop_server_dma_unlock();
        printf("Pattern length set successfully\n");
        return 0;
    }

    if (strcmp(command, "--get-pattern-data")==0)
    {
        if (argc < 3)
        {
            printf("--get-pattern-data requires <pattern index>\n");
            return 1;
        }
        
        int pattern_index = atoi(argv[2]);
        if (pattern_index < 1 || pattern_index > 100)
        {
            printf("Pattern index must be between 1 and 100\n");
            return 1;
        }
        
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            return 1;
        }
        sysop_server_dma_lock();
        get_pattern_data(pattern_index);
        sysop_server_dma_unlock();
        return 0;
    }

    if (strcmp(command, "--get-order-list")==0)
    {
        if (argc < 3)
        {
            printf("--get-order-list requires <voice>\n");
            return 1;
        }
        
        int voice = atoi(argv[2]);
        if (voice < 1 || voice > g_sidwiz_voice_count)
        {
            printf("Voice must be between 1 and %d\n", g_sidwiz_voice_count);
            return 1;
        }
        
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            return 1;
        }
        sysop_server_dma_lock();
        get_order_list(voice);
        sysop_server_dma_unlock();
        return 0;
    }

    if (strcmp(command, "--set-order-list")==0)
    {
        if (argc < 4)
        {
            printf("--set-order-list requires <voice> <hex bytes>\n");
            return 1;
        }
        
        int voice = atoi(argv[2]);
        if (voice < 1 || voice > g_sidwiz_voice_count)
        {
            printf("Voice must be between 1 and %d\n", g_sidwiz_voice_count);
            return 1;
        }
        
        char* hex_string = argv[3];
        
        // Parse hex bytes from the string
        uint8_t data[MAX_ORDER_LIST_SIZE];
        int byte_count = 0;
        char* token = strtok(hex_string, " ,");
        
        while (token != NULL && byte_count < MAX_ORDER_LIST_SIZE)
        {
            unsigned int byte_val;
            if (sscanf(token, "%x", &byte_val) == 1 && byte_val <= 0xFF)
            {
                data[byte_count++] = (uint8_t)byte_val;
            }
            else
            {
                printf("Invalid hex byte: '%s'\n", token);
                return 1;
            }
            token = strtok(NULL, " ,");
        }
        
        if (byte_count == 0)
        {
            printf("No hex bytes provided\n");
            return 1;
        }
        
        printf("Setting order list for voice %d with %d bytes\n", voice, byte_count);
        
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            return 1;
        }
        sysop_server_dma_lock();
        set_order_list(voice, data, byte_count);
        sysop_server_dma_unlock();
        printf("Order list set successfully\n");
        return 0;
    }

    if (strcmp(command, "--voice-shift-octave")==0)
    {
        if (argc < 4)
        {
            printf("--voice-shift-octave requires <voice> <octave shift>\n");
            return 1;
        }
        
        int voice = atoi(argv[2]);
        if (voice < 1 || voice > g_sidwiz_voice_count)
        {
            printf("Voice must be between 1 and %d\n", g_sidwiz_voice_count);
            return 1;
        }
        
        // Parse octave shift (can be +N or -N)
        int octave_shift = atoi(argv[3]);
        
        printf("Shifting voice %d by %d octaves\n", voice, octave_shift);
        
        g_debug_enabled = 1;
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            return 1;
        }
        sysop_server_dma_lock();
        voice_shift_octave(voice, octave_shift);
        sysop_server_dma_unlock();
        printf("Voice octave shift completed\n");
        return 0;
    }

    if (strcmp(command, "--clear-song")==0)
    {
        printf("Clearing all patterns...\n");
        
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            return 1;
        }
        sysop_server_dma_lock();
        clear_song();
        sysop_server_dma_unlock();
        printf("All patterns cleared\n");
        return 0;
    }
 
    if (strcmp(command, "dump")==0)
    {
        char* d64filename = argv[2];
        if (d64filename == NULL)
        {
            printf("dump command requires a d64 filename\n");
            return 1;
        }

        char* filename = argv[3];
        if (filename == NULL)
        {
            printf("dump command requires a filename\n");
            return 1;
        }

        D64Image img;
        if (!sysop_d64_open(&img, d64filename))
        {
            printf("Failed to open disk image '%s'\n", d64filename);
            return 1;
        }

        D64DirectoryEntry entries[256];
        int entry_count = sysop_d64_read_directory(&img, entries, 256);
        if (entry_count < 0)
        {
            printf("Failed to read directory from '%s'\n", d64filename);
            sysop_d64_close(&img);
            return 1;
        }

        const D64DirectoryEntry* entry = find_d64_entry(entries, entry_count, filename);
        if (!entry)
        {
            printf("File '%s' not found\n", filename);
            sysop_d64_close(&img);
            return 1;
        }

        printf("=== Directory Entry Details ===\n");
        printf("Filename: %s\n", entry->filename);
        printf("File Type: %s\n", d64_file_type_name(entry->file_type));
        printf("  - Type code: %d\n", entry->file_type);
        printf("  - Closed flag: %s\n", entry->closed ? "Yes" : "No");
        printf("  - Locked flag: %s\n", entry->locked ? "Yes" : "No");
        printf("First Track: %d (0x%02X)\n", entry->start_track, entry->start_track);
        printf("First Sector: %d (0x%02X)\n", entry->start_sector, entry->start_sector);
        printf("File size: %d blocks (%d bytes)\n", entry->size_in_sectors, entry->size_in_sectors * 254);

        int dump_result = dump_d64_file_sectors(&img, entry);
        sysop_d64_close(&img);
        return dump_result;
    }
    if (strcmp(command, "--load-inst-json")==0)
    {
        if (argc < 4)
        {
            printf("--load-inst-json requires <instrument #> <json string>\n");
            return 1;
        }
        
        int instrument_index = atoi(argv[2]);
        if (instrument_index < 1 || instrument_index > MAX_SIDWIZARD_INSTRUMENTS)
        {
            printf("Instrument index must be between 1 and %d\n", MAX_SIDWIZARD_INSTRUMENTS);
            return 1;
        }
        
        char* json_string = argv[3];
        
        // Initialize instrument data to zeros
        uint8_t data[MAX_INSTRUMENT_SIZE];
        memset(data, 0, MAX_INSTRUMENT_SIZE);
        
        // Set default name (last 8 bytes) to spaces
        for (int i = 0; i < MAX_INST_NAME_LENGTH; i++)
        {
            data[MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH + i] = 0x20;
        }
        
        // Parse JSON manually (simple parser for our specific format)
        // Expected fields: control_register, attack, decay, sustain, release, etc.
        
        // Important: We need to build the instrument in the correct structure:
        // - Header bytes: $00-$0F (16 bytes)
        // - Waveform program: starts at $10, variable length, ends with $FF
        // - PW program: variable offset (if used), variable length, ends with $FF
        // - Filter program: variable offset (if used), variable length, ends with $FF
        // - Data section total: exactly 120 bytes ($00-$77)
        // - Name: last 8 bytes ($78-$7F)
        
        char* ptr = json_string;
        
        // Helper macro to find and parse integer field
        #define PARSE_INT_FIELD(field_name, dest_offset) \
        { \
            char* field_ptr = strstr(ptr, "\"" field_name "\":"); \
            if (field_ptr) { \
                field_ptr += strlen("\"" field_name "\":"); \
                while (*field_ptr == ' ') field_ptr++; \
                int value = atoi(field_ptr); \
                data[dest_offset] = (uint8_t)value; \
            } \
        }
        
        // Parse name
        char* name_ptr = strstr(json_string, "\"name\":");
        if (name_ptr) {
            name_ptr = strchr(name_ptr, '"');
            if (name_ptr) {
                name_ptr++; // Skip opening quote
                name_ptr = strchr(name_ptr, '"');
                if (name_ptr) {
                    name_ptr++; // Skip second quote (field name end)
                    name_ptr = strchr(name_ptr, '"');
                    if (name_ptr) {
                        name_ptr++; // Now at start of value
                        int name_idx = 0;
                        while (*name_ptr && *name_ptr != '"' && name_idx < MAX_INST_NAME_LENGTH) {
                            data[MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH + name_idx] = *name_ptr;
                            name_ptr++;
                            name_idx++;
                        }
                    }
                }
            }
        }
        
        // Parse control bytes
        PARSE_INT_FIELD("control_register", 0x00);
        
        // Parse hard_restart_timer (16-bit value split into bytes 1 and 2)
        char* timer_ptr = strstr(json_string, "\"hard_restart_timer\":");
        if (timer_ptr) {
            timer_ptr += strlen("\"hard_restart_timer\":");
            while (*timer_ptr == ' ') timer_ptr++;
            int timer_value = atoi(timer_ptr);
            data[0x01] = (timer_value >> 8) & 0xFF;
            data[0x02] = timer_value & 0xFF;
        }
        
        // Parse ADSR
        char* attack_ptr = strstr(json_string, "\"attack\":");
        char* decay_ptr = strstr(json_string, "\"decay\":");
        if (attack_ptr && decay_ptr) {
            attack_ptr += strlen("\"attack\":");
            while (*attack_ptr == ' ') attack_ptr++;
            int attack = atoi(attack_ptr);
            
            decay_ptr += strlen("\"decay\":");
            while (*decay_ptr == ' ') decay_ptr++;
            int decay = atoi(decay_ptr);
            
            data[0x03] = ((attack & 0x0F) << 4) | (decay & 0x0F);
        }
        
        char* sustain_ptr = strstr(json_string, "\"sustain\":");
        char* release_ptr = strstr(json_string, "\"release\":");
        if (sustain_ptr && release_ptr) {
            sustain_ptr += strlen("\"sustain\":");
            while (*sustain_ptr == ' ') sustain_ptr++;
            int sustain = atoi(sustain_ptr);
            
            release_ptr += strlen("\"release\":");
            while (*release_ptr == ' ') release_ptr++;
            int release = atoi(release_ptr);
            
            data[0x04] = ((sustain & 0x0F) << 4) | (release & 0x0F);
        }
        
        PARSE_INT_FIELD("vibrato_freq_amp", 0x05);
        PARSE_INT_FIELD("vibrato_delay", 0x06);
        PARSE_INT_FIELD("arpeggio_speed", 0x07);
        PARSE_INT_FIELD("default_chord", 0x08);
        PARSE_INT_FIELD("octave_shift", 0x09);
        
        // Note: We'll calculate pw_table_offset and filter_table_offset after
        // building the tables, since offsets must point to actual table locations
        
        PARSE_INT_FIELD("gate_off_wf", 0x0C);
        PARSE_INT_FIELD("gate_off_pw", 0x0D);
        PARSE_INT_FIELD("gate_off_filter", 0x0E);
        PARSE_INT_FIELD("first_waveform", 0x0F);
        
        // Track where we are writing in the data section (start after header at 0x10)
        int write_offset = 0x10;
        
        // Parse waveform program array (starts at $10)
        // Note: The waveform program starts with first_waveform byte at 0x0F,
        // but the actual program table starts at 0x10
        char* wf_prog_ptr = strstr(json_string, "\"waveform_program\":");
        int has_waveform_entries = 0;
        if (wf_prog_ptr) {
            wf_prog_ptr = strchr(wf_prog_ptr, '[');
            if (wf_prog_ptr) {
                wf_prog_ptr++; // Skip '['
                
                while (*wf_prog_ptr && write_offset < (MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH)) {
                    // Skip whitespace
                    while (*wf_prog_ptr && (*wf_prog_ptr == ' ' || *wf_prog_ptr == '\n' || *wf_prog_ptr == '\r' || *wf_prog_ptr == '\t')) wf_prog_ptr++;
                    
                    if (*wf_prog_ptr == ']') break;
                    if (*wf_prog_ptr != '{') break;
                    
                    has_waveform_entries = 1;
                    
                    // Parse object {waveform, arpeggio, detune}
                    char* wf_ptr = strstr(wf_prog_ptr, "\"waveform\":");
                    char* arp_ptr = strstr(wf_prog_ptr, "\"arpeggio\":");
                    char* det_ptr = strstr(wf_prog_ptr, "\"detune\":");
                    
                    if (wf_ptr && arp_ptr && det_ptr) {
                        wf_ptr += strlen("\"waveform\":");
                        while (*wf_ptr == ' ') wf_ptr++;
                        data[write_offset++] = (uint8_t)atoi(wf_ptr);
                        
                        arp_ptr += strlen("\"arpeggio\":");
                        while (*arp_ptr == ' ') arp_ptr++;
                        data[write_offset++] = (uint8_t)atoi(arp_ptr);
                        
                        det_ptr += strlen("\"detune\":");
                        while (*det_ptr == ' ') det_ptr++;
                        data[write_offset++] = (uint8_t)atoi(det_ptr);
                    }
                    
                    // Move to next object
                    wf_prog_ptr = strchr(wf_prog_ptr, '}');
                    if (!wf_prog_ptr) break;
                    wf_prog_ptr++;
                }
            }
        }
        
        // If waveform program had no entries, just write the first_waveform byte value
        // (it's already in data[0x0F], now we copy it to 0x10 and terminate)
        if (!has_waveform_entries && write_offset == 0x10) {
            data[write_offset++] = data[0x0F];  // Copy first_waveform to program start
        }
        
        // Terminate waveform program with $FF
        if (write_offset < (MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH)) {
            data[write_offset++] = 0xFF;
        }
        
        // Parse pulse width program array
        int pw_table_start = 0;
        char* pw_prog_ptr = strstr(json_string, "\"pulse_width_program\":");
        if (pw_prog_ptr) {
            pw_prog_ptr = strchr(pw_prog_ptr, '[');
            if (pw_prog_ptr) {
                pw_prog_ptr++; // Skip '['
                
                // Skip whitespace to check if array is empty
                char* check_ptr = pw_prog_ptr;
                while (*check_ptr && (*check_ptr == ' ' || *check_ptr == '\n' || *check_ptr == '\r' || *check_ptr == '\t')) check_ptr++;
                
                if (*check_ptr != ']') {
                    // Array is not empty, record where PW table starts
                    pw_table_start = write_offset;
                    
                    while (*pw_prog_ptr && write_offset < (MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH)) {
                        while (*pw_prog_ptr && (*pw_prog_ptr == ' ' || *pw_prog_ptr == '\n' || *pw_prog_ptr == '\r' || *pw_prog_ptr == '\t')) pw_prog_ptr++;
                        
                        if (*pw_prog_ptr == ']') break;
                        if (*pw_prog_ptr != '{') break;
                        
                        char* cmd_ptr = strstr(pw_prog_ptr, "\"command\":");
                        char* val_ptr = strstr(pw_prog_ptr, "\"value\":");
                        char* kt_ptr = strstr(pw_prog_ptr, "\"kt\":");
                        
                        if (cmd_ptr && val_ptr && kt_ptr) {
                            cmd_ptr += strlen("\"command\":");
                            while (*cmd_ptr == ' ') cmd_ptr++;
                            data[write_offset++] = (uint8_t)atoi(cmd_ptr);
                            
                            val_ptr += strlen("\"value\":");
                            while (*val_ptr == ' ') val_ptr++;
                            data[write_offset++] = (uint8_t)atoi(val_ptr);
                            
                            kt_ptr += strlen("\"kt\":");
                            while (*kt_ptr == ' ') kt_ptr++;
                            data[write_offset++] = (uint8_t)atoi(kt_ptr);
                        }
                        
                        pw_prog_ptr = strchr(pw_prog_ptr, '}');
                        if (!pw_prog_ptr) break;
                        pw_prog_ptr++;
                    }
                    
                    // Terminate PW program with $FF
                    if (write_offset < (MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH)) {
                        data[write_offset++] = 0xFF;
                    }
                } else {
                    // Empty PW program - offset points to terminator
                    pw_table_start = write_offset;
                    data[write_offset++] = 0xFF;
                }
            }
        } else {
            // No PW program specified - offset points to terminator
            pw_table_start = write_offset;
            data[write_offset++] = 0xFF;
        }
        
        // Parse filter program array
        int filter_table_start = 0;
        char* filt_prog_ptr = strstr(json_string, "\"filter_program\":");
        if (filt_prog_ptr) {
            filt_prog_ptr = strchr(filt_prog_ptr, '[');
            if (filt_prog_ptr) {
                filt_prog_ptr++; // Skip '['
                
                // Skip whitespace to check if array is empty
                char* check_ptr = filt_prog_ptr;
                while (*check_ptr && (*check_ptr == ' ' || *check_ptr == '\n' || *check_ptr == '\r' || *check_ptr == '\t')) check_ptr++;
                
                if (*check_ptr != ']') {
                    // Array is not empty, record where filter table starts
                    filter_table_start = write_offset;
                    
                    while (*filt_prog_ptr && write_offset < (MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH)) {
                        while (*filt_prog_ptr && (*filt_prog_ptr == ' ' || *filt_prog_ptr == '\n' || *filt_prog_ptr == '\r' || *filt_prog_ptr == '\t')) filt_prog_ptr++;
                        
                        if (*filt_prog_ptr == ']') break;
                        if (*filt_prog_ptr != '{') break;
                        
                        char* cmd_ptr = strstr(filt_prog_ptr, "\"command\":");
                        char* val_ptr = strstr(filt_prog_ptr, "\"value\":");
                        char* kt_ptr = strstr(filt_prog_ptr, "\"kt\":");
                        
                        if (cmd_ptr && val_ptr && kt_ptr) {
                            cmd_ptr += strlen("\"command\":");
                            while (*cmd_ptr == ' ') cmd_ptr++;
                            data[write_offset++] = (uint8_t)atoi(cmd_ptr);
                            
                            val_ptr += strlen("\"value\":");
                            while (*val_ptr == ' ') val_ptr++;
                            data[write_offset++] = (uint8_t)atoi(val_ptr);
                            
                            kt_ptr += strlen("\"kt\":");
                            while (*kt_ptr == ' ') kt_ptr++;
                            data[write_offset++] = (uint8_t)atoi(kt_ptr);
                        }
                        
                        filt_prog_ptr = strchr(filt_prog_ptr, '}');
                        if (!filt_prog_ptr) break;
                        filt_prog_ptr++;
                    }
                    
                    // Terminate filter program with $FF
                    if (write_offset < (MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH)) {
                        data[write_offset++] = 0xFF;
                    }
                } else {
                    // Empty filter program - offset points to terminator
                    filter_table_start = write_offset;
                    data[write_offset++] = 0xFF;
                }
            }
        } else {
            // No filter program specified - offset points to terminator
            filter_table_start = write_offset;
            data[write_offset++] = 0xFF;
        }
        
        // Now set the table offset pointers in the header
        data[0x0A] = (uint8_t)pw_table_start;      // PW table offset (0 if no table)
        data[0x0B] = (uint8_t)filter_table_start;  // Filter table offset (0 if no table)
        
        // Ensure data section is exactly 120 bytes (remaining bytes already zeroed by memset)
        // write_offset shows where we stopped writing; rest is already zero-padded
        
        #undef PARSE_INT_FIELD
        
        // Dump hex bytes for debugging
        printf("=== Instrument Data (128 bytes) ===\n");
        for (int i = 0; i < MAX_INSTRUMENT_SIZE; i++)
        {
            printf("%02X ", data[i]);
            if ((i + 1) % 16 == 0)
            {
                printf("\n");
            }
        }
        printf("\n");
        
        // Initialize and connect
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            return 1;
        }
        sysop_server_dma_lock();
        
        // Load the instrument
        load_instrument_bin(instrument_index, data, MAX_INSTRUMENT_SIZE);
        sysop_server_dma_unlock();
        
        printf("Instrument %d loaded successfully from JSON\n", instrument_index);
        return 0;
    }

    if (strcmp(command, "--load-inst")==0)
    {
        if (argc < 4)
        {
            printf("--load-inst requires <instrument #> <source> [options]\n");
            return 1;
        }
        
        int instrument_index = atoi(argv[2]);
        if (instrument_index < 1 || instrument_index > MAX_SIDWIZARD_INSTRUMENTS)
        {
            printf("Instrument index must be between 1 and %d\n", MAX_SIDWIZARD_INSTRUMENTS);
            return 1;
        }
        
        char* d64filename = argv[3];
        if (d64filename == NULL)
        {
            printf("--load-inst command requires a source (filename or 'hex')\n");
            return 1;
        }
        
        // Initialize and connect
        sysop_init();
        int res = sysop_server_connect();
        if (res != 0)
        {
            printf("sysop_connect failed, error %d\n", res);
            return 1;
        }
        sysop_wait_vic2(0, 1);
        sysop_server_dma_lock();
        //sysop_server_dma_lock();
        
        // Set the current instrument first
        if (set_current_instrument((uint8_t)instrument_index) != 0)
        {
            printf("Invalid instrument index %d\n", instrument_index);
            //sysop_server_dma_unlock();
            sysop_server_dma_unlock();
            return 1;
        }
        
        // Check if this is a hex data load
        if (strcmp(d64filename, "hex") == 0)
        {
            g_debug_enabled = 1;
            dbg_printf("Loading instrument from hex data\n");
            
            if (argc < 5)
            {
                printf("hex option requires a quoted string of comma-delimited hex bytes\n");
                //sysop_server_dma_unlock();
                sysop_server_dma_unlock();
                return 1;
            }
            
            char* hex_string = argv[4];
            
            // Parse comma-delimited hex bytes from the string
            uint8_t data[128];
            int byte_count = 0;
            char* token = strtok(hex_string, ",");
            
            while (token != NULL && byte_count < 128)
            {
                unsigned int byte_val;
                if (sscanf(token, "%x", &byte_val) == 1 && byte_val <= 0xFF)
                {
                    data[byte_count++] = (uint8_t)byte_val;
                }
                else
                {
                    printf("Invalid hex byte: '%s'\n", token);
                    //sysop_server_dma_unlock();
                    sysop_server_dma_unlock();
                    return 1;
                }
                token = strtok(NULL, ",");
            }
            
            if (byte_count == 0)
            {
                printf("No hex bytes provided\n");
                //sysop_server_dma_unlock();
                sysop_server_dma_unlock();
                return 1;
            }
            
            dbg_printf("Parsed %d hex bytes\n", byte_count);
            if (byte_count != MAX_INSTRUMENT_SIZE)
            {
                printf("Error: expected exactly %d bytes for instrument, got %d bytes\n", MAX_INSTRUMENT_SIZE, byte_count);
                //sysop_server_dma_unlock();
                sysop_server_dma_unlock();
                return 1;
            }
            
            // Load the instrument
            load_instrument_bin(instrument_index, data, byte_count);
            //sysop_server_dma_unlock();
            sysop_server_dma_unlock();
            printf("Instrument %d loaded successfully from hex data\n", instrument_index);
            return 0;
        }
        
        // Check if the file is a .d64 file or a raw binary instrument file
        int is_d64 = 0;
        size_t len = strlen(d64filename);
        if (len > 4 && (strcmp(d64filename + len - 4, ".d64") == 0 || strcmp(d64filename + len - 4, ".D64") == 0))
        {
            is_d64 = 1;
        }
        
        // If it's not a .d64 file, treat it as a raw binary instrument file
        if (!is_d64)
        {
            g_debug_enabled = 1;
            dbg_printf("Loading raw binary instrument file '%s'\n", d64filename);
            
            FILE* file = fopen(d64filename, "rb");
            if (file == NULL) {
                printf("Error opening file '%s'\n", d64filename);
                return 1;
            }
            
            // Read enough to accept raw 128-byte slots, raw 128-byte PRGs,
            // and packed SID-Wizard .SWI/.PRG exports.
            uint8_t data[MAX_INSTRUMENT_FILE_SIZE];
            size_t bytes_read = fread(data, 1, MAX_INSTRUMENT_FILE_SIZE, file);
            fclose(file);
            
            if (bytes_read == 0) {
                printf("File is empty\n");
                return 1;
            }
            
            dbg_printf("Read %zu bytes from file\n", bytes_read);
            
            uint8_t normalized[MAX_INSTRUMENT_SIZE];
            const char* input_format = NULL;
            if (!normalize_sidwizard_instrument_bytes(data, (int)bytes_read, normalized, &input_format)) {
                printf("Expected a raw 128-byte instrument, raw 128-byte PRG, or packed SID-Wizard .SWI/.PRG export; got %zu bytes\n", bytes_read);
                return 1;
            }

            // Basic validation: check if normalized slot name bytes look printable.
            int name_looks_valid = 1;
            for (int i = MAX_INSTRUMENT_SIZE - MAX_INST_NAME_LENGTH; i < MAX_INSTRUMENT_SIZE; i++) {
                if (normalized[i] < 0x20 || normalized[i] > 0x7E) {
                    if (normalized[i] != 0xA0) { // Allow shifted space
                        name_looks_valid = 0;
                        break;
                    }
                }
            }
            
            if (!name_looks_valid) {
                printf("Warning: file doesn't appear to have a valid instrument name in the last 8 bytes\n");
            }
            
            dbg_printf("Normalized instrument input format: %s\n", input_format);

            // Load the expanded 128-byte SID-Wizard memory slot.
            load_instrument_bin(instrument_index, normalized, MAX_INSTRUMENT_SIZE);
            //sysop_server_dma_unlock();
            sysop_server_dma_unlock();
            printf("Instrument %d loaded successfully (%s)\n", instrument_index, input_format);
            return 0;
        }
        
        // D64/D81-backed instrument load.  Use libsysop64's disk-image
        // helpers so sector geometry, directory parsing, and sector-chain
        // walking stay in one place.
        D64Image img;
        if (!sysop_d64_open(&img, d64filename))
        {
            printf("Failed to open disk image '%s'\n", d64filename);
            return 1;
        }

        g_debug_enabled = 1; // so printing works

        char* filename = argv[4];
        if (filename == NULL)
        {
            printf("--load-inst command requires a filename from the d64\n");
            sysop_d64_close(&img);
            sysop_server_dma_unlock();
            return 1;
        }
        dbg_printf("Looking for file '%s' in disk image '%s'\n", filename, d64filename);

        D64DirectoryEntry entries[256];
        int entry_count = sysop_d64_read_directory(&img, entries, 256);
        if (entry_count < 0)
        {
            printf("Failed to read directory from '%s'\n", d64filename);
            sysop_d64_close(&img);
            sysop_server_dma_unlock();
            return 1;
        }

        const D64DirectoryEntry* entry = find_d64_entry(entries, entry_count, filename);
        if (!entry)
        {
            printf("File '%s' not found in d64\n", filename);
            sysop_d64_close(&img);
            sysop_server_dma_unlock();
            return 1;
        }

        dbg_printf("Found '%s': type=%s blocks=%d start=%d/%d\n",
                   entry->filename,
                   d64_file_type_name(entry->file_type),
                   entry->size_in_sectors,
                   entry->start_track,
                   entry->start_sector);

        int data_capacity = entry->size_in_sectors * 254;
        if (data_capacity <= 0)
            data_capacity = MAX_INSTRUMENT_FILE_SIZE;

        uint8_t* data = malloc((size_t)data_capacity);
        if (!data)
        {
            dbg_printf("Memory allocation failed\n");
            sysop_d64_close(&img);
            sysop_server_dma_unlock();
            return 1;
        }

        int data_size = sysop_d64_load_file(&img, entry, data, data_capacity);
        if (data_size < 0)
        {
            printf("Failed to load file '%s' from d64\n", filename);
            free(data);
            sysop_d64_close(&img);
            sysop_server_dma_unlock();
            return 1;
        }

        load_instrument(instrument_index, data, data_size);
        sysop_server_dma_unlock();
        free(data);
        sysop_d64_close(&img);
        printf("Instrument %d loaded successfully\n", instrument_index);
        return 0;
    }

    return 0;
}
