/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 *
 * DESCRIPTION:
 *     Public C++ interface for SID file playback.
 *     For C programs, use sysop_sid.h instead.
 */

#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct SidRegisterWrite {
    uint16_t addr;
    uint8_t val;
};

// A single raster-timed $D418 write produced by the digi simulation.
// These are interleaved into video_buffer with CMD_WAIT + CMD_POKE pairs
// so the DMA stream delivers them at the correct cycle within each frame.
struct DigiWrite {
    uint16_t raster_line; // 0-311 (PAL) / 0-262 (NTSC)
    uint8_t  cycle;       // 1-63 (PAL) / 1-65 (NTSC)
    uint8_t  val;         // value to write
    uint16_t addr = 0xD418; // target address (default: $D418 volume DAC)
};

struct SidVoiceSamples {
    std::vector<float> voice[3]; // Pre-filter per-voice output, normalized [-1, 1]
    std::vector<float> combined; // Final mixed+filtered output, normalized [-1, 1]
};

// Returns the voice samples captured during the most recent play_frame() call.
const SidVoiceSamples& get_sid_frame_samples();
// Fills all voice sample vectors with zeros (flat line) without clearing them.
void clear_sid_frame_samples();

class SidPlayer {
public:
    // Construct with machine type so all timing constants are correct from the start.
    // PAL (default): 985248 Hz CPU, 312 lines, 63 cycles/line, 50.125 Hz frame rate.
    // NTSC:         1022727 Hz CPU, 263 lines, 65 cycles/line, 59.826 Hz frame rate.
    SidPlayer(bool is_pal = true);
    bool load(const std::string& path, int subtune = 0);
    bool load_from_memory(const std::vector<uint8_t>& data, int subtune = 0);
    // Two-phase load: call preload_metadata() synchronously to populate filedata
    // (making get_title/get_author immediately usable), then call init_from_preloaded()
    // on a background thread to run the slow cSID_init + do_init CPU emulation.
    bool preload_metadata(const std::vector<uint8_t>& data, int subtune);
    bool init_from_preloaded();
    // Like init_from_preloaded() but does NOT set is_playing.
    // Use from a background thread that holds no mutex during the slow init;
    // the caller sets is_playing=true under sid_mutex after this returns.
    bool init_from_preloaded_unlocked();
    void set_playing(bool playing) { is_playing = playing; }
    bool is_active() const { return is_playing; }
    bool get_is_pal() const { return is_pal_machine; }
    // out_digi: if non-null, receives raster-timed $D418 writes from the digi
    // simulation. The caller emits these as CMD_WAIT+CMD_POKE pairs into
    // video_buffer. Pass nullptr (default) to skip digi simulation entirely
    // (normal non-digi SID playback is unaffected either way).
    void play_frame(std::vector<SidRegisterWrite>& out_pokes,
                    std::vector<DigiWrite>* out_digi = nullptr);

    void set_duration_seconds(float seconds);
    void set_repeat(int count);
    void set_machine_type(bool is_pal);
    void set_volume(uint8_t volume); // 0-15
    // Enable or disable CIA2-driven digi sample simulation (default: enabled).
    // Disable if a tune produces incorrect audio due to false-positive detection.
    void set_digi_enabled(bool enabled);
    // Enable or disable automatic PAL/NTSC clock compensation (default: enabled).
    // When enabled, the player overrides the machine type to match the SID file's
    // required clock if they differ.  Disable to force the tune to play at the
    // current hardware frame rate regardless of the SID header's clock flag.
    void set_speed_compensation(bool enabled);
    void stop();

    std::string get_title() const;
    std::string get_author() const;
    float get_elapsed_seconds() const;
    float get_duration_seconds() const; // -1 if unknown

private:
    std::string current_path;
    int current_subtune = 0;

    bool is_playing = false;
    bool is_pal_machine = true;
    bool hw_is_pal      = true;   // actual hardware standard, never overridden by SID header
    float play_rate_accum = 1.0f; // fractional accumulator for PAL/NTSC rate adaptation
    bool speed_compensation_enabled = false; // When false, SID clock-mismatch override is skipped

    uint8_t master_volume = 15; // 0-15, default max

    int current_frame_pos = 0;
    int duration_frames = -1; // -1 = Infinite / Unknown
    int repeat_count = 0;     // 0 = Play once, -1 = Loop forever
    int initial_repeat_count = 0;

    void restart();
};
