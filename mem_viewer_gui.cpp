#include "mem_viewer_gui.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QMainWindow>

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
#include <csignal>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <unistd.h>

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

    std::fprintf(stderr, "[mem_viewer_gui pid=%ld] ", static_cast<long>(getpid()));
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
constexpr double kAddressX = 10.0;
constexpr double kAddressChars = 8.0;
constexpr double kAddressCharWidth = 7.0;
constexpr double kGapAddressToHex = 6.0;
constexpr double kGapHexToAscii = 6.0;

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

class LocalMemory {
public:
    LocalMemory(void *memory, size_t size)
        : memory_(static_cast<uint8_t *>(memory)), size_(size) {}

    size_t size() const {
        return size_;
    }

    bool read(size_t offset, void *buffer, size_t length) const {
        if (offset >= size_) {
            return false;
        }
        const size_t bounded = std::min(length, size_ - offset);
        std::memcpy(buffer, memory_ + offset, bounded);
        return true;
    }

    bool write(size_t offset, const void *buffer, size_t length) const {
        if (offset >= size_) {
            return false;
        }
        const size_t bounded = std::min(length, size_ - offset);
        std::memcpy(memory_ + offset, buffer, bounded);
        return true;
    }

    bool read_byte(size_t offset, uint8_t &value) const {
        return read(offset, &value, 1);
    }

    bool write_byte(size_t offset, uint8_t value) const {
        return write(offset, &value, 1);
    }

private:
    uint8_t *memory_;
    size_t size_;
};

class MemViewerWidget : public QWidget {
public:
    MemViewerWidget(
        size_t memory_size,
        std::function<bool(size_t, void *, size_t)> read_memory,
        std::function<bool(size_t, const void *, size_t)> write_memory,
        std::atomic<bool> &open_flag,
        QWidget *parent = nullptr)
        : QWidget(parent),
          memory_size_(memory_size),
          read_memory_(std::move(read_memory)),
          write_memory_(std::move(write_memory)),
          open_flag_(open_flag),
          rows_(memory_size_ == 0 ? 0 : ((memory_size_ + kBytesPerRow - 1) / kBytesPerRow)),
          last_seen_(memory_size_, 0),
          changed_at_(memory_size_, -1.0),
          match_mask_(memory_size_, 0),
          font_("Monospace", 11) {
        
        font_.setStyleHint(QFont::Monospace);
        font_.setFixedPitch(true);
        QFontMetrics fm(font_);

        char_width_ = fm.horizontalAdvance('W'); 
        row_height_ = static_cast<double>(fm.lineSpacing()) + 2.0;
        baseline_y_ = static_cast<double>(fm.ascent()) + 1.0;
        
        address_x_ = kAddressX;
        hex_cell_width_ = char_width_; 
        ascii_cell_width_ = char_width_;
        
        hex_start_x_ = address_x_ + kAddressChars * char_width_ + kGapAddressToHex;
        ascii_start_x_ = hex_start_x_ + (kBytesPerRow * 3 - 1) * hex_cell_width_ + kGapHexToAscii;
        content_width_ = ascii_start_x_ + kBytesPerRow * ascii_cell_width_ + 4.0;
        
        hex_highlight_width_ = hex_cell_width_ * 2.2;
        ascii_highlight_width_ = ascii_cell_width_ * 1.2;

        setFocusPolicy(Qt::StrongFocus);
        setMinimumWidth(static_cast<int>(content_width_));
        setMinimumHeight(static_cast<int>(rows_ * row_height_));
        
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, [this]() { onTimer(); });
        timer_->start(kRefreshMs);
        
