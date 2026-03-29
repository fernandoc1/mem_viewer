#include "comparison_widget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QScrollArea>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <algorithm>
#include <cstring>

// Custom hex display widget
class HexDisplayWidget : public QWidget {
public:
    HexDisplayWidget(const uint8_t *data, size_t size, const std::vector<size_t> &diff_positions,
                     bool is_file1, QWidget *parent = nullptr)
        : QWidget(parent), data_(data), size_(size), diff_positions_(diff_positions), is_file1_(is_file1) {
        
        setFont(QFont("Monospace", 11));
        QFontMetrics fm(font());
        char_width_ = fm.horizontalAdvance('W');
        row_height_ = fm.lineSpacing() + 2;
        
        setMinimumWidth(static_cast<int>((8 + 2 + 16 * 3 + 2 + 16 + 4) * char_width_));
        setMinimumHeight(static_cast<int>(((size + 15) / 16) * row_height_ + 40));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setFont(font());
        
        // Dark background
        painter.fillRect(rect(), QColor(0x14, 0x16, 0x1A));
        
        const int kBytesPerRow = 16;
        const double kAddressX = 4.0;
        const double kGapAddressToHex = 16.0;
        const double kGapHexToAscii = 16.0;
        
        double hex_start_x = kAddressX + 8 * char_width_ + kGapAddressToHex;
        double ascii_start_x = hex_start_x + (kBytesPerRow * 3 - 1) * char_width_ + kGapHexToAscii;
        
        double y = 0;
        for (size_t row = 0; row * kBytesPerRow < size_; ++row) {
            // Draw address
            QString addr = QString::asprintf("%08llx", (unsigned long long)(row * kBytesPerRow));
            painter.setPen(QColor(0xBD, 0xC7, 0xD0));
            painter.drawText(QPointF(kAddressX, y + row_height_ * 0.75), addr);
            
            // Draw hex values
            painter.setPen(QColor(0xEE, 0xEE, 0xEF));
            for (int i = 0; i < kBytesPerRow; ++i) {
                size_t offset = row * kBytesPerRow + i;
                if (offset >= size_) break;
                
                uint8_t byte = data_[offset];
                
                // Check if this byte differs
                bool is_diff = std::binary_search(diff_positions_.begin(), diff_positions_.end(), offset);
                if (is_diff) {
                    painter.fillRect(
                        QRectF(hex_start_x + i * 3 * char_width_ - 2, y + 2, char_width_ * 2.2, row_height_ - 4),
                        QColor(255, 100, 0, 60)
                    );
                    painter.setPen(QColor(255, 150, 0));
                } else {
                    painter.setPen(QColor(0xEE, 0xEE, 0xEF));
                }
                
                QString hex = QString::asprintf("%02x", byte);
                painter.drawText(QPointF(hex_start_x + i * 3 * char_width_, y + row_height_ * 0.75), hex);
            }
            
            // Draw ASCII
            painter.setPen(QColor(0xCD, 0xDB, 0xE2));
            QString ascii;
            for (int i = 0; i < kBytesPerRow; ++i) {
                size_t offset = row * kBytesPerRow + i;
                if (offset >= size_) break;
                uint8_t byte = data_[offset];
                ascii += (byte >= 32 && byte < 127) ? QChar(byte) : '.';
            }
            painter.drawText(QPointF(ascii_start_x, y + row_height_ * 0.75), ascii);
            
            y += row_height_;
        }
    }

private:
    const uint8_t *data_;
    size_t size_;
    const std::vector<size_t> &diff_positions_;
    bool is_file1_;
    int char_width_ = 8;
    int row_height_ = 14;
};

ComparisonWidget::ComparisonWidget(std::shared_ptr<DualFileBuffer> dual_buffer, QWidget *parent)
    : QWidget(parent), dual_buffer_(dual_buffer) {
    
    // Create hex display widgets
    HexDisplayWidget *left_display = new HexDisplayWidget(
        dual_buffer_->getBuffer1(), dual_buffer_->getSize1(),
        dual_buffer_->getDiffPositions(), true, this
    );
    
    HexDisplayWidget *right_display = new HexDisplayWidget(
        dual_buffer_->getBuffer2(), dual_buffer_->getSize2(),
        dual_buffer_->getDiffPositions(), false, this
    );

    // Create scroll areas
    QScrollArea *left_scroll = new QScrollArea(this);
    left_scroll->setWidget(left_display);
    left_scroll->setWidgetResizable(false);

    QScrollArea *right_scroll = new QScrollArea(this);
    right_scroll->setWidget(right_display);
    right_scroll->setWidgetResizable(false);

    // Connect scroll bars for synchronization
    connect(left_scroll->verticalScrollBar(), QOverload<int>::of(&QScrollBar::valueChanged),
            this, [this, right_scroll](int value) {
        if (!syncing_scroll_) {
            syncing_scroll_ = true;
            right_scroll->verticalScrollBar()->setValue(value);
            syncing_scroll_ = false;
        }
    });

    connect(right_scroll->verticalScrollBar(), QOverload<int>::of(&QScrollBar::valueChanged),
            this, [this, left_scroll](int value) {
        if (!syncing_scroll_) {
            syncing_scroll_ = true;
            left_scroll->verticalScrollBar()->setValue(value);
            syncing_scroll_ = false;
        }
    });

    // Main layout
    QHBoxLayout *mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(1);
    mainLayout->addWidget(left_scroll, 1);
    mainLayout->addWidget(right_scroll, 1);

    setLayout(mainLayout);
}

ComparisonWidget::~ComparisonWidget() {
}

void ComparisonWidget::jumpToNextDifference() {
    if (!dual_buffer_) return;
    
    size_t current_pos = getCurrentPosition();
    size_t next_diff = dual_buffer_->getNextDiffPosition(current_pos);
    jumpToPosition(next_diff);
}

void ComparisonWidget::jumpToPrevDifference() {
    if (!dual_buffer_) return;
    
    size_t current_pos = getCurrentPosition();
    size_t prev_diff = dual_buffer_->getPrevDiffPosition(current_pos);
    jumpToPosition(prev_diff);
}

void ComparisonWidget::jumpToPosition(size_t position) {
    const int kBytesPerRow = 16;
    size_t row = position / kBytesPerRow;
    int pixel_y = static_cast<int>(row * 16); // Approximate pixel position
    
    QLayout *layout = this->layout();
    for (int i = 0; i < layout->count(); ++i) {
        QWidget *w = layout->itemAt(i)->widget();
        QScrollArea *scroll = qobject_cast<QScrollArea*>(w);
        if (scroll) {
            scroll->verticalScrollBar()->setValue(pixel_y);
        }
    }
}

size_t ComparisonWidget::getCurrentPosition() const {
    const int kBytesPerRow = 16;
    
    QLayout *layout = const_cast<ComparisonWidget*>(this)->layout();
    if (!layout) return 0;
    
    for (int i = 0; i < layout->count(); ++i) {
        QWidget *w = layout->itemAt(i)->widget();
        QScrollArea *scroll = qobject_cast<QScrollArea*>(w);
        if (scroll) {
            int scroll_value = scroll->verticalScrollBar()->value();
            size_t row = scroll_value / 16;
            return row * kBytesPerRow;
        }
    }
    
    return 0;
}

size_t ComparisonWidget::getTotalBytes() const {
    return dual_buffer_ ? dual_buffer_->getMaxSize() : 0;
}

void ComparisonWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
}

void ComparisonWidget::wheelEvent(QWheelEvent *event) {
    QWidget::wheelEvent(event);
}

