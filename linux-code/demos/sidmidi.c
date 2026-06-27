/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * sidmidi.c — Virtual NameSoft MIDI keyboard for SID-Wizard on the C64.
 *
 * Overview
 * --------
 * This program turns the host computer into a virtual NameSoft MIDI
 * interface connected to a real C64 via the Sysop-64 DMA bridge.  When
 * SID-Wizard (or any software that supports the NameSoft MIDI interface)
 * is running on the C64, this program can feed it MIDI note data from:
 *
 *   1. The host keyboard — a piano-style key layout is provided.
 *   2. An attached ALSA MIDI device (e.g. a USB MIDI keyboard or
 *      synthesiser) — pass the device name with -d.
 *   3. A previously recorded .mrec file played back with -p.
 *
 * NameSoft MIDI hardware interface
 * ---------------------------------
 * The NameSoft interface emulates a 6551 ACIA (Asynchronous Communications
 * Interface Adapter) mapped at $DE00–$DE03 in the C64's I/O2 expansion
 * space.  The C64 software expects the following register layout:
 *
 *   $DE00  Control   (write) — initialise the ACIA
 *   $DE01  TX data   (write) — host → C64 transmit data (not used here)
 *   $DE02  Status    (read)  — bit 0: RX data full, bit 7: IRQ pending
 *   $DE03  RX data   (read)  — incoming MIDI byte the C64 reads
 *
 * Each incoming MIDI byte triggers an NMI interrupt on the C64 so that
 * SID-Wizard's MIDI handler can read it immediately.  The Sysop-64
 * library handles the NMI assertion; callers only need to use
 * sysop_namesoft_midi_ready() / sysop_namesoft_midi_write().
 *
 * MIDI message byte layout (3-byte note messages)
 * ------------------------------------------------
 *   byte[0]  Status: 0x90 = Note On  ch.1, 0x80 = Note Off ch.1
 *   byte[1]  Note number: 0–127  (middle C = 60)
 *   byte[2]  Velocity:    0–127  (0 on Note Off)
 *
 * .mrec recording format
 * ----------------------
 * A simple proprietary binary format for capturing and replaying sessions:
 *   Header: "MREC" (4 bytes) + version (1 byte, currently 1)
 *   Each event: delta_time_ms (uint32_t LE) + length (uint8_t) + data bytes
 *
 * Usage
 * -----
 *   sidmidi [-d alsa_device] [-r record_file] [-p playback_file [count]]
 *
 *   -d <device>          Connect to named ALSA MIDI input device.
 *   -r <file>            Record all MIDI events to <file>.
 *   -p <file> [count]    Play back <file> <count> times (default 1).
 *
 * Keyboard layout
 * ---------------
 *   Upper row:  Q W E R T Y U I O P   (white keys)
 *               2 3   5 6 7   9 0     (black keys)
 *
 *   Lower row:  Z X C V B N M , . /   (white keys, one octave)
 *               S D   G H J   L ;     (black keys)
 *
 *   ESC        quit
 *   SPACE      all notes off
 *   + / -      octave up / down
 *   [ / ]      velocity down / up
 *   0–9        program change
 *   H          show help
 *   R          start/stop recording (requires -r)
 *   P          start/stop playback  (requires -r or -p)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/time.h>
#include <alsa/asoundlib.h>
#include "sysop64.h"

/* ── NameSoft MIDI ACIA register addresses ($DE00–$DE03) ─────────────── */

#define NAMESOFT_CONTROL   0xDE00  /* ACIA control register  (write) */
#define NAMESOFT_TX        0xDE01  /* ACIA TX data           (write) */
#define NAMESOFT_STATUS    0xDE02  /* ACIA status register   (read)  */
#define NAMESOFT_RX        0xDE03  /* ACIA RX data           (read)  */

/* ACIA status register bit masks */
#define ACIA_IRQ_REQUEST   0x80    /* bit 7 — interrupt request pending */
#define ACIA_RX_DATA_FULL  0x01    /* bit 0 — receive register has data */

/* ── MIDI protocol constants ─────────────────────────────────────────── */

