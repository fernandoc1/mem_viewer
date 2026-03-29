#include <QApplication>
#include "comparison_window.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    ComparisonWindow window;
    window.show();

    return app.exec();
}
