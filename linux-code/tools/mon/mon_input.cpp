/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "mon_private.h"
#include "mon_input.h"

// ---------------------------------------------------------------------------
// Terminal state
// ---------------------------------------------------------------------------

struct termios oldt;
int old_flags;

void setup_raw_terminal(void)
{
    struct termios newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    old_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, old_flags | O_NONBLOCK);
}

void wait_for_enter(void)
{
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, fl & ~O_NONBLOCK);
    int c; while ((c = getchar()) != '\n' && c != '\r' && c != EOF) {}
    fcntl(STDIN_FILENO, F_SETFL, fl);
}

void reset_term(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, old_flags);
}

void sigintHandler(int signal)
{
    printf("sigint handler\n");
    // Disable NMI trap so it doesn't persist in the FPGA after mon exits
    // and interfere with other tools (e.g. easyflash).
    *((volatile uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_DISABLE_NMI_TRAP;
    reset_term();
    exit(signal);
}

// ---------------------------------------------------------------------------
// Command history (file-local ring buffer)
// ---------------------------------------------------------------------------

static char s_history[HISTORY_MAX_LINES][HISTORY_MAX_LEN];
static int  s_history_count = 0;  // number of valid entries (0 .. HISTORY_MAX_LINES-1)

// ---------------------------------------------------------------------------
// get_line — line editor
//
// Bugs fixed vs the original:
//   - 'len' parameter is now respected; the work buffer is capped to min(len, HISTORY_MAX_LEN).
//   - History ring buffer: once full, the oldest entry is discarded instead of overflowing.
//   - '\b' (pty backspace) now guards against an empty buffer before writing '\0'.
//   - Character append now checks available space before writing.
// ---------------------------------------------------------------------------

char *get_line(char *buf, int len)
{
    // Work buffer: capped to the smaller of the caller's limit or one history slot.
    int cap = (len < HISTORY_MAX_LEN) ? len : HISTORY_MAX_LEN;
    char work[HISTORY_MAX_LEN];
    work[0] = '\0';

    int view_index = -1;  // -1 = "live" input, >=0 = browsing history

    while (1) {
        int ch = getchar();

        if (ch == 27) {
            // ANSI escape sequence: ESC [ <code>
            ch = getchar();
            if (ch != 91)  // not '['
                continue;
            ch = getchar();

            switch (ch) {
                case 65:  // Up arrow — go back one step in history
                    if (s_history_count > 0) {
                        printf("\33[2K\r> ");
                        if (view_index == -1)
                            view_index = s_history_count - 1;
                        else if (view_index > 0)
                            view_index--;
                        printf("%s", s_history[view_index]);
                        strncpy(work, s_history[view_index], cap - 1);
                        work[cap - 1] = '\0';
                    }
                    break;

                case 66:  // Down arrow — go forward one step in history
                    if (view_index >= 0) {
                        printf("\33[2K\r> ");
                        view_index++;
                        if (view_index >= s_history_count) {
                            view_index = s_history_count - 1;
                            work[0] = '\0';
                        } else {
                            printf("%s", s_history[view_index]);
                            strncpy(work, s_history[view_index], cap - 1);
                            work[cap - 1] = '\0';
                        }
                    }
                    break;

                case 67:  // Right arrow
                    printf("\033[C");
                    break;

                case 68:  // Left arrow
                    printf("\033[D");
                    break;

                case 70:  // End key — jump to newest history entry
                    if (s_history_count > 0) {
                        printf("\33[2K\r> ");
                        view_index = s_history_count - 1;
                        printf("%s", s_history[view_index]);
                        strncpy(work, s_history[view_index], cap - 1);
                        work[cap - 1] = '\0';
                    }
                    break;

                case 72:  // Home key — jump to oldest history entry
                    if (s_history_count > 0) {
                        printf("\33[2K\r> ");
                        view_index = 0;
                        printf("%s", s_history[view_index]);
                        strncpy(work, s_history[view_index], cap - 1);
                        work[cap - 1] = '\0';
                    }
                    break;
            }
        }
        else if (ch == '\n') {
            if (strlen(work) == 0)
                return NULL;

            strncpy(buf, work, len - 1);
            buf[len - 1] = '\0';

            // Save to history (ring buffer: shift out oldest when full)
            if (s_history_count < HISTORY_MAX_LINES) {
                strncpy(s_history[s_history_count], work, HISTORY_MAX_LEN - 1);
                s_history[s_history_count][HISTORY_MAX_LEN - 1] = '\0';
                s_history_count++;
            } else {
                memmove(s_history[0], s_history[1],
                        (HISTORY_MAX_LINES - 1) * HISTORY_MAX_LEN);
                strncpy(s_history[HISTORY_MAX_LINES - 1], work, HISTORY_MAX_LEN - 1);
                s_history[HISTORY_MAX_LINES - 1][HISTORY_MAX_LEN - 1] = '\0';
            }
            return buf;
        }
        else if (ch == 0xffffffff) {
            // Non-blocking read returned nothing; spin
            continue;
        }
        else if (ch == 126) {
            // Del key — not implemented, ignore
        }
        else if (ch == 127 || ch == '\b') {
            // Backspace (both VT100 DEL=127 and pty '\b')
            size_t wlen = strlen(work);
            if (wlen > 0) {
                printf("\b \b");
                work[wlen - 1] = '\0';
            }
        }
        else {
            // Printable character: append only if space remains
            size_t wlen = strlen(work);
            if ((int)wlen < cap - 1) {
                work[wlen]     = (char)ch;
                work[wlen + 1] = '\0';
                printf("%c", ch);
            }
        }
    }
}