#define MIDI_NOTE_ON        0x90   /* Note On, channel 1  */
#define MIDI_NOTE_OFF       0x80   /* Note Off, channel 1 */
#define MIDI_PROGRAM_CHANGE 0xC0   /* Program Change, channel 1 */
#define MIDI_PITCH_BEND     0xE0   /* Pitch Bend, channel 1 */
#define MIDI_DEFAULT_VELOCITY 0x7F /* Default note velocity (maximum) */

/* Sysop-64 server command codes for the NameSoft MIDI feature */
#define SYSOP_ENABLE_NAMESOFT_MIDI  37
#define SYSOP_DISABLE_NAMESOFT_MIDI 38

/* ── MIDI recorder ───────────────────────────────────────────────────── */

/*
 * MidiEvent — a single timestamped MIDI message in a .mrec session.
 * delta_time_ms is the number of milliseconds since the previous event.
 */
typedef struct {
    uint32_t delta_time_ms;
    uint8_t  data[3];
    uint8_t  length;
} MidiEvent;

/*
 * MidiRecorder — state for recording and/or playing back a MIDI session.
 *
 * Recording and playback are mutually exclusive; the caller is responsible
 * for not activating both simultaneously.  During playback the events[]
 * array is owned by the recorder and freed by recorder_cleanup().
 */
typedef struct {
    FILE       *file;
    int         recording;
    int         playing;
    uint64_t    start_time_ms;
    uint64_t    last_event_time_ms;
    MidiEvent  *events;
    int         event_count;
    int         event_capacity;
    int         playback_index;
    int         playback_count;    /* 0 = infinite; >0 = play exactly N times */
    int         current_playback;  /* current iteration (1-based) */
} MidiRecorder;

/* Return the current wall-clock time in milliseconds. */
static uint64_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static void sleep_ms(int ms)
{
    usleep(ms * 1000);
}

void recorder_init(MidiRecorder *rec)
{
    memset(rec, 0, sizeof(*rec));
}

/*
 * recorder_start_recording — open a .mrec file for writing and begin
 * capturing all MIDI events sent through send_midi_message().
 */
int recorder_start_recording(MidiRecorder *rec, const char *filename)
{
    rec->file = fopen(filename, "wb");
    if (!rec->file) {
        fprintf(stderr, "recorder: could not open '%s' for writing\n", filename);
        return -1;
    }

    /* Write the fixed 5-byte header: magic + version. */
    fwrite("MREC", 1, 4, rec->file);
    uint8_t version = 1;
    fwrite(&version, 1, 1, rec->file);

    /* Discard any previously loaded playback events so we start fresh. */
    free(rec->events);
    rec->events        = NULL;
    rec->event_count   = 0;
    rec->event_capacity = 0;

    rec->recording          = 1;
    rec->start_time_ms      = get_time_ms();
    rec->last_event_time_ms = rec->start_time_ms;

    printf("Recording started → %s\n", filename);
    return 0;
}

/*
 * recorder_record_event — append one MIDI event with its delta timestamp
 * to the open recording file.  Silently ignored if not recording.
 */
void recorder_record_event(MidiRecorder *rec, const uint8_t *data, int length)
{
    if (!rec->recording || !rec->file) return;

    uint64_t now   = get_time_ms();
    uint32_t delta = (uint32_t)(now - rec->last_event_time_ms);
    rec->last_event_time_ms = now;

    fwrite(&delta, sizeof(uint32_t), 1, rec->file);
    fwrite(&length, 1, 1, rec->file);
    fwrite(data, 1, length, rec->file);
    fflush(rec->file);
}

void recorder_stop_recording(MidiRecorder *rec)
{
    if (rec->recording && rec->file) {
        fclose(rec->file);
        rec->file      = NULL;
        rec->recording = 0;
        printf("Recording stopped.\n");
    }
}

/*
 * recorder_load_file — read a .mrec file into memory ready for playback.
 * The events array is allocated here and freed by recorder_cleanup().
 */