        refreshVisibleBytes();
    }

    ~MemViewerWidget() override {
        open_flag_.store(false, std::memory_order_relaxed);
    }

    void setAutoRefresh(bool enabled) {
        if (enabled) {
            timer_->start(kRefreshMs);
        } else {
            timer_->stop();
        }
    }

    void refreshVisibleBytes() {
        if (memory_size_ == 0) {
            update();
            return;
        }

        QScrollBar *vscroll = verticalScrollBar();
        if (!vscroll) return;
        
        const int scroll_y = vscroll->value();
        const int visible_height = viewport()->height();
        const size_t first_row = static_cast<size_t>(std::max(0, static_cast<int>(std::floor(scroll_y / row_height_))));
        const size_t visible_rows = static_cast<size_t>(std::ceil(visible_height / row_height_)) + 2;
        const size_t last_row = std::min(rows_, first_row + visible_rows);
        const size_t begin = std::min(memory_size_, first_row * kBytesPerRow);
        const size_t end = std::min(memory_size_, last_row * kBytesPerRow);
        const double now = now_seconds();

        visible_begin_ = begin;
        visible_end_ = end;
        visible_cache_.resize(end > begin ? (end - begin) : 0);

        if (end > begin) {
            if (!read_memory_(begin, visible_cache_.data(), visible_cache_.size())) {
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

        if (selected_index_ < memory_size_) {
            uint8_t value_byte = 0;
            if (read_memory_(selected_index_, &value_byte, 1)) {
                last_seen_[selected_index_] = value_byte;
            }
        }

        update();
    }

    void rebuildSearch(const std::string &search_text, SearchFormat format, EndianMode endian, size_t width) {
        std::fill(match_mask_.begin(), match_mask_.end(), 0);
        matches_.clear();
        active_match_index_ = 0;

        uint64_t value = 0;
        if (!parse_uint64_value(search_text, format, value)) {
            update();
            if (onSearchStatusUpdated) onSearchStatusUpdated();
            return;
        }

        if (width == 0 || width > 8 || width > memory_size_) {
            update();
            if (onSearchStatusUpdated) onSearchStatusUpdated();
            return;
        }

        std::vector<uint8_t> pattern(width);
        if (endian == EndianMode::Little) {
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

        while (offset < memory_size_) {
            const size_t span = std::min(chunk_size, memory_size_ - offset);
            if (!read_memory_(offset, chunk.data(), span)) {
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

            if (offset + span >= memory_size_) {
                break;
            }
            offset += span >= width ? (span - width + 1) : span;
        }

        if (!matches_.empty()) {
            selected_index_ = matches_.front();
            scroll_to_index(selected_index_);
        }
        if (onSearchStatusUpdated) onSearchStatusUpdated();
        update();
    }

    void navigateMatch(int direction) {
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
        refreshVisibleBytes();
    }

    void setSelectedIndex(size_t index) {
        if (index >= memory_size_) {
            return;
        }
        selected_index_ = index;
        uint8_t value = last_seen_[index];
        if (read_memory_(index, &value, 1)) {
            last_seen_[index] = value;
        }
        if (onByteSelected) onByteSelected(index, value);
        update();
    }

    size_t getSelectedIndex() const {
        return selected_index_;
    }

    uint8_t getSelectedValue() const {
        if (selected_index_ >= memory_size_) {
            return 0;
        }
        return last_seen_[selected_index_];
    }

    size_t getMatchCount() const {
        return matches_.size();
    }

    size_t getMemorySize() const {
        return memory_size_;
    }

    void applyEdit(uint8_t value) {
        if (selected_index_ >= memory_size_) {
            return;
        }
        if (!write_memory_(selected_index_, &value, 1)) {
            return;
        }
        last_seen_[selected_index_] = value;
        changed_at_[selected_index_] = now_seconds();
        refreshVisibleBytes();
    }

    std::function<void(size_t, uint8_t)> onByteSelected;
    std::function<void()> onSearchStatusUpdated;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        
        QScrollBar *vscroll = verticalScrollBar();
        const int scroll_y = vscroll ? vscroll->value() : 0;
        const int visible_height = height();
        const size_t first_row = static_cast<size_t>(std::max(0, static_cast<int>(std::floor(scroll_y / row_height_))));
        const size_t visible_rows = static_cast<size_t>(std::ceil(visible_height / row_height_)) + 2;
        const size_t last_row = std::min(rows_, first_row + visible_rows);
        const double now = now_seconds();

        painter.fillRect(rect(), QColor(0x14, 0x16, 0x1A));

        painter.setFont(font_);

        for (size_t row = first_row; row < last_row; ++row) {
            const double y = static_cast<double>(row * row_height_);
            const size_t row_base = row * kBytesPerRow;

            if ((row % 2) == 0) {
                painter.fillRect(QRectF(0, y, width(), row_height_), QColor(0xFF, 0xFF, 0xFF, 6));
            }

            char addr[32];
            std::snprintf(addr, sizeof(addr), "%08zx", row_base);
            drawText(painter, address_x_, y + baseline_y_, QColor(0xBD, 0xC7, 0xD0), addr);

            for (size_t col = 0; col < kBytesPerRow; ++col) {
                const size_t index = row_base + col;
                if (index >= memory_size_) {
                    break;
                }

                const double cell_x = hex_start_x_ + static_cast<double>(col * 3) * hex_cell_width_;
                const double ascii_x = ascii_start_x_ + static_cast<double>(col) * ascii_cell_width_;
                const bool selected = index == selected_index_;
                const bool matched = match_mask_[index] != 0;
                const double age = changed_at_[index] < 0.0 ? kFadeSeconds : (now - changed_at_[index]);
                const double fade = std::clamp(1.0 - (age / kFadeSeconds), 0.0, 1.0);
                const uint8_t value = byteForIndex(index);

                if (fade > 0.0 || selected || matched) {
                    double r = 0.14;
                    double g = 0.14;
                    double b = 0.16;
                    int a = 0;

                    if (fade > 0.0) {
                        r = 0.96;
                        g = 0.38;
                        b = 0.14;
                        a = static_cast<int>(255 * (0.16 + 0.42 * fade));
                    }
                    if (matched) {
                        r = 0.85;
                        g = 0.72;
                        b = 0.12;
                        a = std::max(a, static_cast<int>(255 * 0.24));
                    }
                    if (selected) {
                        r = 0.22;
                        g = 0.64;
                        b = 1.0;
                        a = std::max(a, static_cast<int>(255 * 0.35));
                    }

                    painter.fillRect(QRectF(cell_x - 2.0, y + 1.5, hex_highlight_width_, row_height_ - 3.0),
                                     QColor(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255), a));
                    painter.fillRect(QRectF(ascii_x - 1.0, y + 1.5, ascii_highlight_width_, row_height_ - 3.0),
                                     QColor(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255), a));
                }

                char hex[4];
                std::snprintf(hex, sizeof(hex), "%02X", value);
                drawText(painter, cell_x, y + baseline_y_, QColor(0xEE, 0xEE, 0xEF), hex);

                char ascii[2] = { printable(value), '\0' };
                drawText(painter, ascii_x, y + baseline_y_, QColor(0xCD, 0xDB, 0xE2), ascii);
            }
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) {
            return;
        }

        QScrollBar *vscroll = verticalScrollBar();
        const int scroll_y = vscroll ? vscroll->value() : 0;
        const double absolute_y = event->position().y() + scroll_y;
        const size_t row = static_cast<size_t>(absolute_y / row_height_);
        if (row >= rows_) {
            return;
        }

        const double hex_x = event->position().x() - hex_start_x_;
        if (hex_x < -2.0) {
            return;
        }

        const int cell = static_cast<int>((hex_x + 2.0) / hex_cell_width_);
        const int byte_in_row = cell / 3;
        if (byte_in_row < 0 || byte_in_row >= static_cast<int>(kBytesPerRow)) {
            return;
        }

        const size_t index = row * kBytesPerRow + static_cast<size_t>(byte_in_row);
        if (index >= memory_size_) {
            return;
        }

        setSelectedIndex(index);
    }

    void wheelEvent(QWheelEvent *event) override {
        QScrollBar *vscroll = verticalScrollBar();
        if (vscroll) {
            const int delta = event->angleDelta().y();
            if (delta != 0) {
                const int numDegrees = delta / 8;
                const int numSteps = numDegrees / 15;
                for (int i = 0; i < std::abs(numSteps); ++i) {
                    if (numSteps > 0)
                        vscroll->triggerAction(QScrollBar::SliderSingleStepSub);
                    else
                        vscroll->triggerAction(QScrollBar::SliderSingleStepAdd);
                }
            }
            event->accept();
        }
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        updateGeometry();
    }

private:
    void onTimer() {
        refreshVisibleBytes();
    }

private:
    uint8_t byteForIndex(size_t index) const {
        if (index >= visible_begin_ && index < visible_end_) {
            return visible_cache_[index - visible_begin_];
        }
        return last_seen_[index];
    }

    void drawText(QPainter &painter, double x, double y, const QColor &color, const char *text) {
        painter.setPen(color);
        painter.drawText(QPointF(x, y), text);
    }

    static char printable(uint8_t value) {
        return std::isprint(static_cast<unsigned char>(value)) != 0 ? static_cast<char>(value) : '.';
    }

    static double now_seconds() {
        using clock = std::chrono::steady_clock;
        const auto now = clock::now().time_since_epoch();
        return std::chrono::duration<double>(now).count();
    }

    void scroll_to_index(size_t index) {
        QScrollBar *vscroll = verticalScrollBar();
        if (!vscroll) return;
        
        const double row_top = static_cast<double>((index / kBytesPerRow) * row_height_);
        const double row_bottom = row_top + row_height_;
        const double view_top = static_cast<double>(vscroll->value());
        const double view_bottom = view_top + vscroll->pageStep();
        if (row_top < view_top) {
            vscroll->setValue(static_cast<int>(row_top));
        } else if (row_bottom > view_bottom) {
            vscroll->setValue(static_cast<int>(row_bottom - vscroll->pageStep()));
        }
    }

    QScrollBar *verticalScrollBar() const {
        QWidget *p = parentWidget();
        while (p) {
            QScrollArea *sa = qobject_cast<QScrollArea *>(p);
            if (sa) return sa->verticalScrollBar();
            p = p->parentWidget();
        }
        return nullptr;
    }

    QWidget *viewport() {
        QWidget *p = parentWidget();
        while (p) {
            QScrollArea *sa = qobject_cast<QScrollArea *>(p);
            if (sa) return sa->viewport();
            p = p->parentWidget();
        }
        return this;
    }

    size_t memory_size_;
    std::function<bool(size_t, void *, size_t)> read_memory_;
    std::function<bool(size_t, const void *, size_t)> write_memory_;
    std::atomic<bool> &open_flag_;

    QTimer *timer_;

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

    QFont font_;
    double char_width_;
    double row_height_;
    double baseline_y_;
    double address_x_;
    double hex_cell_width_;
    double ascii_cell_width_;
    double hex_start_x_;
    double ascii_start_x_;
    double content_width_;
    double hex_highlight_width_;
    double ascii_highlight_width_;
};

