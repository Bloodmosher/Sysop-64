/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

/*
 * FPGA audio subsystem driver.
 *
 * The audio engine inside the FPGA supports multiple independent playback
 * channels.  All configuration is written through a single 64-bit command
 * register (sysop64_audio_command_map) and status is read back from a
 * companion 64-bit status register (sysop64_audio_status_map).
 *
 * Typical PCM playback sequence:
 *   1. sysop_audio_select_channel(ch)          -- choose channel 0..7
 *   2. sysop_audio_set_sample_format(fmt)      -- e.g. 0 = PCM16_STEREO_48K
 *   3. sysop_audio_set_phase_step(step)        -- or use phase_step_from_rate()
 *   4. sysop_audio_set_volume(left, right)     -- 0..255 per side
 *   5. sysop_audio_play_pcm(addr, frames, loop)
 *   6. sysop_audio_wait_until_done()           -- optional; skip for looping
 */

/* -----------------------------------------------------------------------
 * Command opcodes.  Written to bits [63:56] of the audio command register.
 * The lower 32 bits carry the command-specific payload.
 * ----------------------------------------------------------------------- */
#define AUDIO_CMD_SET_BASE_ADDR      0x01
#define AUDIO_CMD_SET_LENGTH_FRAMES  0x02
#define AUDIO_CMD_SET_LOOP_ENABLE    0x03
#define AUDIO_CMD_START              0x04
#define AUDIO_CMD_STOP               0x05
#define AUDIO_CMD_SET_SAMPLE_FORMAT  0x06
#define AUDIO_CMD_SET_PHASE_STEP     0x07
#define AUDIO_CMD_SELECT_CHANNEL     0x08
#define AUDIO_CMD_SELECT_STATUS_CHANNEL 0x09
#define AUDIO_CMD_SET_VOLUME_LEFT   0x0A
#define AUDIO_CMD_SET_VOLUME_RIGHT  0x0B
#define AUDIO_CMD_SET_SID_VOLUME_LEFT  0x0C
#define AUDIO_CMD_SET_SID_VOLUME_RIGHT 0x0D

/* Status register bit masks (sysop64_audio_status_map) */
#define AUDIO_STATUS_PLAYING_MASK    0x0000000000000001ULL  /* bit  0: channel is playing */
#define AUDIO_STATUS_UNDERRUN_MASK   0x0000000000000002ULL  /* bit  1: underrun occurred  */

/*
 * Issue a command to the FPGA audio engine.
 * Wire format: bits [63:56] = opcode, bits [31:0] = payload.
 * A single 64-bit store is atomic from the FPGA's perspective.
 * Callers must serialise concurrent access if needed.
 */
static void sysop_audio_write_command(
    uint8_t cmd,
    uint32_t payload)
{
    volatile uint64_t *reg = (volatile uint64_t *)sysop64_audio_command_map;

    uint64_t value = ((uint64_t)cmd << 56) | ((uint64_t)payload);

    //printf("sysop_audio_write_command 0x%016" PRIx64 "...\n", value);
    *reg = value;
}

/* Read the raw 64-bit audio status register. */
uint64_t sysop_audio_read_status(void)
{
    volatile uint64_t *reg = (volatile uint64_t *)sysop64_audio_status_map;

    return *reg;
}

bool sysop_audio_is_playing(void)
{
    return (sysop_audio_read_status() & AUDIO_STATUS_PLAYING_MASK) != 0;
}

bool sysop_audio_has_underrun(void)
{
    return (sysop_audio_read_status() & AUDIO_STATUS_UNDERRUN_MASK) != 0;
}

/* Return the number of buffer underruns since the last START command.
 * Underrun count occupies bits [31:8] of the status register. */
uint32_t sysop_audio_get_underrun_count(void)
{
    uint64_t status = sysop_audio_read_status();

    return (uint32_t)((status >> 8) & 0x00FFFFFFu);
}

/* Return the number of 64-bit words currently held in the playback FIFO.
 * FIFO fill level occupies bits [47:38] of the status register. */
uint16_t sysop_audio_get_fifo_usedw(void)
{
    uint64_t status = sysop_audio_read_status();

    return (uint16_t)((status >> 38) & 0x03FFu);
}

void sysop_audio_set_base_addr(uint32_t base_addr)
{
    sysop_audio_write_command(AUDIO_CMD_SET_BASE_ADDR, base_addr);
}

void sysop_audio_set_length_frames(uint32_t length_frames)
{
    sysop_audio_write_command(AUDIO_CMD_SET_LENGTH_FRAMES, length_frames);
}

void sysop_audio_set_loop_enable(bool enable)
{
    sysop_audio_write_command(AUDIO_CMD_SET_LOOP_ENABLE, enable ? 1 : 0);
}

void sysop_audio_start(void)
{
    sysop_audio_write_command(AUDIO_CMD_START, 0);
}

void sysop_audio_stop(void)
{
    sysop_audio_write_command(AUDIO_CMD_STOP, 0);
}

/*
 * Convenience wrapper: configure base address, frame count, and loop flag
 * then immediately start playback.  The channel and sample format must
 * already be configured before calling this function.
 */
void sysop_audio_play_pcm(uint32_t base_addr, uint32_t length_frames, bool loop)
{
    sysop_audio_set_base_addr(base_addr);
    sysop_audio_set_length_frames(length_frames);
    sysop_audio_set_loop_enable(loop);
    sysop_audio_start();
    //sysop_audio_print_status();
    //sysop_audio_start();
}

