#include "mem_viewer_gui.h"

#include <gtkmm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <sys/prctl.h>
#include <sys/uio.h>
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

    std::fprintf(stderr, "[mem_viewer_helper pid=%ld] ", static_cast<long>(getpid()));
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

constexpr size_t kBytesPerRow = 16;
constexpr int kRefreshMs = 100;
constexpr double kFadeSeconds = 1.6;
constexpr size_t kSearchChunkBytes = 64 * 1024;

enum class SearchFormat {
    Hex,
    Decimal,
};

enum class EndianMode {
    Little,
    Big,
};

static std::string trim_copy(const std::string &input) {
    size_t begin = 0;
    size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return input.substr(begin, end - begin);
}

static bool parse_byte_value(const std::string &text, uint8_t &value) {
    std::string s = trim_copy(text);
    if (s.empty()) {
        return false;
    }

    int base = 10;
    std::string digits = s;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits = digits.substr(2);
    } else {
        bool looks_hex = false;
        for (char c : digits) {
            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                looks_hex = true;
                break;
            }
        }
        if (looks_hex || digits.size() == 2) {
            base = 16;
        }
    }

    char *end = nullptr;
    unsigned long parsed = std::strtoul(digits.c_str(), &end, base);
    if (end == nullptr || *end != '\0' || parsed > 0xffUL) {
        return false;
    }
    value = static_cast<uint8_t>(parsed);
    return true;
}

