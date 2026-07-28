/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * Receive frame-oriented tickstream writes and replay them onto the C64 bus.
 *
 * Protocol:
 *   header:  "S64RPLY2" uint16_be lines uint16_be cycles
 *   frame:   uint16_be frame_delta, uint32_be event_count
 *   event:   uint16_be line, uint8 cycle, uint16_be address, uint8 data
 *
 * The receiver reads one complete frame into memory before replaying it.  The
 * first event at a raster slot is delivered with sysop_wait_vic2(line, cycle),
 * then sysop_poke().  Since each poke consumes a C64 cycle, following events
 * scheduled for the next cycle are emitted as consecutive pokes without another
 * wait.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "sysop_library.h"

#define REPLAY_MAGIC "S64RPLY2"
#define REPLAY_MAGIC_BYTES 8
#define DEFAULT_PORT 9100
#define DEFAULT_MAX_EVENTS_PER_FRAME 200000
#define DEFAULT_0001_HELPER_PATCH_ADDR 0x0827
#define DEFAULT_0001_HELPER_PROTECT_START 0x0801
#define DEFAULT_0001_HELPER_PROTECT_END 0x082c
#define DEFAULT_0001_HELPER_LOCATION 0x080d
#define REPLAY_MEMORY_MAP_COUNT 256
#define REPLAY_MEMORY_MAP_SIZE 65536
#define DEFAULT_0001_VALUE 0x37
#define BASIC_START_ADDR 0x0801
#define BASIC_VAR_START_ADDR 0x082d
#define BASIC_KEYBOARD_BUFFER_ADDR 0x0277
#define BASIC_KEY_COUNT_ADDR 0x00c6

struct replay_event {
    uint16_t line;
    uint8_t cycle;
    uint16_t addr;
    uint8_t data;
};

struct replay_options {
    int debug;
    int use_0001_helper;
    int ignore_0001_writes;
    int verify_0001_helper_patch;
    uint16_t helper_0001_patch_addr;
    uint16_t helper_0001_protect_start;
    uint16_t helper_0001_protect_end;
    uint16_t helper_0001_location;
};

struct replay_state {
    uint8_t current_0001_value;
    uint32_t next_available_slot;
    uint64_t helper_0001_invocations;
    uint64_t helper_0001_successes;
    uint64_t helper_0001_patch_verify_ok;
    uint64_t helper_0001_patch_verify_fail;
    uint8_t* mirror_valid[REPLAY_MEMORY_MAP_COUNT];
    uint8_t* mirror_value[REPLAY_MEMORY_MAP_COUNT];
};

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction SIGINT");
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction SIGTERM");
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);
    return 0;
}

static uint16_t read_be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24)
        | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8)
        | p[3];
}

static int read_exact(int fd, void* data, size_t len)
{
    uint8_t* p = (uint8_t*)data;

    while (len > 0 && !g_stop) {
        ssize_t got = recv(fd, p, len, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && !g_stop) {
                continue;
            }
            return -1;
        }
        if (got == 0) {
            return 0;
        }
        p += got;
        len -= (size_t)got;
    }

    return g_stop ? -1 : 1;
}

static int accept_client(int server)
{
    while (!g_stop) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 250000;

        int ready = select(server + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ready == 0) {
            continue;
        }

        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        return client;
    }

    errno = EINTR;
    return -1;
}

static int listen_tcp(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }

    if (listen(sock, 1) < 0) {
        perror("listen");
        close(sock);
        return -1;
    }

    return sock;
}

static int read_header(int fd, uint16_t* lines, uint16_t* cycles)
{
    uint8_t header[REPLAY_MAGIC_BYTES + 4];
    int res = read_exact(fd, header, sizeof(header));
    if (res <= 0) {
        return res;
    }

    if (memcmp(header, REPLAY_MAGIC, REPLAY_MAGIC_BYTES) != 0) {
        fprintf(stderr, "invalid replay stream magic\n");
        return -1;
    }

    *lines = read_be16(header + REPLAY_MAGIC_BYTES);
    *cycles = read_be16(header + REPLAY_MAGIC_BYTES + 2);

    if (*lines == 0 || *cycles == 0) {
        fprintf(stderr, "invalid raster geometry %u x %u\n", *lines, *cycles);
        return -1;
    }

    return 1;
}

