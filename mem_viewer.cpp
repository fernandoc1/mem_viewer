#include "mem_viewer.h"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <limits.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

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
        return nullptr;
    }

    const pid_t parent_pid = getpid();
    const uintptr_t address = reinterpret_cast<uintptr_t>(memory);
    const pid_t child = fork();
    if (child < 0) {
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
        execl(helper.c_str(), helper.c_str(), "--pid", pid_arg, "--address", addr_arg, "--size", size_arg, static_cast<char *>(nullptr));
        _exit(127);
    }

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
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
    }

    delete viewer->process;
    delete viewer;
}

extern "C" int mem_viewer_is_open(MemViewer *viewer) {
    if (viewer == nullptr || viewer->process == nullptr) {
        return 0;
    }
    return child_is_alive(viewer->process->pid) ? 1 : 0;
}
