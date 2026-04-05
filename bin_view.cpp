#include "mem_viewer.h"

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <file> [notes.json ...]\n";
        return 1;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "failed to open file: " << argv[1] << "\n";
        return 1;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff file_size = input.tellg();
    if (file_size < 0) {
        std::cerr << "failed to determine file size: " << argv[1] << "\n";
        return 1;
    }
    input.seekg(0, std::ios::beg);

    const size_t buffer_size = file_size == 0 ? 1 : static_cast<size_t>(file_size);
    auto *buffer = static_cast<unsigned char *>(mem_viewer_shared_malloc(buffer_size));
    if (!buffer) {
        std::cerr << "mem_viewer_shared_malloc failed\n";
        return 1;
    }

    if (file_size == 0) {
        buffer[0] = 0;
    } else if (!input.read(reinterpret_cast<char *>(buffer), file_size)) {
        std::cerr << "failed to read file: " << argv[1] << "\n";
        mem_viewer_shared_free(buffer, buffer_size);
        return 1;
    }

    if (argc > 2) {
        std::string notes_value;
        for (int i = 2; i < argc; ++i) {
            if (!notes_value.empty()) {
                notes_value += ":";
            }
            notes_value += argv[i];
        }
        if (setenv("MEM_VIEWER_NOTES", notes_value.c_str(), 1) != 0) {
            std::cerr << "failed to set MEM_VIEWER_NOTES\n";
            mem_viewer_shared_free(buffer, buffer_size);
            return 1;
        }
    }
    if (setenv("MEM_VIEWER_DISABLE_AUTO_REFRESH", "1", 1) != 0) {
        std::cerr << "failed to set MEM_VIEWER_DISABLE_AUTO_REFRESH\n";
        mem_viewer_shared_free(buffer, buffer_size);
        return 1;
    }
    if (setenv("MEM_VIEWER_STATIC_FILE", "1", 1) != 0) {
        std::cerr << "failed to set MEM_VIEWER_STATIC_FILE\n";
        mem_viewer_shared_free(buffer, buffer_size);
        return 1;
    }
    if (setenv("MEM_VIEWER_NOTES_READ_ONLY", "1", 1) != 0) {
        std::cerr << "failed to set MEM_VIEWER_NOTES_READ_ONLY\n";
        mem_viewer_shared_free(buffer, buffer_size);
        return 1;
    }

    MemViewer *viewer = mem_viewer_open_shared(buffer, buffer_size);
    if (!viewer) {
        std::cerr << "mem_viewer_open_shared failed\n";
        mem_viewer_shared_free(buffer, buffer_size);
        return 1;
    }

    while (mem_viewer_is_open(viewer)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    mem_viewer_destroy(viewer);
    mem_viewer_shared_free(buffer, buffer_size);
    return 0;
}
