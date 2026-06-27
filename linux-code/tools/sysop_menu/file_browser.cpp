/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * file_browser.cpp — file system and D64 directory listing, scroll /
 * selection state, and Cairo/Pango rendering of the file list.
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <algorithm>
#include <iterator>
#include <glib.h>

#include "file_browser.h"
#include "display.h"

/* Provided by disk.cpp */
extern int get_items_from_d64(const std::string &path, std::list<std::string> &list);

/* ------------------------------------------------------------------ */
/* Global state declared in file_browser.h                            */
/* ------------------------------------------------------------------ */

std::string g_root          = "/mnt/data/c64_files";
std::string g_current_folder;

std::list<fs_item> g_file_list;

int top_position     = 0;
int current_position = 0;

/* ------------------------------------------------------------------ */
/* Sorting                                                             */
/* ------------------------------------------------------------------ */

bool fs_item_sort_name(const fs_item &a, const fs_item &b)
{
    std::string upperA, upperB;
    std::transform(a.name.begin(), a.name.end(), std::back_inserter(upperA), ::toupper);
    std::transform(b.name.begin(), b.name.end(), std::back_inserter(upperB), ::toupper);
    if (upperA == ".." && upperB == "..") return false;
    if (upperA == "..") return true;
    if (upperB == "..") return false;
    return upperA < upperB;
}

/* ------------------------------------------------------------------ */
/* String utilities                                                    */
/* ------------------------------------------------------------------ */

bool endsWith(const std::string &str, const std::string &suffix)
{
    size_t sl = suffix.length();
    if (str.length() < sl)
        return false;
    return str.compare(str.length() - sl, sl, suffix) == 0;
}

bool endsWithCaseInsensitive(const std::string &str, const std::string &suffix)
{
    size_t sl = suffix.length();
    if (str.length() < sl)
        return false;
    std::string strLower    = str;
    std::string suffixLower = suffix;
    std::transform(strLower.begin(),    strLower.end(),    strLower.begin(),    ::tolower);
    std::transform(suffixLower.begin(), suffixLower.end(), suffixLower.begin(), ::tolower);
    return strLower.compare(str.length() - sl, sl, suffixLower) == 0;
}

void sanitizeString(std::string &str)
{
    for (char &ch : str) {
        unsigned char uc = static_cast<unsigned char>(ch);
        if (uc < 0x20 || uc >= 0x7F)
            ch = '-';
    }
}

/* ------------------------------------------------------------------ */
/* Directory listing — host filesystem                                 */
/* ------------------------------------------------------------------ */

int get_items(std::string &path, std::list<fs_item> &items)
{
    g_current_folder = path;
    items.clear();

    DIR *dir;
    struct dirent *entry;
    int count = 0;

    printf("Navigating to %s\n", path.c_str());

    if ((dir = opendir(path.c_str())) == NULL) {
        printf("Error opening directory\n");
        return -1;
    }

    bool is_root = (path == g_root);
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name == NULL)
            continue;
        if (strcmp(entry->d_name, ".") == 0)
            continue;
        if (is_root && strcmp(entry->d_name, "..") == 0)
            continue;

        std::string fullpath = path + "/" + entry->d_name;
        printf("Adding %s\n", fullpath.c_str());
        items.push_back({ fullpath,
                          std::string(entry->d_name),
                          entry->d_type,
                          LocationType::FileSystem,
                          std::string(path.c_str()) });
        count++;
        printf("list is now %zu\n", items.size());
    }

    closedir(dir);
    printf("Returning with %d items added\n", count);
    return count;
}

/* ------------------------------------------------------------------ */
/* Directory listing — D64 image                                       */
/* ------------------------------------------------------------------ */

int getd64_items(std::string &parent, std::string &path, std::list<fs_item> &items)
{
    items.clear();

    std::list<std::string> d64list;
    get_items_from_d64(path, d64list);

    items.push_back({ "..", "..", DT_DIR, LocationType::D64, parent });
    for (auto &it : d64list)
        items.push_back({ std::string(it.c_str()),
                          std::string(it.c_str()),
                          0,
                          LocationType::D64,
                          std::string(path.c_str()) });

    g_current_folder = path;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Scroll / selection helpers                                          */
/* ------------------------------------------------------------------ */

void show_items(std::list<fs_item> &items, int start)
{
    (void)items;
    (void)start;
}

void update_position(int position)
{
    set_redraw_needed();
    current_position = position;
}

/* ------------------------------------------------------------------ */
/* Pango rendering of the file list                                    */
/* ------------------------------------------------------------------ */

void drawFiles(cairo_t *cr, int &redraw_needed)
{
    /* Phase 1: rebuild markup only when the list has changed */
    if (redraw_needed) {
        redraw_needed = 0;

        std::string markup_builder;
        int items_drawn  = 0;
        int current_index = 0;

        auto it = g_file_list.begin();
        if (top_position > 0 && top_position < (int)g_file_list.size()) {
            std::advance(it, top_position);
            current_index = top_position;
        }

        while (it != g_file_list.end() && items_drawn <= MAX_DRAW_ITEMS) {
            std::string str = it->name;
            sanitizeString(str);

            char *escaped_c = g_markup_escape_text(str.c_str(), -1);
            std::string name(escaped_c);
            g_free(escaped_c);

            if (it->d_type == DT_DIR)
                name += "/";

            if (current_index == current_position)
                markup_builder += "<span foreground=\"red\">" + name + "</span>\n";
            else
                markup_builder += name + "\n";

            ++it;
            ++current_index;
            ++items_drawn;
        }

        if (!markup_builder.empty()) {
            printf("redrawing...\n");
            printf("markup: %s\n", markup_builder.c_str());
            pango_layout_set_markup(g_layout, markup_builder.c_str(), -1);
        }
    }

    /* Phase 2: paint the cached layout (runs every frame) */
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_move_to(cr, 150, 196);

    if (cr == g_cr1 && g_layout == g_layout2) {
        printf("wrong layout1\n");
        getchar();
    }
    if (cr == g_cr2 && g_layout == g_layout1) {
        printf("wrong layout2\n");
        getchar();
    }

    pango_cairo_show_layout(cr, g_layout);
}
