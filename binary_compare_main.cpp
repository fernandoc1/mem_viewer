#include "file_comparator.h"
#include "shared_file_viewer.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

bool binary_compare_debug_enabled() {
    static const bool enabled = []() {
        const char *value = std::getenv("BINARY_COMPARE_DEBUG");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void binary_compare_debug_log(const char *fmt, ...) {
    if (!binary_compare_debug_enabled()) {
        return;
    }
    std::fprintf(stderr, "[binary_compare] ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

std::string json_escape(const std::string &text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

std::string hex_byte(uint8_t value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02X", static_cast<unsigned>(value));
    return std::string(buffer);
}

std::string hex_offset(size_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%zx", value);
    return std::string(buffer);
}

bool write_diff_annotations(
    const DualFileBuffer &dual_buffer,
    const std::string &file1_name,
    const std::string &file2_name,
    const std::string &path,
    std::string &error
) {
    binary_compare_debug_log("write_diff_annotations path=%s diff_count=%zu", path.c_str(), dual_buffer.getDiffCount());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to create diff notes file";
        return false;
    }

    const std::vector<size_t> &diffs = dual_buffer.getDiffPositions();
    out << "{\n  \"annotations\": [\n";
    bool first_annotation = true;
    size_t annotation_count = 0;
    for (size_t i = 0; i < diffs.size();) {
        const size_t range_start = diffs[i];
        size_t range_end = range_start;
        ++i;
        while (i < diffs.size() && diffs[i] == range_end + 1) {
            range_end = diffs[i];
            ++i;
        }

        std::string note = "diff vs " + file2_name + " from " + file1_name + ": ";
        DualFileBuffer::DiffInfo first_diff = {};
        DualFileBuffer::DiffInfo last_diff = {};
        dual_buffer.getDiffAt(range_start, first_diff);
        dual_buffer.getDiffAt(range_end, last_diff);
        note += hex_offset(range_start);
        if (range_end != range_start) {
            note += "-";
            note += hex_offset(range_end);
        }
        note += " ";
        note += hex_byte(first_diff.byte1);
        note += " != ";
        note += hex_byte(first_diff.byte2);
        if (range_end != range_start) {
            note += " ... ";
            note += hex_byte(last_diff.byte1);
            note += " != ";
            note += hex_byte(last_diff.byte2);
        }

        if (!first_annotation) {
            out << ",\n";
        }
        first_annotation = false;

        out << "    {\n";
        out << "      \"ranges\": [{\"start\": \"" << hex_offset(range_start)
            << "\", \"end\": \"" << hex_offset(range_end) << "\"}],\n";
        out << "      \"note\": \"" << json_escape(note) << "\",\n";
        out << "      \"color\": \"#ff9600\"\n";
        out << "    }";
        ++annotation_count;
        if (annotation_count <= 8 || (annotation_count % 10000) == 0) {
            binary_compare_debug_log("write_diff_annotations progress annotations=%zu range=%s-%s",
                annotation_count,
                hex_offset(range_start).c_str(),
                hex_offset(range_end).c_str());
        }
    }
    out << "\n  ]\n}\n";
    binary_compare_debug_log("write_diff_annotations done annotations=%zu", annotation_count);
    return true;
}

bool create_temp_diff_notes(
    const DualFileBuffer &dual_buffer,
    const std::string &file1_path,
    const std::string &file2_path,
    std::string &temp_path,
    std::string &error
) {
    binary_compare_debug_log("create_temp_diff_notes begin");
    char temp_template[] = "/tmp/binary_compare_diff_XXXXXX.json";
    const int fd = mkstemps(temp_template, 5);
    if (fd < 0) {
        error = "failed to create temporary diff notes file";
        return false;
    }
    close(fd);

    temp_path = temp_template;
    const std::string file1_name = std::filesystem::path(file1_path).filename().string();
    const std::string file2_name = std::filesystem::path(file2_path).filename().string();
    if (!write_diff_annotations(dual_buffer, file1_name, file2_name, temp_path, error)) {
        std::remove(temp_path.c_str());
        temp_path.clear();
        return false;
    }
    binary_compare_debug_log("create_temp_diff_notes done path=%s", temp_path.c_str());
    return true;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <file1> <file2> [notes.json ...]\n";
        return 1;
    }
    binary_compare_debug_log("main begin file1=%s file2=%s extra_notes=%d", argv[1], argv[2], argc - 3);

    DualFileBuffer dual_buffer;
    std::string error;
    if (!dual_buffer.loadFiles(argv[1], argv[2], error)) {
        std::cerr << error << "\n";
        return 1;
    }
    binary_compare_debug_log("main loaded files size1=%zu size2=%zu diff_count=%zu",
        dual_buffer.getSize1(),
        dual_buffer.getSize2(),
        dual_buffer.getDiffCount());

    SharedFileBuffer left_buffer;
    if (!copy_bytes_into_shared_buffer(dual_buffer.getBuffer1(), dual_buffer.getSize1(), left_buffer, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    SharedFileBuffer right_buffer;
    if (!copy_bytes_into_shared_buffer(dual_buffer.getBuffer2(), dual_buffer.getSize2(), right_buffer, error)) {
        free_shared_file_buffer(left_buffer);
        std::cerr << error << "\n";
        return 1;
    }
    binary_compare_debug_log(
        "main copied files into shared buffers left_size=%zu right_size=%zu",
        left_buffer.size,
        right_buffer.size);

    std::string diff_notes_path;
    if (!create_temp_diff_notes(dual_buffer, argv[1], argv[2], diff_notes_path, error)) {
        free_shared_file_buffer(left_buffer);
        free_shared_file_buffer(right_buffer);
        std::cerr << error << "\n";
        return 1;
    }
    binary_compare_debug_log("main diff notes ready path=%s", diff_notes_path.c_str());

    std::vector<std::string> note_paths;
    note_paths.push_back(diff_notes_path);
    for (int i = 3; i < argc; ++i) {
        note_paths.emplace_back(argv[i]);
    }

    const std::string left_title =
        "Binary Compare: " + std::filesystem::path(argv[1]).filename().string();
    const std::string right_title =
        "Binary Compare: " + std::filesystem::path(argv[2]).filename().string();
    const int rc = run_dual_shared_file_viewer(
        left_buffer,
        right_buffer,
        note_paths,
        left_title,
        right_title,
        true,
        error);
    binary_compare_debug_log("main viewer returned rc=%d", rc);
    free_shared_file_buffer(left_buffer);
    free_shared_file_buffer(right_buffer);
    std::remove(diff_notes_path.c_str());
    if (rc != 0 && !error.empty()) {
        std::cerr << error << "\n";
    }
    return rc;
}
