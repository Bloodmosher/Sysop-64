/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

#include <string>
#include <list>
#include <cairo.h>

/* Maximum number of file entries visible on screen at once */
#define MAX_DRAW_ITEMS 20

enum LocationType {
    FileSystem,
    D64
};

struct fs_item {
    std::string   fullpath;
    std::string   name;
    unsigned char d_type;
    LocationType  locationType;
    std::string   parent;
};

/* Root directory and the folder currently being displayed */
extern std::string g_root;
extern std::string g_current_folder;

/* The list of items in the current view */
extern std::list<fs_item> g_file_list;

/* Scroll and selection state */
extern int top_position;
extern int current_position;

/* Sorting predicate — sorts by name, ".." always first */
bool fs_item_sort_name(const fs_item &a, const fs_item &b);

/* String utilities */
bool endsWith(const std::string &str, const std::string &suffix);
bool endsWithCaseInsensitive(const std::string &str, const std::string &suffix);
void sanitizeString(std::string &str);

/* Directory listing */
int get_items(std::string &path, std::list<fs_item> &items);
int getd64_items(std::string &parent, std::string &path, std::list<fs_item> &items);
void show_items(std::list<fs_item> &items, int start);

/* Selection state */
void update_position(int position);

/* Cairo rendering — draws the visible portion of g_file_list */
void drawFiles(cairo_t *cr, int &redraw_needed);

#endif /* FILE_BROWSER_H */