static int read_frame(int fd, struct replay_event* events, uint32_t max_events, uint16_t* frame_delta, uint32_t* event_count)
{
    uint8_t frame_header[6];
    int res = read_exact(fd, frame_header, sizeof(frame_header));
    if (res <= 0) {
        return res;
    }

    *frame_delta = read_be16(frame_header) & 0x03ff;
    uint32_t count = read_be32(frame_header + 2);
    if (count > max_events) {
        fprintf(stderr, "frame has %u events, max is %u\n", count, max_events);
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint8_t bytes[6];
        res = read_exact(fd, bytes, sizeof(bytes));
        if (res <= 0) {
            return res;
        }

        events[i].line = read_be16(bytes);
        events[i].cycle = bytes[2];
        events[i].addr = read_be16(bytes + 3);
        events[i].data = bytes[5];
    }

    *event_count = count;
    return 1;
}

static void wait_frame_delta(uint16_t frame_delta, struct replay_state* state)
{
    for (uint16_t i = 0; i < frame_delta && !g_stop; i++) {
        sysop_wait_vic2(0, 1);
        if (i + 1 < frame_delta) {
            sysop_wait_vic2(0, 2);
        }
    }

    if (frame_delta != 0) {
        state->next_available_slot = 0;
    }
}

static uint32_t raster_slot(uint16_t line, uint8_t cycle, uint16_t cycles_per_line)
{
    uint8_t clamped_cycle = cycle;

    if (clamped_cycle < 1) {
        clamped_cycle = 1;
    } else if (clamped_cycle > cycles_per_line) {
        clamped_cycle = (uint8_t)cycles_per_line;
    }

    return ((uint32_t)line * cycles_per_line) + (clamped_cycle - 1);
}

static void begin_frame_dma_tag(uint32_t* frame_tag)
{
    if (*frame_tag != 0) {
        uint32_t tag = sysop_dma_tag_data();
        while (tag != *frame_tag && !g_stop) {
            usleep(50);
            tag = sysop_dma_tag_data();
        }
    }

    if (g_stop) {
        return;
    }

    *frame_tag = (*frame_tag + 1) & 0x00ffffff;
    if (*frame_tag == 0) {
        *frame_tag = 1;
    }

    sysop_dma_write_tag(*frame_tag);
}

static int parse_address(const char* text, const char** tail, uint16_t* value)
{
    const char* p = text;
    int base = 16;

    if (*p == '$') {
        p++;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
    }

    char* parse_tail;
    unsigned long parsed = strtoul(p, &parse_tail, base);
    if (parse_tail == p || parsed > 0xffff) {
        return -1;
    }

    if (tail != NULL) {
        *tail = parse_tail;
    } else if (*parse_tail != '\0') {
        return -1;
    }

    *value = (uint16_t)parsed;
    return 0;
}

static int parse_address_range(const char* text, uint16_t* start, uint16_t* end)
{
    const char* range_end;
    uint16_t parsed_start;
    uint16_t parsed_end;

    if (parse_address(text, &range_end, &parsed_start) != 0 || *range_end != '-') {
        return -1;
    }

    if (parse_address(range_end + 1, NULL, &parsed_end) != 0) {
        return -1;
    }

    if (parsed_start <= parsed_end) {
        *start = (uint16_t)parsed_start;
        *end = (uint16_t)parsed_end;
    } else {
        *start = (uint16_t)parsed_end;
        *end = (uint16_t)parsed_start;
    }

    return 0;
}

