#pragma once

#include <QJsonArray>
#include <QMainWindow>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

#include "ConfigFile.h"

class QComboBox;
class QLabel;
class QPushButton;
class QWidget;

class LauncherWindow : public QMainWindow
{
    Q_OBJECT

public:
    LauncherWindow();

private:
    QWidget* buildLevelPage();
    QWidget* buildOptionPage(const QString& page);
    void     fillSpawns();
    bool     saveConfig();
    void     launch(const QStringList& args);
    void     openToolsLauncher();

    QString    m_gameDir;
    ConfigFile m_cfg;
    QJsonArray m_maps;

    QComboBox*   m_mapBox        = nullptr;
    QComboBox*   m_spawnBox      = nullptr;
    QComboBox*   m_difficultyBox = nullptr;
    QLabel*      m_status        = nullptr;
    QPushButton* m_playButton    = nullptr;

    // One writer per option widget: pushes the widget's value into m_cfg.
    std::vector<std::function<void()>> m_writers;
};
