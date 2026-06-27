/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

static int g_sockfd = -1;

/*
 * Establishes a TCP connection to the Sysop socket server.
 * Returns 0 on success, -1 on failure.
 */
int sysop_server_connect() {
    struct sockaddr_in serv_addr;

    if ((g_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SYSOP_SERVER_PORT);

    if (inet_pton(AF_INET, SYSOP_SERVER_ADDR, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        close(g_sockfd);
        g_sockfd = -1;
        return -1;
    }

    if (connect(g_sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection Failed");
        close(g_sockfd);
        g_sockfd = -1;
        return -1;
    }

    // Set TCP_NODELAY option
    int flag = 1;
    if (setsockopt(g_sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int)) == -1) {
        perror("Error setting TCP_NODELAY option");
        close(g_sockfd);
        g_sockfd = -1;
        return -1;
    }    

    return 0;
}

/* Closes the TCP connection to the Sysop socket server. */
void sysop_server_disconnect() {
    if (g_sockfd != -1) {
        close(g_sockfd);
        g_sockfd = -1;
    }
}

static void sysop_dma_lock_internal(int closeConsole)
{
    if (g_sockfd == -1) {
        fprintf(stderr, "sysop_server_dma_lock failed: Not connected.\n");
        return;
    }
    uint8_t buffer[2];
    buffer[0] = SYSOP_SERVER_CMD_DMA_LOCK;
    buffer[1] = closeConsole ? (uint8_t)1 : (uint8_t)0;

    if (send(g_sockfd, buffer, sizeof(buffer), 0) < 0) {
        perror("sysop_server_dma_lock send failed");
    }
    uint8_t response;
    if (recv(g_sockfd, &response, sizeof(response), 0) < 0) {
        perror("sysop_server_dma_lock receive failed");
    }
}

void sysop_server_display_message(const char* msg, int length, int displayTimeMilliseconds)
{
    if (g_sockfd == -1) {
        fprintf(stderr, "sysop_server_dma_lock failed: Not connected.\n");
        return;
    }
    uint8_t buffer[6];
    buffer[0] = SYSOP_SERVER_CMD_SHOW_MESSAGE;
    buffer[1] = (uint8_t)(displayTimeMilliseconds & 0xFF);
    buffer[2] = (uint8_t)(displayTimeMilliseconds >> 8) & 0xFF;
    buffer[3] = (uint8_t)(displayTimeMilliseconds >> 16) & 0xFF;
    buffer[4] = (uint8_t)(displayTimeMilliseconds >> 24) & 0xFF;
    buffer[5] = (uint8_t)length;

    if (send(g_sockfd, buffer, sizeof(buffer), 0) < 0) {
        perror("sysop_server_display_message send failed");
    }
    if (send(g_sockfd, msg, length, 0) < 0) {
        perror("sysop_server_display_message send failed");
    }
}

void sysop_server_hide_messages()
{
    if (g_sockfd == -1) {
        fprintf(stderr, "sysop_server_hide_messages failed: Not connected.\n");
        return;
    }
    uint8_t buffer[1];
    buffer[0] = SYSOP_SERVER_CMD_HIDE_MESSAGE;

    if (send(g_sockfd, buffer, sizeof(buffer), 0) < 0) {
        perror("sysop_server_hide_messages send failed");
    }
}

void sysop_server_queue_hide_messages()
{
    if (g_sockfd == -1) {
        fprintf(stderr, "sysop_server_queue_hide_messages failed: Not connected.\n");
        return;
    }
    uint8_t buffer[1];
    buffer[0] = SYSOP_SERVER_CMD_QUEUE_HIDE_MESSAGE;

    if (send(g_sockfd, buffer, sizeof(buffer), 0) < 0) {
        perror("sysop_server_queue_hide_messages send failed");
    }
}

void sysop_server_dma_lock()
{
    sysop_dma_lock_internal(1);
}

void sysop_server_dma_lock2()
{
    sysop_dma_lock_internal(0);
}

void sysop_server_dma_unlock()
{
    if (g_sockfd == -1) {
        fprintf(stderr, "sysop_server_dma_unlock failed: Not connected.\n");
        return;
    }
    uint8_t buffer[1];
    buffer[0] = SYSOP_SERVER_CMD_DMA_UNLOCK;

    if (send(g_sockfd, buffer, sizeof(buffer), 0) < 0) {
        perror("sysop_server_dma_lock send failed");
    }
    uint8_t response;
    if (recv(g_sockfd, &response, sizeof(response), 0) < 0) {
        perror("sysop_server_dma_lock receive failed");
        return;
    }
}

void sysop_server_console_close()
{
    if (g_sockfd == -1) {
        fprintf(stderr, "sysop_server_console_close failed: Not connected.\n");
        return;
    }
    uint8_t buffer[1];
    buffer[0] = SYSOP_SERVER_CMD_CONSOLE_CLOSE;

    if (send(g_sockfd, buffer, sizeof(buffer), 0) < 0) {
        perror("sysop_server_console_close send failed");
    }
}