static void poke_bytes(uint16_t addr, const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        sysop_poke((uint16_t)(addr + i), data[i]);
    }
}

static int build_basic_sys_stub(uint16_t target, uint8_t* out, size_t out_size, size_t* out_len)
{
    char target_text[6];
    int digits = snprintf(target_text, sizeof(target_text), "%u", target);
    if (digits < 1 || digits >= (int)sizeof(target_text)) {
        return -1;
    }

    size_t len = (size_t)digits + 8;
    if (len > out_size) {
        return -1;
    }

    uint16_t next_line = (uint16_t)(BASIC_START_ADDR + len - 2);
    out[0] = (uint8_t)(next_line & 0xff);
    out[1] = (uint8_t)(next_line >> 8);
    out[2] = 0x0a;
    out[3] = 0x00;
    out[4] = 0x9e;
    memcpy(out + 5, target_text, (size_t)digits);
    out[5 + digits] = 0x00;
    out[6 + digits] = 0x00;
    out[7 + digits] = 0x00;
    *out_len = len;
    return 0;
}

static size_t build_0001_helper_code(uint16_t location, uint8_t* out, size_t out_size, uint16_t* patch_addr)
{
    uint8_t code[] = {
        0x78,                         /* sei */
        0xa9, 0x7f,                   /* lda #$7f */
        0x8d, 0x0d, 0xdc,             /* sta $dc0d */
        0x8d, 0x0d, 0xdd,             /* sta $dd0d */
        0xad, 0x0d, 0xdc,             /* lda $dc0d */
        0xad, 0x0d, 0xdd,             /* lda $dd0d */
        0xa9, 0x00,                   /* lda #$00 */
        0x8d, 0x1a, 0xd0,             /* sta $d01a */
        0xa9, 0xff,                   /* lda #$ff */
        0x8d, 0x19, 0xd0,             /* sta $d019 */
        0xa9, DEFAULT_0001_VALUE,     /* lda #$37 */
        0x85, 0x01,                   /* sta $01 */
        0x4c, 0x00, 0x00              /* jmp helper_loop */
    };

    if (sizeof(code) > out_size) {
        return 0;
    }

    uint16_t loop_addr = (uint16_t)(location + 25);
    code[30] = (uint8_t)(loop_addr & 0xff);
    code[31] = (uint8_t)(loop_addr >> 8);

    memcpy(out, code, sizeof(code));
    *patch_addr = (uint16_t)(location + 26);
    return sizeof(code);
}

static int install_0001_helper_program(struct replay_options* options)
{
    uint8_t basic_stub[16];
    uint8_t helper_code[64];
    size_t basic_len = 0;
    uint16_t patch_addr = 0;

    if (build_basic_sys_stub(options->helper_0001_location, basic_stub, sizeof(basic_stub), &basic_len) != 0) {
        fprintf(stderr, "failed to build BASIC SYS stub for $%04X\n", options->helper_0001_location);
        return -1;
    }

    size_t helper_len = build_0001_helper_code(
        options->helper_0001_location,
        helper_code,
        sizeof(helper_code),
        &patch_addr);
    if (helper_len == 0) {
        fprintf(stderr, "failed to build $0001 helper code\n");
        return -1;
    }

    poke_bytes(BASIC_START_ADDR, basic_stub, basic_len);
    for (uint16_t addr = (uint16_t)(BASIC_START_ADDR + basic_len); addr < BASIC_VAR_START_ADDR; addr++) {
        sysop_poke(addr, 0x00);
    }
    poke_bytes(options->helper_0001_location, helper_code, helper_len);

    sysop_poke(0x002d, (uint8_t)(BASIC_VAR_START_ADDR & 0xff));
    sysop_poke(0x002e, (uint8_t)(BASIC_VAR_START_ADDR >> 8));

    options->helper_0001_patch_addr = patch_addr;
    options->helper_0001_protect_start = options->helper_0001_location;
    options->helper_0001_protect_end = (uint16_t)(options->helper_0001_location + helper_len - 1);
    return 0;
}

