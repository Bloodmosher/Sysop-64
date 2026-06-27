/*
 * Sysop-64 
 * https://github.com/Bloodmosher/Sysop-64 
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project 
 *
 * sysop64_terminal_parser.cpp
 *
 * VT100/ANSI terminal escape-sequence parser and screen buffer manager.
 *
 * process_buffer() is the core entry point: it ingests raw bytes from the
 * PTY master and maintains a state machine across calls for the four
 * sequence types:
 *
 *   ESC sequences  - cursor save/restore, keypad mode
 *   CSI sequences  - cursor movement, erase, insert/delete, scroll regions
 *   OSC sequences  - terminal properties (title, colour queries)
 *   DCS sequences  - device-control strings (consumed and discarded)
 *
 * Printable characters are written directly into the CircularBuffer line
 * store at the current cursor position. The buffer uses a fixed-height
 * vector of std::string rows; cursor_row/cursor_col are 1-based.
 *
 * Reference: https://www.xfree86.org/current/ctlseqs.html
 */

#include "sysop64_internal.h"
// Initialises buffer by populating it with MAX_VISIBLE_LINES blank lines
// (space-padded to term_cols) if empty, then marks all lines for redraw.
void init_buffer(CircularBuffer* buffer) 
{
    //buffer->count = 0;
    if (buffer->lines.size() == 0)
    {
        for (int i=0;i<MAX_VISIBLE_LINES;i++)
        {
            buffer->lines.push_back(std::string(term_cols, ' '));
        }
        //buffer->lines.push_back(std::string(""));
    }
    buffer->redraw_all = true;
}



/*void process_buffer(const char* data, int n)
{
    CircularBuffer* buffer = &line_buffer;
    char* current_line = buffer->lines[(buffer->start + buffer->count) % MAX_LINES];
    int len = strlen(current_line);
    
    int offset = 0;
    for (int i=0;i<n;i++)
    {
        if (data[i] == '\n')
        {
            if (buffer->count == MAX_LINES) {
                // If the buffer is full, remove the oldest line
                buffer->lines[buffer->start][0] = '\0';
                buffer->start = (buffer->start + 1) % MAX_LINES;
                buffer->count--;
            }

            current_line[len+offset] = '\0';
            buffer->count++;

            current_line = buffer->lines[(buffer->start + buffer->count) % MAX_LINES];
            current_line[0] = '\0';
            len = 0;
            offset = 0;
        }
        else
        {
            current_line[len+offset] = data[i];
            current_line[len+offset+1] = '\0';
            offset++;
        }
    }

    update_framebuffer();
}
*/



int in_esc_seq = 0;
int in_csi_seq = 0;
int in_osc_seq = 0;
int in_dcs_seq = 0;
std::vector<char> parameter_bytes;
std::vector<char> intermediate_bytes;
char final_byte;

int cursor_position = 0;
int cursor_row = 1;
int cursor_col = 1;

int main_cursor_position = 0;
int main_cursor_row = 1;
int main_cursor_col = 1;
int main_current_line_index = 0;

std::string ansi_escape_string;

int cursor_key_mode = 0; // TODO: implement behaviors

int scroll_region_min = 0;
int scroll_region_max = 0;


int save_cursor_row = 0;
int save_cursor_col = 0;
int scroll_top = 0;

int tab_spacing = 8;

// Parses the accumulated parameter_bytes as a decimal integer. Returns
// defaultVal if parameter_bytes is empty.
int get_integer_or_default(std::vector<char>& parameter_bytes, int defaultVal)
{
    int nval = defaultVal;
    if (parameter_bytes.size() > 0)
    {
        char ascii_text[255];
        int j = 0;
        for (;j<parameter_bytes.size();j++)
        {
            ascii_text[j] = parameter_bytes[j];
        }
        ascii_text[j] = '\0';
        nval = atoi(ascii_text);
        //printf("CSI delete '%s' (%d) chars\n", ascii_text, ndelete);
    }
    return nval;
}

