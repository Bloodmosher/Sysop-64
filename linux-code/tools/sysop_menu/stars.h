/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#ifndef STARS_H
#define STARS_H

void initIncTable(void);
void updateStars();
void advanceStars();
void drawStars(cairo_t* cr, int center_x, int center_y, int setOrClear);

#endif /* STARS_H */
