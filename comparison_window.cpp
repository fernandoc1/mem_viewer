#include "comparison_window.h"
#include "comparison_widget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QStatusBar>
#include <QLabel>
#include <QMessageBox>
#include <QDockWidget>
#include <QPushButton>

ComparisonWindow::ComparisonWindow(QWidget *parent)
    : QMainWindow(parent), dual_buffer_(std::make_shared<DualFileBuffer>()) {
    setWindowTitle("Binary File Comparator");
    setGeometry(100, 100, 1400, 700);

    createMenuBar();
    createDockWidgets();

    // Central widget - will hold comparison widget when files are loaded
    QWidget *central = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setCentralWidget(central);

    // Create placeholder
    QLabel *placeholder = new QLabel("Open two files to compare...");
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setStyleSheet("color: #666; font-size: 14pt;");
    layout->addWidget(placeholder);

    // Status bar
    statusBar()->showMessage("Ready");
}

ComparisonWindow::~ComparisonWindow() {
}

void ComparisonWindow::createMenuBar() {
    QMenuBar *menuBar = new QMenuBar(this);
    setMenuBar(menuBar);

    // File menu
    QMenu *fileMenu = menuBar->addMenu("&File");
    
    QAction *openAction = fileMenu->addAction("&Open Files...");
    connect(openAction, &QAction::triggered, this, &ComparisonWindow::onOpenFiles);
    
    fileMenu->addSeparator();
    
    QAction *quitAction = fileMenu->addAction("&Quit");
    quitAction->setShortcut(Qt::CTRL | Qt::Key_Q);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    // Navigation menu
    QMenu *navMenu = menuBar->addMenu("&Navigate");
    
    QAction *nextAction = navMenu->addAction("Next &Difference");
    nextAction->setShortcut(Qt::CTRL | Qt::Key_N);
    connect(nextAction, &QAction::triggered, this, &ComparisonWindow::onNextDifference);
    
    QAction *prevAction = navMenu->addAction("Previous &Difference");
    prevAction->setShortcut(Qt::CTRL | Qt::Key_P);
    connect(prevAction, &QAction::triggered, this, &ComparisonWindow::onPrevDifference);

    // Help menu
    QMenu *helpMenu = menuBar->addMenu("&Help");
    
    QAction *aboutAction = helpMenu->addAction("&About");
    connect(aboutAction, &QAction::triggered, this, &ComparisonWindow::onAbout);
}

void ComparisonWindow::createDockWidgets() {
    // Left dock for file 1 info
    QDockWidget *dock1 = new QDockWidget("File 1", this);
    QWidget *dock1Widget = new QWidget();
    QVBoxLayout *dock1Layout = new QVBoxLayout(dock1Widget);
    file1_label_ = new QLabel("(none)");
    file1_label_->setWordWrap(true);
    dock1Layout->addWidget(new QLabel("File 1:"));
    dock1Layout->addWidget(file1_label_);
    dock1Layout->addStretch();
    dock1->setWidget(dock1Widget);
    addDockWidget(Qt::TopDockWidgetArea, dock1);

    // Right dock for file 2 info
    QDockWidget *dock2 = new QDockWidget("File 2", this);
    QWidget *dock2Widget = new QWidget();
    QVBoxLayout *dock2Layout = new QVBoxLayout(dock2Widget);
    file2_label_ = new QLabel("(none)");
    file2_label_->setWordWrap(true);
    dock2Layout->addWidget(new QLabel("File 2:"));
    dock2Layout->addWidget(file2_label_);
    dock2Layout->addStretch();
    dock2->setWidget(dock2Widget);
    addDockWidget(Qt::TopDockWidgetArea, dock2);

    // Bottom dock for statistics
    QDockWidget *dockStats = new QDockWidget("Statistics", this);
    QWidget *statsWidget = new QWidget();
    QVBoxLayout *statsLayout = new QVBoxLayout(statsWidget);
    stats_label_ = new QLabel("No comparison loaded");
    stats_label_->setWordWrap(true);
    statsLayout->addWidget(stats_label_);
    statsLayout->addStretch();
    dockStats->setWidget(statsWidget);
    addDockWidget(Qt::BottomDockWidgetArea, dockStats);
}

void ComparisonWindow::updateStatusBar() {
    if (dual_buffer_) {
        QString status = QString::asprintf(
            "File 1: %lu bytes | File 2: %lu bytes | Differences: %lu",
            dual_buffer_->getSize1(),
            dual_buffer_->getSize2(),
            dual_buffer_->getDiffCount()
        );
        statusBar()->showMessage(status);
        
        QString stats = QString::asprintf(
            "<b>File 1:</b> %lu bytes<br><b>File 2:</b> %lu bytes<br><b>Different bytes:</b> %lu (%0.1f%%)",
            dual_buffer_->getSize1(),
            dual_buffer_->getSize2(),
            dual_buffer_->getDiffCount(),
            (dual_buffer_->getDiffCount() * 100.0) / std::max(1UL, dual_buffer_->getMaxSize())
        );
        stats_label_->setText(stats);
    }
}

void ComparisonWindow::onOpenFiles() {
    QString file1 = QFileDialog::getOpenFileName(this, "Open First File");
    if (file1.isEmpty()) return;

    QString file2 = QFileDialog::getOpenFileName(this, "Open Second File");
    if (file2.isEmpty()) return;

    loadComparison(file1, file2);
}

void ComparisonWindow::loadComparison(const QString &file1_path, const QString &file2_path) {
    std::string error_msg;
    if (!dual_buffer_->loadFiles(file1_path.toStdString(), file2_path.toStdString(), error_msg)) {
        QMessageBox::critical(this, "Error Loading Files", QString::fromStdString(error_msg));
        return;
    }

    // Update labels
    file1_label_->setText(file1_path);
    file2_label_->setText(file2_path);

    // Remove old comparison widget if exists
    QWidget *central = centralWidget();
    if (comparison_widget_) {
        comparison_widget_->deleteLater();
        comparison_widget_ = nullptr;
    }

    // Create and set new comparison widget
    comparison_widget_ = new ComparisonWidget(dual_buffer_, this);
    
    QLayout *layout = central->layout();
    if (layout->count() > 0) {
        QLayoutItem *item = layout->takeAt(0);
        if (item && item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    layout->addWidget(comparison_widget_);

    updateStatusBar();
    statusBar()->showMessage("Comparison loaded");
}

void ComparisonWindow::onNextDifference() {
    if (!comparison_widget_) return;
    comparison_widget_->jumpToNextDifference();
}

void ComparisonWindow::onPrevDifference() {
    if (!comparison_widget_) return;
    comparison_widget_->jumpToPrevDifference();
}

void ComparisonWindow::onAbout() {
    QMessageBox::about(this, "About Binary File Comparator",
        "Binary File Comparator v1.0\n\n"
        "Compare two binary files side-by-side with highlighted differences.\n\n"
        "Shortcuts:\n"
        "  Ctrl+O - Open files\n"
        "  Ctrl+N - Next difference\n"
        "  Ctrl+P - Previous difference\n"
        "  Ctrl+Q - Quit");
}

void ComparisonWindow::closeEvent(QCloseEvent *event) {
    if (comparison_widget_) {
        comparison_widget_->deleteLater();
    }
    QMainWindow::closeEvent(event);
}