static void poke_run_command(void)
{
    uint16_t addr = BASIC_KEYBOARD_BUFFER_ADDR;
    sysop_poke(addr++, 0x52);
    sysop_poke(addr++, 0x55);
    sysop_poke(addr++, 0x4e);
    sysop_poke(addr++, 0x0d);
    sysop_poke(BASIC_KEY_COUNT_ADDR, 0x04);
}

static int is_helper_protected_write(const struct replay_event* event, const struct replay_options* options)
{
    return options->use_0001_helper
        && event->addr >= options->helper_0001_protect_start
        && event->addr <= options->helper_0001_protect_end;
}

static int ensure_mirror_map(struct replay_state* state, uint8_t map_value)
{
    if (state->mirror_valid[map_value] != NULL && state->mirror_value[map_value] != NULL) {
        return 0;
    }

    state->mirror_valid[map_value] = (uint8_t*)calloc(REPLAY_MEMORY_MAP_SIZE, sizeof(uint8_t));
    state->mirror_value[map_value] = (uint8_t*)calloc(REPLAY_MEMORY_MAP_SIZE, sizeof(uint8_t));
    if (state->mirror_valid[map_value] == NULL || state->mirror_value[map_value] == NULL) {
        perror("calloc replay mirror map");
        free(state->mirror_valid[map_value]);
        free(state->mirror_value[map_value]);
        state->mirror_valid[map_value] = NULL;
        state->mirror_value[map_value] = NULL;
        return -1;
    }

    return 0;
}

static int is_redundant_write(const struct replay_event* event, const struct replay_options* options, struct replay_state* state)
{
    if (event->addr == 0x0001) {
        if (event->data == state->current_0001_value) {
            if (options->debug) {
                fprintf(stderr,
                    "tickreplay_receiver: skipped redundant $0001 line=%u cycle=%u value=$%02X\n",
                    event->line,
                    event->cycle,
                    event->data);
            }
            return 1;
        }
        return 0;
    }

    uint8_t* valid = state->mirror_valid[state->current_0001_value];
    uint8_t* value = state->mirror_value[state->current_0001_value];
    if (valid == NULL || value == NULL) {
        return 0;
    }

    if (valid[event->addr] && value[event->addr] == event->data) {
        if (options->debug) {
            fprintf(stderr,
                "tickreplay_receiver: skipped redundant write map=$%02X line=%u cycle=%u addr=$%04X value=$%02X\n",
                state->current_0001_value,
                event->line,
                event->cycle,
                event->addr,
                event->data);
        }
        return 1;
    }

    return 0;
}

static void remember_write(const struct replay_event* event, struct replay_state* state)
{
    state->mirror_valid[state->current_0001_value][event->addr] = 1;
    state->mirror_value[state->current_0001_value][event->addr] = event->data;

    if (event->addr == 0x0001) {
        state->current_0001_value = event->data;
    }
}

static void replay_frame(
    const struct replay_event* events,
    uint32_t event_count,
    uint16_t lines_per_frame,
    uint16_t cycles_per_line,
    const struct replay_options* options,
    struct replay_state* state)
{
    const uint32_t slots_per_frame = (uint32_t)lines_per_frame * cycles_per_line;

