/*
 * Sysop-64 
 * https://github.com/Bloodmosher/Sysop-64 
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project 
 *
 * sysop64_server.cpp
 *
 * TCP command server for the sysop64 application. Listens on loopback
 * port SYSOP_SERVER_PORT (6510) in a background thread (handleAccept).
 * Each connected client gets its own thread (handleClient) which reads a
 * one-byte command type and dispatches to the appropriate handler.
 *
 * Supported commands:
 *   SYSOP_SERVER_CMD_DMA_LOCK        - acquire the DMA mutex (blocks until granted).
 *   SYSOP_SERVER_CMD_DMA_UNLOCK      - release the DMA mutex.
 *   SYSOP_SERVER_CMD_CONSOLE_CLOSE   - request the C64 console overlay to close.
 *   SYSOP_SERVER_CMD_SHOW_MESSAGE    - queue an animated message for the display panel.
 *   SYSOP_SERVER_CMD_HIDE_MESSAGE    - immediately hide the message panel.
 *   SYSOP_SERVER_CMD_QUEUE_HIDE_MESSAGE - enqueue a hide-message command so it
 *                               executes after any pending display messages.
 *
 * DMA access is reference-counted: handleDmaEnable / handleDmaDisable track
 * the count per-thread and globally, only calling the real sysop_dma_enable /
 * sysop_dma_disable when the count transitions between 0 and 1.
 */

#include "sysop64_internal.h"
// dma broker code

// Global mutex variable
static pthread_mutex_t lock;

static int dma_refCount = 0;
__thread int per_thread_dma_refCount = 0;

// Increments the global and per-thread DMA reference counts. Calls
// sysop_dma_enable() only when the global count transitions from 0 to 1.
void handleDmaEnable()
{
    dma_refCount++;
    per_thread_dma_refCount++;
    if (dma_refCount == 1) {
        printf("sysop_dma_enable()\n");
        sysop_dma_enable();
    }

    printf("dma_refCount %d\n", dma_refCount);
}

// Decrements the global and per-thread DMA reference counts. Calls
// sysop_dma_disable() only when the global count reaches 0. No-ops if the
// calling thread's own count is already zero.
void handleDmaDisable()
{
    if (per_thread_dma_refCount > 0) {
        per_thread_dma_refCount--;
        dma_refCount--;
        if (dma_refCount == 0) {
            printf("sysop_dma_disable()\n");
            sysop_dma_disable();
        }
    }
    printf("dma_refCount %d\n", dma_refCount);
}


/**
 * Initializes a recursive mutex.
 * * This function creates a mutex attribute, sets its type to RECURSIVE,
 * and then initializes the global mutex with these attributes. A recursive
 * mutex allows the same thread to lock it multiple times without deadlocking.
 */
int initialize_lock() {
    int status;
    pthread_mutexattr_t attr;

    // Initialize the mutex attributes object
    status = pthread_mutexattr_init(&attr);
    if (status != 0) {
        perror("pthread_mutexattr_init");
        return status;
    }

    // Set the mutex type to RECURSIVE
    // This allows a thread to lock the same mutex multiple times.
    status = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (status != 0) {
        perror("pthread_mutexattr_settype");
        pthread_mutexattr_destroy(&attr);
        return status;
    }

    // Initialize the mutex with the specified attributes
    status = pthread_mutex_init(&lock, &attr);
    if (status != 0) {
        perror("pthread_mutex_init");
        pthread_mutexattr_destroy(&attr);
        return status;
    }

    // The attributes object is no longer needed after mutex initialization
    pthread_mutexattr_destroy(&attr);

    printf("Recursive lock initialized successfully.\n");
    return 0;
}

// Acquires the DMA lock on behalf of the console (main UI thread) and
// enables DMA. Prints diagnostic messages on lock acquisition.
void console_acquire_lock()
{
    printf("Console attempting to acquire lock...\n");
    int status = pthread_mutex_lock(&lock);
    if (status != 0) {
        perror("pthread_mutex_lock");
        // In a real application, you might want to handle this more gracefully
        exit(EXIT_FAILURE); 
    }
    printf("Console acquired lock.\n");

    handleDmaEnable();
}

// Releases the DMA lock held by the console and disables DMA.
void console_release_lock()
{
    handleDmaDisable();

    printf("Console attempting to release lock...\n");
    int status = pthread_mutex_unlock(&lock);
    if (status != 0) {
        perror("pthread_mutex_unlock");
        //exit(EXIT_FAILURE);
        //return -1;
    }
    printf("Console released lock.\n");
}

/**
 * Acquires the lock.
 * * This function blocks until the mutex becomes available. If the calling
 * thread already holds the lock, the call succeeds immediately and increments
 * an internal lock counter.
 */
