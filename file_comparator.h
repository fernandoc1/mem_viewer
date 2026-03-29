#ifndef FILE_COMPARATOR_H
#define FILE_COMPARATOR_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <memory>

class DualFileBuffer {
public:
    struct DiffInfo {
        size_t position;
        uint8_t byte1;
        uint8_t byte2;
    };

    static constexpr size_t MAX_FILE_SIZE = 100 * 1024 * 1024; // 100MB limit

    DualFileBuffer() = default;
    ~DualFileBuffer();

    // Load files and perform initial diff analysis
    bool loadFiles(const std::string& file1_path, const std::string& file2_path, std::string& error_msg);

    // Get raw buffer access
    const uint8_t* getBuffer1() const { return buffer1_.get(); }
    const uint8_t* getBuffer2() const { return buffer2_.get(); }

    // Get sizes
    size_t getSize1() const { return size1_; }
    size_t getSize2() const { return size2_; }

    // Get the maximum size (for display)
    size_t getMaxSize() const { return std::max(size1_, size2_); }

    // Difference tracking
    const std::vector<size_t>& getDiffPositions() const { return diff_positions_; }
    size_t getDiffCount() const { return diff_positions_.size(); }

    // Navigate differences
    size_t getNextDiffPosition(size_t current_position) const;
    size_t getPrevDiffPosition(size_t current_position) const;

    // Check if byte at position differs
    bool isDifferentAt(size_t position) const;

    // Get difference info
    bool getDiffAt(size_t position, DiffInfo& out_diff) const;

private:
    void analyzeDifferences();

    std::unique_ptr<uint8_t[]> buffer1_;
    std::unique_ptr<uint8_t[]> buffer2_;
    size_t size1_ = 0;
    size_t size2_ = 0;

    std::vector<size_t> diff_positions_;
};

#endif
