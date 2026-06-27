/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * playsound — play a WAV file through the FPGA audio engine.
 *
 * Architecture
 * ------------
 * The FPGA contains a hardware audio DMA engine that streams PCM
 * samples from a region of DDR SDRAM directly to I2S/HDMI audio.
 * This tool:
 *   1. Parses the WAV file header and extracts raw PCM bytes.
 *   2. Converts/resamples the audio to a format the FPGA understands
 *      (see the three code paths in sysop_audio_play_wav_file below).
 *   3. Writes the samples into DDR SDRAM via /dev/mem mmap.
 *   4. Calls the sysop library to tell the FPGA engine where the data
 *      is, how many frames to play, and to start playback.
 *   5. Polls until playback finishes, then returns.
 *
 * DDR memory layout
 * -----------------
 * Base: SYSOP64_AUDIO_PCM_ADDR (0x28000000)
 * Each logical audio channel occupies SYSOP64_AUDIO_CHANNEL_SIZE (1 MB):
 *   channel 0: 0x28000000 – 0x280FFFFF
 *   channel 1: 0x28100000 – 0x281FFFFF
 *   ...
 *
 * FPGA audio formats
 * ------------------
 * Two hardware formats are supported (see SYSOP64_AUDIO_FORMAT_* below):
 *
 *   PCM16_STEREO_48K  — 16-bit signed stereo interleaved at 48 kHz.
 *     DDR layout per frame (4 bytes): [right_hi][right_lo][left_hi][left_lo]
 *     Stored as uint32_t: (right << 16) | left.
 *     Length passed to the engine = number of stereo frames.
 *
 *   U8_MONO_VAR — 8-bit unsigned mono at a variable sample rate.
 *     Raw bytes written as-is; the FPGA interpolates internally.
 *     A "phase step" register controls the effective playback rate.
 *     Length passed to the engine = number of bytes.
 *
 * WAV conversion paths (chosen automatically from the input file):
 *   1. 8-bit mono any-rate   → native U8_MONO_VAR (no conversion).
 *   2. 16-bit stereo 48 kHz  → direct copy, no conversion.
 *   3. Everything else       → resample to 16-bit stereo 48 kHz using
 *                              linear interpolation, then write as PCM16.
 */

#include <error.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include "sysop64.h"

// assume 100MB for now
#define MEM_SIZE (1024*1024*100)

/* Base DDR address of the audio PCM buffer region (physical). */
#define SYSOP64_AUDIO_PCM_ADDR   0x28000000u

/* Native sample rate of the FPGA audio engine (Hz).
 * All PCM16_STEREO_48K audio must be at this rate. */
#define SYSOP64_AUDIO_RATE       48000

/* Bytes per output frame in PCM16_STEREO_48K mode:
 * 2 channels × 2 bytes = 4 bytes. */
#define SYSOP64_AUDIO_FRAME_SIZE 4

/* DDR bytes reserved per logical audio channel (1 MB).
 * Channel N starts at SYSOP64_AUDIO_PCM_ADDR + N * SYSOP64_AUDIO_CHANNEL_SIZE. */
#define SYSOP64_AUDIO_CHANNEL_SIZE (1024*1024)


/* Little-endian byte readers for WAV header fields.
 * WAV files are always little-endian regardless of host byte order. */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Linear interpolation between two int16 samples.
 * frac is a 16-bit fractional position (0 = all a, 0xFFFF = almost all b).
 * Used when resampling from the WAV sample rate to 48 kHz. */
static int16_t lerp16(int16_t a, int16_t b, uint32_t frac)
{
    // frac is 16.16 fraction, using lower 16 bits
    int32_t diff = (int32_t)b - (int32_t)a;
    return (int16_t)((int32_t)a + ((diff * (int32_t)frac) >> 16));
}

/* FPGA audio format codes passed to sysop_audio_set_sample_format().
 *
 * PCM16_STEREO_48K: interleaved signed 16-bit stereo at 48 kHz.
 *   sysop_audio_play_pcm() length = number of stereo frames.
 *   Phase step must be 65536 (= 2^16 = no rate adjustment).
 *
 * U8_MONO_VAR: unsigned 8-bit mono at a variable sample rate.
 *   sysop_audio_play_pcm() length = number of bytes.
 *   Phase step = sysop_audio_phase_step_from_rate(sample_rate); the
 *   FPGA uses this to interpolate the source rate to its internal clock. */
