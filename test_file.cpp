#include "mem_viewer.h"

#include <cstdio>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <file>\n";
        return 1;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "failed to open file: " << argv[1] << "\n";
        return 1;
    }

    std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (buffer.empty()) {
        buffer.push_back(0);
    }

    MemViewer *viewer = mem_viewer_open(buffer.data(), buffer.size());
    if (!viewer) {
        std::cerr << "mem_viewer_open failed\n";
        return 1;
    }

    while (mem_viewer_is_open(viewer)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    mem_viewer_destroy(viewer);
    return 0;
}