    for (uint32_t i = 0; i < event_count && !g_stop; i++) {
        if (options->ignore_0001_writes && events[i].addr == 0x0001) {
            if (options->debug) {
                fprintf(stderr,
                    "tickreplay_receiver: ignored $0001 write line=%u cycle=%u value=$%02X\n",
                    events[i].line,
                    events[i].cycle,
                    events[i].data);
            }
            continue;
        }

        if (is_helper_protected_write(&events[i], options)) {
            if (options->debug) {
                fprintf(stderr,
                    "tickreplay_receiver: skipped helper write line=%u cycle=%u addr=$%04X value=$%02X\n",
                    events[i].line,
                    events[i].cycle,
                    events[i].addr,
                    events[i].data);
            }
            continue;
        }

        if (is_redundant_write(&events[i], options, state)) {
            continue;
        }

        if (ensure_mirror_map(state, state->current_0001_value) != 0) {
            g_stop = 1;
            break;
        }

        uint32_t target_slot = raster_slot(events[i].line, events[i].cycle, cycles_per_line);

        if (state->next_available_slot == UINT32_MAX || target_slot > state->next_available_slot) {
            sysop_wait_vic2(events[i].line, events[i].cycle);
            state->next_available_slot = target_slot;
        } else if (target_slot < state->next_available_slot) {
            if (state->next_available_slot != slots_per_frame || target_slot != 0) {
                sysop_wait_vic2(events[i].line, events[i].cycle);
                state->next_available_slot = target_slot;
            }
        }

        if (events[i].addr == 0x0001) {
            if (options->debug) {
                fprintf(stderr,
                    "tickreplay_receiver: $0001 write line=%u cycle=%u value=$%02X\n",
                    events[i].line,
                    events[i].cycle,
                    events[i].data);
            }

            if (options->use_0001_helper) {
                state->helper_0001_invocations++;
                sysop_dma_write_0001(options->helper_0001_patch_addr, events[i].data);
                state->helper_0001_successes++;
                remember_write(&events[i], state);
                if (options->verify_0001_helper_patch) {
                    sysop_dma_wait_empty();
                    sysop_dma_wait_not_busy();

                    uint8_t patched_value = sysop_peek(options->helper_0001_patch_addr);
                    if (patched_value == events[i].data) {
                        state->helper_0001_patch_verify_ok++;
                        fprintf(stderr,
                            "tickreplay_receiver: verified $0001 helper patch=$%04X value=$%02X\n",
                            options->helper_0001_patch_addr,
                            patched_value);
                    } else {
                        state->helper_0001_patch_verify_fail++;
                        fprintf(stderr,
                            "tickreplay_receiver: FAILED $0001 helper patch=$%04X expected=$%02X got=$%02X\n",
                            options->helper_0001_patch_addr,
                            events[i].data,
                            patched_value);
                    }
                }
                if (options->debug) {
                    fprintf(stderr,
                        "tickreplay_receiver: queued $0001 helper value=$%02X patch=$%04X\n",
                        events[i].data,
                        options->helper_0001_patch_addr);
                }
                state->next_available_slot = UINT32_MAX;
                continue;
            }
        }

        sysop_poke(events[i].addr, events[i].data);
        remember_write(&events[i], state);
        state->next_available_slot++;
    }
}

static void usage(const char* argv0)
{
    fprintf(stderr,
        "usage: %s [--port N] [--max-events N] [--debug] [--use-0001-helper]\n"
        "          [--ignore-01-writes] [--verify-0001-helper-patch]\n"
        "          [--helper-0001-patch ADDR] [--helper-0001-protect START-END]\n"
        "          [--helper-0001-location ADDR]\n"
        "\n"
        "Receives tickreplay_sender.py frames and replays timed C64 writes.\n"
        "\n"
        "  --debug                 Log $0001 helper activity and skipped helper writes.\n"
        "  --use-0001-helper       Apply $0001 writes with FPGA DMA command 69,\n"
        "                          which patches the C64 helper, releases DMA,\n"
        "                          waits for CHL, and reasserts DMA. Enabled by default.\n"
        "  --ignore-01-writes      Drop replay writes to $0001 and do not install\n"
        "                          or run the $0001 helper.\n"
        "  --verify-0001-helper-patch\n"
        "                          Slow test mode: drain DMA after each command 69,\n"
        "                          peek the helper immediate byte, and report match.\n"
        "  --helper-0001-patch     Helper immediate byte address, default $%04X.\n"
        "  --helper-0001-protect   Skip replay writes to this helper range,\n"
        "                          default $%04X-$%04X.\n"
        "  --helper-0001-location  Install helper machine code here, default $%04X.\n",
        argv0,
        DEFAULT_0001_HELPER_PATCH_ADDR,
        DEFAULT_0001_HELPER_PROTECT_START,
        DEFAULT_0001_HELPER_PROTECT_END,
        DEFAULT_0001_HELPER_LOCATION);
}