#define SYSOP64_AUDIO_FORMAT_PCM16_STEREO_48K 0
#define SYSOP64_AUDIO_FORMAT_U8_MONO_VAR      1

/* Round n up to the next multiple of the system page size.
 * Required because mmap() maps whole pages; mapping a smaller region
 * than a full page causes SIGBUS on some kernels. */
static size_t page_align(size_t n)
{
    long page = sysconf(_SC_PAGESIZE);
    return (n + page - 1) & ~(size_t)(page - 1);
}

/* Fetch one sample from a WAV PCM buffer and return it as int16.
 *
 * Supports 8-bit and 16-bit depths and arbitrary channel counts.
 *
 * 8-bit WAV samples are unsigned (0..255, silence = 128).
 * They are converted to signed int16 by centering and scaling:
 *   signed = (sample - 128) << 8
 * This maps 0 → -32768, 128 → 0, 255 → +32512.
 *
 * 16-bit WAV samples are already signed int16 (little-endian); they
 * are read directly from the raw byte buffer cast to int16_t *. */
static int16_t sample_to_s16(
    const uint8_t *pcm,
    uint32_t frame_index,
    uint16_t channels,
    uint16_t bits_per_sample,
    uint16_t channel)
{
    if (bits_per_sample == 8) {
        uint8_t s = pcm[frame_index * channels + channel];
        return (int16_t)(((int)s - 128) << 8);
    }

    const int16_t *s16 = (const int16_t *)pcm;
    return s16[frame_index * channels + channel];
}

/* sysop_audio_play_wav_file() — load a WAV file and play it on a
 * given FPGA audio channel.
 *
 * Parses the RIFF/WAV header (fmt + data chunks), then selects one
 * of the three code paths described in the file header above.
 *
 * Parameters:
 *   path    — filesystem path to the .wav file
 *   channel — logical audio channel index (0, 1, …)
 *   loop    — if true, the FPGA engine loops the sample indefinitely;
 *              note: this function still returns after the first play
 *              because it polls sysop_audio_is_playing() which will
 *              remain true.  Intended for background use when loop=true.
 *
 * Returns 0 on success, -1 on any error. */
