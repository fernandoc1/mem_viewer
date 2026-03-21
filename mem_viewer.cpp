#include "mem_viewer.h"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <limits.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
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

    std::fprintf(stderr, "[mem_viewer pid=%ld] ", static_cast<long>(getpid()));
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

struct MemViewerProcess {
    explicit MemViewerProcess(pid_t child_pid) : pid(child_pid) {}
    pid_t pid;
};

static bool child_is_alive(pid_t pid) {
    if (pid <= 0) {
        return false;
    }

    int status = 0;
    const pid_t result = waitpid(pid, &status, WNOHANG);
    if (result > 0) {
        mem_viewer_debug_log("child %ld exited with status=0x%x", static_cast<long>(pid), status);
    }
    return result == 0;
}

static std::string helper_path() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&mem_viewer_open), &info) == 0 || info.dli_fname == nullptr) {
        return "mem_viewer_helper";
    }

    std::string path(info.dli_fname);
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return "mem_viewer_helper";
    }
    return path.substr(0, slash + 1) + "mem_viewer_helper";
}

}  // namespace

struct MemViewer {
    MemViewerProcess *process;
};

extern "C" MemViewer *mem_viewer_open(void *memory, size_t size) {
    if (memory == nullptr || size == 0) {
        mem_viewer_debug_log("mem_viewer_open rejected memory=%p size=%zu", memory, size);
        return nullptr;
    }

    const pid_t parent_pid = getpid();
    const uintptr_t address = reinterpret_cast<uintptr_t>(memory);
    mem_viewer_debug_log("mem_viewer_open parent_pid=%ld address=0x%llx size=%zu",
        static_cast<long>(parent_pid),
        static_cast<unsigned long long>(address),
        size);
    const pid_t child = fork();
    if (child < 0) {
        mem_viewer_debug_log("fork failed: %s", std::strerror(errno));
        return nullptr;
    }

    if (child == 0) {
        char pid_arg[32];
        char addr_arg[32];
        char size_arg[32];
        std::snprintf(pid_arg, sizeof(pid_arg), "%ld", static_cast<long>(parent_pid));
        std::snprintf(addr_arg, sizeof(addr_arg), "%llu", static_cast<unsigned long long>(address));
        std::snprintf(size_arg, sizeof(size_arg), "%zu", size);

        const std::string helper = helper_path();
        mem_viewer_debug_log("child exec helper=%s --pid %s --address %s --size %s",
            helper.c_str(), pid_arg, addr_arg, size_arg);
        execl(helper.c_str(), helper.c_str(), "--pid", pid_arg, "--address", addr_arg, "--size", size_arg, static_cast<char *>(nullptr));
        mem_viewer_debug_log("execl failed for helper=%s: %s", helper.c_str(), std::strerror(errno));
        _exit(127);
    }

    mem_viewer_debug_log("spawned helper child_pid=%ld", static_cast<long>(child));
    auto *viewer = new MemViewer;
    viewer->process = new MemViewerProcess(child);
    return viewer;
}

extern "C" void mem_viewer_destroy(MemViewer *viewer) {
    if (viewer == nullptr || viewer->process == nullptr) {
        return;
    }

    const pid_t pid = viewer->process->pid;
    if (pid > 0) {
        mem_viewer_debug_log("destroy sending SIGTERM to child_pid=%ld", static_cast<long>(pid));
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
        mem_viewer_debug_log("child_pid=%ld reaped with status=0x%x", static_cast<long>(pid), status);
    }

    delete viewer->process;
    delete viewer;
}

extern "C" int mem_viewer_is_open(MemViewer *viewer) {
    if (viewer == nullptr || viewer->process == nullptr) {
        mem_viewer_debug_log("mem_viewer_is_open called with null viewer");
        return 0;
    }
    return child_is_alive(viewer->process->pid) ? 1 : 0;
}
