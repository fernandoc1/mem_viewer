#include "mem_viewer_gui.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv) {
    pid_t pid = -1;
    uintptr_t address = 0;
    size_t size = 0;

    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const char *value = argv[i + 1];
        if (key == "--pid") {
            pid = static_cast<pid_t>(std::strtol(value, nullptr, 10));
        } else if (key == "--address") {
            address = static_cast<uintptr_t>(std::strtoull(value, nullptr, 10));
        } else if (key == "--size") {
            size = static_cast<size_t>(std::strtoull(value, nullptr, 10));
        }
    }

    if (pid <= 0 || address == 0 || size == 0) {
        std::fprintf(stderr, "invalid mem_viewer_helper arguments\n");
        return 2;
    }

    return mem_viewer_run_gui(pid, address, size);
}