int sysop_audio_play_wav_file(const char *path, int channel, bool loop)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen wav");
        return -1;
    }

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "Not a RIFF/WAVE file\n");
        fclose(f);
        return -1;
    }

    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;

    uint8_t *pcm_data = NULL;
    uint32_t pcm_bytes = 0;

    /* Parse RIFF/WAVE chunks.  WAV files can contain extra chunks
     * (e.g. LIST, cue, smpl) that we skip; only 'fmt ' and 'data'
     * are needed.  Pad bytes after odd-length chunks are consumed
     * so the next chunk header is always word-aligned. */
    while (!feof(f)) {
        uint8_t chunk[8];

        if (fread(chunk, 1, 8, f) != 8)
            break;

        uint32_t chunk_size = rd32(chunk + 4);

        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t *fmt = malloc(chunk_size);
            if (!fmt) {
                fclose(f);
                return -1;
            }

            if (fread(fmt, 1, chunk_size, f) != chunk_size) {
                free(fmt);
                fclose(f);
                return -1;
            }

            audio_format    = rd16(fmt + 0);
            channels        = rd16(fmt + 2);
            sample_rate     = rd32(fmt + 4);
            bits_per_sample = rd16(fmt + 14);

            free(fmt);
        } else if (memcmp(chunk, "data", 4) == 0) {
            pcm_data = malloc(chunk_size);
            if (!pcm_data) {
                fclose(f);
                return -1;
            }

            if (fread(pcm_data, 1, chunk_size, f) != chunk_size) {
                free(pcm_data);
                fclose(f);
                return -1;
            }

            pcm_bytes = chunk_size;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }

        if (chunk_size & 1)
            fseek(f, 1, SEEK_CUR);
    }

    fclose(f);

    if (!pcm_data) {
        fprintf(stderr, "No WAV data chunk found\n");
        return -1;
    }

    if (audio_format != 1) {
        fprintf(stderr, "Unsupported WAV format: only PCM supported\n");
        free(pcm_data);
        return -1;
    }

    if (bits_per_sample != 8 && bits_per_sample != 16) {
        fprintf(stderr,
                "Unsupported WAV depth %u: only 8-bit and 16-bit PCM supported\n",
                bits_per_sample);
        free(pcm_data);
        return -1;
    }

    if (channels != 1 && channels != 2) {
        fprintf(stderr, "Unsupported WAV channels: only mono/stereo supported\n");
        free(pcm_data);
        return -1;
    }

    uint32_t src_frame_size = channels * (bits_per_sample / 8);
    uint32_t src_frames = pcm_bytes / src_frame_size;

    /* native_u8_mono: 8-bit mono at any sample rate.
     * Copy raw bytes straight to DDR and let the FPGA's
     * variable-rate engine handle interpolation internally.
     * This avoids the resample loop and saves memory. */
    bool native_u8_mono =
        bits_per_sample == 8 &&
        channels == 1;

    /* direct_pcm16_stereo_48k: source already matches the FPGA
     * native format exactly — memcpy to DDR, no conversion. */
    bool direct_pcm16_stereo_48k =
        bits_per_sample == 16 &&
        channels == 2 &&
        sample_rate == SYSOP64_AUDIO_RATE;

    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) {
        perror("open /dev/mem");
        free(pcm_data);
        return -1;
    }

    uint32_t base_addr = SYSOP64_AUDIO_PCM_ADDR + (channel * SYSOP64_AUDIO_CHANNEL_SIZE);
    printf("channel=%u base=0x%08X align32=%u\n", channel, base_addr, base_addr & 31);

    /* --- Code path 1: native U8 mono ---
     * Write raw bytes to DDR, configure U8_MONO_VAR format, set the
     * phase step so the FPGA plays back at the original sample rate,
     * then kick off playback and wait for it to finish. */
    if (native_u8_mono) {
        size_t map_size = page_align(pcm_bytes);

        void *dst_map = mmap(
            NULL,
            map_size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            memfd,
            base_addr);

        if (dst_map == MAP_FAILED) {
            perror("mmap pcm");
            close(memfd);
            free(pcm_data);
            return -1;
        }

        /* Memory barrier: ensure all DDR writes are visible to the
         * FPGA DMA engine before the play command is issued. */
        memcpy(dst_map, pcm_data, pcm_bytes);
        __sync_synchronize();

        sysop_audio_select_channel(channel);
        sysop_audio_select_status_channel(channel);
        sysop_audio_stop();
        usleep(1000);  // brief pause to let the engine idle
        sysop_audio_set_sample_format(SYSOP64_AUDIO_FORMAT_U8_MONO_VAR);
        sysop_audio_set_phase_step(
            sysop_audio_phase_step_from_rate(sample_rate));

        /* Length = byte count for U8_MONO_VAR mode. */
        sysop_audio_play_pcm(
            base_addr,
            pcm_bytes,   // length in bytes for native u8 mode
            loop);

        printf("Started native U8 mono WAV: %u bytes, %.3f seconds, rate=%u, phase_step=%u\n",
               pcm_bytes,
               (double)src_frames / (double)sample_rate,
               sample_rate,
               sysop_audio_phase_step_from_rate(sample_rate));

        while (sysop_audio_is_playing()) {
            //sysop_audio_print_status();
            usleep(100000);
        }
        sysop_audio_print_status();

        munmap(dst_map, map_size);
        close(memfd);
        free(pcm_data);
        return 0;
    }

    /* --- Code path 2 & 3: PCM16 stereo output ---
     * Both direct-copy and resampled paths land here.
     * Calculate the number of output frames. */
    uint32_t dst_frames;

    if (direct_pcm16_stereo_48k) {
        /* Same frame count as source: no sample rate conversion needed. */
        dst_frames = src_frames;
    } else {
        /* Scale the frame count by the ratio of rates, rounding down.
         * Integer arithmetic is safe here; precision loss is < 1 frame. */
        dst_frames =
            (uint32_t)(((uint64_t)src_frames * SYSOP64_AUDIO_RATE) / sample_rate);
    }

    uint32_t dst_bytes = dst_frames * SYSOP64_AUDIO_FRAME_SIZE;
    size_t map_size = page_align(dst_bytes);

    void *dst_map = mmap(
        NULL,
        map_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        memfd,
        base_addr);

    if (dst_map == MAP_FAILED) {
        perror("mmap pcm");
        close(memfd);
        free(pcm_data);
        return -1;
    }

    uint32_t *dst = (uint32_t *)dst_map;

    /* --- Code path 2: direct copy (source is already 16-bit stereo 48k) --- */
    if (direct_pcm16_stereo_48k) {
        memcpy(dst, pcm_data, dst_bytes);
    } else {
        /* --- Code path 3: resample to 16-bit stereo 48 kHz ---
         * For each output frame i, compute the corresponding fractional
         * position in the source buffer using 16.16 fixed-point:
         *   src_pos = i * src_rate / dst_rate
         * Linearly interpolate between the two surrounding source
         * frames to reduce aliasing.
         *
         * Mono sources are duplicated to both L and R channels. */
        for (uint32_t i = 0; i < dst_frames; i++) {
            /* src_pos_fp: source position as 16.16 fixed-point.
             * Upper 16 bits = integer sample index,
             * lower 16 bits = fractional part for lerp. */
            uint64_t src_pos_fp =
                ((uint64_t)i * sample_rate << 16) / SYSOP64_AUDIO_RATE;

            uint32_t src_index = src_pos_fp >> 16;
            uint32_t frac = src_pos_fp & 0xffff;

            if (src_index >= src_frames)
                src_index = src_frames - 1;

            uint32_t src_next = src_index + 1;
            if (src_next >= src_frames)
                src_next = src_index;

            int16_t l0, r0, l1, r1;

            if (channels == 1) {
                l0 = r0 = sample_to_s16(
                    pcm_data, src_index, channels, bits_per_sample, 0);

                l1 = r1 = sample_to_s16(
                    pcm_data, src_next, channels, bits_per_sample, 0);
            } else {
                l0 = sample_to_s16(
                    pcm_data, src_index, channels, bits_per_sample, 0);
                r0 = sample_to_s16(
                    pcm_data, src_index, channels, bits_per_sample, 1);

                l1 = sample_to_s16(
                    pcm_data, src_next, channels, bits_per_sample, 0);
                r1 = sample_to_s16(
                    pcm_data, src_next, channels, bits_per_sample, 1);
            }

            int16_t left  = lerp16(l0, l1, frac);
            int16_t right = lerp16(r0, r1, frac);

            /* Pack the interpolated stereo frame into one uint32.
             * FPGA expects: bits[15:0]  = left  channel (int16)
             *               bits[31:16] = right channel (int16) */
            dst[i] =
                ((uint32_t)(uint16_t)right << 16) |
                ((uint32_t)(uint16_t)left);
        }
    }

    /* Memory barrier: all DDR writes must complete before the
     * FPGA DMA engine sees the play command. */
    __sync_synchronize();
    sysop_audio_select_channel(channel);
    sysop_audio_select_status_channel(channel);

    sysop_audio_stop();
    usleep(1000);  // brief pause to let the engine idle

    sysop_audio_set_sample_format(SYSOP64_AUDIO_FORMAT_PCM16_STEREO_48K);
    /* Phase step 65536 = 2^16 = 1.0: play at exactly the native rate. */
    sysop_audio_set_phase_step(65536);

    /* Length = frame count for PCM16_STEREO_48K mode. */
    sysop_audio_play_pcm(
        base_addr,
        dst_frames,
        loop);

    printf("Started PCM16 stereo 48k WAV: %u frames, %.3f seconds\n",
           dst_frames,
           (double)dst_frames / 48000.0);

    while (sysop_audio_is_playing()) {
        //sysop_audio_print_status();
        usleep(100000);
    }
    //sysop_audio_print_status();

    munmap(dst_map, map_size);
    close(memfd);
    free(pcm_data);

    return 0;
}

int main(int argc, char **argv) 
{
    if (argc < 2) { 
        printf("Expected argument: <path to .wav file> [channel]\n");
        return 1;
    }

    sysop_init();
    int channel = 0;
    if (argc > 2) { 
        channel = atoi(argv[2]);
    }
    
    printf("Playing %s in audio channel %d\n", argv[1], channel);

    sysop_audio_play_wav_file(argv[1], channel, false);
    sysop_uninit();
    return 0;
}
