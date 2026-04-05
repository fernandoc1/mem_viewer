#ifndef SHARED_FILE_VIEWER_H
#define SHARED_FILE_VIEWER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SharedFileBuffer {
    unsigned char *data = nullptr;
    size_t size = 0;
};

bool load_file_into_shared_buffer(const char *path, SharedFileBuffer &buffer, std::string &error);
bool copy_bytes_into_shared_buffer(const uint8_t *data, size_t size, SharedFileBuffer &buffer, std::string &error);
void free_shared_file_buffer(SharedFileBuffer &buffer);
int run_shared_file_viewer(const SharedFileBuffer &buffer, const std::vector<std::string> &note_paths, std::string &error);

#endif