static bool parse_uint64_value(const std::string &text, SearchFormat format, uint64_t &value) {
    std::string s = trim_copy(text);
    if (s.empty()) {
        return false;
    }

    int base = format == SearchFormat::Hex ? 16 : 10;
    if (base == 16 && s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s = s.substr(2);
    }

    char *end = nullptr;
    unsigned long long parsed = std::strtoull(s.c_str(), &end, base);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

class RemoteMemory {
public:
    RemoteMemory(pid_t pid, uintptr_t address, size_t size)
        : pid_(pid), address_(address), size_(size) {}

    size_t size() const {
        return size_;
    }

    bool read(size_t offset, void *buffer, size_t length) const {
        if (offset >= size_) {
            return false;
        }
        const size_t bounded = std::min(length, size_ - offset);
        struct iovec local = {buffer, bounded};
        struct iovec remote = {reinterpret_cast<void *>(address_ + offset), bounded};
        const ssize_t result = process_vm_readv(pid_, &local, 1, &remote, 1, 0);
        if (result != static_cast<ssize_t>(bounded) && mem_viewer_debug_enabled()) {
            mem_viewer_debug_log("process_vm_readv failed pid=%ld offset=%zu length=%zu result=%zd errno=%d (%s)",
                static_cast<long>(pid_), offset, bounded, result, errno, std::strerror(errno));
        }
        return result == static_cast<ssize_t>(bounded);
    }

    bool write(size_t offset, const void *buffer, size_t length) const {
        if (offset >= size_) {
            return false;
        }
        const size_t bounded = std::min(length, size_ - offset);
        struct iovec local = {const_cast<void *>(buffer), bounded};
        struct iovec remote = {reinterpret_cast<void *>(address_ + offset), bounded};
        const ssize_t result = process_vm_writev(pid_, &local, 1, &remote, 1, 0);
        if (result != static_cast<ssize_t>(bounded) && mem_viewer_debug_enabled()) {
            mem_viewer_debug_log("process_vm_writev failed pid=%ld offset=%zu length=%zu result=%zd errno=%d (%s)",
                static_cast<long>(pid_), offset, bounded, result, errno, std::strerror(errno));
        }
        return result == static_cast<ssize_t>(bounded);
    }

    bool read_byte(size_t offset, uint8_t &value) const {
        return read(offset, &value, 1);
    }

    bool write_byte(size_t offset, uint8_t value) const {
        return write(offset, &value, 1);
    }

private:
    pid_t pid_;
    uintptr_t address_;
    size_t size_;
};

class MemViewerWindow : public Gtk::Window {
public:
    MemViewerWindow(pid_t target_pid, uintptr_t target_address, size_t size, std::atomic<bool> &open_flag)
        : memory_(target_pid, target_address, size),
          open_flag_(open_flag),
          root_(Gtk::Orientation::HORIZONTAL, 10),
          main_panel_(Gtk::Orientation::VERTICAL, 8),
          side_panel_(Gtk::Orientation::VERTICAL, 8),
          search_nav_(Gtk::Orientation::HORIZONTAL, 4),
          auto_refresh_("Auto refresh"),
          refresh_button_("Refresh"),
          apply_button_("Write byte"),
          prev_button_("Prev"),
          next_button_("Next"),
          status_label_("No byte selected"),
          rows_(memory_.size() == 0 ? 0 : ((memory_.size() + kBytesPerRow - 1) / kBytesPerRow)),
          last_seen_(memory_.size(), 0),
          changed_at_(memory_.size(), -1.0),
          match_mask_(memory_.size(), 0) {
        mem_viewer_debug_log("creating window for target_pid=%ld address=0x%llx size=%zu",
            static_cast<long>(target_pid),
            static_cast<unsigned long long>(target_address),
            size);
        set_title("Memory Viewer");
        set_default_size(900, 680);
        set_hide_on_close(true);

        root_.set_margin_top(8);
        root_.set_margin_bottom(8);
        root_.set_margin_start(8);
        root_.set_margin_end(8);
        set_child(root_);

        main_panel_.set_hexpand(true);
        main_panel_.set_vexpand(true);
        root_.append(main_panel_);

        side_panel_.set_size_request(220, -1);
        root_.append(side_panel_);

        auto_refresh_.set_active(true);

        width_combo_.append("1");
        width_combo_.append("2");
        width_combo_.append("4");
        width_combo_.append("8");
        width_combo_.set_active(0);
        format_combo_.append("Hex");
        format_combo_.append("Decimal");
        format_combo_.set_active(0);
        endian_combo_.append("Little");
        endian_combo_.append("Big");
        endian_combo_.set_active(0);

        auto *refresh_frame = Gtk::make_managed<Gtk::Frame>("Refresh");
        auto *refresh_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        refresh_box->set_margin_top(8);
        refresh_box->set_margin_bottom(8);
        refresh_box->set_margin_start(8);
        refresh_box->set_margin_end(8);
        refresh_box->append(auto_refresh_);
        refresh_box->append(refresh_button_);
        refresh_frame->set_child(*refresh_box);
        side_panel_.append(*refresh_frame);

        auto *search_frame = Gtk::make_managed<Gtk::Frame>("Search");
        auto *search_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        search_box->set_margin_top(8);
        search_box->set_margin_bottom(8);
        search_box->set_margin_start(8);
        search_box->set_margin_end(8);
        search_box->append(*Gtk::make_managed<Gtk::Label>("Value"));
        search_box->append(search_entry_);
        search_box->append(*Gtk::make_managed<Gtk::Label>("Format"));
        search_box->append(format_combo_);
        search_box->append(*Gtk::make_managed<Gtk::Label>("Bytes"));
        search_box->append(width_combo_);
        search_box->append(*Gtk::make_managed<Gtk::Label>("Endian"));
        search_box->append(endian_combo_);
        search_nav_.append(prev_button_);
        search_nav_.append(next_button_);
        search_box->append(search_nav_);
        search_frame->set_child(*search_box);
        side_panel_.append(*search_frame);

        edit_entry_.set_placeholder_text("Selected byte: hex or decimal");
        auto *edit_frame = Gtk::make_managed<Gtk::Frame>("Edit");
        auto *edit_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        edit_box->set_margin_top(8);
        edit_box->set_margin_bottom(8);
        edit_box->set_margin_start(8);
        edit_box->set_margin_end(8);
        edit_box->append(*Gtk::make_managed<Gtk::Label>("Selected byte"));
        edit_box->append(edit_entry_);
        edit_box->append(apply_button_);
        edit_frame->set_child(*edit_box);
        side_panel_.append(*edit_frame);

        auto *status_frame = Gtk::make_managed<Gtk::Frame>("Status");
        auto *status_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        status_box->set_margin_top(8);
        status_box->set_margin_bottom(8);
        status_box->set_margin_start(8);
        status_box->set_margin_end(8);
        status_label_.set_wrap(true);
        status_box->append(status_label_);
        status_frame->set_child(*status_box);
        side_panel_.append(*status_frame);

        scrolled_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        scrolled_.set_child(area_);
        main_panel_.append(scrolled_);

        click_controller_ = Gtk::GestureClick::create();
        click_controller_->set_button(1);

        area_.set_draw_func(sigc::mem_fun(*this, &MemViewerWindow::on_draw));
        area_.set_hexpand(true);
        area_.set_vexpand(true);
        area_.set_content_width(760);
        area_.set_content_height(static_cast<int>(rows_ * row_height_));
        area_.add_controller(click_controller_);

        click_controller_->signal_pressed().connect(sigc::mem_fun(*this, &MemViewerWindow::on_click));
        refresh_button_.signal_clicked().connect(sigc::mem_fun(*this, &MemViewerWindow::refresh_visible_bytes));
        apply_button_.signal_clicked().connect(sigc::mem_fun(*this, &MemViewerWindow::apply_edit));
        prev_button_.signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &MemViewerWindow::navigate_match), -1));
        next_button_.signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &MemViewerWindow::navigate_match), 1));
        search_entry_.signal_activate().connect(sigc::mem_fun(*this, &MemViewerWindow::rebuild_search));
        format_combo_.signal_changed().connect(sigc::mem_fun(*this, &MemViewerWindow::rebuild_search));
        width_combo_.signal_changed().connect(sigc::mem_fun(*this, &MemViewerWindow::rebuild_search));
        endian_combo_.signal_changed().connect(sigc::mem_fun(*this, &MemViewerWindow::rebuild_search));
        signal_close_request().connect(sigc::mem_fun(*this, &MemViewerWindow::on_close_request), false);

        if (auto vadj = scrolled_.get_vadjustment()) {
            vadj->signal_value_changed().connect(sigc::mem_fun(*this, &MemViewerWindow::on_scroll_changed));
        }

        timer_ = Glib::signal_timeout().connect(sigc::mem_fun(*this, &MemViewerWindow::on_timer), kRefreshMs);
        refresh_visible_bytes();
    }

    ~MemViewerWindow() override {
        open_flag_.store(false, std::memory_order_relaxed);
    }