int recorder_load_file(MidiRecorder *rec, const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "recorder: could not open '%s' for playback\n", filename);
        return -1;
    }

    char magic[4];
    uint8_t version;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "MREC", 4) != 0) {
        fprintf(stderr, "recorder: '%s' is not a valid .mrec file\n", filename);
        fclose(f);
        return -1;
    }
    if (fread(&version, 1, 1, f) != 1 || version != 1) {
        fprintf(stderr, "recorder: unsupported .mrec version in '%s'\n", filename);
        fclose(f);
        return -1;
    }

    free(rec->events);
    rec->event_capacity = 1000;
    rec->events         = malloc(rec->event_capacity * sizeof(MidiEvent));
    rec->event_count    = 0;

    while (!feof(f)) {
        uint32_t delta;
        uint8_t  length;

        if (fread(&delta,  sizeof(uint32_t), 1, f) != 1) break;
        if (fread(&length, 1,                1, f) != 1) break;
        if (length > 3) { fprintf(stderr, "recorder: bad event length\n"); break; }

        if (rec->event_count >= rec->event_capacity) {
            rec->event_capacity *= 2;
            rec->events = realloc(rec->events, rec->event_capacity * sizeof(MidiEvent));
        }

        MidiEvent *evt = &rec->events[rec->event_count];
        evt->delta_time_ms = delta;
        evt->length        = length;
        if (fread(evt->data, 1, length, f) != length) break;
        rec->event_count++;
    }

    fclose(f);
    printf("Loaded %d MIDI events from '%s'\n", rec->event_count, filename);
    return 0;
}

void recorder_start_playback(MidiRecorder *rec)
{
    if (rec->event_count == 0) return;

    rec->playing        = 1;
    rec->playback_index = 0;
    rec->start_time_ms      = get_time_ms();
    rec->last_event_time_ms = rec->start_time_ms;

    if (rec->current_playback == 0)
        rec->current_playback = 1;

    if (rec->playback_count > 0)
        printf("Playback started (%d events) — iteration %d/%d\n",
               rec->event_count, rec->current_playback, rec->playback_count);
    else
        printf("Playback started (%d events)\n", rec->event_count);
}

/*
 * recorder_update_playback — call once per main loop iteration.
 *
 * Returns:
 *   1   — an event is ready; *data and *length are set to the event bytes.
 *   0   — no event ready yet (delta time has not elapsed).
 *  -1   — all iterations complete; playback is now stopped.
 */
int recorder_update_playback(MidiRecorder *rec, uint8_t **data, int *length)
{
    if (!rec->playing) return 0;

    if (rec->playback_index >= rec->event_count) {
        /* End of one iteration. */
        if (rec->playback_count > 0 && rec->current_playback < rec->playback_count) {
            /* More iterations remain — restart. */
            rec->current_playback++;
            rec->playback_index     = 0;
            rec->last_event_time_ms = get_time_ms();
            printf("Starting playback iteration %d/%d\n",
                   rec->current_playback, rec->playback_count);
        } else {
            rec->playing = 0;
            if (rec->playback_count > 0)
                printf("Playback complete (%d iterations).\n", rec->playback_count);
            else
                printf("Playback finished.\n");
            return -1;
        }
    }

    uint64_t   elapsed = get_time_ms() - rec->last_event_time_ms;
    MidiEvent *evt     = &rec->events[rec->playback_index];

    if (elapsed >= evt->delta_time_ms) {
        *data   = evt->data;
        *length = evt->length;
        rec->last_event_time_ms = get_time_ms();
        rec->playback_index++;
        return 1;
    }

    return 0;
}

void recorder_stop_playback(MidiRecorder *rec)
{
    if (rec->playing) {
        rec->playing = 0;
        printf("Playback stopped.\n");
    }
}

void recorder_cleanup(MidiRecorder *rec)
{
    recorder_stop_recording(rec);
    free(rec->events);
    rec->events = NULL;
}

/* ── ALSA MIDI input ─────────────────────────────────────────────────── */

/*
 * connect_to_device — subscribe our ALSA sequencer port to the first port
 * of the ALSA client whose name contains device_name (case-sensitive
 * substring match).  Does nothing if no matching client is found.
 */
