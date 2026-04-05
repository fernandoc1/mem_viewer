#include "shared_file_viewer.h"

#include "mem_viewer.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

namespace {

bool shared_file_viewer_debug_enabled() {
    static const bool enabled = []() {
        const char *value = std::getenv("BINARY_COMPARE_DEBUG");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void shared_file_viewer_debug_log(const char *fmt, ...) {
    if (!shared_file_viewer_debug_enabled()) {
        return;
    }
    std::fprintf(stderr, "[shared_file_viewer] ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

bool set_env_or_error(const char *name, const char *value, std::string &error) {
    if (setenv(name, value, 1) != 0) {
        error = std::string("failed to set ") + name;
        return false;
    }
    return true;
}

std::string join_note_paths(const std::vector<std::string> &note_paths) {
    std::string joined;
    for (size_t i = 0; i < note_paths.size(); ++i) {
        if (!joined.empty()) {
            joined += ":";
        }
        joined += note_paths[i];
    }
    return joined;
}

}  // namespace

bool load_file_into_shared_buffer(const char *path, SharedFileBuffer &buffer, std::string &error) {
    shared_file_viewer_debug_log("load_file_into_shared_buffer path=%s", path);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = std::string("failed to open file: ") + path;
        return false;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff file_size = input.tellg();
    if (file_size < 0) {
        error = std::string("failed to determine file size: ") + path;
        return false;
    }
    input.seekg(0, std::ios::beg);

    const size_t buffer_size = file_size == 0 ? 1 : static_cast<size_t>(file_size);
    auto *shared = static_cast<unsigned char *>(mem_viewer_shared_malloc(buffer_size));
    if (shared == nullptr) {
        error = "mem_viewer_shared_malloc failed";
        return false;
    }

    if (file_size == 0) {
        shared[0] = 0;
    } else if (!input.read(reinterpret_cast<char *>(shared), file_size)) {
        mem_viewer_shared_free(shared, buffer_size);
        error = std::string("failed to read file: ") + path;
        return false;
    }

    buffer.data = shared;
    buffer.size = buffer_size;
    shared_file_viewer_debug_log("load_file_into_shared_buffer done size=%zu", buffer.size);
    return true;
}

bool copy_bytes_into_shared_buffer(const uint8_t *data, size_t size, SharedFileBuffer &buffer, std::string &error) {
    shared_file_viewer_debug_log("copy_bytes_into_shared_buffer size=%zu", size);
    const size_t buffer_size = size == 0 ? 1 : size;
    auto *shared = static_cast<unsigned char *>(mem_viewer_shared_malloc(buffer_size));
    if (shared == nullptr) {
        error = "mem_viewer_shared_malloc failed";
        return false;
    }

    if (size == 0) {
        shared[0] = 0;
    } else {
        std::memcpy(shared, data, size);
    }

    buffer.data = shared;
    buffer.size = buffer_size;
    shared_file_viewer_debug_log("copy_bytes_into_shared_buffer done size=%zu", buffer.size);
    return true;
}

void free_shared_file_buffer(SharedFileBuffer &buffer) {
    if (buffer.data != nullptr) {
        mem_viewer_shared_free(buffer.data, buffer.size);
        buffer.data = nullptr;
        buffer.size = 0;
    }
}

int run_shared_file_viewer(const SharedFileBuffer &buffer, const std::vector<std::string> &note_paths, std::string &error) {
    if (buffer.data == nullptr || buffer.size == 0) {
        error = "shared buffer is empty";
        return 1;
    }

    shared_file_viewer_debug_log("run_shared_file_viewer begin size=%zu note_files=%zu", buffer.size, note_paths.size());

    if (!note_paths.empty()) {
        const std::string joined = join_note_paths(note_paths);
        shared_file_viewer_debug_log("run_shared_file_viewer MEM_VIEWER_NOTES length=%zu", joined.size());
        if (!set_env_or_error("MEM_VIEWER_NOTES", joined.c_str(), error)) {
            return 1;
        }
    }
    if (!set_env_or_error("MEM_VIEWER_DISABLE_AUTO_REFRESH", "1", error)) {
        return 1;
    }
    if (!set_env_or_error("MEM_VIEWER_STATIC_FILE", "1", error)) {
        return 1;
    }

    MemViewer *viewer = mem_viewer_open_shared(buffer.data, buffer.size);
    if (viewer == nullptr) {
        error = "mem_viewer_open_shared failed";
        return 1;
    }
    shared_file_viewer_debug_log("run_shared_file_viewer viewer opened");

    while (mem_viewer_is_open(viewer)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    mem_viewer_destroy(viewer);
    shared_file_viewer_debug_log("run_shared_file_viewer viewer closed");
    return 0;
}

int run_dual_shared_file_viewer(
    const SharedFileBuffer &left_buffer,
    const SharedFileBuffer &right_buffer,
    const std::vector<std::string> &note_paths,
    const std::string &left_title,
    const std::string &right_title,
    bool notes_read_only,
    std::string &error) {
    if (left_buffer.data == nullptr || left_buffer.size == 0 ||
        right_buffer.data == nullptr || right_buffer.size == 0) {
        error = "shared buffer is empty";
        return 1;
    }

    shared_file_viewer_debug_log(
        "run_dual_shared_file_viewer begin left_size=%zu right_size=%zu note_files=%zu read_only=%d",
        left_buffer.size,
        right_buffer.size,
        note_paths.size(),
        notes_read_only ? 1 : 0);

    if (!note_paths.empty()) {
        const std::string joined = join_note_paths(note_paths);
        shared_file_viewer_debug_log("run_dual_shared_file_viewer MEM_VIEWER_NOTES length=%zu", joined.size());
        if (!set_env_or_error("MEM_VIEWER_NOTES", joined.c_str(), error)) {
            return 1;
        }
    }
    if (!set_env_or_error("MEM_VIEWER_DISABLE_AUTO_REFRESH", "1", error) ||
        !set_env_or_error("MEM_VIEWER_STATIC_FILE", "1", error) ||
        !set_env_or_error("MEM_VIEWER_LEFT_TITLE", left_title.c_str(), error) ||
        !set_env_or_error("MEM_VIEWER_RIGHT_TITLE", right_title.c_str(), error) ||
        !set_env_or_error("MEM_VIEWER_NOTES_READ_ONLY", notes_read_only ? "1" : "0", error)) {
        return 1;
    }

    MemViewer *viewer = mem_viewer_open_shared_dual(
        left_buffer.data,
        left_buffer.size,
        right_buffer.data,
        right_buffer.size);
    if (viewer == nullptr) {
        error = "mem_viewer_open_shared_dual failed";
        return 1;
    }
    shared_file_viewer_debug_log("run_dual_shared_file_viewer viewer opened");

    while (mem_viewer_is_open(viewer)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    mem_viewer_destroy(viewer);
    shared_file_viewer_debug_log("run_dual_shared_file_viewer viewer closed");
    return 0;
}
