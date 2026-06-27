/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 *
 * DESCRIPTION:
 *     C-compatible wrapper implementation for SidPlayer.
 *
 *     This file bridges the C++ SidPlayer class to the C-friendly
 *     sysop_sid.h API using the opaque-handle pattern:
 *       - SidPlayerHandle is forward-declared in sysop_sid.h (opaque to C)
 *       - Defined here as a thin struct owning a SidPlayer instance
 *       - All extern "C" functions delegate straight to the C++ class
 */

#include "sysop_sid.h"
#include "sid_player.h"

#include <algorithm>
#include <cstring>
#include <vector>

/* Full definition of the opaque handle; C code only sees the forward decl. */
struct SidPlayerHandle {
    SidPlayer player;
    explicit SidPlayerHandle(bool pal) : player(pal) {}
};

extern "C" {

/* ---- Lifecycle ---- */

SidPlayerHandle *sid_player_create(int is_pal)
{
    return new SidPlayerHandle(is_pal != 0);
}

void sid_player_destroy(SidPlayerHandle *h)
{
    delete h;
}

/* ---- Loading ---- */

int sid_player_load(SidPlayerHandle *h, const char *path, int subtune)
{
    return h->player.load(path, subtune) ? 1 : 0;
}

int sid_player_load_from_memory(SidPlayerHandle *h,
                                const uint8_t *data, int len,
                                int subtune)
{
    std::vector<uint8_t> buf(data, data + len);
    return h->player.load_from_memory(buf, subtune) ? 1 : 0;
}

/* ---- Playback ---- */

void sid_player_play_frame(SidPlayerHandle *h,
                           SidPoke *pokes, int max_pokes, int *num_pokes)
{
    std::vector<SidRegisterWrite> out;
    h->player.play_frame(out);

    int n = (int)std::min((int)out.size(), max_pokes);
    for (int i = 0; i < n; i++) {
        pokes[i].addr = out[i].addr;
        pokes[i].val  = out[i].val;
    }
    if (num_pokes)
        *num_pokes = n;
}

void sid_player_play_frame_ex(SidPlayerHandle *h,
                              SidPoke      *pokes, int max_pokes, int *num_pokes,
                              SidDigiWrite *digis, int max_digis, int *num_digis)
{
    std::vector<SidRegisterWrite> out;
    std::vector<DigiWrite>        digi_out;
    h->player.play_frame(out, &digi_out);

    int n = (int)std::min((int)out.size(), max_pokes);
    for (int i = 0; i < n; i++) {
        pokes[i].addr = out[i].addr;
        pokes[i].val  = out[i].val;
    }
    if (num_pokes)
        *num_pokes = n;

    int nd = (int)std::min((int)digi_out.size(), max_digis);
    for (int i = 0; i < nd; i++) {
        digis[i].raster_line = digi_out[i].raster_line;
        digis[i].cycle       = digi_out[i].cycle;
        digis[i].val         = digi_out[i].val;
        digis[i].addr        = digi_out[i].addr;
    }
    if (num_digis)
        *num_digis = nd;
}

void sid_player_stop(SidPlayerHandle *h)
{
    h->player.stop();
}

int sid_player_is_playing(SidPlayerHandle *h)
{
    return h->player.is_active() ? 1 : 0;
}

int sid_player_is_pal(SidPlayerHandle *h)
{
    return h->player.get_is_pal() ? 1 : 0;
}

/* ---- Metadata ---- */

void sid_player_get_title(SidPlayerHandle *h, char *buf, int buf_size)
{
    if (!buf || buf_size <= 0) return;
    std::string s = h->player.get_title();
    strncpy(buf, s.c_str(), (size_t)(buf_size - 1));
    buf[buf_size - 1] = '\0';
}

void sid_player_get_author(SidPlayerHandle *h, char *buf, int buf_size)
{
    if (!buf || buf_size <= 0) return;
    std::string s = h->player.get_author();
    strncpy(buf, s.c_str(), (size_t)(buf_size - 1));
    buf[buf_size - 1] = '\0';
}

/* ---- Configuration ---- */

void sid_player_set_duration_seconds(SidPlayerHandle *h, float seconds)
{
    h->player.set_duration_seconds(seconds);
}

void sid_player_set_repeat(SidPlayerHandle *h, int count)
{
    h->player.set_repeat(count);
}

void sid_player_set_machine_type(SidPlayerHandle *h, int is_pal)
{
    h->player.set_machine_type(is_pal != 0);
}

void sid_player_set_volume(SidPlayerHandle *h, uint8_t volume)
{
    h->player.set_volume(volume);
}

void sid_player_set_digi_enabled(SidPlayerHandle *h, int enabled)
{
    h->player.set_digi_enabled(enabled != 0);
}

/* ---- Status ---- */

float sid_player_get_elapsed_seconds(SidPlayerHandle *h)
{
    return h->player.get_elapsed_seconds();
}

float sid_player_get_duration_seconds(SidPlayerHandle *h)
{
    return h->player.get_duration_seconds();
}

} /* extern "C" */