int main(int argc, char** argv)
{
    uint16_t port = DEFAULT_PORT;
    uint32_t max_events = DEFAULT_MAX_EVENTS_PER_FRAME;
    struct replay_options options;

    memset(&options, 0, sizeof(options));
    options.use_0001_helper = 1;
    options.helper_0001_patch_addr = DEFAULT_0001_HELPER_PATCH_ADDR;
    options.helper_0001_protect_start = DEFAULT_0001_HELPER_PROTECT_START;
    options.helper_0001_protect_end = DEFAULT_0001_HELPER_PROTECT_END;
    options.helper_0001_location = DEFAULT_0001_HELPER_LOCATION;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--max-events") == 0 && i + 1 < argc) {
            max_events = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--debug") == 0) {
            options.debug = 1;
        } else if (strcmp(argv[i], "--use-0001-helper") == 0) {
            options.use_0001_helper = 1;
        } else if (strcmp(argv[i], "--ignore-01-writes") == 0) {
            options.ignore_0001_writes = 1;
            options.use_0001_helper = 0;
        } else if (strcmp(argv[i], "--verify-0001-helper-patch") == 0) {
            options.verify_0001_helper_patch = 1;
        } else if (strcmp(argv[i], "--helper-0001-patch") == 0 && i + 1 < argc) {
            if (parse_address(argv[++i], NULL, &options.helper_0001_patch_addr) != 0) {
                fprintf(stderr, "invalid --helper-0001-patch address\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--helper-0001-location") == 0 && i + 1 < argc) {
            if (parse_address(argv[++i], NULL, &options.helper_0001_location) != 0) {
                fprintf(stderr, "invalid --helper-0001-location address\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--helper-0001-protect") == 0 && i + 1 < argc) {
            if (parse_address_range(argv[++i], &options.helper_0001_protect_start, &options.helper_0001_protect_end) != 0) {
                fprintf(stderr, "invalid --helper-0001-protect range, expected START-END\n");
                return 1;
            }
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (options.ignore_0001_writes) {
        options.use_0001_helper = 0;
    }

    if (install_signal_handlers() != 0) {
        return 1;
    }

    struct replay_event* events = (struct replay_event*)calloc(max_events, sizeof(struct replay_event));
    if (events == NULL) {
        perror("calloc");
        return 1;
    }

    int server = listen_tcp(port);
    if (server < 0) {
        free(events);
        return 1;
    }

    if (sysop_init() < 0) {
        close(server);
        free(events);
        return 1;
    }

    if (sysop_server_connect() != 0) {
        fprintf(stderr, "sysop_server_connect failed\n");
        sysop_uninit();
        close(server);
        free(events);
        return 1;
    }

    fprintf(stderr, "tickreplay_receiver: resetting C64\n");
    sysop_c64_reset();
    sleep(5);

    if (options.use_0001_helper) {
        sysop_server_dma_lock();
        fprintf(stderr, "tickreplay_receiver: installing $0001 helper\n");
        if (install_0001_helper_program(&options) != 0) {
            sysop_server_dma_unlock();
            sysop_server_disconnect();
            sysop_uninit();
            close(server);
            free(events);
            return 1;
        }
        sysop_dma_wait_empty();
        sysop_dma_wait_not_busy();

        poke_run_command();
        sysop_dma_wait_empty();
        sysop_dma_wait_not_busy();
        fprintf(stderr, "tickreplay_receiver: injected RUN, releasing DMA for helper startup\n");
        sysop_server_dma_unlock();
        sleep(2);
        sysop_server_dma_lock();
    } else {
        sysop_server_dma_lock();
    }

    fprintf(stderr, "tickreplay_receiver: listening on port %u\n", port);
    if (options.use_0001_helper) {
        fprintf(stderr,
            "tickreplay_receiver: $0001 helper enabled location=$%04X patch=$%04X protect=$%04X-$%04X fpga_cmd=69\n",
            options.helper_0001_location,
            options.helper_0001_patch_addr,
            options.helper_0001_protect_start,
            options.helper_0001_protect_end);
        if (options.verify_0001_helper_patch) {
            fprintf(stderr,
                "tickreplay_receiver: verifying $0001 helper patch after every command 69; replay will be slow\n");
        }
    } else if (options.ignore_0001_writes) {
        fprintf(stderr, "tickreplay_receiver: ignoring replay writes to $0001\n");
    }

    int client = accept_client(server);
    if (client < 0) {
        if (!g_stop) {
            perror("accept");
        }
        sysop_server_dma_unlock();
        sysop_server_disconnect();
        sysop_uninit();
        close(server);
        free(events);
        return 1;
    }

    int one = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval recv_timeout;
    recv_timeout.tv_sec = 0;
    recv_timeout.tv_usec = 250000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

    uint16_t lines = 0;
    uint16_t cycles = 0;
    if (read_header(client, &lines, &cycles) <= 0) {
        close(client);
        sysop_server_dma_unlock();
        sysop_server_disconnect();
        sysop_uninit();
        close(server);
        free(events);
        return 1;
    }

    fprintf(stderr, "tickreplay_receiver: connected, geometry=%u lines x %u cycles\n", lines, cycles);

    uint64_t frame_number = 0;
    uint32_t dma_frame_tag = 0;
    struct replay_state state;
    memset(&state, 0, sizeof(state));
    state.current_0001_value = DEFAULT_0001_VALUE;
    state.next_available_slot = UINT32_MAX;
    while (!g_stop) {
        uint16_t frame_delta = 0;
        uint32_t event_count = 0;
        int res = read_frame(client, events, max_events, &frame_delta, &event_count);
        if (res < 0) {
            perror("read_frame");
            break;
        }
        if (res == 0) {
            break;
        }

        if (frame_number == 0) {
            fprintf(stderr, "tickreplay_receiver: first frame delta=%u events=%u\n", frame_delta, event_count);
        }

        begin_frame_dma_tag(&dma_frame_tag);
        if (g_stop) {
            break;
        }

        wait_frame_delta(frame_delta, &state);
        replay_frame(events, event_count, lines, cycles, &options, &state);
        frame_number++;

        if ((frame_number & 31ULL) == 0) {
            fprintf(stderr,
                "tickreplay_receiver: frame %llu delta=%u events=%u $0001_helpers=%llu applied=%llu verify_ok=%llu verify_fail=%llu\n",
                (unsigned long long)frame_number,
                frame_delta,
                event_count,
                (unsigned long long)state.helper_0001_invocations,
                (unsigned long long)state.helper_0001_successes,
                (unsigned long long)state.helper_0001_patch_verify_ok,
                (unsigned long long)state.helper_0001_patch_verify_fail);
        }
    }

    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();
    close(client);
    close(server);
    for (uint32_t i = 0; i < REPLAY_MEMORY_MAP_COUNT; i++) {
        free(state.mirror_valid[i]);
        free(state.mirror_value[i]);
    }
    free(events);

    fprintf(stderr,
        "tickreplay_receiver: stopped, $0001_helpers=%llu applied=%llu verify_ok=%llu verify_fail=%llu\n",
        (unsigned long long)state.helper_0001_invocations,
        (unsigned long long)state.helper_0001_successes,
        (unsigned long long)state.helper_0001_patch_verify_ok,
        (unsigned long long)state.helper_0001_patch_verify_fail);
    return 0;
}