class MemViewerWindow : public QMainWindow {
public:
    MemViewerWindow(
        size_t memory_size,
        std::function<bool(size_t, void *, size_t)> read_memory,
        std::function<bool(size_t, const void *, size_t)> write_memory,
        std::atomic<bool> &open_flag)
        : open_flag_(open_flag) {
        
        setWindowTitle("Memory Viewer");
        resize(900, 680);
        setAttribute(Qt::WA_DeleteOnClose);

        QWidget *central = new QWidget(this);
        setCentralWidget(central);

        QHBoxLayout *root_layout = new QHBoxLayout(central);
        root_layout->setContentsMargins(8, 8, 8, 8);
        root_layout->setSpacing(10);

        QWidget *main_panel = new QWidget();
        QVBoxLayout *main_layout = new QVBoxLayout(main_panel);
        main_layout->setContentsMargins(0, 0, 0, 0);
        main_layout->setSpacing(0);

        scroll_area_ = new QScrollArea();
        scroll_area_->setWidgetResizable(true);
        scroll_area_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll_area_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        main_layout->addWidget(scroll_area_);

        QWidget *side_panel = new QWidget();
        side_panel->setFixedWidth(180);
        QVBoxLayout *side_layout = new QVBoxLayout(side_panel);
        side_layout->setSpacing(8);

        auto_refresh_ = new QCheckBox("Auto refresh");
        auto_refresh_->setChecked(true);
        
        refresh_button_ = new QPushButton("Refresh");
        connect(refresh_button_, &QPushButton::clicked, this, [this]() {
            if (viewer_widget_) {
                viewer_widget_->refreshVisibleBytes();
            }
        });

        QFrame *refresh_frame = new QFrame();
        refresh_frame->setFrameStyle(QFrame::Box | QFrame::Raised);
        QVBoxLayout *refresh_layout = new QVBoxLayout(refresh_frame);
        refresh_layout->setContentsMargins(8, 8, 8, 8);
        refresh_layout->setSpacing(6);
        refresh_layout->addWidget(auto_refresh_);
        refresh_layout->addWidget(refresh_button_);
        side_layout->addWidget(refresh_frame);

        QFrame *search_frame = new QFrame();
        search_frame->setFrameStyle(QFrame::Box | QFrame::Raised);
        QVBoxLayout *search_layout = new QVBoxLayout(search_frame);
        search_layout->setContentsMargins(8, 8, 8, 8);
        search_layout->setSpacing(6);

        search_layout->addWidget(new QLabel("Value"));
        search_entry_ = new QLineEdit();
        search_layout->addWidget(search_entry_);

        search_layout->addWidget(new QLabel("Format"));
        format_combo_ = new QComboBox();
        format_combo_->addItem("Hex");
        format_combo_->addItem("Decimal");
        search_layout->addWidget(format_combo_);

        search_layout->addWidget(new QLabel("Bytes"));
        width_combo_ = new QComboBox();
        width_combo_->addItem("1");
        width_combo_->addItem("2");
        width_combo_->addItem("4");
        width_combo_->addItem("8");
        search_layout->addWidget(width_combo_);

        search_layout->addWidget(new QLabel("Endian"));
        endian_combo_ = new QComboBox();
        endian_combo_->addItem("Little");
        endian_combo_->addItem("Big");
        search_layout->addWidget(endian_combo_);

        QHBoxLayout *search_nav = new QHBoxLayout();
        prev_button_ = new QPushButton("Prev");
        next_button_ = new QPushButton("Next");
        search_nav->addWidget(prev_button_);
        search_nav->addWidget(next_button_);
        search_layout->addLayout(search_nav);

        connect(search_entry_, &QLineEdit::returnPressed, this, [this]() {
            rebuildSearch();
        });
        connect(format_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
            rebuildSearch();
        });
        connect(width_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
            rebuildSearch();
        });
        connect(endian_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
            rebuildSearch();
        });
        connect(prev_button_, &QPushButton::clicked, this, [this]() {
            if (viewer_widget_) {
                viewer_widget_->navigateMatch(-1);
            }
        });
        connect(next_button_, &QPushButton::clicked, this, [this]() {
            if (viewer_widget_) {
                viewer_widget_->navigateMatch(1);
            }
        });

        side_layout->addWidget(search_frame);

        QFrame *edit_frame = new QFrame();
        edit_frame->setFrameStyle(QFrame::Box | QFrame::Raised);
        QVBoxLayout *edit_layout = new QVBoxLayout(edit_frame);
        edit_layout->setContentsMargins(8, 8, 8, 8);
        edit_layout->setSpacing(6);

        edit_layout->addWidget(new QLabel("Selected byte"));
        edit_entry_ = new QLineEdit();
        edit_entry_->setPlaceholderText("Selected byte: hex or decimal");
        edit_layout->addWidget(edit_entry_);

        apply_button_ = new QPushButton("Write byte");
        connect(apply_button_, &QPushButton::clicked, this, [this]() {
            applyEdit();
        });
        edit_layout->addWidget(apply_button_);

        connect(edit_entry_, &QLineEdit::returnPressed, this, [this]() {
            applyEdit();
        });

        side_layout->addWidget(edit_frame);

        QFrame *status_frame = new QFrame();
        status_frame->setFrameStyle(QFrame::Box | QFrame::Raised);
        QVBoxLayout *status_layout = new QVBoxLayout(status_frame);
        status_layout->setContentsMargins(8, 8, 8, 8);
        status_layout->setSpacing(6);

        status_label_ = new QLabel();
        status_label_->setWordWrap(true);
        status_layout->addWidget(status_label_);

        side_layout->addWidget(status_frame);

        side_layout->addStretch();

        root_layout->addWidget(main_panel, 1);
        root_layout->addWidget(side_panel);

        viewer_widget_ = new MemViewerWidget(memory_size, read_memory, write_memory, open_flag, scroll_area_);
        scroll_area_->setWidget(viewer_widget_);

        viewer_widget_->onByteSelected = [this](size_t index, uint8_t value) {
            updateStatus(index, value);
            char text[8];
            std::snprintf(text, sizeof(text), "%02X", value);
            edit_entry_->setText(text);
        };
        viewer_widget_->onSearchStatusUpdated = [this]() {
            updateStatus(std::numeric_limits<size_t>::max(), 0);
        };

        connect(auto_refresh_, &QCheckBox::toggled, viewer_widget_, [this](bool checked) {
            if (viewer_widget_) {
                viewer_widget_->setAutoRefresh(checked);
            }
        });

        updateStatus(std::numeric_limits<size_t>::max(), 0);
    }

    ~MemViewerWindow() override {
        open_flag_.store(false, std::memory_order_relaxed);
    }

