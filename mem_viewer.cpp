#include "mem_viewer.h"
#include "mem_viewer_gui.h"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <string_view>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

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
    const char *env_helper = std::getenv("MEM_VIEWER_HELPER");
    if (env_helper != nullptr && env_helper[0] != '\0') {
        return env_helper;
    }

    std::vector<std::string> candidates;

    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&mem_viewer_open), &info) != 0 && info.dli_fname != nullptr) {
        std::string path(info.dli_fname);
        const size_t slash = path.find_last_of('/');
        if (slash != std::string::npos) {
            candidates.push_back(path.substr(0, slash + 1) + "mem_viewer_helper");
        }
    }

    char exe_path[PATH_MAX];
    const ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (exe_len > 0) {
        exe_path[exe_len] = '\0';
        std::string path(exe_path);
        const size_t slash = path.find_last_of('/');
        if (slash != std::string::npos) {
            candidates.push_back(path.substr(0, slash + 1) + "mem_viewer_helper");
        }
    }

    candidates.emplace_back("./mem_viewer_helper");
    candidates.emplace_back("mem_viewer_helper");

    for (const auto &candidate : candidates) {
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }

    return candidates.empty() ? "mem_viewer_helper" : candidates.front();
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

extern "C" MemViewer *mem_viewer_open_shared(void *memory, size_t size) {
    if (memory == nullptr || size == 0) {
        mem_viewer_debug_log("mem_viewer_open_shared rejected memory=%p size=%zu", memory, size);
        return nullptr;
    }

    mem_viewer_debug_log("mem_viewer_open_shared memory=%p size=%zu", memory, size);
    const pid_t child = fork();
    if (child < 0) {
        mem_viewer_debug_log("fork failed in mem_viewer_open_shared: %s", std::strerror(errno));
        return nullptr;
    }

    if (child == 0) {
        mem_viewer_debug_log("shared child entering GUI directly");
        const int status = mem_viewer_run_gui_shared(memory, size);
        _exit(status == 0 ? 0 : 1);
    }

    mem_viewer_debug_log("spawned shared child_pid=%ld", static_cast<long>(child));
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

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

extern "C" void *mem_viewer_shared_malloc(size_t size) {
    if (size == 0) return nullptr;

    // Use memfd_create to create an anonymous file in RAM
    int fd = (int)syscall(SYS_memfd_create, "mem_viewer_shared", MFD_CLOEXEC);
    if (fd < 0) {
        mem_viewer_debug_log("memfd_create failed: %s", std::strerror(errno));
        return nullptr;
    }

    // Set the size of the shared memory region
    if (ftruncate(fd, size) < 0) {
        mem_viewer_debug_log("ftruncate failed: %s", std::strerror(errno));
        close(fd);
        return nullptr;
    }

    // Map the shared memory region into the process's address space
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        mem_viewer_debug_log("mmap failed: %s", std::strerror(errno));
        close(fd);
        return nullptr;
    }

    // Once mapped, the fd can be closed, but the mapping remains
    close(fd);
    return ptr;
}

extern "C" void mem_viewer_shared_free(void *ptr, size_t size) {
    if (ptr == nullptr || size == 0) return;
    munmap(ptr, size);
}