int acquire_lock(int clientSocket) {
    printf("Attempting to acquire lock...\n");
    int status = pthread_mutex_lock(&lock);
    if (status != 0) {
        perror("pthread_mutex_lock");
        // In a real application, you might want to handle this more gracefully
        exit(EXIT_FAILURE); 
    }
    printf("Lock acquired.\n");

    handleDmaEnable();

    // Send a response back to the client
    uint8_t response = 0x01; // Indicating success
    write(clientSocket, &response, 1);
    return 0; // No exit loop needed
}

/**
 * Destroys the lock to free resources.
 * * Should only be called when the lock is no longer needed and is unlocked.
 */
void destroy_lock() {
    int status = pthread_mutex_destroy(&lock);
    if (status != 0) {
        perror("pthread_mutex_destroy");
    } else {
        printf("Lock destroyed successfully.\n");
    }
}

/**
 * Releases the lock.
 * * If the calling thread acquired the lock multiple times, this function
 * decrements the internal lock counter. The mutex only becomes available
 * to other threads when the counter reaches zero.
 */
int release_lock(int clientSocket) {

    handleDmaDisable();

    printf("Attempting to release lock...\n");
    int status = pthread_mutex_unlock(&lock);
    if (status != 0) {
        perror("pthread_mutex_unlock");
        //exit(EXIT_FAILURE);
        //return -1;
    }
    printf("Lock released.\n");

    // Send a response back to the client
    uint8_t response = 0x01; // Indicating success
    write(clientSocket, &response, 1);
    return 0; // No exit loop needed
}

// Reads a SYSOP_SERVER_CMD_SHOW_MESSAGE payload from clientSocket (4-byte timeout,
// 1-byte length, then the message text), enqueues a QueuedMessage for the
// display panel, and returns 0 on success or -1 on read error.
int handle_show_message_request(int clientSocket) {
    printf("Received show message request\n");
    
    static pthread_mutex_t message_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&message_mutex);
    
    // Read the timeout value (32-bit signed integer)
    int32_t timeout_ms = 0;
    int totalRead = 0;
    while (totalRead < sizeof(int32_t)) {
        int bytesRead = read(clientSocket, ((char*)&timeout_ms) + totalRead, sizeof(int32_t) - totalRead);
        if (bytesRead <= 0) {
            printf("Error reading timeout value\n");
            pthread_mutex_unlock(&message_mutex);
            return -1;
        }
        totalRead += bytesRead;
    }
    
    uint8_t msgLen = 0;
    int bytesRead = read(clientSocket, &msgLen, 1);
    if (bytesRead != 1) {
        printf("Error reading message length\n");
        pthread_mutex_unlock(&message_mutex);
        return -1;
    }
    
    char *message = (char*)malloc(msgLen + 1);
    if (message == NULL) {
        printf("Error allocating memory for message\n");
        pthread_mutex_unlock(&message_mutex);
        return -1;
    }
    
    totalRead = 0;
    while (totalRead < msgLen) {
        bytesRead = read(clientSocket, message + totalRead, msgLen - totalRead);
        if (bytesRead <= 0) {
            printf("Error reading message data\n");
            free(message);
            pthread_mutex_unlock(&message_mutex);
            return -1;
        }
        totalRead += bytesRead;
    }
    
    message[msgLen] = '\0';
    printf("Received message: %s (timeout: %d ms)\n", message, timeout_ms);
    message_display_show_requested = 1;
    
    // Add message to queue
    QueuedMessage queued_msg;
    queued_msg.msg_type = 0;  // Type 0 for display messages
    queued_msg.message = message;
    queued_msg.timeout_ms = timeout_ms;
    
    pthread_mutex_lock(&g_message_queue_mutex);
    g_message_queue.push(queued_msg);
    pthread_mutex_unlock(&g_message_queue_mutex);
    
    free(message);
    pthread_mutex_unlock(&message_mutex);
    return 0;
}

#define BUFFER_SIZE 1024

