#include "mem_viewer_gui.h"

#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

static bool mem_viewer_debug_enabled() {
    static const bool enabled = []() {
        const char *value = std::getenv("MEM_VIEWER_DEBUG");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static void mem_viewer_debug_log(const char *fmt, ...) {
    if (!mem_viewer_debug_enabled()) {
        return;
    }

    std::fprintf(stderr, "[mem_viewer_helper_main pid=%ld] ", static_cast<long>(getpid()));
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

}

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

    mem_viewer_debug_log("helper args parsed pid=%ld address=0x%llx size=%zu argc=%d",
        static_cast<long>(pid),
        static_cast<unsigned long long>(address),
        size,
        argc);

    if (pid <= 0 || address == 0 || size == 0) {
        std::fprintf(stderr, "invalid mem_viewer_helper arguments\n");
        return 2;
    }

    mem_viewer_debug_log("launching GUI");
    return mem_viewer_run_gui(pid, address, size);
}
