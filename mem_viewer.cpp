#include "mem_viewer.h"

#include <gtkmm.h>
#include <gtkmm/init.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr size_t kBytesPerRow = 16;
constexpr int kRefreshMs = 100;
constexpr double kFadeSeconds = 1.6;

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

class MemViewerWindow : public Gtk::Window {
public:
    MemViewerWindow(uint8_t *memory, size_t size, std::atomic<bool> &open_flag)
        : memory_(memory),
          size_(size),
          open_flag_(open_flag),
          root_(Gtk::Orientation::VERTICAL, 8),
          toolbar_(Gtk::Orientation::HORIZONTAL, 8),
          search_nav_(Gtk::Orientation::HORIZONTAL, 4),
          auto_refresh_("Auto refresh"),
          refresh_button_("Refresh"),
          apply_button_("Write byte"),
          prev_button_("Prev"),
          next_button_("Next"),
          status_label_("No byte selected"),
          rows_(size_ == 0 ? 0 : ((size_ + kBytesPerRow - 1) / kBytesPerRow)),
          last_seen_(size_),
          changed_at_(size_, -1.0),
          match_mask_(size_, 0) {
        set_title("Memory Viewer");
        set_default_size(1200, 720);

        if (size_ > 0) {
            std::memcpy(last_seen_.data(), memory_, size_);
        }

        root_.set_margin_top(8);
        root_.set_margin_bottom(8);
        root_.set_margin_start(8);
        root_.set_margin_end(8);
        set_child(root_);

        auto_refresh_.set_active(true);
        toolbar_.append(auto_refresh_);
        toolbar_.append(refresh_button_);

        width_combo_.append("1");
        width_combo_.append("2");
        width_combo_.append("4");
        width_combo_.append("8");
        width_combo_.set_active(0);
        toolbar_.append(*Gtk::make_managed<Gtk::Label>("Search"));
        toolbar_.append(search_entry_);
        toolbar_.append(*Gtk::make_managed<Gtk::Label>("Format"));
        format_combo_.append("Hex");
        format_combo_.append("Decimal");
        format_combo_.set_active(0);
        toolbar_.append(format_combo_);
        toolbar_.append(*Gtk::make_managed<Gtk::Label>("Bytes"));
        toolbar_.append(width_combo_);
        toolbar_.append(*Gtk::make_managed<Gtk::Label>("Endian"));
        endian_combo_.append("Little");
        endian_combo_.append("Big");
        endian_combo_.set_active(0);
        toolbar_.append(endian_combo_);
        search_nav_.append(prev_button_);
        search_nav_.append(next_button_);
        toolbar_.append(search_nav_);

        edit_entry_.set_placeholder_text("Selected byte: hex or decimal");
        toolbar_.append(edit_entry_);
        toolbar_.append(apply_button_);

        root_.append(toolbar_);
        root_.append(status_label_);

        scrolled_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        scrolled_.set_child(area_);
        root_.append(scrolled_);

        click_controller_ = Gtk::GestureClick::create();
        click_controller_->set_button(1);

        area_.set_draw_func(sigc::mem_fun(*this, &MemViewerWindow::on_draw));
        area_.set_hexpand(true);
        area_.set_vexpand(true);
        area_.set_content_width(1200);
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
        if (size_ == 0) {
            area_.queue_draw();
            return;
        }

        auto vadj = scrolled_.get_vadjustment();
        const double value = vadj ? vadj->get_value() : 0.0;
        const double page_size = vadj ? vadj->get_page_size() : 0.0;
        const size_t first_row = static_cast<size_t>(std::max(0.0, std::floor(value / row_height_)));
        const size_t visible_rows = static_cast<size_t>(std::ceil(page_size / row_height_)) + 2;
        const size_t last_row = std::min(rows_, first_row + visible_rows);
        const size_t begin = std::min(size_, first_row * kBytesPerRow);
        const size_t end = std::min(size_, last_row * kBytesPerRow);
        const double now = now_seconds();

        bool changed = false;
        for (size_t i = begin; i < end; ++i) {
            const uint8_t current = memory_[i];
            if (current != last_seen_[i]) {
                last_seen_[i] = current;
                changed_at_[i] = now;
                changed = true;
            }
        }

        if (selected_index_ < size_) {
            update_status();
        }

        if (changed || begin != last_visible_begin_ || end != last_visible_end_) {
            last_visible_begin_ = begin;
            last_visible_end_ = end;
            area_.queue_draw();
        } else {
            area_.queue_draw();
        }
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

        size_t width = current_search_width();
        if (width == 0 || width > 8 || width > size_) {
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

        for (size_t i = 0; i + width <= size_; ++i) {
            bool matched = true;
            for (size_t j = 0; j < width; ++j) {
                if (memory_[i + j] != pattern[j]) {
                    matched = false;
                    break;
                }
            }
            if (!matched) {
                continue;
            }
            matches_.push_back(i);
            for (size_t j = 0; j < width; ++j) {
                match_mask_[i + j] = 1;
            }
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
        update_status();
        area_.queue_draw();
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
        if (selected_index_ >= size_) {
            return;
        }
        uint8_t value = 0;
        if (!parse_byte_value(edit_entry_.get_text(), value)) {
            return;
        }
        memory_[selected_index_] = value;
        last_seen_[selected_index_] = value;
        changed_at_[selected_index_] = now_seconds();
        update_status();
        area_.queue_draw();
    }

    void update_status() {
        if (selected_index_ >= size_) {
            std::ostringstream ss;
            ss << "Buffer size: " << size_ << " bytes";
            if (!matches_.empty()) {
                ss << " | Matches: " << matches_.size();
            }
            status_label_.set_text(ss.str());
            return;
        }

        char text[256];
        std::snprintf(
            text,
            sizeof(text),
            "Selected: 0x%08zx | hex=%02X | dec=%u | Matches=%zu",
            selected_index_,
            memory_[selected_index_],
            static_cast<unsigned>(memory_[selected_index_]),
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
        if (hex_x < 0) {
            return;
        }

        const int cell = static_cast<int>(hex_x / hex_cell_width_);
        const int byte_in_row = cell / 3;
        if (byte_in_row < 0 || byte_in_row >= static_cast<int>(kBytesPerRow)) {
            return;
        }

        size_t index = row * kBytesPerRow + static_cast<size_t>(byte_in_row);
        if (index >= size_) {
            return;
        }

        selected_index_ = index;
        char text[8];
        std::snprintf(text, sizeof(text), "%02X", memory_[index]);
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
                if (index >= size_) {
                    break;
                }

                const double cell_x = hex_start_x_ + static_cast<double>(col * 3) * hex_cell_width_;
                const double ascii_x = ascii_start_x_ + static_cast<double>(col) * ascii_cell_width_;
                const bool selected = index == selected_index_;
                const bool matched = match_mask_[index] != 0;
                const double age = changed_at_[index] < 0.0 ? kFadeSeconds : (now - changed_at_[index]);
                const double fade = std::clamp(1.0 - (age / kFadeSeconds), 0.0, 1.0);

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
                std::snprintf(hex, sizeof(hex), "%02X", memory_[index]);
                draw_text(cr, cell_x, y + baseline_y_, 0.93, 0.93, 0.94, hex);

                char ascii[2] = { printable(memory_[index]), '\0' };
                draw_text(cr, ascii_x, y + baseline_y_, 0.80, 0.86, 0.89, ascii);
            }
        }
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

    uint8_t *memory_;
    size_t size_;
    std::atomic<bool> &open_flag_;

    Gtk::Box root_;
    Gtk::Box toolbar_;
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

    size_t selected_index_ = std::numeric_limits<size_t>::max();
    size_t active_match_index_ = 0;
    size_t last_visible_begin_ = std::numeric_limits<size_t>::max();
    size_t last_visible_end_ = std::numeric_limits<size_t>::max();

    const double row_height_ = 24.0;
    const double font_size_ = 14.0;
    const double baseline_y_ = 17.0;
    const double hex_start_x_ = 130.0;
    const double ascii_start_x_ = 710.0;
    const double hex_cell_width_ = 13.0;
    const double ascii_cell_width_ = 12.0;
};

struct MemViewerImpl {
    explicit MemViewerImpl(void *memory_ptr, size_t memory_size)
        : memory(static_cast<uint8_t *>(memory_ptr)), size(memory_size), open(false), shutdown(false) {}

    uint8_t *memory;
    size_t size;
    std::atomic<bool> open;
    std::atomic<bool> shutdown;
    std::thread ui_thread;
    Glib::RefPtr<Glib::MainLoop> loop;
    MemViewerWindow *window = nullptr;
};

static void run_ui_thread(MemViewerImpl *impl) {
    Glib::init();
    Gio::init();
    Gtk::init_gtkmm_internals();

    if (gtk_init_check() == FALSE) {
        impl->open.store(false, std::memory_order_relaxed);
        return;
    }

    MemViewerWindow window(impl->memory, impl->size, impl->open);
    auto loop = Glib::MainLoop::create(false);
    impl->loop = loop;
    impl->window = &window;
    impl->open.store(true, std::memory_order_relaxed);
    window.signal_hide().connect([loop]() { loop->quit(); });
    window.present();
    loop->run();
    impl->window = nullptr;
    impl->loop.reset();
    impl->open.store(false, std::memory_order_relaxed);
}

}  // namespace

struct MemViewer {
    MemViewerImpl *impl;
};

extern "C" MemViewer *mem_viewer_open(void *memory, size_t size) {
    if (memory == nullptr || size == 0) {
        return nullptr;
    }

    auto *viewer = new MemViewer;
    viewer->impl = new MemViewerImpl(memory, size);
    viewer->impl->ui_thread = std::thread(run_ui_thread, viewer->impl);
    return viewer;
}

extern "C" void mem_viewer_destroy(MemViewer *viewer) {
    if (viewer == nullptr || viewer->impl == nullptr) {
        return;
    }

    MemViewerImpl *impl = viewer->impl;
    impl->shutdown.store(true, std::memory_order_relaxed);

    if (impl->loop) {
        auto loop = impl->loop;
        Glib::signal_idle().connect_once([loop]() { loop->quit(); });
    }

    if (impl->ui_thread.joinable()) {
        impl->ui_thread.join();
    }

    delete impl;
    delete viewer;
}

extern "C" int mem_viewer_is_open(MemViewer *viewer) {
    if (viewer == nullptr || viewer->impl == nullptr) {
        return 0;
    }
    return viewer->impl->open.load(std::memory_order_relaxed) ? 1 : 0;
}
