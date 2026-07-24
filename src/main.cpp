#include <QApplication>

#include "ui/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("reader");
    QApplication::setOrganizationName("reader");

    MainWindow window;
    window.show();

    return QApplication::exec();
}
