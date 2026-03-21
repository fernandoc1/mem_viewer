#define _GNU_SOURCE

#include "mem_viewer.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void sleep_us(long usec) {
    struct timespec ts;
    ts.tv_sec = usec / 1000000L;
    ts.tv_nsec = (usec % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    const size_t size = 1 << 16;
    int wait_mode = 0;
    int updater_pid = -1;

    uint8_t *buffer = (uint8_t *)mem_viewer_shared_malloc(size);
    if (!buffer) {
        fprintf(stderr, "mem_viewer_shared_malloc failed\n");
        return 1;
    }

    for (size_t i = 0; i < size; ++i) {
        buffer[i] = (uint8_t)i;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--wait") == 0) {
            wait_mode = 1;
        }
    }

    updater_pid = fork();
    if (updater_pid < 0) {
        perror("fork");
        mem_viewer_shared_free(buffer, size);
        return 1;
    }

    if (updater_pid == 0) {
        while (1) {
            for (size_t i = 0; i < size; i += 97) {
                buffer[i] += 1;
            }
            sleep_us(20000);
        }
    }

    MemViewer *viewer = mem_viewer_open_shared(buffer, size);
    if (!viewer) {
        fprintf(stderr, "mem_viewer_open_shared failed\n");
        kill(updater_pid, SIGTERM);
        waitpid(updater_pid, NULL, 0);
        mem_viewer_shared_free(buffer, size);
        return 1;
    }

    while (mem_viewer_is_open(viewer)) {
        buffer[0] += 3;
        buffer[1] ^= 0x5a;
        sleep_us(wait_mode ? 25000 : 16000);
        if (!wait_mode) {
            static int ticks = 0;
            if (++ticks > 900) {
                break;
            }
        }
    }

    mem_viewer_destroy(viewer);
    kill(updater_pid, SIGTERM);
    waitpid(updater_pid, NULL, 0);
    mem_viewer_shared_free(buffer, size);
    return 0;
}
