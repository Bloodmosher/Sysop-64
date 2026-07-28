/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * Stream c64_tick_sampler data to a TCP client.
 *
 * The default stream payload is raw little-endian c64_tick_sampler data: each
 * 32-byte record is four packed 64-bit C64 bus samples.  With --busmon-debug,
 * each 32-byte record is one c64_bus_monitor_debug_sampler packet.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "sysop_defines.h"
#include "sysop_library.h"

#define STREAM_CHUNK_BYTES (256 * 1024)
#define CATCHUP_RAM_SHADOW_CMD 40
#define CATCHUP_IO_SHADOW_CMD 39
#define CATCHUP_IO_START 0xD000
#define CATCHUP_IO_END 0xDFFF
#define CATCHUP_RAM_START 0x0100
#define CATCHUP_RAM_UNDER_IO_0001 0x34
#define CATCHUP_D011 0xD011
#define CATCHUP_SYNTH_LINE_START 0
#define CATCHUP_SYNTH_LINE_END 311
#define CATCHUP_SYNTH_CYCLES_PER_LINE 63

static volatile sig_atomic_t g_stop = 0;

struct synthetic_timing {
    uint16_t frame;
    uint16_t line;
    uint8_t cycle;
};

static void handle_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint32_t parse_u32(const char* text)
{
    return (uint32_t)strtoul(text, NULL, 0);
}

static void install_signal_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);

    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static bool chunk_has_port01_mismatch(const uint8_t* data, uint32_t len, uint64_t* raw_out)
{
    for (uint32_t offset = 0; offset + SYSOP64_TICK_SAMPLER_WORD_BYTES <= len; offset += SYSOP64_TICK_SAMPLER_WORD_BYTES) {
        uint64_t raw;
        memcpy(&raw, data + offset, sizeof(raw));

        if (((raw >> 53) & 1u) == 0) {
            if (raw_out != NULL) {
                *raw_out = raw;
            }
            return true;
        }
    }

    return false;
}

static int listen_tcp(const char* bind_addr, uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    int one = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        perror("setsockopt SO_REUSEADDR");
    }
#ifdef SO_REUSEPORT
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
        perror("setsockopt SO_REUSEPORT");
    }
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid bind IPv4 address: %s\n", bind_addr);
        close(sock);
        return -1;
    }

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