static void connect_to_device(snd_seq_t *seq, int my_port, const char *device_name)
{
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t   *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);

    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        if (!strstr(snd_seq_client_info_get_name(cinfo), device_name))
            continue;

        int client_id = snd_seq_client_info_get_client(cinfo);
        snd_seq_port_info_set_client(pinfo, client_id);
        snd_seq_port_info_set_port(pinfo, -1);

        if (snd_seq_query_next_port(seq, pinfo) < 0)
            continue;

        int port_id = snd_seq_port_info_get_port(pinfo);

        snd_seq_port_subscribe_t *subs;
        snd_seq_port_subscribe_alloca(&subs);

        snd_seq_addr_t sender = { client_id, port_id };
        snd_seq_addr_t dest   = { snd_seq_client_id(seq), my_port };

        snd_seq_port_subscribe_set_sender(subs, &sender);
        snd_seq_port_subscribe_set_dest(subs, &dest);

        if (snd_seq_subscribe_port(seq, subs) == 0) {
            printf("ALSA: connected to '%s' (%d:%d)\n",
                   snd_seq_client_info_get_name(cinfo), client_id, port_id);
            return;
        }
    }
    printf("ALSA: no device matching '%s' found\n", device_name);
}

/* ── Terminal raw-mode keyboard input ────────────────────────────────── */

/*
 * The main loop needs non-blocking, unbuffered key input so notes can be
 * triggered without pressing Enter.  We switch the terminal to raw mode
 * for the duration of the program and restore it on exit.
 *
 * Limitation: Linux terminal input cannot detect key-release events, so
 * notes are given a fixed 200 ms duration and then automatically turned
 * off.  A real MIDI keyboard connected via ALSA does support note-off.
 */
static struct termios g_saved_term;

static void init_keyboard(void)
{
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_saved_term);
    raw = g_saved_term;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
}

static void restore_keyboard(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_term);
}

static int kbhit(void)
{
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
}

/* ── Note name helper ────────────────────────────────────────────────── */

/* Return a human-readable name for a MIDI note number (e.g. 60 → "C4"). */
static const char *note_name(int note)
{
    static const char *names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    static char buf[8];
    snprintf(buf, sizeof(buf), "%s%d", names[note % 12], (note / 12) - 1);
    return buf;
}

/* ── Keyboard → MIDI note mapping ────────────────────────────────────── */

/*
 * key_to_note — map a host keyboard character to a MIDI note number
 * relative to the given octave.  Returns -1 if the key is not a note key.
 *
 * The layout covers two adjacent keyboard rows, providing about two
 * octaves from a single piano-style mapping:
 *
 *   Lower row (white): Z=C  X=D  C=E  V=F  B=G  N=A  M=B  ,=C  .=D  /=E
 *   Lower row (black): S=C# D=D# G=F# H=G# J=A# L=C# ;=D#
 *
 *   Upper row (white): Q=C  W=D  E=E  R=F  T=G  Y=A  U=B  I=C  O=D  P=E
 *   Upper row (black): 2=C# 3=D# 5=F# 6=G# 7=A# 9=C# 0=D#
 */