// Processes n bytes from data, updating the CircularBuffer and cursor state.
// ppResponseBytes / nResponseBytes are output parameters: if the terminal
// sequence requires a response (e.g. OSC colour query), the caller's buffer
// pointer is set to a malloc'd response that must be written back to the PTY.
void process_buffer(const char* data, int n, char** ppResponseBytes, int& nResponseBytes)
{
    CircularBuffer* buffer = current_line_buffer;
    
    if (ppResponseBytes)
    {
        *ppResponseBytes = NULL;
        nResponseBytes = 0;
    }
    //printf("size %d\n", buffer->lines.size());
    if (buffer->lines.size() == 0)
    {
        //buffer->lines.push_back(std::string(term_cols, ' '));
        buffer->lines.push_back(std::string(""));
        cursor_row = 1;
        cursor_col = 1;
        //printf("Added new line, count: %d\n", buffer->lines.size());
    }
    //char* current_line = buffer->lines[buffer->count];
    //printf("getting current line at index %d\n", buffer->lines.size()-1);
    //char* current_line = buffer->lines[buffer->lines.size()-1];


    
    //int current_line_index = buffer->lines.size()-1;
    int current_line_index = scroll_top + cursor_row - 1;
    if (current_line_index < 0)
        current_line_index = buffer->lines.size()-1;

    buffer->redraw_needed[current_line_index] = true;

    int offset = 0;
    
    char prev_bytes[2];
    int prev_byte_index = 0;
    
    //for (int i=0;i<n;i++)
    for (int i=0;i<n;i++)
    {
        //printf("i is %d\n", i);
        if (data[i] == 0x1b && in_dcs_seq == 0 && in_csi_seq == 0 && in_osc_seq == 0)
        {
            in_esc_seq = 1;
            prev_byte_index = 0;
            printf("Starting esc seq\n");
            continue;
        }
        else if (in_esc_seq == 1)
        {
            //printf("first esc byte %02X\n", data[i]);
            if (data[i] == 0x5d) // ']' // OSC sequence
            {
                in_osc_seq = 1;
                in_esc_seq = 0;
                ansi_escape_string = "\\x1b]"; // start of OSC sequence
                parameter_bytes.clear();
                intermediate_bytes.clear();
                //final_byte = 0;
                printf("Starting OSC sequence\n");  

                //printf("TODO: OSC\n");
                //printf("getchar()\n");
                //getchar();
            }
            else if (data[i] == 0x5b) //'[')
            {
                in_esc_seq = 0;
                in_csi_seq = 1;
                parameter_bytes.clear();
                intermediate_bytes.clear();
                ansi_escape_string = "\\x1b[";
                printf("Starting CSI sequence\n");
            }
            else if (data[i] == 0x37)
            {
                printf("save cursor row %d col %d\n", cursor_row, cursor_col);
                save_cursor_col = cursor_col;
                save_cursor_row = cursor_row;
                in_esc_seq = 0;
            }
            else if (data[i] == 0x38)
            {
                printf("restore cursor to row %d col %d\n", save_cursor_row, save_cursor_col);
                cursor_row = save_cursor_row;
                cursor_col = save_cursor_col;
                buffer->redraw_needed[current_line_index] = true;
                current_line_index = scroll_top + cursor_row - 1;
                buffer->redraw_needed[current_line_index] = true;
                printf("current_line_index is %d\n", current_line_index);
                in_esc_seq = 0;
            }
            else if (data[i] == 0x50) // DCS sequence
            {
                printf("Starting DCS sequence\n");
                in_dcs_seq = 1;
                in_esc_seq = 0;
                parameter_bytes.clear();
                intermediate_bytes.clear();
                ansi_escape_string = "\\x1bP"; // not sure this is right
            }
            else if (data[i] == 0x3D)
            {
                printf("DECKPAM (DEC Keypad Application Mode)\n");
                in_esc_seq = 0;
            }
            else if (data[i] == 0x3E)
            {
                printf("DECKPNM (Retrun keypad to numeric mode)\n");
                in_esc_seq = 0;
            }
            else
            {
                printf("missing sequence handler for type %02X\n", data[i]);
                abort();
                printf("esc seq end\n");
                in_esc_seq = 0;
            }
            
            continue;
        }
        else if (in_dcs_seq > 0)
        {
            printf("DCS byte %02X\n", data[i]);
            if (in_dcs_seq == 1 && data[i] == 0x1b)
            {
                in_dcs_seq = 2;// look for term
            }
            if (in_dcs_seq > 1) // expecting termination
            {
                if (data[i] == 0x1b) {
                    // start of ESC
                }
                else if (data[i] == 0x5c) {
                    printf("Ending DSC (we ate it)\n");
                    in_dcs_seq = 0;
                }
                else {
                    printf("Don't know what to do with DSC termination of %02X\n", data[i]);
                    abort();
                }
            }
            continue;
        }
        else if (in_osc_seq > 0)
        {
            printf("OSC index %i, byte %02X\n", i, data[i]);
            if (in_osc_seq == 1)
            {
                if (data[i] == ';')
                {
                    printf("Have OSC command, now looking for data\n");
                    in_osc_seq = 2; // now collecting the OSC command
                    ansi_escape_string += ';';
                }
                else if (data[i] >= 0x30 && data[i] <= 0x3F)
                {
                    parameter_bytes.push_back(data[i]);
                    ansi_escape_string += data[i];
                }
                continue;
            }
            else if (in_osc_seq == 2)
            {
                if (data[i] == 0x07) // BEL
                {
                    printf("OSC sequence complete: %s\n", ansi_escape_string.c_str());
                    printf("TODO: handle\n");
                    //getchar();
                    //in_osc_seq = 3 // cannot proceed here or we'll eat the next byte which is not correct;
                    in_osc_seq = 0;
                }
                else if (data[i] >= 0x20 && data[i] <= 0x7E) 
                {
                    ansi_escape_string += data[i];
                }
                continue;
            }
            else if (in_osc_seq == 3)
            {
                /*
                if (data[i] == 0x1b) {
                    // start of ESC
                }
                else if (data[i] == 0x5c) {
                    printf("Ending OSC (we ate it)\n");
                    in_osc_seq = 0;
                }
                else {
                    printf("Don't know what to do with OSC termination of %02X\n", data[i]);
                    abort();
                }
                */
                if (strcmp(ansi_escape_string.c_str(), "\\x1b]10;?") == 0) // report terminal foreground color
                {
                    printf("Handling OSC report term foreground color\n");
                    if (ppResponseBytes != NULL)
                    {
                        std::string response = "\x1b]10;rgb:ffff/ffff/ffff\x07";
                        
                        printf("MALLOC %d bytes\n", response.size());
                        char* respBuffer = (char*)malloc(response.size());
                        int j=0;
                        for (int k=0;k<response.size();k++)
                        {
                            respBuffer[j++] = response[k];
                        }

                        int respSize = response.size();

                        if (*ppResponseBytes == NULL)
                        {
                            *ppResponseBytes = respBuffer;
                            nResponseBytes = respSize;
                        }
                        else
                        {
                            //printf("Response buffer already allocated, need to make space for next part\n");
                            //exit(0);
                            printf("Combining buffers - TODO: dump this to validate");

                            char* combinedBuffer = (char*)malloc(respSize + nResponseBytes);
                            for (int k=0;k<nResponseBytes;k++)
                            {
                                combinedBuffer[k] = (*ppResponseBytes)[k];
                            }
                            for (int k=0;k<respSize;k++)
                            {
                                combinedBuffer[nResponseBytes + k] = respBuffer[k];
                            }
                            free(respBuffer);
                            free(*ppResponseBytes);
                            *ppResponseBytes = combinedBuffer;
                            nResponseBytes = respSize + nResponseBytes;
                        }

                        printf("!!!!!!!!!!!!!!!!!!!\n");
                    }

                    in_osc_seq = 0;
                }
                else {
                    printf("No handling yet for OSC sequence '%s'\n", ansi_escape_string.c_str());
                    abort();
                }
                //in_osc_seq = 0; // end of OSC sequence

            }
            continue;
        }
        else if (in_csi_seq > 0) // collecting parameter bytes
        {
            if (data[i] >= 0x30 && data[i] <= 0x3F)
            {
                parameter_bytes.push_back(data[i]);
                ansi_escape_string += data[i];
            }
            else if (data[i] >= 0x20 && data[i] <= 0x2F)
            {
                intermediate_bytes.push_back(data[i]);
                ansi_escape_string += data[i];
            }
            else if (data[i] >= 0x40 && data[i] <= 0x7E)
            {
                final_byte = data[i];
                ansi_escape_string += data[i];

                LOG("INFO: CSI: %s\n", ansi_escape_string.c_str());

                if (final_byte == 'H') // move cursor
                {
                    std::string row;
                    std::string col;
                    
                    if (parameter_bytes.size() == 0)
                    {
                        printf("default to top left\n");
                        cursor_col = 1;
                        cursor_row = 1;
                    }
                    else
                    {
                        int j = 0;
                        for ( ; j<parameter_bytes.size(); j++)
                        {
                            if (parameter_bytes[j] == 0x3b) // ;
                            {
                                j++;
                                break;
                            }
                            row += parameter_bytes[j];
                        }
                        for ( ; j<parameter_bytes.size(); j++)
                        {
                            col += parameter_bytes[j];
                        }
                        
                        if (row.size() == 0)
                        {
                            cursor_row = 1;
                        }
                        else
                        {
                            cursor_row = std::atoi(row.c_str());
                        }

                        if (col.size() == 0)
                        {
                            cursor_col = 1;
                        }
                        else
                        {
                            cursor_col = std::atoi(col.c_str());
                        }

                    }
                    
                    if (row == "999" && col == "999")
                    {
                        //cursor_col = 1;
                        //cursor_col = 1;
                        cursor_col = term_cols;
                        cursor_row = term_rows;
                    }
                    
                    LOG("set cursor to row %d, col %d, line count %d\n", cursor_row, cursor_col, buffer->lines.size());
                    if (cursor_row > scroll_region_max)
                    {
                        printf("TODO: cursor_row %d is past scroll_region_max %d\n", cursor_row, scroll_region_max);
                        //printf("hit enter\n");
                        //getchar();
                    }
                    
                    //while (buffer->lines.size() < cursor_row)
                    while (buffer->lines.size() <= cursor_row)
                    {
                        printf("Error should not do this anymore, hit enter\n");
                        getchar();
                        //printf("error - cursor_y not in range of content -- need to allocate? size is currently %d\n", buffer->lines.size());
                        LOG("allocating new line for updated cursor_row position\n");
                        buffer->lines.push_back(std::string(term_cols, ' '));
                        //buffer->lines.push_back(std::string(""));
                        //exit(0);
                    }
                    buffer->redraw_needed[current_line_index] = true;
                    current_line_index = scroll_top + cursor_row - 1;
                    buffer->redraw_needed[current_line_index] = true;
                    buffer->redraw_all = true;
                }
                else if (final_byte == 'r') // set scrolling region
                {
                    std::string xs;
                    int j = 0;
                    for ( ; j<parameter_bytes.size(); j++)
                    {
                        printf("parameter byte index %d value %02X\n", j, parameter_bytes[j]);
                        if (parameter_bytes[j] == 0x3b) // ;
                        {
                            j++;
                            break;
                        }
                        xs += parameter_bytes[j];
                    }
                    std::string ys;
                    for ( ; j<parameter_bytes.size(); j++)
                    {
                        ys += parameter_bytes[j];
                    }
                    
                    if (xs.size() == 0)
                    {
                        scroll_region_min = 0;
                    }
                    else
                    {
                        scroll_region_min = std::atoi(xs.c_str());
                    }

                    if (ys.size() == 0)
                    {
                        scroll_region_max  = 1;
                    }
                    else
                    {
                        scroll_region_max = std::atoi(ys.c_str());
                    }

                    printf("Set scroll range: %d, %d\n", scroll_region_min, scroll_region_max);

                }
                else if (final_byte == 'J') // erase in display
                {
                    if (parameter_bytes.size() == 0) // clear from cursor to end
                    {
                        buffer->lines.clear();
                        //current_line_index = 0;
                        current_line_index = 0;
                        scroll_top = 0;
                        buffer->lines.push_back(std::string(term_cols, ' '));
                        //buffer->lines.push_back(std::string(""));
                        printf("handled clear escape seq\n");
                        buffer->redraw_all = true;
                    }
                    else
                    {
                        std::string xs;
                        int j = 0;
                        for ( ; j<parameter_bytes.size(); j++)
                        {
                            if (parameter_bytes[j] == 0x3b) // ;
                            {
                                j++;
                                break;
                            }
                            xs += parameter_bytes[j];
                        }
                        std::string ys;
                        for ( ; j<parameter_bytes.size(); j++)
                        {
                            ys += parameter_bytes[j];
                        }

                        if (xs == "2")
                        {
                            printf("erase all?\n");
                            buffer->lines.clear();
                            buffer->redraw_all = true;
                            for (int i=0;i<MAX_VISIBLE_LINES;i++) {
                                buffer->lines.push_back(std::string(term_cols, ' '));
                            }
                            //buffer->lines.push_back(std::string(""));
                            current_line_index = 0;
                        }
                        else
                        {
                            printf("TODO: handle 'J' parameters: %s\n", ansi_escape_string.c_str());
                            //printf("hit enter\n");
                            //getchar();
                            //exit(0);
                        }
                    }
                }
                else if (final_byte == 'C')
                {
                    std::string param1;
                    for (int j=0; j<parameter_bytes.size(); j++)
                    {
                        param1 += parameter_bytes[j];
                    }
                    int amt = 0;
                    if (param1.size() > 0)
                    {
                        amt = std::atoi(param1.c_str());
                    }
                    printf("Cursor forward %d times\n", amt);
                    cursor_col += amt;
                    
                    /*
                    for (int j=0;j<amt;j++)
                    {
                        buffer->lines[current_line_index].push_back(' ');
                    }
                    */
                }
                else if (strcmp(ansi_escape_string.c_str(), "\\x1b[>c") == 0) // Device Attribute Request
                {
                    if (ppResponseBytes != NULL)
                    {
                        std::string response = "\x1b[>0;136;0c";
                        
                        printf("MALLOC %d bytes\n", response.size());
                        char* respBuffer = (char*)malloc(response.size());
                        int j=0;
                        for (int k=0;k<response.size();k++)
                        {
                            respBuffer[j++] = response[k];
                        }

                        int respSize = response.size();

                        if (*ppResponseBytes == NULL)
                        {
                            *ppResponseBytes = respBuffer;
                            nResponseBytes = respSize;
                        }
                        else
                        {
                            //printf("Response buffer already allocated, need to make space for next part\n");
                            //exit(0);
                            printf("Combining buffers - TODO: dump this to validate");

                            char* combinedBuffer = (char*)malloc(respSize + nResponseBytes);
                            for (int k=0;k<nResponseBytes;k++)
                            {
                                combinedBuffer[k] = (*ppResponseBytes)[k];
                            }
                            for (int k=0;k<respSize;k++)
                            {
                                combinedBuffer[nResponseBytes + k] = respBuffer[k];
                            }
                            free(respBuffer);
                            free(*ppResponseBytes);
                            *ppResponseBytes = combinedBuffer;
                            nResponseBytes = respSize + nResponseBytes;
                        }

                        printf("!!!!!!!!!!!!!!!!!!!\n");
                    }
                }
                else if (strcmp(ansi_escape_string.c_str(), "\\x1b[?1049h") == 0) // enable alternative screen buffer
                {
                    printf("switching to alternative screen buffer\n");

                    main_cursor_position = cursor_position;
                    main_cursor_row = cursor_row; 
                    main_cursor_col = cursor_col; 
                    main_current_line_index = current_line_index;

                    current_line_buffer = &alt_line_buffer;
                    buffer = current_line_buffer;
                    
                    if (buffer->lines.size() == 0)
                    {
                        for (int i=0;i<MAX_VISIBLE_LINES;i++)
                        {
                            buffer->lines.push_back(std::string(term_cols, ' '));
                        }
                        //buffer->lines.push_back(std::string(""));
                    }
                    buffer->redraw_all = true;
                    // not sure if this matters, should it be 0?
                    current_line_index = buffer->lines.size()-1;
                }
                else if (strcmp(ansi_escape_string.c_str(), "\\x1b[?1049l") == 0) // disable alternative screen buffer
                {
                    printf("Switching to main  screen buffer\n");
                    current_line_buffer = &line_buffer;
                    buffer = current_line_buffer;
                    
                    if (buffer->lines.size() == 0)
                    {
                        //buffer->lines.push_back(std::string(term_cols, ' '));
                        for (int i=0;i<MAX_VISIBLE_LINES;i++)
                        {
                            buffer->lines.push_back(std::string(term_cols, ' '));
                        }
                        //buffer->lines.push_back(std::string(""));
                    }
                    buffer->redraw_all = true;
                    
                    // TODO: is this suppose to restore some saved position?
                    cursor_position = main_cursor_position;
                    cursor_row = main_cursor_row;
                    cursor_col = main_cursor_col;
                    current_line_index = main_current_line_index;

                    //current_line_index = buffer->lines.size()-1;
                }
                else if (final_byte == 'l' && parameter_bytes.size() > 0 && parameter_bytes[0] == '?') // DEC private mode reset
                {
                    std::string xs;
                    int j = 1;
                    for ( ; j<parameter_bytes.size(); j++)
                    {
                        if (parameter_bytes[j] == 0x3b) // ;
                        {
                            j++;
                            break;
                        }
                        xs += parameter_bytes[j];
                    }
                    std::string ys;
                    for ( ; j<parameter_bytes.size(); j++)
                    {
                        ys += parameter_bytes[j];
                    }
                    
                    if (xs == "l")
                    {
                        printf("TODO: stop cursor blink\n");
                    }
                    else if (xs == "25")
                    {
                        printf("TODO: reset (turn off) cursor visibility\n");
                       // printf("DEBUG: hit enter\n");
                       // getchar();
                    }
                    else if (xs == "1")
                    {
                        printf("TODO reset: application cursor keys\n");
                    }
                    else if (xs == "12")
                    {
                        printf("TODO reset blinking cursor at rate %s\n", ys.c_str());
                    }
                    else if (xs == "25")
                    {
                        printf("TODO reset: show cursor\n");
                    }
                    else if (xs == "1000")
                    {
                        printf("TODO reset: enable xterm mouse reporting\n");
                    }
                    else if (xs == "1002")
                    {
                        printf("TODO reset: enable mouse motion events\n");
                    }
                    else if (xs == "2004")
                    {
                        printf("TODO reset: handle bracketed paste mode\n");
                    }
                    // \x1b[?1004h
                    else if (xs == "1004")
                    {
                        printf("TODO reset: enable focus tracking\n");
                    }
                    else
                    {
printf("hit enter after this\n");
                        printf("TODO: handle reset some other dec mode %s", ansi_escape_string.c_str());
                        printf("ys: %s\n", ys.c_str());
                        printf("xs: %s\n", xs.c_str());
getchar();
                        abort();
                    }
                }
                else if (final_byte == 'm')
                {
                    if (parameter_bytes.size() == 0)
                    {
                        // is this SGR '0' default?
                        printf("normal character attributes\n");
                    }
                    else
                    {
                        // TODO: I think technically this can have more than 2 parameters
                        std::string xs;
                        int j = 0;
                        for ( ; j<parameter_bytes.size(); j++)
                        {
                            if (parameter_bytes[j] == 0x3b) // ;
                            {
                                j++;
                                break;
                            }
                            xs += parameter_bytes[j];
                        }
                        std::string ys;
                        for ( ; j<parameter_bytes.size(); j++)
                        {
                            ys += parameter_bytes[j];
                        }
                        
                        if (xs == "7")
                        {
                            printf("TODO: reverse ON\n");
                        }
                        else if (xs == "27")
                        {
                            printf("TODO: reverse OFF\n");
                        }
                        else
                        {
                            printf("TODO: handle 'm' correctly: %s\n", ansi_escape_string.c_str());
                            //exit(0);
                        }
                    }
                }
                else if (final_byte == 'h' && parameter_bytes.size() > 0 && parameter_bytes[0] == '?') // DEC private mode
                {
                    std::string xs;
                    int j = 1;
                    for ( ; j<parameter_bytes.size(); j++)
                    {
                        if (parameter_bytes[j] == 0x3b) // ;
                        {
                            j++;
                            break;
                        }
                        xs += parameter_bytes[j];
                    }
                    std::string ys;
                    for ( ; j<parameter_bytes.size(); j++)
                    {
                        ys += parameter_bytes[j];
                    }

                    
                    if (xs == "1")
                    {
                        printf("TODO: application cursor keys\n");
                        printf("INFO: %s\n", ansi_escape_string.c_str());
                        printf("xs: %s\n", xs.c_str());
                        printf("ys: %s\n", ys.c_str());
                        // this is supposed to make it so that arrow keys send "ESC O A, ESC O B, etc." instead of "ESC [ A, ESC [ B, etc."
                        //printf("hit enter\n");
                        //getchar();
                    }
                    else if (xs == "12")
                    {
                        printf("TODO start blinking cursor at rate %s\n", ys.c_str());
                    }
                    else if (xs == "25")
                    {
                        printf("TODO: show cursor\n");
                    }
                    else if (xs == "1000")
                    {
                        printf("TODO: enable xterm mouse reporting\n");
                    }
                    else if (xs == "1002")
                    {
                        printf("TODO: enable mouse motion events\n");
                    }
                    else if (xs == "2004")
                    {
                        printf("TODO: handle bracketed paste mode\n");
                    }
                    // \x1b[?1004h
                    else if (xs == "1004")
                    {
                        printf("TODO: enable focus tracking\n");
                    }
                    else
                    {
                        printf("INFO: DEC Private Mode (set)\n");
                        printf("INFO: %s\n", ansi_escape_string.c_str());
                        printf("xs: %s\n", xs.c_str());
                        printf("ys: %s\n", ys.c_str());
                        printf("hit enter\n");
                        getchar();

                        //printf("TODO: handle some other dec mode %s", ansi_escape_string.c_str());
                        //printf("getchar()\n");
                        //getchar();
                        //exit(0);
                    }
                }
                else if (final_byte == 'L') // insert blank lines
                {
                    std::string xs;
                    int j = 1;
                    for ( ; j<parameter_bytes.size(); j++)
                    {
                        if (parameter_bytes[j] == 0x3b) // ;
                        {
                            j++;
                            break;
                        }
                        xs += parameter_bytes[j];
                    }
                    std::string ys;
                    for ( ; j<parameter_bytes.size(); j++)
                    {
                        ys += parameter_bytes[j];
                    }
                    printf("TODO: insert blank lines\n");
                    printf("INFO: %s\n", ansi_escape_string.c_str());
                    printf("xs: %s\n", xs.c_str());
                    printf("ys: %s\n", ys.c_str());

                    LOG("Scrolling down by one, range %d to %d\n", scroll_region_min, scroll_region_max);
/*
                    LOG("Before:\n");
                    int cnt=0;
                    for (auto& it : buffer->lines) {
                        LOG("%d %s\n", cnt++, it.c_str());
                    }
*/
                    shift_lines_down(buffer->lines, scroll_region_min-1, scroll_region_max-1);
                    buffer->redraw_all = true;

 /*                   LOG("after:\n");
                    cnt = 0;
                    for (auto& it : buffer->lines) {
                        LOG("%d %s\n", cnt++, it.c_str());
                    }
*/
                }
                else if (final_byte == 'K') // erase part of a line
                {
                    if (parameter_bytes.size() == 0)
                    {
                        // TODO: this is supposed to be 'clear from cursor x to end of line' but we aren't tracking the cursor yet
                        //buffer->lines[current_line_index].erase(buffer->lines[current_line_index].size() - 1);
                        printf("Handling ESC [ K \n");
                        printf("Before: '%s'\n", buffer->lines[current_line_index].c_str());
                        //exit(0);
                        //buffer->lines[current_line_index].clear();
                        //buffer->lines[current_line_index].erase(cursor_col-1, 
                        for (int j=cursor_col-1; j<buffer->lines[current_line_index].size();j++)
                        {
                            buffer->lines[current_line_index][j] = ' ';
                        }

                        printf("After : '%s'\n", buffer->lines[current_line_index].c_str());
                    }
                    else
                    {
                        if (parameter_bytes[0] == 0x32) // 2 is "erase mode"
                        {
                            buffer->lines[current_line_index] = std::string(term_cols, ' ');
                            //buffer->lines.push_back(std::string(""));
                        }
                        else
                        {
                            printf("TODO: handle 'K' properly\n");
                            for (int j=0;j<parameter_bytes.size();j++)
                            {
                                printf(" %02X", parameter_bytes[j]);
                            }
                            
                            printf("getchar()\n");
                            getchar();
                            exit(0);
                        }
                    }
                    
                }
                /*else if (strcmp(ansi_escape_string.c_str(), "\\x1b[7m") == 0) // reverse
                {
                    printf("TODO: reverse ON\n");
                }
                else if (strcmp(ansi_escape_string.c_str(), "\\x1b[27m") == 0) // reverse
                {
                    printf("TODO: reverse OFF\n");
                }
                */
                else if (strcmp(ansi_escape_string.c_str(), "\\x1b[r") == 0) // reset scroll region?
                {
                    printf("TODO: reset scroll restrictions\n");
                }
                else if (strcmp(ansi_escape_string.c_str(), "\\x1b[?1h") == 0) // enable cursor key mode
                {
                    cursor_key_mode = 1;
                    printf("enabled cursor key mode\n");
                }
                else if (strcmp(ansi_escape_string.c_str(), "\\x1b[6n") == 0) // DSR
                {
                    printf("DSR\n");
                    // TODO: technically we could need to include multiple responses
                    if (ppResponseBytes != NULL)
                    {
                        //std::string xs = std::to_string(cursor_x);
                        //std::string ys = std::to_string(cursor_y);
                        std::string row = std::to_string(cursor_row);
                        //std::string column = "80"; //"15";
                        std::string column = std::to_string(cursor_col);

                        printf("Responding to DSR report cursor position: row %d col %d\n", cursor_row, cursor_col);

                        int new_nResponseBytes = 2 /* ESC[ */ + strlen(row.c_str()) + 1 /* ; */ + strlen(column.c_str()) + 1 /* R */;
                        printf("Allocating %d bytes for response\n", new_nResponseBytes);
                        char* respBuffer = (char*)malloc(new_nResponseBytes);

                        int j=0;
                        respBuffer[j++] = 0x1b; // ESC
                        respBuffer[j++] = 0x5b; // [

                        for (int k=0;k<row.size();k++)
                        {
                            respBuffer[j++] = row[k];
                        }
                        respBuffer[j++] = 0x3b; // ;
                        
                        for (int k=0;k<column.size();k++)
                        {
                            respBuffer[j++] = column[k];
                        }
                        printf("j is %d before final byte\n", j);
                        respBuffer[j++] = 'R'; // final byte

                        printf("n = %d and i = %d\n", n, i);

                        //int respSize = j; // response.size();
                        int respSize = new_nResponseBytes;
                        

                        if (*ppResponseBytes == NULL)
                        {
                            *ppResponseBytes = respBuffer;
                            nResponseBytes = respSize;
                        }
                        else
                        {
                            //printf("Response buffer already allocated, need to make space for next part\n");
                            //exit(0);
                            printf("Combining buffers - TODO: dump this to validate");

                            char* combinedBuffer = (char*)malloc(respSize + nResponseBytes);
                            for (int k=0;k<nResponseBytes;k++)
                            {
                                combinedBuffer[k] = (*ppResponseBytes)[k];
                            }
                            for (int k=0;k<respSize;k++)
                            {
                                combinedBuffer[nResponseBytes + k] = respBuffer[k];
                            }
                            free(respBuffer);
                            free(*ppResponseBytes);
                            *ppResponseBytes = combinedBuffer;
                            nResponseBytes = respSize + nResponseBytes;
                        }
                    }
                }
                else if (final_byte == 'P') // del
                {
                    int ndelete = get_integer_or_default(parameter_bytes, 1);
                    printf("CSI delete %d chars\n", ndelete);

                    printf("Before: '%s'\n", buffer->lines[current_line_index].c_str());
                    printf("cursor_col: %d\n", cursor_col);
                    if (buffer->lines[current_line_index].size() > 0)
                    {
                        for (int j=0;j<ndelete;j++)
                        {
                            buffer->lines[current_line_index].erase(cursor_col-1, 1);
                            buffer->lines[current_line_index] += ' '; // keep the correct number of characters in the line buffer
                            //cursor_col--;
                        }
                    }
                    printf("After:  '%s'\n", buffer->lines[current_line_index].c_str());
                    printf("cursor_col: %d\n", cursor_col);
                }
                else if (final_byte == '@') // append spaces
                {
                    int nchars = get_integer_or_default(parameter_bytes, 1);
                    printf("CSI append %d spaces\n", nchars);

                    printf("Before: '%s'\n", buffer->lines[current_line_index].c_str());
                    printf("cursor_col: %d\n", cursor_col);
                    if (buffer->lines[current_line_index].size() > 0)
                    {
                        for (int j=0;j<nchars;j++)
                        {
                            printf("APPEND ' '\n");
                            if (cursor_col-1 > term_cols-1) {
                                printf("wrong calculation 1 (hit enter)\n");
                                getchar();
                                abort();
                            }
                            buffer->lines[current_line_index][cursor_col-1] = ' ';
                            // TODO: move or not?
                            //cursor_col++;
                        }
                    }
                    printf("After:  '%s'\n", buffer->lines[current_line_index].c_str());
                    printf("cursor_col: %d\n", cursor_col);
                }
                else
                {
                    printf("TOOD: handle csi sequence: ");
                    for (int j=0;j<parameter_bytes.size();j++)
                    {
                        printf(" %02X", parameter_bytes[j]);
                    }
                    for (int j=0;j<intermediate_bytes.size();j++)
                    {
                        printf(" %02X", intermediate_bytes[j]);
                    }
                    printf(" %02X ('%c')\n", final_byte, final_byte);
                    printf("final sequence in ascii: %s\n", ansi_escape_string.c_str());
                    //exit(0);
                }

                in_csi_seq = 0;
            }
            else
            {
                // not expected
                in_csi_seq = 0;
            }
            
            continue;
        }

        else if (in_esc_seq == 2)
        {
            printf("next esc byte %02X\n", data[i]);

            if (data[i] == 0x37)
            {
                printf("TODO: set cursor to home position\n");
            }
            else if (data[i] == 0x48) //'H') // move cursor to home
            {
                printf("cursor to home\n");
                cursor_row = 1;
                cursor_col = 1;
                current_line_index = 0;
            }
            else if (data[i] == 0x4A) //'J')
            {
                // clear from position to end of display
                // TODO: handle other cursor positions
                //current_line[0] = '\0';
                
                /* TODO 
                for (int j=0;j<buffer->count;j++)
                {
                    buffer->lines[j][0] = '\0';
                }
                buffer->count = 1; // one line for the current line
                current_line = buffer->lines[buffer->count];
                offset = 0;
                printf("handled clear escape seq\n");
                */

                buffer->lines.clear();
                buffer->redraw_all = true;
                current_line_index = 0;
                buffer->lines.push_back(std::string(term_cols, ' '));
                //buffer->lines.push_back(std::string(""));
                printf("handled clear escape seq\n");
            }
            printf("esc seq end\n");
            in_esc_seq = 0;
            continue;
        }
        else if (in_esc_seq == 3)
        {
            in_esc_seq = 0;
            continue;
        }
        /*else if (strcmp(data, "\b \b") == 0)
        {
            printf("Erasing...\n");
            if (buffer->lines[current_line_index].size() > 0)
            {
                buffer->lines[current_line_index].erase(buffer->lines[current_line_index].size() - 1);
            }
            break;
        }*/
        else if (data[i] == '\n')
        {
            printf("got newline, cursor_row is %d\n", cursor_row);
            // TODO: fix the need for this
            buffer->redraw_all = true;

            if (cursor_row == scroll_region_max || cursor_row == buffer->lines.size()-1)
            {
                //LOG("scrolling down a line\n");
// try not using scroll_top
                // scroll_top++;
                
                LOG("Scrolling by one, range %d to %d\n", scroll_region_min, scroll_region_max);

/*                LOG("Before:\n");
                int cnt=0;
                for (auto& it : buffer->lines) {
                    LOG("%d %s\n", cnt++, it.c_str());
                }
*/
                if (scroll_region_min > 0 && scroll_region_max > 0)
                    shift_lines_up(buffer->lines, scroll_region_min-1, scroll_region_max-1);
                else
                    shift_lines_up(buffer->lines, 0, buffer->lines.size()-1);

                buffer->redraw_all = true;

 /*               LOG("after:\n");
                cnt = 0;
                for (auto& it : buffer->lines) {
                    LOG("%d %s\n", cnt++, it.c_str());
                }
*/

                //LOG("allocating new line and scrolling, new scroll_top is %d\n", scroll_top);
                //cursor_row++;            
                //buffer->lines.push_back(std::string(term_cols, ' '));
                //current_line_index = cursor_row - 1;
                //current_line_index = scroll_top + cursor_row - 1;
                cursor_col = 1;
            }
            else 
            {
                cursor_row++;            
                buffer->redraw_needed[current_line_index] = true;
                current_line_index = scroll_top + cursor_row - 1;
                buffer->redraw_needed[current_line_index] = true;
                cursor_col = 1;
            }
/*
            else
            {
                //printf("what is this case?\n");
                //printf("hit enter\n");
                //getchar();
                cursor_row++;            
                while (buffer->lines.size() < cursor_row)
                {
                    //printf("error - cursor_y not in range of content -- need to allocate? size is currently %d\n", buffer->lines.size());
                    LOG("allocating new line for updated cursor_row position (2), term_cols %d, lines size %d\n", term_cols, buffer->lines.size());
                    
                    buffer->lines.push_back(std::string(term_cols, ' '));
                    //buffer->lines.push_back(std::string(""));
                }
                //current_line_index = cursor_row - 1;
                current_line_index = scroll_top + cursor_row - 1;
                cursor_col = 1;
            }
*/
        }
        else
        {
            buffer->redraw_needed[current_line_index] = true;

            if (data[i] == 0x07) // BEL or "beep"
            {
                printf("TODO: BEEP\n");
            }
            else if (data[i] == '\t')
            {
                LOG("Appending tab\n");
                int tab_width = 8;
                int tabspace = tab_width - ((cursor_col-1) % tab_width);
                for (int j=0;j<tabspace;j++)
                {
                    //current_line[len+offset+i] = ' ';
                    //current_line += " ";
                    
                    if (cursor_col > term_cols-1) {
                        printf("wrong calculation 2 (hit enter)\n");
                        getchar();
                        abort();
                    }
                    buffer->lines[current_line_index][cursor_col] = ' ';
                    //buffer->lines[current_line_index].push_back(' ');

                    LOG("APPEND ' ' (for tab)\n");
                    cursor_col++;
                    //if (cursor_col % tab_spacing == 0)
                    //    break;
                }
            }
            else if (data[i] == '\r')
            {
                cursor_col = 1;
                buffer->redraw_needed[current_line_index] = true;
            }
            else if (data[i] == '\b')
            {
                if (buffer->lines[current_line_index].size() > 0)
                {
                    cursor_col--;
                    buffer->redraw_needed[current_line_index] = true;
                    //buffer->lines[current_line_index].erase(buffer->lines[current_line_index].size() - 1);
                }
            }
            else 
            {
                // verbose logging of characters appended on lines (not esc sequences)
                //LOG("APPEND (index %d) %02X '%c' to %d\n", i, data[i], data[i], cursor_col-1);
               // LOG("TODO: this code has a memory issue\n");
                
                //buffer->lines[current_line_index][cursor_col-1] = data[i];

                // if needed, wrap to the next line
                if (cursor_col > 0 && term_cols < cursor_col) {
                    if (cursor_row == scroll_region_max || cursor_row == buffer->lines.size()-1)
                    {
                        LOG("Scrolling by one, range %d to %d\n", scroll_region_min, scroll_region_max);
        /*                LOG("Before:\n");
                        int cnt=0;
                        for (auto& it : buffer->lines) {
                            LOG("%d %s\n", cnt++, it.c_str());
                        }
        */
                        if (scroll_region_min > 0 && scroll_region_max > 0)
                            shift_lines_up(buffer->lines, scroll_region_min-1, scroll_region_max-1);
                        else
                            shift_lines_up(buffer->lines, 0, buffer->lines.size()-1);

                        buffer->redraw_all = true;

         /*               LOG("after:\n");
                        cnt = 0;
                        for (auto& it : buffer->lines) {
                            LOG("%d %s\n", cnt++, it.c_str());
                        }
        */

                        cursor_col = 1;
                    }
                    else 
                    {
                        cursor_row++;            
                        current_line_index = scroll_top + cursor_row - 1;
                        cursor_col = 1;
    /*
                        printf("TODO: why is resize needed here? cursor_col %d, line size %d\n", cursor_col, line.size());
                        getchar();
                        line.resize(cursor_col, ' '); // pad with spaces or your default fill char
    */
                    }

                }
                std::string& line = buffer->lines[current_line_index];
                buffer->redraw_needed[current_line_index] = true;

                if (cursor_col -1 >= line.size()) {
                    printf("about to assert, cursor_col is %d, line size is %d\n", cursor_col, line.size());
                    getchar();
                }
                assert(cursor_col-1 < line.size());
                // Now it's safe to write
                line[cursor_col - 1] = data[i];                


                //buffer->lines[current_line_index].push_back(data[i]);
                cursor_col++;
                
//                printf("AFTER (%d) '%s'\n", current_line_index, buffer->lines[current_line_index].c_str());

                //printf("current line: %s\n", current_line.c_str());
                //current_line[len+offset+1] = '\0';
            }
        }
    }

//    printf("calling update_framebuffer\n");
    update_framebuffer();
}
