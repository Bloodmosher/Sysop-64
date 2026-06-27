/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 *
 * DESCRIPTION:
 *     C-compatible wrapper around the SidPlayer C++ class.
 *     Safe to include from both C and C++ translation units.
 *
 *     C++ callers may use sid_player.h directly for the full class API.
 *     C callers (and mixed-language programs) should use this header.
 *
 * USAGE (C):
 *     SidPlayerHandle *p = sid_player_create(1);   // 1 = PAL
 *     sid_player_load(p, "tune.sid", 0);
 *     while (sid_player_is_playing(p)) {
 *         SidPoke pokes[64]; int n;
 *         sid_player_play_frame(p, pokes, 64, &n);
 *         for (int i = 0; i < n; i++) sysop_poke(pokes[i].addr, pokes[i].val);
 *         sysop_wait_vblank();
 *     }
 *     sid_player_destroy(p);
 */

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle; definition lives in sysop_sid.cpp. */
typedef struct SidPlayerHandle SidPlayerHandle;

/*
 * A single SID register write.
 * addr: $D400–$D41C (or $D418 for volume/filter mode register)
 * val:  byte value to write
 */
typedef struct {
    uint16_t addr;
    uint8_t  val;
} SidPoke;

/*
 * A raster-timed write produced by the CIA2 digi sample simulation.
 * Deliver via sysop_wait_vic2(raster_line, cycle) followed by sysop_poke(addr, val).
 * addr is usually $D418 (volume DAC nibble); $D020 border stripes may also
 * appear as a visual debug aid and can be ignored or applied as desired.
 */
typedef struct {
    uint16_t raster_line; /* 0–311 PAL / 0–262 NTSC */
    uint8_t  cycle;       /* 1–63 PAL / 1–65 NTSC   */
    uint8_t  val;
    uint16_t addr;
} SidDigiWrite;

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

/** Create a new player. is_pal: 1 = PAL (50 Hz), 0 = NTSC (60 Hz). */
SidPlayerHandle *sid_player_create(int is_pal);

/** Free all resources. Player must not be used after this call. */
void             sid_player_destroy(SidPlayerHandle *h);

/* ------------------------------------------------------------------
 * Loading
 * ------------------------------------------------------------------ */

/**
 * Load a .sid file from disk.
 * subtune: 0-based index (use 0 for the default tune).
 * Returns 1 on success, 0 on failure.
 */
int sid_player_load(SidPlayerHandle *h, const char *path, int subtune);

/**
 * Load a .sid file from a raw memory buffer.
 * Returns 1 on success, 0 on failure.
 */
int sid_player_load_from_memory(SidPlayerHandle *h,
                                const uint8_t *data, int len,
                                int subtune);

/* ------------------------------------------------------------------
 * Playback
 * ------------------------------------------------------------------ */

/**
 * Advance the emulator by one hardware frame and collect SID register writes.
 *
 * Call this once per VBlank (~50 Hz PAL, ~60 Hz NTSC).
 * pokes[0..n-1] are filled with register writes to apply to the real C64.
 * *num_pokes receives the actual count written (at most max_pokes).
 * Use max_pokes >= 64 to avoid dropping writes for typical tunes.
 */
void sid_player_play_frame(SidPlayerHandle *h,
                           SidPoke *pokes, int max_pokes, int *num_pokes);

/**
 * Like sid_player_play_frame() but also activates the CIA2 digi simulation.
 *
 * digis[0..nd-1] receives raster-timed writes sorted by (raster_line, cycle).
 * Deliver each one with sysop_wait_vic2(digis[i].raster_line, digis[i].cycle)
 * then sysop_poke(digis[i].addr, digis[i].val), in order, during the current frame.
 *
 * Use max_digis >= 512 (a busy digi tune fires ~300-600 NMIs per PAL frame).
 * When digi is active, $D418 writes are removed from the regular pokes stream
 * and appear only in digis[] at their correct raster positions.
 */
void sid_player_play_frame_ex(SidPlayerHandle *h,
                              SidPoke      *pokes, int max_pokes, int *num_pokes,
                              SidDigiWrite *digis, int max_digis, int *num_digis);

/** Stop playback and mute all SID voices. */
void sid_player_stop(SidPlayerHandle *h);

/** Returns 1 while the player is playing, 0 when stopped or finished. */
int  sid_player_is_playing(SidPlayerHandle *h);

/** Returns 1 if the player is configured for PAL, 0 for NTSC. */
int  sid_player_is_pal(SidPlayerHandle *h);

/* ------------------------------------------------------------------
 * Metadata
 * ------------------------------------------------------------------ */

/**
 * Copy the SID title/author string into buf (NUL-terminated).
 * buf_size should be >= 33 to hold the full 32-char field + NUL.
 */
void sid_player_get_title(SidPlayerHandle *h, char *buf, int buf_size);
void sid_player_get_author(SidPlayerHandle *h, char *buf, int buf_size);

/* ------------------------------------------------------------------
 * Configuration  (may be called before or after load)
 * ------------------------------------------------------------------ */

/** Set playback duration in seconds. Pass -1 (or 0) for infinite. */
void  sid_player_set_duration_seconds(SidPlayerHandle *h, float seconds);

/** Set repeat count. -1 = loop forever, 0 = play once, N = play N+1 times. */
void  sid_player_set_repeat(SidPlayerHandle *h, int count);

/** Override machine type. is_pal: 1 = PAL, 0 = NTSC. */
void  sid_player_set_machine_type(SidPlayerHandle *h, int is_pal);

/** Set master volume (0–15). Default is 15 (maximum). */
void  sid_player_set_volume(SidPlayerHandle *h, uint8_t volume);

/** Enable/disable CIA2-driven digi sample simulation. Default: enabled. */
void  sid_player_set_digi_enabled(SidPlayerHandle *h, int enabled);

/* ------------------------------------------------------------------
 * Status
 * ------------------------------------------------------------------ */

/** Seconds elapsed since the last load/restart. */
float sid_player_get_elapsed_seconds(SidPlayerHandle *h);

/** Total duration in seconds, or -1 if unknown/infinite. */
float sid_player_get_duration_seconds(SidPlayerHandle *h);

#ifdef __cplusplus
} /* extern "C" */
#endif
