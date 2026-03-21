#define _POSIX_C_SOURCE 199309L

#include "mem_viewer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void sleep_us(long usec) {
    struct timespec ts;
    ts.tv_sec = usec / 1000000L;
    ts.tv_nsec = (usec % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    const size_t size = 1 << 16;
    uint8_t *buffer = (uint8_t *)malloc(size);
    int wait_mode = 1;
    size_t i;

    if (!buffer) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    for (i = 0; i < size; ++i) {
        buffer[i] = (uint8_t)i;
    }

    for (i = 1; i < (size_t)argc; ++i) {
        if (strcmp(argv[i], "--wait") == 0) {
            wait_mode = 1;
        } else if (strcmp(argv[i], "--no-wait") == 0) {
            wait_mode = 0;
        }
    }

    MemViewer *viewer = mem_viewer_open(buffer, size);
    if (!viewer) {
        fprintf(stderr, "mem_viewer_open failed\n");
        free(buffer);
        return 1;
    }

    srand((unsigned)time(NULL));

    while (mem_viewer_is_open(viewer)) {
        size_t index = (size_t)(rand() % (int)size);
        buffer[index] += 1;
        buffer[(index + 1) % size] ^= 0x5a;
        sleep_us(wait_mode ? 25000 : 16000);
        if (!wait_mode) {
            static int ticks = 0;
            if (++ticks > 900) {
                break;
            }
        }
    }

    mem_viewer_destroy(viewer);
    free(buffer);
    return 0;
}