// Per-client handler thread. Loops reading one-byte command types from
// clientSocket and dispatching to the appropriate handler until the client
// disconnects or returns an exit-loop signal. On disconnect, ensures the
// DMA lock is released and signals the console to re-acquire if needed.
void *handleClient(void *arg) 
{
    int clientSocket = *((int *)arg);
    char buffer[BUFFER_SIZE];
    int dma_lock = 0;

    // Keep processing commands until the connection is closed
    while (1) 
    {
        uint8_t cmdType = 0;
        int bytesRead = read(clientSocket, &cmdType, 1);
        if (bytesRead != 1)
        {
            printf("Unexpected data on socket when reading command type\n");
            break;
        }

        uint8_t exitLoop = 0;

        switch(cmdType)
        {
            case SYSOP_SERVER_CMD_DMA_LOCK:
            {
                uint8_t close_console = 0;
                bytesRead = read(clientSocket, &close_console, 1);
                console_close_requested  = close_console;
                if (c64_console_active) {
                    console_yield_lock = 1;
                    printf("Console is active, requesting lock yield\n");
                }
                exitLoop = acquire_lock(clientSocket);
                if (!exitLoop)
                    dma_lock = 1;
            }
            break;

            case SYSOP_SERVER_CMD_DMA_UNLOCK:
            exitLoop = release_lock(clientSocket);
            dma_lock = 0;
            if (console_yield_occurred && !console_reacquire_lock) {
                printf("Client DMA unlock, requesting console reacquire lock\n");
                console_reacquire_lock = 1;
            }
            break;

            case SYSOP_SERVER_CMD_CONSOLE_CLOSE:
            printf("Received console close request\n");
            console_close_requested = 1;
            break;
            case SYSOP_SERVER_CMD_SHOW_MESSAGE:
            {
                handle_show_message_request(clientSocket);
            }
            break;

            case SYSOP_SERVER_CMD_HIDE_MESSAGE:
            printf("Received hide message request\n");
            message_display_close_requested = 1;
            break;
            case SYSOP_SERVER_CMD_QUEUE_HIDE_MESSAGE:
            {
                printf("Received queue hide message request\n");
                
                // Add a hide message command to the queue
                QueuedMessage queued_msg;
                queued_msg.msg_type = 1;  // Type 1 for hide message command
                queued_msg.message = "";
                queued_msg.timeout_ms = 0;
                
                pthread_mutex_lock(&g_message_queue_mutex);
                g_message_queue.push(queued_msg);
                pthread_mutex_unlock(&g_message_queue_mutex);
            }
            break;
            
            default:
            printf("Unsupported cmd %hc\n", cmdType);
            break;
        }

        if (exitLoop)
            break;
    }

    printf("Client disconnected\n");

    if (dma_lock) {
        release_lock(clientSocket);
    }
    if (console_yield_occurred && !console_reacquire_lock) {
        printf("Client DMA unlock, requesting console reacquire lock\n");
        console_reacquire_lock = 1;
    }

    // Close the client socket
    close(clientSocket);
    free(arg);

    return NULL;
}

// Accept-loop thread entry point. Creates the server socket, binds to
// port SYSOP_SERVER_PORT, and loops accepting incoming connections.
// Spawns a detached handleClient thread for each accepted client.
void *handleAccept(void *arg) 
{
    int serverSocket, clientSocket;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    pthread_t tid;

    // Create socket
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == -1) {
        perror("Error creating socket");
        exit(EXIT_FAILURE);
    }

    // Set SO_REUSEADDR option
    int reuse = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &reuse, sizeof(reuse)) == -1) {
        perror("Error setting SO_REUSEADDR option");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    // Set TCP_NODELAY option
    int flag = 1;
    if (setsockopt(serverSocket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int)) == -1) {
        perror("Error setting TCP_NODELAY option");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }    


    // Set up server address structure — bind to loopback only so the
    // command socket is not reachable from other machines on the network.
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serverAddr.sin_port = htons(SYSOP_SERVER_PORT);

    // Bind the socket
    if (bind(serverSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1) {
        perror("Error binding socket");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(serverSocket, 5) == -1) {
        perror("Error listening for connections");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d...\n", SYSOP_SERVER_PORT);

    //sysop_open_bridge();
    //sysop_dma_enable();
    //signal(SIGINT, sigintHandler);

    //initialize_lock();

    while (1) {
        // Accept incoming connection
        clientSocket = accept(serverSocket, (struct sockaddr *)&clientAddr, &addrLen);
        if (clientSocket == -1) {
            perror("Error accepting connection");
            continue;
        }
        flag = 1;
        if (setsockopt(clientSocket,            /* socket descriptor */
                        IPPROTO_TCP,          /* level */
                        TCP_NODELAY,          /* option name */
                        (char *)&flag,        /* option value */
                        sizeof(int)) == -1)         /* option length */
        {
            printf("Error setting TCP_NODELAY\n");
        }

        printf("Accepted connection from %s:%d\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));

        // Create a separate thread to handle the client
        int *clientSocketPtr = (int *)malloc(sizeof(int));
        *clientSocketPtr = clientSocket;

        if (pthread_create(&tid, NULL, handleClient, clientSocketPtr) != 0) {
            perror("Error creating thread");
            close(clientSocket);
            free(clientSocketPtr);
        } else {
            // Detach the thread to allow it to run independently
            pthread_detach(tid);
        }
    }

    // Close the server socket (unreachable in this example)
    close(serverSocket);
}