static int key_to_note(int key, int octave)
{
    int semitone = -1;

    switch (tolower(key)) {
        /* Lower row — white and black keys */
        case 'z': semitone =  0; break;  /* C  */
        case 's': semitone =  1; break;  /* C# */
        case 'x': semitone =  2; break;  /* D  */
        case 'd': semitone =  3; break;  /* D# */
        case 'c': semitone =  4; break;  /* E  */
        case 'v': semitone =  5; break;  /* F  */
        case 'g': semitone =  6; break;  /* F# */
        case 'b': semitone =  7; break;  /* G  */
        case 'h': semitone =  8; break;  /* G# */
        case 'n': semitone =  9; break;  /* A  */
        case 'j': semitone = 10; break;  /* A# */
        case 'm': semitone = 11; break;  /* B  */
        case ',': semitone = 12; break;  /* C  (next octave) */
        case 'l': semitone = 13; break;  /* C# */
        case '.': semitone = 14; break;  /* D  */
        case ';': semitone = 15; break;  /* D# */
        case '/': semitone = 16; break;  /* E  */

        /* Upper row — white and black keys */
        case 'q': semitone =  0; break;  /* C  */
        case '2': semitone =  1; break;  /* C# */
        case 'w': semitone =  2; break;  /* D  */
        case '3': semitone =  3; break;  /* D# */
        case 'e': semitone =  4; break;  /* E  */
        case 'r': semitone =  5; break;  /* F  */
        case '5': semitone =  6; break;  /* F# */
        case 't': semitone =  7; break;  /* G  */
        case '6': semitone =  8; break;  /* G# */
        case 'y': semitone =  9; break;  /* A  */
        case '7': semitone = 10; break;  /* A# */
        case 'u': semitone = 11; break;  /* B  */
        case 'i': semitone = 12; break;  /* C  (next octave) */
        case '9': semitone = 13; break;  /* C# */
        case 'o': semitone = 14; break;  /* D  */
        case '0': semitone = 15; break;  /* D# */
        case 'p': semitone = 16; break;  /* E  */

        default: return -1;
    }

    /* MIDI note = octave * 12 + semitone, clamped to 0–127. */
    int note = octave * 12 + semitone;
    if (note < 0)   note = 0;
    if (note > 127) note = 127;
    return note;
}

/* ── MIDI send primitives ────────────────────────────────────────────── */

/*
 * write_midi_byte — deliver one MIDI byte to the C64 via the virtual
 * NameSoft interface.
 *
 * sysop_namesoft_midi_ready() polls the bridge until the previous NMI
 * has been serviced and the ACIA receive register is free.  Once ready,
 * sysop_namesoft_midi_write() writes the byte and asserts NMI so that
 * SID-Wizard's interrupt handler can pick it up immediately.
 *
 * The polling loop includes a 100 ms sleep to avoid busy-waiting — in
 * normal operation the C64 processes each byte within a few hundred
 * microseconds, so the loop almost never repeats.
 */
static int write_midi_byte(uint8_t byte)
{
    uint8_t ready = sysop_namesoft_midi_ready();
    while (!ready) {
        sleep_ms(100);
        ready = sysop_namesoft_midi_ready();
    }
    sysop_namesoft_midi_write(byte);
    return 1;
}

/*
 * send_midi_message — transmit a complete MIDI message and optionally
 * append it to the active recording.
 *
 * Multi-byte messages (e.g. Note On/Off) are sent byte-by-byte with a
 * 1 ms gap between bytes to give the C64's NMI handler time to process
 * each byte before the next one arrives.
 */
static int send_midi_message(const uint8_t *bytes, int count, MidiRecorder *rec)
{
    if (rec && rec->recording)
        recorder_record_event(rec, bytes, count);

    for (int i = 0; i < count; i++) {
        if (!write_midi_byte(bytes[i])) {
            fprintf(stderr, "send_midi_message: failed at byte %d\n", i);
            return 0;
        }
        sleep_ms(1);
    }
    return 1;
}

/* ── Help text ───────────────────────────────────────────────────────── */

