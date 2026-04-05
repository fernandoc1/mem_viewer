#include "shared_file_viewer.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <file> [notes.json ...]\n";
        return 1;
    }

    SharedFileBuffer buffer;
    std::string error;
    if (!load_file_into_shared_buffer(argv[1], buffer, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    std::vector<std::string> note_paths;
    for (int i = 2; i < argc; ++i) {
        note_paths.emplace_back(argv[i]);
    }

    const int rc = run_shared_file_viewer(buffer, note_paths, error);
    free_shared_file_buffer(buffer);
    if (rc != 0 && !error.empty()) {
        std::cerr << error << "\n";
    }
    return rc;
}
