/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <error.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "sysop64.h"

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("Usage: showmsg [message-to-show [<display-time-milliseconds> [--no-hide]]] | [--hide]\n");
        exit(EXIT_FAILURE);
    }

    sysop_init();

    int res = sysop_server_connect();
    if (res == -1)
    {
        perror("sysop_server_connect failed");
        exit(EXIT_FAILURE);
    }

    if (strcmp(argv[1], "--hide") == 0)
    {
        sysop_server_hide_messages();
    }
    /*
    else if (strcmp(argv[1], "--queue-hide") == 0)
    {
        sysop_server_queue_hide_messages();
    }
    */
    else
    {
        int len = strlen(argv[1]);
        int display_time = 5000;
        if (argc > 2)
        {
            display_time = strtoll(argv[2], NULL, 10);
        }
        printf("Show message '%s' len %d, display time %d\n", argv[1], len, display_time);
        sysop_server_display_message(argv[1], len, display_time);
        if (argc >= 4 && strcmp(argv[3], "--no-hide") == 0)
        {
            // skip the hide
        }
        else
        {
            sysop_server_queue_hide_messages();
        }
    }
    sysop_server_disconnect();
    sysop_uninit();
}
