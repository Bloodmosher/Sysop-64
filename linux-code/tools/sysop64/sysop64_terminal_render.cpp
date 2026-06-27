/*
 * Sysop-64 
 * https://github.com/Bloodmosher/Sysop-64 
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project 
 *
 * sysop64_terminal_render.cpp
 *
 * Terminal line rendering using Cairo and Pango.
 *
 * render_lines() is called from draw_to_context() to composite the current
 * CircularBuffer contents onto a Cairo surface. It determines the line
 * height from Pango metrics, then calls draw_lines() to iterate over the
 * visible rows.
 *
 * draw_lines() renders each line of the buffer as a Pango layout. It skips
 * lines that are neither dirty nor part of a full redraw. For the row that
 * contains the cursor, it erases the previous cursor rectangle before
 * redrawing the text, then measures the cursor column position with Pango
 * and overlays a green block cursor at the correct X offset.
 */

#include "sysop64_internal.h"
/*
void add_line(CircularBuffer* buffer, const char* line) 
{
    strncpy(buffer->lines[buffer->count], line, LINE_WIDTH);
    buffer->count++;
}
*/

char tmp_line[1024];

int previous_cursor_x = -1;
int previous_cursor_y = -1;
int previous_cursor_width = -1;
int previous_cursor_height = -1;

// Renders all visible lines of buffer onto cr, starting at pixel position
// (x, y) with each line occupying height pixels. Skips lines that are
// neither dirty nor part of a full redraw. Draws a green block cursor at
// the current cursor_col position on cursor_row.
void draw_lines(cairo_t* cr, CircularBuffer* buffer, int x, int y, int height) 
{
    //printf("DRAWLINES\n");
    //printf("cursor row %d col %d\n", cursor_row, cursor_col);
    
    int i = 0;
    //if (buffer->count + 1 >= MAX_VISIBLE_LINES)

    //if (buffer->lines.size()  >= MAX_VISIBLE_LINES)

    if (buffer->lines.size()  > MAX_VISIBLE_LINES)
    {
        //i = buffer->count + 1 - MAX_VISIBLE_LINES;
        printf("line calc 1\n");
        i = buffer->lines.size() -  MAX_VISIBLE_LINES;
    }
    //i += scroll_top;



/*    // trying something new... if it works delete the above
    if (cursor_row > MAX_VISIBLE_LINES)
    {
        printf("line calc 2\n");
        i = cursor_row - MAX_VISIBLE_LINES;
    }
*/


    printf("start drawing at line %d, buffer line count is %d\n", i, buffer->lines.size());
    int drawn = 0;
    //for (; i < buffer->count + 1; i++)  // +1 to draw the "current" line
    for (; i < buffer->lines.size(); i++)  // +1 to draw the "current" line
    //for (i = 0; i < term_rows; i++)
    {
        if (!buffer->redraw_needed[i] && !buffer->redraw_all) {
            y += height;
            continue;
        }
        drawn++;

/*
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 1, 1, 1, 0);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, x, y, g_framebuffer_width-200, y+height-20);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 1, 1, 1);  // white
*/

        //buffer->redraw_needed[i] = false;

        //printf("render_line i=%d\n", i);
        const char* text = buffer->lines[i].c_str();

        if (strlen(text) > 0)
        {
            if (i + 1 == cursor_row && previous_cursor_x != -1)
            {
                //cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
                cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
                cairo_set_source_rgba(cr, 0, 0, 0, 0);
                cairo_set_line_width(cr, 1.0);
            //    printf("Erase cursor box: %d, %d, %d, %d\n", previous_cursor_x, previous_cursor_y, previous_cursor_width, previous_cursor_height);
                //cairo_rectangle(cr, previous_cursor_x, previous_cursor_y, previous_cursor_width, previous_cursor_height);
                cairo_rectangle(cr, x, y, g_framebuffer_width-200-4, height);
                cairo_fill(cr);
               // cairo_stroke(cr);
                cairo_set_source_rgba(cr, 1, 1, 1, 1);
                cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            }
            //cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            //cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
            cairo_move_to(cr, x, y);
            //printf("line %d, move to %d, %d\n", i, x, y);
            
            //char *unescaped_string = g_strunescape(text, NULL);
            char *escaped_string = g_strescape(text, NULL);
            //pango_layout_set_text(g_layout, text, -1);
            pango_layout_set_text(g_layout, escaped_string, -1);
            pango_cairo_show_layout(cr, g_layout);
            //printf("RENDER: %d %s\n", i, escaped_string);
            g_free(escaped_string);

            //cairo_show_text(cr, text);


            //if (i == buffer->count) // after the last char, draw a cursor
            
            
            //if (i == buffer->lines.size()-1) // after the last char, draw a cursor - TODO: draw it where the cursor is supposedly
            if (i + 1 == cursor_row)
            {
                //printf("Drawing cursor, text length is %d\n", strlen(text));
                
                //cairo_text_extents_t text_extents;
                //cairo_text_extents(cr, text, &text_extents);

                strcpy(tmp_line, text);
                tmp_line[cursor_col-1] = '\0';
                
                //printf("Using '%s' to measure where cursor should be\n", tmp_line);
                
                //cairo_text_extents(cr, tmp_line, &text_extents);
                int pwidth, pheight;
                pango_layout_set_text(g_layout, tmp_line, -1);
                pango_layout_get_size(g_layout, &pwidth, &pheight);
                pwidth /= PANGO_SCALE;
                pheight /= PANGO_SCALE;


                //cairo_move_to(x (int)text_extens.width + 5, y);
                cairo_set_source_rgba(cr, 0, 1, 0, 1);
                //cairo_rectangle(cr, x + (int)text_extents.width + (int)text_extents.x_advance + 7, y-(int)text_extents.height+4, 20, (int)text_extents.height);
                
                
                cairo_rectangle(cr, x + pwidth, y+2, 20, pheight-4);
                previous_cursor_x = x + pwidth;
                previous_cursor_y = y+2;
                previous_cursor_width = 20;
                previous_cursor_height = pheight-4;
             //   printf("Saved cursor box: %d, %d, %d, %d\n", previous_cursor_x, previous_cursor_y, previous_cursor_width, previous_cursor_height);
                
                //int xoffset = (cursor_col > 1 ) ? (cursor_col-2)*20 : 0;
                //cairo_rectangle(cr, x + xoffset, y-(int)text_extents.height+4, 20, (int)text_extents.height);
                
                cairo_fill(cr);

                cairo_set_source_rgb(cr, 1, 1, 1);  // white


                //printf("Raw bytes on prompt line: ");
  /*              for (int j=0;j<strlen(text);j++)
                {
                    printf("%02X ", text[j]);
                }
                printf("\n");
                */
            }
            y += height;
        }
    }
    printf("Finished drawing %d lines\n", drawn);
    //buffer->redraw_all = false;
}