static void print_help(void)
{
    printf("\n");
    printf("┌────────────────────────────────────────────────────────────┐\n");
    printf("│      Virtual NameSoft MIDI Keyboard for SID-Wizard         │\n");
    printf("└────────────────────────────────────────────────────────────┘\n");
    printf("\n");
    printf("Keyboard layout (two-octave piano):\n");
    printf("  Upper row:  Q W E R T Y U I O P    (white keys)\n");
    printf("              2 3   5 6 7   9 0       (black keys)\n");
    printf("\n");
    printf("  Lower row:  Z X C V B N M , . /    (white keys)\n");
    printf("              S D   G H J   L ;       (black keys)\n");
    printf("\n");
    printf("Commands:\n");
    printf("  ESC    quit\n");
    printf("  SPACE  all notes off\n");
    printf("  + / -  octave up / down\n");
    printf("  [ / ]  velocity down / up\n");
    printf("  0–9    program change\n");
    printf("  H      show this help\n");
    printf("  R      start/stop recording  (requires -r)\n");
    printf("  P      start/stop playback   (requires -r or -p)\n");
    printf("\n");
    printf("Note: terminal input has no key-release events.\n");
    printf("      Notes play for 200 ms then turn off automatically.\n");
    printf("      Use an ALSA MIDI device (-d) for real sustain.\n");
    printf("────────────────────────────────────────────────────────────\n");
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    char *alsa_device   = NULL;
    char *record_file   = NULL;
    char *playback_file = NULL;
    int   playback_count = 1;
    int   opt;

    while ((opt = getopt(argc, argv, "d:r:p:")) != -1) {
        switch (opt) {
        case 'd':
            alsa_device = optarg;
            break;
        case 'r':
            record_file = optarg;
            break;
        case 'p':
            playback_file = optarg;
            /* Optional count argument immediately after the filename. */
            if (optind < argc && argv[optind][0] != '-') {
                playback_count = atoi(argv[optind]);
                if (playback_count < 1) playback_count = 1;
                optind++;
            }
            break;
        default:
            fprintf(stderr,
                "Usage: %s [-d alsa_device] [-r record_file] "
                "[-p playback_file [count]]\n", argv[0]);
            return 1;
        }
    }

    if (record_file && playback_file) {
        fprintf(stderr, "Error: -r and -p are mutually exclusive.\n");
        return 1;
    }

    printf("sidmidi — Virtual NameSoft MIDI for SID-Wizard\n");
    printf("===============================================\n\n");

    /* ------------------------------------------------------------------
     * Connect to the C64 via the Sysop-64 bridge.
     * sysop_server_connect() does not need DMA lock — the NameSoft MIDI path
     * uses the server command channel (sysop_command()) rather than raw
     * DMA pokes.
     * ------------------------------------------------------------------ */
    sysop_init();
    if (sysop_server_connect() != 0) {
        fprintf(stderr, "Error: could not connect to C64.\n");
        sysop_uninit();
        return 1;
    }
    printf("Connected to C64.\n");

    /* Enable the virtual NameSoft MIDI interface on the bridge side.
     * This configures the bridge to accept sysop_namesoft_midi_write()
     * calls and handle the NMI signalling to the C64. */
    sysop_disable_io();
    sysop_command(SYSOP_ENABLE_NAMESOFT_MIDI);

    /* Initialise the ACIA registers to a known state:
     *   $DE02 (status) = 0    clear any stale IRQ / data-full flags
     *   $DE03 (RX)     = 0    clear receive register
     *   $DE00 (control) = 0x95 enable RX interrupt (standard ACIA init) */
    sysop_io_poke(NAMESOFT_STATUS, 0x00);
    sysop_io_poke(NAMESOFT_RX,     0x00);
    sysop_io_poke(NAMESOFT_CONTROL, 0x95);
    printf("Virtual NameSoft MIDI device ready at $DE00.\n");

    /* ------------------------------------------------------------------
     * Initialise the MIDI recorder.
     * ------------------------------------------------------------------ */
    MidiRecorder recorder;
    recorder_init(&recorder);
    recorder.playback_count = playback_file ? playback_count : 0;

    if (playback_file) {
        if (recorder_load_file(&recorder, playback_file) != 0) {
            sysop_server_disconnect();
            sysop_uninit();
            return 1;
        }
        recorder_start_playback(&recorder);
    }

    /* ------------------------------------------------------------------
     * Open ALSA MIDI input (optional).
     * If -d was given we create a sequencer port and subscribe it to the
     * named device.  ALSA events are then polled in the main loop below.
     * ------------------------------------------------------------------ */
    snd_seq_t *seq = NULL;
    if (alsa_device) {
        if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
            fprintf(stderr, "ALSA: could not open sequencer.\n");
            seq = NULL;
        } else {
            snd_seq_set_client_name(seq, "sidmidi");
            int port_id = snd_seq_create_simple_port(seq, "MIDI In",
                SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                SND_SEQ_PORT_TYPE_APPLICATION);
            if (port_id < 0) {
                fprintf(stderr, "ALSA: could not create port.\n");
            } else {
                connect_to_device(seq, port_id, alsa_device);
                snd_seq_nonblock(seq, 1);  /* non-blocking poll in main loop */
            }
        }
    }

    /* ------------------------------------------------------------------
     * Switch the terminal to raw/non-blocking mode for keyboard input.
     * Skipped in playback-only mode since we don't need key input there.
     * ------------------------------------------------------------------ */
    if (!playback_file)
        init_keyboard();

    print_help();

    /* ------------------------------------------------------------------
     * Main event loop
     * ------------------------------------------------------------------
     * Three input sources are polled each iteration:
     *   1. recorder   — pending playback events with their timestamps
     *   2. seq        — ALSA MIDI events from an external device
     *   3. kbhit/getchar — host keyboard (raw terminal)
     * ------------------------------------------------------------------ */
    uint8_t velocity = MIDI_DEFAULT_VELOCITY;
    int     octave   = 4;   /* default: middle-C octave */
    int     program  = 1;
    int     running  = 1;

    printf("\nReady. Press keys to play MIDI notes (H for help).\n");
    if (record_file)   printf("Press R to start recording to '%s'.\n", record_file);
    if (playback_file) printf("Playing back from '%s'.\n", playback_file);
    printf("\n");

    while (running) {

        /* ── 1. Playback events ─────────────────────────────────────── */
        if (recorder.playing) {
            uint8_t *pb_data;
            int      pb_len;
            int      result = recorder_update_playback(&recorder, &pb_data, &pb_len);

            if (result == 1) {
                send_midi_message(pb_data, pb_len, NULL);  /* don't re-record */
                if (pb_len >= 2) {
                    uint8_t status = pb_data[0] & 0xF0;
                    if (status == MIDI_NOTE_ON && pb_len == 3)
                        printf("[Playback] Note ON  %s vel=%d\n",
                               note_name(pb_data[1]), pb_data[2]);
                    else if (status == MIDI_NOTE_OFF && pb_len == 3)
                        printf("[Playback] Note OFF %s\n", note_name(pb_data[1]));
                }
            } else if (result == -1 && playback_file) {
                /* Finite playback finished — exit cleanly. */
                printf("Playback complete. Exiting.\n");
                running = 0;
                break;
            }
        }

        /* ── 2. ALSA MIDI input ──────────────────────────────────────── */
        if (seq) {
            snd_seq_event_t *ev = NULL;
            while (snd_seq_event_input(seq, &ev) >= 0) {

                if (ev->type == SND_SEQ_EVENT_NOTEON) {
                    if (ev->data.note.velocity > 0) {
                        printf("[ALSA] Note ON  %s vel=%d\n",
                               note_name(ev->data.note.note),
                               ev->data.note.velocity);
                        uint8_t msg[] = { MIDI_NOTE_ON,
                                          (uint8_t)ev->data.note.note,
                                          (uint8_t)ev->data.note.velocity };
                        send_midi_message(msg, 3, &recorder);
                    } else {
                        /* MIDI convention: Note On with velocity 0 = Note Off. */
                        printf("[ALSA] Note OFF %s\n", note_name(ev->data.note.note));
                        uint8_t msg[] = { MIDI_NOTE_OFF,
                                          (uint8_t)ev->data.note.note, 0 };
                        send_midi_message(msg, 3, &recorder);
                    }

                } else if (ev->type == SND_SEQ_EVENT_NOTEOFF) {
                    printf("[ALSA] Note OFF %s\n", note_name(ev->data.note.note));
                    uint8_t msg[] = { MIDI_NOTE_OFF,
                                      (uint8_t)ev->data.note.note, 0 };
                    send_midi_message(msg, 3, &recorder);

                } else if (ev->type == SND_SEQ_EVENT_PITCHBEND) {
                    /* ALSA pitch bend range: –8192 to +8191.
                     * MIDI pitch bend range: 0 to 16383 (centre = 8192). */
                    int val = ev->data.control.value + 8192;
                    if (val < 0)     val = 0;
                    if (val > 16383) val = 16383;
                    uint8_t msg[] = { MIDI_PITCH_BEND,
                                      (uint8_t)(val & 0x7F),
                                      (uint8_t)((val >> 7) & 0x7F) };
                    send_midi_message(msg, 3, &recorder);

                } else if (ev->type == SND_SEQ_EVENT_PGMCHANGE) {
                    int prog = ev->data.control.value;
                    printf("[ALSA] Program Change %d\n", prog);
                    uint8_t msg[] = { MIDI_PROGRAM_CHANGE, (uint8_t)prog };
                    send_midi_message(msg, 2, &recorder);
                }
            }
        }

        /* ── 3. Host keyboard ────────────────────────────────────────── */
        if (!playback_file && kbhit()) {
            int key = getchar();

            if (key == 27) { /* ESC — quit */
                printf("\nQuitting...\n");
                running = 0;

            } else if (key == ' ') { /* SPACE — all notes off */
                printf("\n[All Notes Off]\n");
                uint8_t msg[] = { 0xB0, 0x7B, 0x00 };  /* CC 123 */
                send_midi_message(msg, 3, &recorder);

            } else if (key == '+' || key == '=') {
                if (octave < 8) octave++;
                printf("\n[Octave: %d]\n", octave);

            } else if (key == '-' || key == '_') {
                if (octave > 1) octave--;
                printf("\n[Octave: %d]\n", octave);

            } else if (key == '[') {
                if (velocity > 8) velocity -= 8;
                printf("\n[Velocity: %d]\n", velocity);

            } else if (key == ']') {
                if (velocity < 127) velocity += 8;
                printf("\n[Velocity: %d]\n", velocity);

            } else if (key == 'h' || key == 'H') {
                print_help();

            } else if (key == 'r' || key == 'R') {
                if (!record_file) {
                    printf("\n[No record file specified — use -r <file>]\n");
                } else if (recorder.recording) {
                    recorder_stop_recording(&recorder);
                } else {
                    recorder_start_recording(&recorder, record_file);
                }

            } else if (key == 'p' || key == 'P') {
                if (recorder.playing) {
                    recorder_stop_playback(&recorder);
                } else {
                    const char *src = record_file ? record_file : playback_file;
                    if (!src) {
                        printf("\n[No playback file — use -r or -p <file>]\n");
                    } else if (recorder_load_file(&recorder, src) == 0) {
                        recorder.playback_index = 0;
                        recorder_start_playback(&recorder);
                    }
                }

            } else if (key >= '0' && key <= '9') {
                /* Program change: keys 1–9 select instruments 1–9,
                 * key 0 selects instrument 10 (SID-Wizard convention). */
                program = (key == '0') ? 10 : (key - '0');
                uint8_t msg[] = { MIDI_PROGRAM_CHANGE, (uint8_t)program };
                if (send_midi_message(msg, 2, &recorder))
                    printf("\n[Program: %d]\n", program);

            } else {
                /* Try to map the key to a MIDI note. */
                int note = key_to_note(key, octave);
                if (note >= 0) {
                    uint8_t msg[] = { MIDI_NOTE_ON, (uint8_t)note, velocity };
                    if (send_midi_message(msg, 3, &recorder))
                        printf("\n[Note ON  %s vel=%d]\n", note_name(note), velocity);

                    /* Auto note-off after 200 ms.
                     * Terminal input provides no key-release events so we
                     * use a fixed duration.  This feels playable for most
                     * tempos; lower values feel staccato, higher legato. */
                    sleep_ms(200);
                    msg[0] = MIDI_NOTE_OFF;
                    msg[2] = 0;
                    if (send_midi_message(msg, 3, &recorder))
                        printf("[Note OFF %s]\n", note_name(note));
                }
            }
        }

        sleep_ms(1);
    }

    /* ------------------------------------------------------------------
     * Shutdown — send all-notes-off, restore terminal, disconnect.
     * ------------------------------------------------------------------ */
    printf("\nSending All Notes Off...\n");
    uint8_t all_off[] = { 0xB0, 0x7B, 0x00 };
    send_midi_message(all_off, 3, NULL);

    recorder_cleanup(&recorder);

    if (!playback_file)
        restore_keyboard();

    if (seq)
        snd_seq_close(seq);

    sysop_server_disconnect();
    sysop_uninit();

    printf("Disconnected. Goodbye!\n");
    return 0;
}