/* Spin-wait until the selected channel finishes playing.
 * Polls at 100 µs intervals.  Not suitable for tight real-time loops;
 * use sysop_audio_is_playing() directly when latency matters. */
void sysop_audio_wait_until_done(void)
{
    while (sysop_audio_is_playing())
    {
        usleep(100);
    }
}

void sysop_audio_print_status(void)
{
    uint64_t status = sysop_audio_read_status();

    bool playing  = (status & 0x1ULL) != 0;
    bool underrun = (status & 0x2ULL) != 0;

    uint32_t base_addr =
        (uint32_t)(status >> 32);

    uint16_t length =
        (uint16_t)((status >> 16) & 0xffff);

    uint8_t format =
        (uint8_t)((status >> 14) & 0x3);

    const char *format_name;

    switch (format) {
        case 0:
            format_name = "PCM16_STEREO_48K";
            break;

        case 1:
            format_name = "U8_MONO_VAR";
            break;

        default:
            format_name = "UNKNOWN";
            break;
    }

    printf("Audio Status       : 0x%016llX\n",
           (unsigned long long)status);

    printf("Playing            : %s\n",
           playing ? "YES" : "NO");

    printf("Underrun           : %s\n",
           underrun ? "YES" : "NO");

    printf("Base Addr          : 0x%08X\n",
           base_addr);

    printf("Length             : %u (0x%04X)\n",
           length,
           length);

    printf("Format             : %u (%s)\n",
           format,
           format_name);
}


uint16_t sysop_audio_dbg_cmd_count(void)
{
    return (uint16_t)((sysop_audio_read_status() >> 16) & 0xffff);
}

uint16_t sysop_audio_dbg_start_cmd_count(void)
{
    return (uint16_t)((sysop_audio_read_status() >> 32) & 0xffff);
}

uint16_t sysop_audio_dbg_bridge_start_count(void)
{
    return (uint16_t)((sysop_audio_read_status() >> 48) & 0xffff);
}

void sysop_audio_set_sample_format(uint32_t format)
{
    sysop_audio_write_command(AUDIO_CMD_SET_SAMPLE_FORMAT, format);
}

void sysop_audio_set_phase_step(uint32_t phase_step)
{
    sysop_audio_write_command(AUDIO_CMD_SET_PHASE_STEP, phase_step);
}

/*
 * Convert a PCM source sample rate (Hz) to the FPGA phase-step value.
 * The FPGA resampler uses a Q16.16 fixed-point ratio relative to 48 kHz:
 *   phase_step = round((source_rate / 48000) * 65536)
 *              = (source_rate << 16) / 48000
 * Examples: 44100 Hz -> 0xEAC0, 48000 Hz -> 0x10000, 22050 Hz -> 0x7560.
 * Pass the result to sysop_audio_set_phase_step().
 */
uint32_t sysop_audio_phase_step_from_rate(uint32_t source_rate)
{
    return (uint32_t) (((uint64_t)source_rate << 16) / 48000ULL);
}

/* Select the active channel (0-7) for all subsequent set_*, start, and
 * stop commands.  Does not affect which channel is monitored by the
 * status register; use sysop_audio_select_status_channel() for that. */
void sysop_audio_select_channel(uint32_t channel)
{
    sysop_audio_write_command(AUDIO_CMD_SELECT_CHANNEL, channel & 7);
}

/* Select which channel's state is reflected in the status register (0-7).
 * Allows monitoring a channel independently of the active command target. */
void sysop_audio_select_status_channel(uint32_t channel)
{
    sysop_audio_write_command(AUDIO_CMD_SELECT_STATUS_CHANNEL, channel & 7);
}

/* Set PCM playback volume for the active channel.  Range 0 (mute) to
 * 255 (full scale).  Values above 255 are silently clamped to 255.
 * Left and right channels are set independently; use sysop_audio_set_volume()
 * to set both at once. */
void sysop_audio_set_volume_left(uint32_t volume)
{
    if (volume > 255)
        volume = 255;

    sysop_audio_write_command(AUDIO_CMD_SET_VOLUME_LEFT, volume);
}

void sysop_audio_set_volume_right(uint32_t volume)
{
    if (volume > 255)
        volume = 255;

    sysop_audio_write_command(AUDIO_CMD_SET_VOLUME_RIGHT, volume);
}

void sysop_audio_set_volume(uint32_t left, uint32_t right)
{
    sysop_audio_set_volume_left(left);
    sysop_audio_set_volume_right(right);
}

/* Set the volume of the C64 SID chip audio mixed into the active channel.
 * This is independent of the PCM volume set by sysop_audio_set_volume().
 * Range 0 (mute) to 255 (full scale); values above 255 are clamped. */
void sysop_audio_set_sid_volume_left(uint32_t volume)
{
    if (volume > 255)
        volume = 255;

    sysop_audio_write_command(AUDIO_CMD_SET_SID_VOLUME_LEFT, volume);
}

void sysop_audio_set_sid_volume_right(uint32_t volume)
{
    if (volume > 255)
        volume = 255;

    sysop_audio_write_command(AUDIO_CMD_SET_SID_VOLUME_RIGHT, volume);
}

void sysop_audio_set_sid_volume(uint32_t left, uint32_t right)
{
    sysop_audio_set_sid_volume_left(left);
    sysop_audio_set_sid_volume_right(right);
}