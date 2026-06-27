/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "sysop64.h"

int main(int argc, char** argv)
{
    sysop_init();
    int pressed = 0;
    while(1) {
        if (pressed == 0 && sysop_is_button_pressed(1) && sysop_is_button_pressed(2)) {
            const char* cmd = "sysop_restart";
            system(cmd); 
        }
        usleep(50000);
    }
}
