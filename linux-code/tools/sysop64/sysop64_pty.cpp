/*
 * Sysop-64 
 * https://github.com/Bloodmosher/Sysop-64 
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project 
 *
 * sysop64_pty.cpp
 *
 * Pseudoterminal (PTY) management and the main I/O event loop.
 *
 * pty_loop() creates a PTY pair, configures the slave in raw mode, forks a
 * child process that executes /usr/bin/login with the slave as its
 * controlling terminal, then enters a select()-based event loop in the
 * parent. The loop multiplexes:
 *
 *   - Physical keyboard events (current_keyboard_fd) → map to ASCII and
 *     write to the PTY master.
 *   - PTY master output → feed to process_buffer() for terminal emulation.
 *   - C64 keyboard scan via pipe (pipe_fds) when console mode is active.
 *   - inotify events → hot-plug keyboard detection.
 *   - Console button polling and message-display updates on each iteration.
 *
 * Returns when the child process exits (EOF on PTY master).
 */

#include "sysop64_internal.h"

// Creates a PTY pair, forks a login shell on the slave, and runs the main
// I/O event loop until the child exits. See file header for full details.
int pty_loop() {
    int ptm, pts;
    char slave_path[20];

    // Create a pseudoterminal (master)
    //if ((ptm = posix_openpt(O_RDWR)) == -1) {
    if ((ptm = posix_openpt(O_RDWR | O_NOCTTY)) == -1) {
        error_exit("posix_openpt");
    }

    // Grant access to the slave pseudoterminal
    if (grantpt(ptm) == -1 || unlockpt(ptm) == -1) {
        error_exit("grantpt/unlockpt");
    }

    // Get the path of the slave pseudoterminal
    if (ptsname_r(ptm, slave_path, sizeof(slave_path)) != 0) {
        error_exit("ptsname_r");
    }

    printf("Pseudoterminal created: %s\n", slave_path);

    // Open the slave pseudoterminal
    //if ((pts = open(slave_path, O_RDWR)) == -1) {
    if ((pts = open(slave_path, O_RDWR | O_NOCTTY)) == -1) {
        error_exit("open slave");
    }

    init_keyboard_monitor();

    int mouse_fd = open(MOUSE_EVENT_FILE, O_RDONLY);
    if (mouse_fd == -1) {
        printf("Error opening mouse\n");
    }

    int pipe_fds[2]; // Pipe file descriptors: pipe_fds[0] is read, pipe_fds[1] is write.

        // Pipe used to pass Linux input_event structs from scan_keys_pipe()
        // (which runs inline in this loop) to the keyboard-read path below.
    if (pipe(pipe_fds) == -1) {
        printf("Error opening pipe");
        exit(EXIT_FAILURE);
    }

    if (pipe_fds[0] == -1 || pipe_fds[1] == -1) {
        printf("Error creating pipe");
        exit(EXIT_FAILURE);
    }


    // Fork a child process
    pid_t pid = fork();

    //int mfd;
    //pid_t pid = forkpty(&mfd, NULL, NULL, NULL);

    if (pid == -1) {
        error_exit("fork");
    } else if (pid == 0) {
        // Child: replace stdin/stdout/stderr with the PTY slave and exec login.
        close(ptm);  // Close the master pseudoterminal in the child process

        setsid();
        ioctl(pts, TIOCSCTTY, 0);

/*
        if (setpgid(0, 0) == -1) {
            perror("setpgid");
            exit(EXIT_FAILURE);
        }
*/
        struct termios slave_orig_term_settings; // Saved terminal settings
        struct termios new_term_settings; // Current terminal settings        

        // Save the default parameters of the slave side of the PTY
        int rc = tcgetattr(pts, &slave_orig_term_settings);


        // Set raw mode on the slave side of the PTY
        new_term_settings = slave_orig_term_settings;
        cfmakeraw(&new_term_settings);
    //    new_term_settings.c_lflag &= ~(ICANON | ECHO);
        new_term_settings.c_lflag |= ECHO;

        tcsetattr(pts, TCSANOW, &new_term_settings);

        //set_raw_mode(pts);
        struct winsize ws;
        ws.ws_row = term_rows;
        //ws.ws_col = 80;
        ws.ws_col = term_cols;

        if (ioctl(pts, TIOCSWINSZ, &ws) == -1)
        {
            perror("Error setting slave terminal size\n");
        }
        
//        ioctl(pts, TIOCSCTTY, 0);

        // Duplicate the slave pseudoterminal to stdin, stdout, and stderr
        dup2(pts, 0);
        dup2(pts, 1);
        dup2(pts, 2);

        // Close the original file descriptor for the slave pseudoterminal
        close(pts);

        // Execute a shell in the child process
        //execlp("/bin/sh", "sh", NULL);
        //execlp("/bin/bash", "bash", NULL);

        //execlp("/bin/bash", "bash", "-i", "-l", NULL);
        execlp("/usr/bin/login", "login", NULL);


        perror("execlp");
        exit(EXIT_FAILURE);
    } else {
        // Parent: drive the select() event loop.
        close(pts);  // Close the slave pseudoterminal in the parent process

        int tty = open("/dev/tty", O_RDWR);
        if (tty >= 0) {
            ioctl(tty, TIOCNOTTY);  // drop controlling tty
            close(tty);
            printf("DROPPED CONTROLLING TTY\n");
        }

        // Set the terminal attributes for the master pseudoterminal
        //struct termios tm;
        //tcgetattr(ptm, &tm);
        //cfmakeraw(&tm);
        //tm.c_lflag &= ~(ICANON | ECHO);
        //tcsetattr(ptm, TCSANOW, &tm);

        //input_loop();
        // Use select to monitor both the keyboard and the pseudoterminal
        fd_set fds;


        //struct libevdev *dev = NULL;
        //int keyboard_fd = open(KEYBOARD_EVENT_FILE, O_RDONLY | O_NONBLOCK);
        //libevdev_new_from_fd(keyboard_fd, &dev);
        struct input_event ev;

        ssize_t n;

        int wait_for_key_release = 0;
        int wait_for_c64_key_release = 0;
        
        char keyboard_buffer[100];
        printf("input_event size is %d\n", sizeof(struct input_event));
        ssize_t keyboard_buffer_bytes = 0;

        int nResponseBytes = 0;
        char* pResponseBytes = NULL;

        while (1) {
            FD_ZERO(&fds);
            if (current_keyboard_fd != -1) {
                FD_SET(current_keyboard_fd, &fds);
            }
            if (mouse_fd != -1) {
                FD_SET(mouse_fd, &fds);
            }
            if (pipe_fds[0] != -1) {
                FD_SET(pipe_fds[0], &fds);
            }

            FD_SET(inotify_fd, &fds);

            FD_SET(ptm, &fds);

            int max_fd = (current_keyboard_fd > ptm) ? current_keyboard_fd : ptm;
            max_fd = (mouse_fd > max_fd) ? mouse_fd : max_fd;
            max_fd = (pipe_fds[0] > max_fd) ? pipe_fds[0] : max_fd;
            if (inotify_fd > max_fd) max_fd = inotify_fd;
            //int max_fd = ptm;

            struct timeval timeout;
            timeout.tv_sec = 0;
            //timeout.tv_usec = 100000;
            timeout.tv_usec = 10000; // shorter timeout if we are scanning the c64 keyboard in this loop

            // Wait for activity on either the keyboard or pseudoterminal
            if (select(max_fd + 1, &fds, NULL, NULL, &timeout) == -1) {
                error_exit("select");
            }

            if (framebuffer_update_needed)
                update_framebuffer();
            

        //while(1) {
            
            if (c64_console_active && console_yield_lock) {
                // release dma but keep the frame buffer visible for now
                c64_console_active = 0;
                console_yield_occurred = 1;
                printf("Console yielding lock\n");
                console_release_lock();

                if (console_close_requested) {
                    if (framebuffer_visible) {
                        toggle_ui_visibility();
                    }
                    console_close_requested = 0;
                }
            }
            if (!c64_console_active && console_reacquire_lock) {
                // reacquire dma lock
                console_acquire_lock();
                c64_console_active = 1;
                console_reacquire_lock = 0;
                console_yield_occurred = 0;
                console_yield_lock = 0; // TODO: there could be more of these pending right?
                printf("Console reacquired  lock\n");
            }

            if (c64_console_active) {
                //sysop_server_dma_lock();
                //printf("Scanning keys in c64 console mode\n");
                scan_keys_pipe(pipe_fds[1]);
                //sysop_server_dma_unlock();
            }
          //  usleep(20000); // Sleep for 20 milliseconds
        //}
            
            handle_console_button();
            handleConsoleCloseRequest();
            
            handleMessageDisplay();
           


            // Check if there is data to read from the keyboard

        /*    if (FD_ISSET(keyboard_fd, &fds)) {
                n = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);

                if (n == 0) {
                    // Write the event to the pseudoterminal
                    //write(ptm, &ev, sizeof(ev));
                    map_key(&ev);
                } else if (n == -EAGAIN) {
                    // No events available, handle accordingly
                    // ...
                } else {
                    // Error or end of events, handle accordingly
                    // ...
                }                
            }
            */

            // Check for device events
            if (FD_ISSET(inotify_fd, &fds)) {
                process_inotify_events();
            }

/*
            if (FD_ISSET(mouse_fd, &fds)) {
                unsigned char data[3];
                int left, middle, right;
                signed  char x, y;
                ssize_t bytesRead = read(mouse_fd, data, sizeof(data));

                if (bytesRead > 0)
                {
                    left = data[0] & 0x1;
                    right = data[0] & 0x2;
                    middle = data[0] & 0x4;

                    if (right && left) 
                    {
                        toggle_ui_visibility();
                    }
                    
                    x = data[1];
                    y = data[2];
                    //printf("x=%d, y=%d, left=%d, middle=%d, right=%d\n", x, y, left, middle, right);

                    int new_mouse_x = g_mouse_x;
                    int new_mouse_y = g_mouse_y;

                    if (x != 0) {
                        new_mouse_x += x;
                        
                        if (new_mouse_x < 0)
                            new_mouse_x = 0;
                        
                        if (new_mouse_x > g_framebuffer_width)
                            new_mouse_x = g_framebuffer_width;
                    }
                    
                    if (y != 0) {
                        new_mouse_y += -y;
                        
                        if (new_mouse_y < 0)
                            new_mouse_y = 0;
                        
                        if (new_mouse_y > g_framebuffer_height)
                            new_mouse_y = g_framebuffer_height;
                    }

                    g_prev_mouse_x = g_mouse_x;
                    g_prev_mouse_y = g_mouse_y;

                    g_mouse_x = new_mouse_x;
                    g_mouse_y = new_mouse_y;

                    update_framebuffer();
                }   

                //ssize_t bytesRead = read(mouse_fd, &ev, sizeof(struct input_event));
                //if (bytesRead == sizeof(struct input_event)) {
                //    if (ev.type == EV_REL && (ev.code == REL_X || ev.code == REL_Y)) {
                //        printf("Mouse moved in the %s direction: %d\n", (ev.code == REL_X) ? "X" : "Y", ev.value);
                //    } else if (ev.type == EV_KEY && (ev.code == BTN_LEFT || ev.code == BTN_RIGHT) && ev.value == 1) {
                //        printf("%s mouse button pressed\n", (ev.code == BTN_LEFT) ? "Left" : "Right");
                //    }
               // }
            }
*/

            // c64 keyboard
            if (FD_ISSET(pipe_fds[0], &fds)) {
                //char buffer[16];
                //ssize_t n = read(keyboard_fd, buffer, sizeof(buffer));

                int readSize = sizeof(struct input_event) - keyboard_buffer_bytes;
//                printf("Reading %d bytes\n", readSize);
//                keyboard_buffer_bytes += read(keyboard_fd, &keyboard_buffer[keyboard_buffer_bytes], readSize);
                keyboard_buffer_bytes += read(pipe_fds[0], &keyboard_buffer[keyboard_buffer_bytes], readSize);

                struct input_event* ev = (struct input_event*)keyboard_buffer;
                if (keyboard_buffer_bytes == sizeof(struct input_event))
                {
                    keyboard_buffer_bytes = 0; // reset for next read
                    //ssize_t bytesRead = read(keyboard_fd, &ev, sizeof(struct input_event));
                    //if (bytesRead == sizeof(struct input_event) && ev.type == EV_KEY) { //} && ev.value == 1) {
                    if (ev->type == EV_KEY) 
                    { //} && ev.value == 1) {
//                        printf("Key Event Code=%d, Value=%d, Mapped=%s, c64_ctrl_pressed=%d, c64_shift_pressed=%d\n", ev->code, ev->value, libevdev_event_code_get_name(ev->type, ev->code), c64_ctrl_pressed, c64_shift_pressed);
                        //map_key(&ev);
                        
                        if (ev->code == KEY_ESC && ev->value > 0 && c64_ctrl_pressed && wait_for_c64_key_release == 0)
                        {
                            wait_for_c64_key_release = 1;
                            // toggle_ui_visibility();

                            if (framebuffer_visible) {
                                toggle_ui_visibility();
                            }
                            console_release_lock();
                            c64_console_active = 0;
                        }
                        else if (ev->code == KEY_ESC && ev->value == 0 && wait_for_c64_key_release == 1)
                        {
                            wait_for_c64_key_release = 0;
                        }
                        else if (ev->code == KEY_UP)
                        {
                            const char* escape_sequence = "\x1B[A";
                            if (ev->value == 1) 
                            {
                                write(ptm, escape_sequence, strlen(escape_sequence));
                            }
                        }
                        else if (ev->code == KEY_DOWN)
                        {
                            // for c64 keyboard the keys are combined and shift is used
                            if (c64_shift_pressed)
                            {
                                const char* escape_sequence = "\x1B[A";
                                if (ev->value == 1) 
                                {
                                    write(ptm, escape_sequence, strlen(escape_sequence));
                                }
                            }
                            else
                            {                            
                                const char* escape_sequence = "\x1B[B";
                                if (ev->value == 1) 
                                {
                                    write(ptm, escape_sequence, strlen(escape_sequence));
                                }
                            }
                        }
                        else if (ev->code == KEY_LEFT)
                        {
                            const char* escape_sequence = "\x1B[D";
                            if (ev->value == 1) 
                            {
                                write(ptm, escape_sequence, strlen(escape_sequence));
                            }
                        }
                        else if (ev->code == KEY_RIGHT)
                        {
                            // for c64 keyboard the keys are combined and shift is used
                            if (c64_shift_pressed)
                            {
                                const char* escape_sequence = "\x1B[D";
                                if (ev->value == 1) 
                                {
                                    write(ptm, escape_sequence, strlen(escape_sequence));
                                }
                            }
                            else
                            {
                                const char* escape_sequence = "\x1B[C";
                                if (ev->value == 1) 
                                {
                                    write(ptm, escape_sequence, strlen(escape_sequence));
                                }
                            }
                        }
                        else if (ev->code == KEY_TAB)
                        {
                            const char* escape_sequence = "\t";
                            if (ev->value == 1) 
                            {
                                write(ptm, escape_sequence, strlen(escape_sequence));
                            }
                        }
                        else if (ev->code == KEY_BACKSPACE)
                        {
                            const char* escape_sequence = "\b \b";
                            //const char* escape_sequence = "\b ";
                            if (ev->value == 1) 
                            {
                                //printf(escape_sequence);
                                //fflush(stdout);

                                // no - you'll do it twice!!
                                //process_buffer(escape_sequence, strlen(escape_sequence), NULL, nResponseBytes);
                               
                                write(ptm, escape_sequence, strlen(escape_sequence));
                            }
                        }
                        else
                        {
                            unsigned char c = map_key_with_c64_mapping(ev);
                        
//here1
//                            if (ev->code == KEY_ESC && ev->value > 0 && c64_ctrl_pressed && wait_for_c64_key_release == 0)
                            if (c == 3 && ev->value == 1) // ctrl -c
                            {
                                printf("sending ctrl-c\n");
                                sendSignalToProcessesWithSameSID(pid, SIGINT);
                            }
                            else
                            {
                                //printf("map_key returned '%c'\n", c);
                                if (ev->value == 1 && c != 0)
                                {
                                    //echo here
                                    //printf("%c", c); // for debugging print this
                                    //fflush(stdout);
                                    
                                    //char buf[2];
                                    //sprintf(buf, "%c", c);
                                    //process_buffer(buf, 1, NULL, nResponseBytes);

                                    write(ptm, &c, 1);
                                }
                            }
                            
                        }
                        //write(ptm, &ev, sizeof(ev));
                    }
                    else
                    {
//                        printf("Skipped event of type %d\n", ev->type);
                    }

                    //printf("ctrl pressed state after processing: %d\n", ctrl_pressed);
//                    printf("Key Event Code=%d, Value=%d, Mapped=%s, ctrl_pressed=%d, wait_for_key_release=%d, frame_buffer=%d\n", ev->code, ev->value, libevdev_event_code_get_name(ev->type, ev->code), ctrl_pressed, wait_for_key_release, pFrameBuffer == pFrameBuffer1 ? 1 : 2);
                }
                else
                {
                    printf("SKIPPED READ because was too small?\n");
                }


                /*if (n > 0) {
                    // Write the data to the master pseudoterminal
                    write(ptm, buffer, n);
                } else if (n == -1) {
                    perror("read from keyboard");
                    break;
                }
                */
                
            }



            // usb keyboard

            if (FD_ISSET(current_keyboard_fd, &fds)) {
            //if (FD_ISSET(pipe_fds[0], &fds)) {
                //char buffer[16];
                //ssize_t n = read(keyboard_fd, buffer, sizeof(buffer));

                int readSize = sizeof(struct input_event) - keyboard_buffer_bytes;
                //printf("Reading %d bytes\n", readSize);
                
                ssize_t n = read(current_keyboard_fd, &keyboard_buffer[keyboard_buffer_bytes], readSize);
                if (n < 0 && errno != EINTR) 
                {
                    printf("Error reading from keyboard, closing fd\n");
                    close(current_keyboard_fd);
                    current_keyboard_fd = -1;
                    current_keyboard_path[0] = '\0';
                }
                else 
                {
                    keyboard_buffer_bytes += n;
                    //keyboard_buffer_bytes += read(pipe_fds[0], &keyboard_buffer[keyboard_buffer_bytes], readSize);

                    struct input_event* ev = (struct input_event*)keyboard_buffer;
                    if (keyboard_buffer_bytes == sizeof(struct input_event))
                    {
                        keyboard_buffer_bytes = 0; // reset for next read
                        //ssize_t bytesRead = read(keyboard_fd, &ev, sizeof(struct input_event));
                        //if (bytesRead == sizeof(struct input_event) && ev.type == EV_KEY) { //} && ev.value == 1) {
                        if (ev->type == EV_KEY) 
                        { //} && ev.value == 1) {
//                            printf("Key Event Code=%d, Value=%d, Mapped=%s, ctrl_pressed=%d, shift_pressed=%d, wait_for_key_release=%d\n", ev->code, ev->value, libevdev_event_code_get_name(ev->type, ev->code), ctrl_pressed, shift_pressed, wait_for_key_release);
                            //map_key(&ev);
                            
                            if (ev->code == KEY_GRAVE && ev->value > 0 && ctrl_pressed && wait_for_key_release == 0)
                            {
                                wait_for_key_release = 1;
                                toggle_ui_visibility();
                            }
                            else if (ev->code == KEY_GRAVE && ev->value == 0 && wait_for_key_release == 1)
                            {
                                wait_for_key_release = 0;
                            }
                            else if (ev->code == KEY_UP)
                            {
                                const char* escape_sequence = "\x1B[A";
                                if (ev->value == 1) 
                                {
                                    write(ptm, escape_sequence, strlen(escape_sequence));
                                }
                            }
                            else if (ev->code == KEY_DOWN)
                            {
                                const char* escape_sequence = "\x1B[B";
                                if (ev->value == 1) 
                                {
                                    write(ptm, escape_sequence, strlen(escape_sequence));
                                }
                            }
                            else if (ev->code == KEY_LEFT)
                            {
                                const char* escape_sequence = "\x1B[D";
                                if (ev->value == 1) 
                                {
                                    write(ptm, escape_sequence, strlen(escape_sequence));
                                }
                            }
                            else if (ev->code == KEY_RIGHT)
                            {
                                const char* escape_sequence = "\x1B[C";
                                if (ev->value == 1) 
                                {
                                    write(ptm, escape_sequence, strlen(escape_sequence));
                                }
                            }
                            else if (ev->code == KEY_TAB)
                            {
                                const char* escape_sequence = "\t";
                                if (ev->value == 1) 
                                {
                                    write(ptm, escape_sequence, strlen(escape_sequence));
                                }
                            }
                            else if (ev->code == KEY_BACKSPACE)
                            {
                                const char* escape_sequence = "\b \b";
                                //const char* escape_sequence = "\b ";
                                if (ev->value == 1) 
                                {
 //                                   printf(escape_sequence);
 //                                   fflush(stdout);

                                    // no - you'll do it twice!!
                                    //process_buffer(escape_sequence, strlen(escape_sequence), NULL, nResponseBytes);
                                
                                    write(ptm, escape_sequence, strlen(escape_sequence));
                                }
                            }
                            else
                            {
                                unsigned char c = map_key(ev);
                            
                                if (c == 3 && ev->value == 1) // ctrl -c
                                {
                                    sendSignalToProcessesWithSameSID(pid, SIGINT);
                                }
                                else
                                {
  //                                  printf("map_key returned '%c'\n", c);
                                    if (ev->value == 1 && c != 0)
                                    {
                                        // echo here
  //                                      printf("%c", c); // for debugging print this
  //                                      fflush(stdout);
                                        
                                        //char buf[2];
                                        //sprintf(buf, "%c", c);
                                        //process_buffer(buf, 1, NULL, nResponseBytes);

                                        write(ptm, &c, 1);
                                    }
                                }
                                
                            }
                            //write(ptm, &ev, sizeof(ev));
                        }
                        else
                        {
 //                           printf("Skipped event of type %d\n", ev->type);
                        }

                        //printf("ctrl pressed state after processing: %d\n", ctrl_pressed);
//                        printf("Key Event Code=%d, Value=%d, Mapped=%s, ctrl_pressed=%d, wait_for_key_release=%d, frame_buffer=%d\n", ev->code, ev->value, libevdev_event_code_get_name(ev->type, ev->code), ctrl_pressed, wait_for_key_release, pFrameBuffer == pFrameBuffer1 ? 1 : 2);
                    }
                    else
                    {
                        printf("SKIPPED READ because was too small?\n");
                    }
                }


                /*if (n > 0) {
                    // Write the data to the master pseudoterminal
                    write(ptm, buffer, n);
                } else if (n == -1) {
                    perror("read from keyboard");
                    break;
                }
                */
                
            }
            

            // Check if there is data to read from the pseudoterminal
            if (FD_ISSET(ptm, &fds)) {
                char buffer[16384];
                n = read(ptm, buffer, sizeof(buffer));

                if (n > 0) {
                    LOG("received %d bytes from pty\n", n);
                    
                     for (int j=0;j<n;j++)
                     {
                         LOG("%02X ", buffer[j]);
                     }
                     LOG("\n");
                    
                    process_buffer(buffer, n, &pResponseBytes, nResponseBytes);
                    if (pResponseBytes != NULL)
                    {
                        printf("Sending response buffer with %d bytes:\n", nResponseBytes);
                        for (int j=0;j<nResponseBytes;j++)
                        {
                            printf("%02X ", pResponseBytes[j]);
                        }
                        printf("\n");

                        write(ptm, pResponseBytes, nResponseBytes);
                        free(pResponseBytes);
                        pResponseBytes = NULL;
                    }

                    // Drain all remaining PTY data before the next framebuffer
                    // update. This coalesces a burst of output (e.g. a long
                    // directory listing) into a single render pass instead of
                    // paying one sysop_wait_hdmi_vblank() per read chunk.
                    while (1) {
                        fd_set drain_fds;
                        FD_ZERO(&drain_fds);
                        FD_SET(ptm, &drain_fds);
                        struct timeval zero = {0, 0};
                        if (select(ptm + 1, &drain_fds, NULL, NULL, &zero) <= 0)
                            break;
                        n = read(ptm, buffer, sizeof(buffer));
                        if (n <= 0) break;
                        LOG("drain: %d bytes\n", n);
                        process_buffer(buffer, n, &pResponseBytes, nResponseBytes);
                        if (pResponseBytes != NULL) {
                            write(ptm, pResponseBytes, nResponseBytes);
                            free(pResponseBytes);
                            pResponseBytes = NULL;
                        }
                    }

                    fflush(stdout);
                } else if (n == 0) {
                    // EOF from the pseudoterminal, terminate the program
                    break;
                } else if (n == -1) {
                    perror("read from pseudoterminal");
                    break;
                }
            }
        }

        // Wait for the child process to terminate
        int status;
        waitpid(pid, &status, 0);

        cleanup_keyboard_monitor();

        // Close file descriptors
        close(ptm);
        printf("Parent process exiting.\n");
    }

    return 0;
}