// Entry point called from draw_to_context(). Measures the Pango line height
// for the current font, positions the start of the text area, then delegates
// to draw_lines() to render the full line buffer.
void render_lines(cairo_t* cr, int width, int height)
{
    const char* text = "root@socfpga:~/code# ";
    //cairo_select_font_face(context, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    //cairo_text_extents_t text_extents;
    //cairo_text_extents(cr, text, &text_extents);
    //printf("text_extents height: %d\n", (int)text_extents.height);

    double x = 100;
    //double y = height/2;
    double y = 0;
    //height += ( text_extents.height) / 2 - text_extents.y_bearing;
    
    //y += (int)text_extents.height;
    
    //int text_extens_height = 27;
    //y += text_extens_height;


    int pwidth, pheight;
    pango_layout_set_text(g_layout, text, -1);
    pango_layout_get_size(g_layout, &pwidth, &pheight);
    pwidth /= PANGO_SCALE;
    pheight /= PANGO_SCALE;

    y += pheight;
    printf("pango_layout_get_size %d, %d\n", pwidth, pheight);


    // Set color
    cairo_set_source_rgb(cr, 1, 1, 1);  // white

    // Draw text
    //draw_lines(cr, &line_buffer, x, y, (int)text_extents.height + 5);
    //draw_lines(cr, current_line_buffer, x, y, text_extens_height + 5);
    draw_lines(cr, current_line_buffer, x, y, pheight);


}


