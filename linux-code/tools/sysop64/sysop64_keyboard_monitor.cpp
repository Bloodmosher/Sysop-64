/*
 * Sysop-64 
 * https://github.com/Bloodmosher/Sysop-64 
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project 
 */


#include "sysop64_internal.h"
#define BITS_PER_LONG (sizeof(long) * 8)
#define test_bit(nr, addr) (((1UL << ((nr) % BITS_PER_LONG)) & ((addr)[(nr) / BITS_PER_LONG])) != 0)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)

int inotify_fd = -1;
static int wd_input = -1;
static int wd_dev = -1;
int current_keyboard_fd = -1;
char current_keyboard_path[256] = {0};

// Returns 1 if the evdev device at device_path looks like a full keyboard
// (has EV_KEY capability and reports KEY_Q + KEY_W). Returns 0 otherwise.
static int is_keyboard(const char *device_path) {
    int fd = open(device_path, O_RDONLY);
    if (fd < 0) return 0;

    unsigned long ev_bits[NLONGS(EV_MAX)];
    ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
    if (!test_bit(EV_KEY, ev_bits)) {
        close(fd);
        return 0;
    }

    unsigned long key_bits[NLONGS(KEY_MAX)];
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
    if (test_bit(KEY_Q, key_bits) && test_bit(KEY_W, key_bits)) {
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

// scandir filter that accepts only entries whose names start with "event".
static int event_device_filter(const struct dirent *d) {
    return strncmp("event", d->d_name, 5) == 0;
}

// Scans /dev/input for keyboard devices. Opens and exclusively grabs the
// first one found. If the active keyboard has changed, closes the old fd
// first. If no keyboard is present, closes the current fd and logs a message.
static void scan_keyboard_devices() 
{
    struct dirent **namelist;
    bool found_keyboard = false;

    int n = scandir("/dev/input", &namelist, event_device_filter, alphasort);
    if (n >= 0) {
        for (int i = 0; i < n; i++) {
            char device_path[256];
            snprintf(device_path, sizeof(device_path), "/dev/input/%s", namelist[i]->d_name);
            if (is_keyboard(device_path)) {
                found_keyboard = true;

                // If we found a keyboard and it's different from our current one
                if (strcmp(current_keyboard_path, device_path) != 0) {
                    // Close existing keyboard if we have one
                    if (current_keyboard_fd >= 0) {
                        close(current_keyboard_fd);
                        current_keyboard_fd = -1;
                    }
                    
                    // Try to open the new keyboard
                    //current_keyboard_fd = open(device_path, O_RDONLY);
                    current_keyboard_fd = open(device_path, O_RDONLY|O_CLOEXEC);
                    if (current_keyboard_fd >= 0) {
                        strncpy(current_keyboard_path, device_path, sizeof(current_keyboard_path) - 1);
                        printf("✅ Keyboard connected: %s\n", current_keyboard_path);
                    }
                    if (ioctl(current_keyboard_fd, EVIOCGRAB, 1) == -1) { perror("EVIOCGRAB"); close(current_keyboard_fd); return; }
                }
                break;
            }
        }
        
        // Free scandir allocations
        for (int i = 0; i < n; i++) {
            free(namelist[i]);
        }
        free(namelist);
    }

    // If we didn't find any keyboard
    if (!found_keyboard && current_keyboard_fd >= 0) {
        close(current_keyboard_fd);
        current_keyboard_fd = -1;
        current_keyboard_path[0] = '\0';
        printf("❌ Keyboard disconnected. Waiting...\n");
    }
}

// Sets up inotify watches on /dev and /dev/input, then performs an initial
// keyboard scan. Safe to call multiple times; returns immediately if already
// initialized.
void init_keyboard_monitor() {
    if (inotify_fd != -1)
        return;

    inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        error_exit("inotify_init failed");
    }

    // Watch /dev for /dev/input creation/deletion
    wd_dev = inotify_add_watch(inotify_fd, "/dev", IN_CREATE | IN_DELETE);
    if (wd_dev < 0) {
        error_exit("Could not watch /dev");
    }

    // Try to watch /dev/input
    wd_input = inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE | IN_DELETE);
    if (wd_input < 0) {
        printf("⚠️  /dev/input does not exist yet. Waiting for it to be created.\n");
    }

    // Initial keyboard scan
    scan_keyboard_devices();
}

// Removes all inotify watches and closes the keyboard file descriptor.
void cleanup_keyboard_monitor() {
    if (wd_dev >= 0) {
        inotify_rm_watch(inotify_fd, wd_dev);
    }
    if (wd_input >= 0) {
        inotify_rm_watch(inotify_fd, wd_input);
    }
    if (inotify_fd >= 0) {
        close(inotify_fd);
        inotify_fd = -1;
    }
    if (current_keyboard_fd >= 0) {
        close(current_keyboard_fd);
        current_keyboard_fd = -1;
    }
}

// Drains pending inotify events. Re-scans keyboard devices whenever an
// event node appears or disappears in /dev/input, or when /dev/input itself
// is created or deleted.
void process_inotify_events() {
    char buffer[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
    
    int length = read(inotify_fd, buffer, sizeof(buffer));
    if (length > 0) {
        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *) &buffer[i];
            
            if (event->wd == wd_input && event->len && strncmp("event", event->name, 5) == 0) {
                printf("⚠️  Device change in /dev/input. Re-scanning...\n");
                usleep(500000); // Wait for driver to settle
                scan_keyboard_devices();
            }
            else if (event->wd == wd_dev && event->len && strcmp(event->name, "input") == 0) {
                if (event->mask & IN_CREATE) {
                    printf("ℹ️  Directory /dev/input created. Setting up watch and scanning...\n");
                    wd_input = inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE | IN_DELETE);
                    usleep(500000);
                    scan_keyboard_devices();
                } else if (event->mask & IN_DELETE) {
                    printf("ℹ️  Directory /dev/input deleted.\n");
                    wd_input = -1;
                    scan_keyboard_devices();
                }
            }
            i += sizeof(struct inotify_event) + event->len;
        }
    }
}
