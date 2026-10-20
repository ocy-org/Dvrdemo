#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("DvrProject");     // QSettings据此定位存储
    app.setApplicationName("DvrConsole");
    MainWindow w;
    w.show();
    return app.exec();
}