private:
    void rebuildSearch() {
        if (!viewer_widget_) return;
        
        const std::string search_text = search_entry_->text().toStdString();
        const SearchFormat format = format_combo_->currentIndex() == 0 ? SearchFormat::Hex : SearchFormat::Decimal;
        const EndianMode endian = endian_combo_->currentIndex() == 0 ? EndianMode::Little : EndianMode::Big;
        const size_t width = static_cast<size_t>(width_combo_->currentText().toULongLong());
        
        viewer_widget_->rebuildSearch(search_text, format, endian, width);
    }

    void applyEdit() {
        if (!viewer_widget_) return;
        
        uint8_t value = 0;
        const std::string text = edit_entry_->text().toStdString();
        if (!parse_byte_value(text, value)) {
            return;
        }
        viewer_widget_->applyEdit(value);
    }

    void updateStatus(size_t index, uint8_t value) {
        if (!viewer_widget_) return;
        
        if (index >= viewer_widget_->getMemorySize()) {
            std::ostringstream ss;
            ss << "Buffer size: " << viewer_widget_->getMemorySize() << " bytes";
            const size_t match_count = viewer_widget_->getMatchCount();
            if (match_count > 0) {
                ss << " | Matches: " << match_count;
            }
            status_label_->setText(QString::fromStdString(ss.str()));
            return;
        }

        char text[256];
        std::snprintf(
            text,
            sizeof(text),
            "Selected: 0x%08zx | hex=%02X | dec=%u | Matches=%zu",
            index,
            value,
            static_cast<unsigned>(value),
            viewer_widget_->getMatchCount());
        status_label_->setText(QString::fromLatin1(text));
    }

    std::atomic<bool> &open_flag_;
    QScrollArea *scroll_area_;
    MemViewerWidget *viewer_widget_;
    QCheckBox *auto_refresh_;
    QPushButton *refresh_button_;
    QLineEdit *search_entry_;
    QComboBox *format_combo_;
    QComboBox *width_combo_;
    QComboBox *endian_combo_;
    QPushButton *prev_button_;
    QPushButton *next_button_;
    QLineEdit *edit_entry_;
    QPushButton *apply_button_;
    QLabel *status_label_;
};

}  // namespace

