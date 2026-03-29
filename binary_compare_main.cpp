#include <QApplication>
#include <QMessageBox>
#include "comparison_window.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    ComparisonWindow window;
    
    // If two files are provided as arguments, load them automatically
    if (argc == 3) {
        QString file1 = QString::fromUtf8(argv[1]);
        QString file2 = QString::fromUtf8(argv[2]);
        window.loadFilesFromPaths(file1, file2);
    } else if (argc > 1 && argc != 3) {
        QMessageBox::warning(nullptr, "Invalid Arguments",
            "Usage: binary_compare [file1 file2]\n\n"
            "If no files are provided, you can open them through the GUI.\n"
            "If two files are provided, they will be compared automatically.");
    }
    
    window.show();
    return app.exec();
}
