#ifndef COMPARISON_WINDOW_H
#define COMPARISON_WINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <memory>
#include "file_comparator.h"

class ComparisonWidget;

class ComparisonWindow : public QMainWindow {
    Q_OBJECT

public:
    ComparisonWindow(QWidget *parent = nullptr);
    ~ComparisonWindow() override;
    
    // Load files directly (used for command-line arguments)
    void loadFilesFromPaths(const QString &file1, const QString &file2);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onOpenFiles();
    void onNextDifference();
    void onPrevDifference();
    void onAbout();

private:
    void createMenuBar();
    void createDockWidgets();
    void updateStatusBar();
    void loadComparison(const QString &file1, const QString &file2);

    ComparisonWidget *comparison_widget_ = nullptr;
    std::shared_ptr<DualFileBuffer> dual_buffer_;
    
    QLabel *file1_label_ = nullptr;
    QLabel *file2_label_ = nullptr;
    QLabel *stats_label_ = nullptr;
};

#endif
