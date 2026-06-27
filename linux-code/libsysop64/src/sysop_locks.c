/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

static int library_lock_fd = 0;

void get_library_lock(void) {
    int fd = open("/tmp/my_staticlib.lock", O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("open");
        return;
    }

    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,  // lock the entire file
    };

    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        perror("fcntl");
        close(fd);
        return;
    }

    //return fd;  // keep it open while locked
    library_lock_fd = fd;
}

void release_library_lock() 
{
    if (library_lock_fd != 0)
    {
        struct flock fl = { .l_type = F_UNLCK, .l_whence = SEEK_SET };
        fcntl(library_lock_fd, F_SETLK, &fl);
        close(library_lock_fd);
        library_lock_fd = 0;
    }
}

// just an advisory lock...
static int framebuffer_lock_fd = 0;

int sysop_framebuffer_lock() {
    int fd = open("/tmp/sysop_framebuffer.lock", O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("open");
        return 0;
    }

    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,  // lock the entire file
    };

    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        perror("fcntl");
        close(fd);
        return 0;
    }

    framebuffer_lock_fd = fd;

    return 1;
}

void sysop_framebuffer_unlock()
{
    if (framebuffer_lock_fd != 0)
    {
        struct flock fl = { .l_type = F_UNLCK, .l_whence = SEEK_SET };
        fcntl(framebuffer_lock_fd, F_SETLK, &fl);
        close(framebuffer_lock_fd);
        framebuffer_lock_fd = 0;
    }
}
