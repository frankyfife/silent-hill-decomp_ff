#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

#include "LauncherWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("SilentHillPC Launcher");

    // Dark Fusion: the game's own palette, and it reads the same on every
    // Windows theme.
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, QColor(32, 32, 34));
    p.setColor(QPalette::WindowText, QColor(220, 220, 220));
    p.setColor(QPalette::Base, QColor(22, 22, 24));
    p.setColor(QPalette::AlternateBase, QColor(40, 40, 42));
    p.setColor(QPalette::Text, QColor(220, 220, 220));
    p.setColor(QPalette::Button, QColor(45, 45, 48));
    p.setColor(QPalette::ButtonText, QColor(220, 220, 220));
    p.setColor(QPalette::Highlight, QColor(140, 30, 30));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor(45, 45, 48));
    p.setColor(QPalette::ToolTipText, QColor(220, 220, 220));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(110, 110, 110));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(110, 110, 110));
    app.setPalette(p);

    LauncherWindow w;
    w.show();
    return app.exec();
}