private:
    bool on_close_request() {
        mem_viewer_debug_log("window close request");
        timer_.disconnect();
        open_flag_.store(false, std::memory_order_relaxed);
        hide();
        return false;
    }

    bool on_timer() {
        if (!auto_refresh_.get_active()) {
            return true;
        }
        refresh_visible_bytes();
        return true;
    }

    void on_scroll_changed() {
        refresh_visible_bytes();
    }

    void refresh_visible_bytes() {
        if (memory_.size() == 0) {
            area_.queue_draw();
            return;
        }

        auto vadj = scrolled_.get_vadjustment();
        const double value = vadj ? vadj->get_value() : 0.0;
        const double page_size = vadj ? vadj->get_page_size() : 0.0;
        const size_t first_row = static_cast<size_t>(std::max(0.0, std::floor(value / row_height_)));
        const size_t visible_rows = static_cast<size_t>(std::ceil(page_size / row_height_)) + 2;
        const size_t last_row = std::min(rows_, first_row + visible_rows);
        const size_t begin = std::min(memory_.size(), first_row * kBytesPerRow);
        const size_t end = std::min(memory_.size(), last_row * kBytesPerRow);
        const double now = now_seconds();

        visible_begin_ = begin;
        visible_end_ = end;
        visible_cache_.resize(end > begin ? (end - begin) : 0);

        if (end > begin) {
            if (!memory_.read(begin, visible_cache_.data(), visible_cache_.size())) {
                std::fill(visible_cache_.begin(), visible_cache_.end(), 0);
            }
        }

        for (size_t i = 0; i < visible_cache_.size(); ++i) {
            const size_t index = begin + i;
            const uint8_t current = visible_cache_[i];
            if (current != last_seen_[index]) {
                last_seen_[index] = current;
                changed_at_[index] = now;
            }
        }

        if (selected_index_ < memory_.size()) {
            uint8_t value_byte = 0;
            if (memory_.read_byte(selected_index_, value_byte)) {
                last_seen_[selected_index_] = value_byte;
            }
        }

        update_status();
        area_.queue_draw();
    }

    void rebuild_search() {
        std::fill(match_mask_.begin(), match_mask_.end(), 0);
        matches_.clear();
        active_match_index_ = 0;

        uint64_t value = 0;
        if (!parse_uint64_value(search_entry_.get_text(), current_search_format(), value)) {
            area_.queue_draw();
            update_status();
            return;
        }

        const size_t width = current_search_width();
        if (width == 0 || width > 8 || width > memory_.size()) {
            area_.queue_draw();
            update_status();
            return;
        }

        std::vector<uint8_t> pattern(width);
        if (current_endian_mode() == EndianMode::Little) {
            for (size_t i = 0; i < width; ++i) {
                pattern[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xffU);
            }
        } else {
            for (size_t i = 0; i < width; ++i) {
                pattern[width - 1 - i] = static_cast<uint8_t>((value >> (8 * i)) & 0xffU);
            }
        }

        const size_t chunk_size = std::max(kSearchChunkBytes, width);
        std::vector<uint8_t> chunk(chunk_size + width);
        size_t offset = 0;

        while (offset < memory_.size()) {
            const size_t span = std::min(chunk_size, memory_.size() - offset);
            if (!memory_.read(offset, chunk.data(), span)) {
                break;
            }

            for (size_t i = 0; i + width <= span; ++i) {
                if (std::memcmp(chunk.data() + i, pattern.data(), width) != 0) {
                    continue;
                }
                const size_t match_index = offset + i;
                matches_.push_back(match_index);
                for (size_t j = 0; j < width; ++j) {
                    match_mask_[match_index + j] = 1;
                }
            }

            if (offset + span >= memory_.size()) {
                break;
            }
            offset += span >= width ? (span - width + 1) : span;
        }

        if (!matches_.empty()) {
            selected_index_ = matches_.front();
            scroll_to_index(selected_index_);
        }
        update_status();
        area_.queue_draw();
    }

    void navigate_match(int direction) {
        if (matches_.empty()) {
            return;
        }
        const size_t count = matches_.size();
        if (direction < 0) {
            active_match_index_ = (active_match_index_ + count - 1) % count;
        } else {
            active_match_index_ = (active_match_index_ + 1) % count;
        }
        selected_index_ = matches_[active_match_index_];
        scroll_to_index(selected_index_);
        refresh_visible_bytes();
    }

    void scroll_to_index(size_t index) {
        if (auto vadj = scrolled_.get_vadjustment()) {
            const double row_top = static_cast<double>((index / kBytesPerRow) * row_height_);
            const double row_bottom = row_top + row_height_;
            const double view_top = vadj->get_value();
            const double view_bottom = view_top + vadj->get_page_size();
            if (row_top < view_top) {
                vadj->set_value(row_top);
            } else if (row_bottom > view_bottom) {
                vadj->set_value(row_bottom - vadj->get_page_size());
            }
        }
    }

    void apply_edit() {
        if (selected_index_ >= memory_.size()) {
            return;
        }
        uint8_t value = 0;
        if (!parse_byte_value(edit_entry_.get_text(), value)) {
            return;
        }
        if (!memory_.write_byte(selected_index_, value)) {
            return;
        }
        last_seen_[selected_index_] = value;
        changed_at_[selected_index_] = now_seconds();
        refresh_visible_bytes();
    }

    void update_status() {
        if (selected_index_ >= memory_.size()) {
            std::ostringstream ss;
            ss << "Buffer size: " << memory_.size() << " bytes";
            if (!matches_.empty()) {
                ss << " | Matches: " << matches_.size();
            }
            status_label_.set_text(ss.str());
            return;
        }

        const uint8_t value = last_seen_[selected_index_];
        char text[256];
        std::snprintf(
            text,
            sizeof(text),
            "Selected: 0x%08zx | hex=%02X | dec=%u | Matches=%zu",
            selected_index_,
            value,
            static_cast<unsigned>(value),
            matches_.size());
        status_label_.set_text(text);
    }

    void on_click(int, double x, double y) {
        auto vadj = scrolled_.get_vadjustment();
        const double scroll_y = vadj ? vadj->get_value() : 0.0;
        const double absolute_y = y + scroll_y;
        const size_t row = static_cast<size_t>(absolute_y / row_height_);
        if (row >= rows_) {
            return;
        }

        const double hex_x = x - hex_start_x_;
        if (hex_x < 0.0) {
            return;
        }

        const int cell = static_cast<int>(hex_x / hex_cell_width_);
        const int byte_in_row = cell / 3;
        if (byte_in_row < 0 || byte_in_row >= static_cast<int>(kBytesPerRow)) {
            return;
        }

        const size_t index = row * kBytesPerRow + static_cast<size_t>(byte_in_row);
        if (index >= memory_.size()) {
            return;
        }

        selected_index_ = index;
        uint8_t value = last_seen_[index];
        if (memory_.read_byte(index, value)) {
            last_seen_[index] = value;
        }
        char text[8];
        std::snprintf(text, sizeof(text), "%02X", value);
        edit_entry_.set_text(text);
        update_status();
        area_.queue_draw();
    }

    void on_draw(const Cairo::RefPtr<Cairo::Context> &cr, int width, int height) {
        auto vadj = scrolled_.get_vadjustment();
        const double scroll_y = vadj ? vadj->get_value() : 0.0;
        const size_t first_row = static_cast<size_t>(std::max(0.0, std::floor(scroll_y / row_height_)));
        const size_t visible_rows = static_cast<size_t>(std::ceil(height / row_height_)) + 2;
        const size_t last_row = std::min(rows_, first_row + visible_rows);
        const double now = now_seconds();

        cr->set_source_rgb(0.08, 0.09, 0.10);
        cr->paint();

        cr->select_font_face("Monospace", Cairo::ToyFontFace::Slant::NORMAL, Cairo::ToyFontFace::Weight::NORMAL);
        cr->set_font_size(font_size_);

        for (size_t row = first_row; row < last_row; ++row) {
            const double y = static_cast<double>(row * row_height_);
            const size_t row_base = row * kBytesPerRow;

            if ((row % 2) == 0) {
                cr->set_source_rgba(1.0, 1.0, 1.0, 0.025);
                cr->rectangle(0.0, y, width, row_height_);
                cr->fill();
            }

            char addr[32];
            std::snprintf(addr, sizeof(addr), "%08zx", row_base);
            draw_text(cr, 16.0, y + baseline_y_, 0.74, 0.78, 0.82, addr);

            for (size_t col = 0; col < kBytesPerRow; ++col) {
                const size_t index = row_base + col;
                if (index >= memory_.size()) {
                    break;
                }

                const double cell_x = hex_start_x_ + static_cast<double>(col * 3) * hex_cell_width_;
                const double ascii_x = ascii_start_x_ + static_cast<double>(col) * ascii_cell_width_;
                const bool selected = index == selected_index_;
                const bool matched = match_mask_[index] != 0;
                const double age = changed_at_[index] < 0.0 ? kFadeSeconds : (now - changed_at_[index]);
                const double fade = std::clamp(1.0 - (age / kFadeSeconds), 0.0, 1.0);
                const uint8_t value = byte_for_index(index);

                if (fade > 0.0 || selected || matched) {
                    double r = 0.14;
                    double g = 0.14;
                    double b = 0.16;
                    double a = 0.0;

                    if (fade > 0.0) {
                        r = 0.96;
                        g = 0.38;
                        b = 0.14;
                        a = 0.16 + 0.42 * fade;
                    }
                    if (matched) {
                        r = 0.85;
                        g = 0.72;
                        b = 0.12;
                        a = std::max(a, 0.24);
                    }
                    if (selected) {
                        r = 0.22;
                        g = 0.64;
                        b = 1.0;
                        a = std::max(a, 0.35);
                    }

                    cr->set_source_rgba(r, g, b, a);
                    cr->rectangle(cell_x - 3.0, y + 2.0, hex_cell_width_ * 2.2, row_height_ - 4.0);
                    cr->fill();
                    cr->rectangle(ascii_x - 2.0, y + 2.0, ascii_cell_width_, row_height_ - 4.0);
                    cr->fill();
                }

                char hex[4];
                std::snprintf(hex, sizeof(hex), "%02X", value);
                draw_text(cr, cell_x, y + baseline_y_, 0.93, 0.93, 0.94, hex);

                char ascii[2] = { printable(value), '\0' };
                draw_text(cr, ascii_x, y + baseline_y_, 0.80, 0.86, 0.89, ascii);
            }
        }
    }

    uint8_t byte_for_index(size_t index) const {
        if (index >= visible_begin_ && index < visible_end_) {
            return visible_cache_[index - visible_begin_];
        }
        return last_seen_[index];
    }

    void draw_text(const Cairo::RefPtr<Cairo::Context> &cr, double x, double y, double r, double g, double b, const char *text) {
        cr->set_source_rgb(r, g, b);
        cr->move_to(x, y);
        cr->show_text(text);
    }

    static char printable(uint8_t value) {
        return std::isprint(static_cast<unsigned char>(value)) != 0 ? static_cast<char>(value) : '.';
    }

    static double now_seconds() {
        using clock = std::chrono::steady_clock;
        const auto now = clock::now().time_since_epoch();
        return std::chrono::duration<double>(now).count();
    }

    SearchFormat current_search_format() const {
        return format_combo_.get_active_row_number() == 0 ? SearchFormat::Hex : SearchFormat::Decimal;
    }

    EndianMode current_endian_mode() const {
        return endian_combo_.get_active_row_number() == 0 ? EndianMode::Little : EndianMode::Big;
    }

    size_t current_search_width() const {
        const auto text = width_combo_.get_active_text();
        if (text.empty()) {
            return 1;
        }
        return static_cast<size_t>(std::strtoul(text.c_str(), nullptr, 10));
    }

    RemoteMemory memory_;
    std::atomic<bool> &open_flag_;

    Gtk::Box root_;
    Gtk::Box main_panel_;
    Gtk::Box side_panel_;
    Gtk::Box search_nav_;
    Gtk::CheckButton auto_refresh_;
    Gtk::Button refresh_button_;
    Gtk::ScrolledWindow scrolled_;
    Gtk::DrawingArea area_;
    Glib::RefPtr<Gtk::GestureClick> click_controller_;
    Gtk::Entry search_entry_;
    Gtk::ComboBoxText format_combo_;
    Gtk::ComboBoxText width_combo_;
    Gtk::ComboBoxText endian_combo_;
    Gtk::Entry edit_entry_;
    Gtk::Button apply_button_;
    Gtk::Button prev_button_;
    Gtk::Button next_button_;
    Gtk::Label status_label_;
    sigc::connection timer_;

    const size_t rows_;
    std::vector<uint8_t> last_seen_;
    std::vector<double> changed_at_;
    std::vector<uint8_t> match_mask_;
    std::vector<size_t> matches_;
    std::vector<uint8_t> visible_cache_;

    size_t visible_begin_ = 0;
    size_t visible_end_ = 0;
    size_t selected_index_ = std::numeric_limits<size_t>::max();
    size_t active_match_index_ = 0;

    const double row_height_ = 20.0;
    const double font_size_ = 12.0;
    const double baseline_y_ = 14.5;
    const double hex_start_x_ = 84.0;
    const double ascii_start_x_ = 500.0;
    const double hex_cell_width_ = 8.6;
    const double ascii_cell_width_ = 8.2;
};

}  // namespace

int mem_viewer_run_gui(pid_t target_pid, uintptr_t target_address, size_t size) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    mem_viewer_debug_log("starting GUI target_pid=%ld address=0x%llx size=%zu",
        static_cast<long>(target_pid),
        static_cast<unsigned long long>(target_address),
        size);

    auto app = Gtk::Application::create("com.example.memviewer", Gio::Application::Flags::NON_UNIQUE);
    mem_viewer_debug_log("Gtk::Application created");
    std::atomic<bool> open_flag{false};
    std::unique_ptr<MemViewerWindow> window;

    app->signal_activate().connect([&]() {
        mem_viewer_debug_log("Gtk application activate");
        if (!window) {
            window = std::make_unique<MemViewerWindow>(target_pid, target_address, size, open_flag);
            window->signal_hide().connect([&]() { app->quit(); });
            app->add_window(*window);
            mem_viewer_debug_log("window created and added to application");
        }
        open_flag.store(true, std::memory_order_relaxed);
        window->present();
        mem_viewer_debug_log("window presented");
    });

    mem_viewer_debug_log("entering Gtk::Application::run()");
    const int rc = app->run();
    mem_viewer_debug_log("Gtk::Application::run() returned rc=%d", rc);
    return rc;
}