int mem_viewer_run_gui(pid_t target_pid, uintptr_t target_address, size_t size) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    mem_viewer_debug_log("starting Qt GUI target_pid=%ld address=0x%llx size=%zu",
        static_cast<long>(target_pid),
        static_cast<unsigned long long>(target_address),
        size);

    RemoteMemory memory(target_pid, target_address, size);
    
    int argc = 1;
    const char *argv[] = {"mem_viewer", nullptr};
    QApplication app(argc, const_cast<char **>(argv));
    
    mem_viewer_debug_log("QApplication created");
    std::atomic<bool> open_flag{false};
    
    auto *window = new MemViewerWindow(
        memory.size(),
        [&memory](size_t offset, void *buffer, size_t length) { return memory.read(offset, buffer, length); },
        [&memory](size_t offset, const void *buffer, size_t length) { return memory.write(offset, buffer, length); },
        open_flag);
    
    open_flag.store(true, std::memory_order_relaxed);
    window->show();
    mem_viewer_debug_log("window shown");

    const int rc = app.exec();
    mem_viewer_debug_log("QApplication::exec() returned rc=%d", rc);
    return rc;
}

int mem_viewer_run_gui_shared(void *memory_ptr, size_t size) {
    mem_viewer_debug_log("starting shared Qt GUI memory=%p size=%zu", memory_ptr, size);

    LocalMemory memory(memory_ptr, size);
    
    int argc = 1;
    const char *argv[] = {"mem_viewer_shared", nullptr};
    QApplication app(argc, const_cast<char **>(argv));
    
    std::atomic<bool> open_flag{false};
    
    auto *window = new MemViewerWindow(
        memory.size(),
        [&memory](size_t offset, void *buffer, size_t length) { return memory.read(offset, buffer, length); },
        [&memory](size_t offset, const void *buffer, size_t length) { return memory.write(offset, buffer, length); },
        open_flag);
    
    open_flag.store(true, std::memory_order_relaxed);
    window->show();
    mem_viewer_debug_log("shared window shown");

    const int rc = app.exec();
    mem_viewer_debug_log("shared QApplication::exec() returned rc=%d", rc);
    return rc;
}
