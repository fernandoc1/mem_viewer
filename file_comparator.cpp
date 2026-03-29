#include "file_comparator.h"
#include <fstream>
#include <algorithm>
#include <sstream>

DualFileBuffer::~DualFileBuffer() {
    buffer1_.reset();
    buffer2_.reset();
}

bool DualFileBuffer::loadFiles(const std::string& file1_path, const std::string& file2_path, std::string& error_msg) {
    // Open file 1
    std::ifstream file1(file1_path, std::ios::binary | std::ios::ate);
    if (!file1.is_open()) {
        error_msg = "Failed to open file 1: " + file1_path;
        return false;
    }

    size_t size1 = file1.tellg();
    if (size1 > MAX_FILE_SIZE) {
        std::ostringstream oss;
        oss << "File 1 is too large (" << (size1 / 1024 / 1024) << "MB, max " 
            << (MAX_FILE_SIZE / 1024 / 1024) << "MB)";
        error_msg = oss.str();
        return false;
    }

    file1.seekg(0, std::ios::beg);

    // Open file 2
    std::ifstream file2(file2_path, std::ios::binary | std::ios::ate);
    if (!file2.is_open()) {
        error_msg = "Failed to open file 2: " + file2_path;
        return false;
    }

    size_t size2 = file2.tellg();
    if (size2 > MAX_FILE_SIZE) {
        std::ostringstream oss;
        oss << "File 2 is too large (" << (size2 / 1024 / 1024) << "MB, max " 
            << (MAX_FILE_SIZE / 1024 / 1024) << "MB)";
        error_msg = oss.str();
        return false;
    }

    file2.seekg(0, std::ios::beg);

    // Allocate buffers
    try {
        buffer1_ = std::make_unique<uint8_t[]>(size1);
        buffer2_ = std::make_unique<uint8_t[]>(size2);
    } catch (const std::bad_alloc&) {
        error_msg = "Failed to allocate memory for file buffers";
        return false;
    }

    // Read file 1
    if (!file1.read(reinterpret_cast<char*>(buffer1_.get()), size1)) {
        error_msg = "Failed to read file 1";
        return false;
    }

    // Read file 2
    if (!file2.read(reinterpret_cast<char*>(buffer2_.get()), size2)) {
        error_msg = "Failed to read file 2";
        return false;
    }

    size1_ = size1;
    size2_ = size2;

    // Analyze differences
    analyzeDifferences();

    return true;
}

void DualFileBuffer::analyzeDifferences() {
    diff_positions_.clear();

    size_t max_size = std::max(size1_, size2_);

    for (size_t i = 0; i < max_size; ++i) {
        uint8_t b1 = (i < size1_) ? buffer1_[i] : 0;
        uint8_t b2 = (i < size2_) ? buffer2_[i] : 0;

        // Consider position different if bytes differ or if one file is shorter
        bool differs = (b1 != b2);
        if (i >= size1_ || i >= size2_) {
            differs = true;
        }

        if (differs) {
            diff_positions_.push_back(i);
        }
    }
}

bool DualFileBuffer::isDifferentAt(size_t position) const {
    return std::binary_search(diff_positions_.begin(), diff_positions_.end(), position);
}

size_t DualFileBuffer::getNextDiffPosition(size_t current_position) const {
    if (diff_positions_.empty()) {
        return 0;
    }

    auto it = std::upper_bound(diff_positions_.begin(), diff_positions_.end(), current_position);
    if (it != diff_positions_.end()) {
        return *it;
    }

    return diff_positions_.front();
}

size_t DualFileBuffer::getPrevDiffPosition(size_t current_position) const {
    if (diff_positions_.empty()) {
        return 0;
    }

    auto it = std::lower_bound(diff_positions_.begin(), diff_positions_.end(), current_position);
    if (it != diff_positions_.begin()) {
        --it;
        return *it;
    }

    return diff_positions_.back();
}

bool DualFileBuffer::getDiffAt(size_t position, DiffInfo& out_diff) const {
    if (!isDifferentAt(position)) {
        return false;
    }

    out_diff.position = position;
    out_diff.byte1 = (position < size1_) ? buffer1_[position] : 0;
    out_diff.byte2 = (position < size2_) ? buffer2_[position] : 0;

    return true;
}