static int accept_client(int server)
{
    while (!g_stop) {
        fd_set read_fds;
        struct timeval timeout;

        FD_ZERO(&read_fds);
        FD_SET(server, &read_fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 250000;

        int ready = select(server + 1, &read_fds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            return -1;
        }
        if (ready == 0) {
            continue;
        }

        int sock = accept(server, NULL, NULL);
        if (sock < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            return -1;
        }

        return sock;
    }

    return -1;
}

static int send_all(int sock, const uint8_t* data, size_t len)
{
    while (len > 0 && !g_stop) {
        ssize_t sent = send(sock, data, len, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            return -1;
        }
        data += sent;
        len -= (size_t)sent;
    }

    return g_stop ? -1 : 0;
}

static void advance_synthetic_timing(struct synthetic_timing* timing)
{
    timing->cycle++;
    if (timing->cycle <= CATCHUP_SYNTH_CYCLES_PER_LINE) {
        return;
    }

    timing->cycle = 1;
    timing->line++;
    if (timing->line <= CATCHUP_SYNTH_LINE_END) {
        return;
    }

    timing->line = CATCHUP_SYNTH_LINE_START;
    timing->frame = (uint16_t)((timing->frame + 1) & 0x00ff);
}

static uint64_t make_synthetic_write_sample(
    uint16_t addr,
    uint8_t data,
    uint8_t selected_tick,
    const struct synthetic_timing* timing)
{
    return ((uint64_t)(timing->frame & 0x00ff) << 56)
        | ((uint64_t)1 << 55) /* _irq inactive */
        | ((uint64_t)1 << 54) /* _dma inactive */
        | ((uint64_t)1 << 53) /* synthetic $01=CHL match */
        | ((uint64_t)(selected_tick & 0x3f) << 47)
        | ((uint64_t)(timing->line & 0x01ff) << 38)
        | ((uint64_t)timing->cycle << 30)
        | ((uint64_t)0 << 24)
        | ((uint64_t)addr << 8)
        | data;
}

static int send_shadow_write_range(
    int sock,
    uint16_t start,
    uint32_t count,
    uint8_t selected_tick,
    struct synthetic_timing* timing,
    int skip_addr,
    uint64_t* total_sent)
{
    uint8_t chunk[STREAM_CHUNK_BYTES];
    size_t used = 0;

    for (uint32_t i = 0; i < count && !g_stop; i++) {
        uint16_t addr = (uint16_t)(start + i);
        if (skip_addr >= 0 && addr == (uint16_t)skip_addr) {
            continue;
        }

        uint8_t data = sysop_internal_peek(addr);
        uint64_t sample = make_synthetic_write_sample(addr, data, selected_tick, timing);
        advance_synthetic_timing(timing);

        memcpy(chunk + used, &sample, sizeof(sample));
        used += sizeof(sample);

        if (used == sizeof(chunk)) {
            if (send_all(sock, chunk, used) < 0) {
                return -1;
            }
            *total_sent += used;
            used = 0;
        }
    }

    if (used != 0) {
        if (send_all(sock, chunk, used) < 0) {
            return -1;
        }
        *total_sent += used;
    }

    return g_stop ? -1 : 0;
}

static int send_synthetic_write(
    int sock,
    uint16_t addr,
    uint8_t data,
    uint8_t selected_tick,
    struct synthetic_timing* timing,
    uint64_t* total_sent)
{
    uint64_t sample = make_synthetic_write_sample(addr, data, selected_tick, timing);
    advance_synthetic_timing(timing);

    if (send_all(sock, (const uint8_t*)&sample, sizeof(sample)) < 0) {
        return -1;
    }

    *total_sent += sizeof(sample);
    return 0;
}

static int send_shadow_catchup(int sock, uint8_t selected_tick, uint64_t* total_sent)
{
    uint8_t restore_0001;
    uint8_t restore_d011;
    struct synthetic_timing timing;

    timing.frame = 0;
    timing.line = CATCHUP_SYNTH_LINE_START;
    timing.cycle = 1;

    fprintf(stderr, "tickstream: sending shadow catch-up RAM image\n");
    sysop_command(CATCHUP_RAM_SHADOW_CMD);
    restore_0001 = sysop_internal_peek(0x0001);

    sysop_command(CATCHUP_IO_SHADOW_CMD);
    restore_d011 = sysop_internal_peek(CATCHUP_D011);
    fprintf(stderr, "tickstream: blanking screen during synthetic catch-up, restore D011=$%02X\n", restore_d011);
    if (send_synthetic_write(sock, CATCHUP_D011, restore_d011 & (uint8_t)~0x10, selected_tick, &timing, total_sent) < 0) {
        sysop_command(CATCHUP_RAM_SHADOW_CMD);
        return -1;
    }
    sysop_command(CATCHUP_RAM_SHADOW_CMD);

    fprintf(stderr,
        "tickstream: sending RAM shadow with synthetic $0001=$%02X, restore=$%02X\n",
        CATCHUP_RAM_UNDER_IO_0001,
        restore_0001);
    if (send_synthetic_write(sock, 0x0001, CATCHUP_RAM_UNDER_IO_0001, selected_tick, &timing, total_sent) < 0) {
        return -1;
    }

    if (send_shadow_write_range(sock, CATCHUP_RAM_START, CATCHUP_IO_START - CATCHUP_RAM_START, selected_tick, &timing, -1, total_sent) < 0) {
        return -1;
    }
    if (send_shadow_write_range(sock, CATCHUP_IO_START, CATCHUP_IO_END - CATCHUP_IO_START + 1, selected_tick, &timing, -1, total_sent) < 0) {
        return -1;
    }
    if (send_shadow_write_range(sock, CATCHUP_IO_END + 1, 0x10000 - (CATCHUP_IO_END + 1), selected_tick, &timing, -1, total_sent) < 0) {
        return -1;
    }

    if (send_synthetic_write(sock, 0x0001, restore_0001, selected_tick, &timing, total_sent) < 0) {
        return -1;
    }

    fprintf(stderr, "tickstream: sending shadow catch-up IO image\n");
    sysop_command(CATCHUP_IO_SHADOW_CMD);
    if (send_shadow_write_range(sock, CATCHUP_IO_START, CATCHUP_IO_END - CATCHUP_IO_START + 1, selected_tick, &timing, CATCHUP_D011, total_sent) < 0) {
        sysop_command(CATCHUP_RAM_SHADOW_CMD);
        return -1;
    }

    if (send_synthetic_write(sock, CATCHUP_D011, restore_d011, selected_tick, &timing, total_sent) < 0) {
        sysop_command(CATCHUP_RAM_SHADOW_CMD);
        return -1;
    }

    sysop_command(CATCHUP_RAM_SHADOW_CMD);
    fprintf(stderr,
        "tickstream: shadow catch-up complete, synthetic position frame=%u line=%u cycle=%u\n",
        timing.frame,
        timing.line,
        timing.cycle);
    return 0;
}

static void usage(const char* argv0)
{
    fprintf(stderr,
        "usage: %s <port> [options]\n"
        "\n"
        "options:\n"
        "  --bind ADDR           local address to bind (default 0.0.0.0)\n"
        "  --tick N              selected 50 MHz tick inside PHI2 cycle (default 44)\n"
        "  --seconds N           stop after N seconds (default: run until Ctrl-C)\n"
        "  --writes-only         capture and stream every write sample\n"
        "  --busmon-debug       stream c64_bus_monitor_debug_sampler packets\n"
        "  --exit-on-01-mismatch\n"
        "                        stop when inferred $0001 no longer matches CHL wires\n"
        "  --reset-on-client     start sampling after client connect, then reset\n"
        "                        the C64 to capture the reset sequence\n"
        "  --catch-up-shadow     on client connect, freeze C64 and send synthetic\n"
        "                        writes for RAM shadow plus $D000-$DFFF IO shadow\n"
        "  --range START END     capture only addresses in inclusive range\n"
        "  --start-address ADDR  SDRAM ring start physical address (default 0x21000000)\n"
        "  --end-address ADDR    SDRAM ring end physical address (default start + 0x06000000)\n",
        argv0
    );
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    uint16_t port = (uint16_t)parse_u32(argv[1]);
    if (port == 0) {
        fprintf(stderr, "port must be between 1 and 65535\n");
        usage(argv[0]);
        return 1;
    }

    const char* bind_addr = "0.0.0.0";
    int seconds = 0;
    bool catch_up_shadow = false;
    bool exit_on_01_mismatch = false;
    bool reset_on_client = false;
    bool busmon_debug = false;
    bool sysop_server_connected = false;
    bool dma_locked = false;
    bool sampler_started = false;

    struct sysop_tick_sampler_config config;
    sysop_tick_sampler_default_config(&config);

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (strcmp(argv[i], "--tick") == 0 && i + 1 < argc) {
            config.selected_tick = (uint8_t)parse_u32(argv[++i]);
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = (int)parse_u32(argv[++i]);
        } else if (strcmp(argv[i], "--writes-only") == 0) {
            config.writes_only = true;
        } else if (strcmp(argv[i], "--busmon-debug") == 0) {
            busmon_debug = true;
        } else if (strcmp(argv[i], "--exit-on-01-mismatch") == 0) {
            exit_on_01_mismatch = true;
        } else if (strcmp(argv[i], "--reset-on-client") == 0) {
            reset_on_client = true;
        } else if (strcmp(argv[i], "--catch-up-shadow") == 0) {
            catch_up_shadow = true;
        } else if (strcmp(argv[i], "--range") == 0 && i + 2 < argc) {
            config.range_filter = true;
            config.range_start = (uint16_t)parse_u32(argv[++i]);
            config.range_end = (uint16_t)parse_u32(argv[++i]);
        } else if (strcmp(argv[i], "--start-address") == 0 && i + 1 < argc) {
            config.buffer_start_address = parse_u32(argv[++i]);
        } else if (strcmp(argv[i], "--end-address") == 0 && i + 1 < argc) {
            config.buffer_end_address = parse_u32(argv[++i]);
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    uint32_t buffer_size = config.buffer_end_address - config.buffer_start_address;
    if (buffer_size == 0 || (buffer_size % SYSOP64_TICK_SAMPLER_WORD_BYTES) != 0) {
        fprintf(stderr, "buffer size must be nonzero and %u-byte aligned\n", SYSOP64_TICK_SAMPLER_WORD_BYTES);
        return 1;
    }

    install_signal_handlers();

    if (sysop_init() < 0) {
        return 1;
    }

    if (catch_up_shadow) {
        if (sysop_server_connect() < 0) {
            sysop_uninit();
            return 1;
        }
        sysop_server_connected = true;
    }

    uint8_t* buffer = (uint8_t*)sysop_tick_sampler_map_buffer_at(config.buffer_start_address, buffer_size);
    if (buffer == MAP_FAILED) {
        perror("mmap tick sampler buffer");
        if (sysop_server_connected) {
            sysop_server_disconnect();
        }
        sysop_uninit();
        return 1;
    }

    int server = listen_tcp(bind_addr, port);
    if (server < 0) {
        munmap(buffer, buffer_size);
        if (sysop_server_connected) {
            sysop_server_disconnect();
        }
        sysop_uninit();
        return 1;
    }

    fprintf(stderr, "tickstream: listening on %s:%u\n", bind_addr, port);
    int sock = accept_client(server);
    if (sock < 0) {
        close(server);
        munmap(buffer, buffer_size);
        if (sysop_server_connected) {
            sysop_server_disconnect();
        }
        sysop_uninit();
        return 1;
    }

    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval send_timeout;
    send_timeout.tv_sec = 0;
    send_timeout.tv_usec = 250000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    fprintf(stderr, "tickstream: client connected\n");

    config.continuous = true;
    config.capture_sample_limit = 0;
    sysop_tick_sampler_configure(&config);

    uint32_t read_offset = 0;
    uint32_t hps_wrap_count = 0;
    uint64_t total_sent = 0;
    time_t start_time = time(NULL);

    if (catch_up_shadow) {
        fprintf(stderr, "tickstream: locking DMA for shadow catch-up\n");
        sysop_server_dma_lock();
        dma_locked = true;

        if (send_shadow_catchup(sock, config.selected_tick, &total_sent) < 0) {
            perror("send shadow catch-up");
            g_stop = 1;
        }
    }

    if (!g_stop) {
        if (busmon_debug) {
            sysop_command(SYSOP64_CMD_ID_TRIGGER_BUSMON_DEBUG_SAMPLER);
        } else {
            sysop_tick_sampler_start();
        }
        sampler_started = true;
        fprintf(stderr, "tickstream: started %s tick=%u buffer=0x%08x..0x%08x\n",
            busmon_debug ? "busmon-debug" : "tick",
            config.selected_tick,
            config.buffer_start_address,
            config.buffer_end_address
        );

        if (reset_on_client) {
            fprintf(stderr, "tickstream: resetting C64 after sampling started\n");
            sysop_c64_reset();
        }
    }

    if (dma_locked) {
        fprintf(stderr, "tickstream: releasing DMA after shadow catch-up/start\n");
        sysop_server_dma_unlock();
        dma_locked = false;
    }

    while (!g_stop) {
        struct sysop_tick_sampler_status status;
        sysop_tick_sampler_read_status(&status);

        if (status.dropped != 0) {
            fprintf(stderr, "tickstream: FPGA FIFO dropped %u samples\n", status.dropped);
            break;
        }

        uint32_t write_offset = status.current_write_address - config.buffer_start_address;
        uint64_t fpga_total = ((uint64_t)status.wrap_count * buffer_size) + write_offset;
        uint64_t hps_total = ((uint64_t)hps_wrap_count * buffer_size) + read_offset;

        if (fpga_total < hps_total) {
            fprintf(stderr, "tickstream: sampler position moved backwards; resyncing\n");
            read_offset = write_offset;
            hps_wrap_count = status.wrap_count;
            usleep(1000);
            continue;
        }

        uint64_t backlog = fpga_total - hps_total;
        if (backlog >= buffer_size) {
            fprintf(stderr, "tickstream: ring overrun, unread data was overwritten\n");
            break;
        }

        backlog &= ~(uint64_t)(SYSOP64_TICK_SAMPLER_WORD_BYTES - 1);

        while (backlog > 0 && !g_stop) {
            uint32_t chunk = (uint32_t)backlog;
            uint32_t to_end = buffer_size - read_offset;

            if (chunk > STREAM_CHUNK_BYTES) {
                chunk = STREAM_CHUNK_BYTES;
            }
            if (chunk > to_end) {
                chunk = to_end;
            }

            chunk &= ~(SYSOP64_TICK_SAMPLER_WORD_BYTES - 1);
            if (chunk == 0) {
                break;
            }

            uint64_t mismatch_raw = 0;
            bool chunk_mismatch = exit_on_01_mismatch
                && chunk_has_port01_mismatch((const uint8_t*)buffer + read_offset, chunk, &mismatch_raw);

            if (send_all(sock, buffer + read_offset, chunk) < 0) {
                perror("send");
                g_stop = 1;
                break;
            }
            total_sent += chunk;

            read_offset += chunk;
            backlog -= chunk;

            if (chunk_mismatch) {
                fprintf(stderr,
                    "tickstream: captured inferred $0001 mismatch raw=0x%016" PRIX64 "\n",
                    mismatch_raw);
                g_stop = 1;
            }

            if (read_offset >= buffer_size) {
                read_offset = 0;
                hps_wrap_count++;
            }
        }

        if (seconds > 0 && (time(NULL) - start_time) >= seconds) {
            break;
        }

        if (backlog == 0) {
            usleep(1000);
        }
    }

    if (sampler_started) {
        if (busmon_debug) {
            sysop_command(SYSOP64_CMD_ID_STOP_BUSMON_DEBUG_SAMPLER);
        } else {
            sysop_tick_sampler_stop();
        }
    }
    if (dma_locked) {
        sysop_server_dma_unlock();
        dma_locked = false;
    }
    fprintf(stderr, "tickstream: stopped, sent %" PRIu64 " bytes\n", total_sent);

    close(sock);
    close(server);
    munmap(buffer, buffer_size);
    if (sysop_server_connected) {
        sysop_server_disconnect();
    }
    sysop_uninit();
    return 0;
}
