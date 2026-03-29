#ifndef COMPARISON_WIDGET_H
#define COMPARISON_WIDGET_H

#include <QWidget>
#include <QScrollBar>
#include <memory>
#include <functional>
#include "file_comparator.h"

class ComparisonWidget : public QWidget {
    Q_OBJECT

public:
    ComparisonWidget(std::shared_ptr<DualFileBuffer> dual_buffer, QWidget *parent = nullptr);
    ~ComparisonWidget() override;

    // Navigation
    void jumpToNextDifference();
    void jumpToPrevDifference();
    void jumpToPosition(size_t position);

    // Query state
    size_t getCurrentPosition() const;
    size_t getTotalBytes() const;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    std::shared_ptr<DualFileBuffer> dual_buffer_;
    bool syncing_scroll_ = false;
};

#endif
