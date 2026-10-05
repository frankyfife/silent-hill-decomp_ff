#include "LauncherWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Options.h"

static const char* kGameExe  = "SilentHillPC.exe";
static const char* kToolsExe = "SilentHillPC_Launcher.exe";

// The launcher may sit next to the game or in a subfolder of it (so its Qt
// DLLs stay out of the game folder).
static QString FindGameDir()
{
    QDir dir(QCoreApplication::applicationDirPath());
    for (int up = 0; up < 2; up++)
    {
        if (QFileInfo::exists(dir.filePath(kGameExe)))
            return dir.absolutePath();
        if (!dir.cdUp())
            break;
    }
    return QString();
}

LauncherWindow::LauncherWindow()
{
    setWindowTitle(tr("Silent Hill PC - Launcher"));

    m_gameDir = FindGameDir();
    if (!m_gameDir.isEmpty())
        m_cfg.load(QDir(m_gameDir).filePath("config.cfg"));

    {
        QFile f(":/level_select.json");
        if (f.open(QIODevice::ReadOnly))
            m_maps = QJsonDocument::fromJson(f.readAll()).object().value("maps").toArray();
    }

    auto* tabs = new QTabWidget;
    tabs->addTab(buildLevelPage(), tr("Level Select"));
    QStringList pages;
    for (const OptionDef& o : OptionTable())
        if (!pages.contains(o.page))
            pages << o.page;
    for (const QString& p : pages)
        tabs->addTab(buildOptionPage(p), QString(tr(p.toUtf8().constData())).replace("&", "&&"));

    m_status = new QLabel;
    m_status->setText(m_gameDir.isEmpty()
                          ? tr("SilentHillPC.exe not found next to or above the launcher.")
                          : QDir::toNativeSeparators(m_gameDir));

    auto* tools = new QPushButton(tr("Mods && Tools..."));
    tools->setToolTip(tr("Opens the original launcher: Mod Manager, asset tools, updates."));
    tools->setEnabled(!m_gameDir.isEmpty() && QFileInfo::exists(QDir(m_gameDir).filePath(kToolsExe)));
    connect(tools, &QPushButton::clicked, this, &LauncherWindow::openToolsLauncher);

    auto* save = new QPushButton(tr("Save"));
    connect(save, &QPushButton::clicked, this, [this] {
        m_status->setText(saveConfig() ? tr("Settings saved.") : tr("Could not write config.cfg."));
    });

    m_playButton = new QPushButton(tr("Play"));
    m_playButton->setDefault(true);
    m_playButton->setEnabled(!m_gameDir.isEmpty());
    connect(m_playButton, &QPushButton::clicked, this, [this] { launch({}); });

    auto* bottom = new QHBoxLayout;
    bottom->addWidget(m_status, 1);
    bottom->addWidget(tools);
    bottom->addWidget(save);
    bottom->addWidget(m_playButton);

    auto* root = new QVBoxLayout;
    root->addWidget(tabs, 1);
    root->addLayout(bottom);
    auto* central = new QWidget;
    central->setLayout(root);
    setCentralWidget(central);
    resize(620, 520);
}

QWidget* LauncherWindow::buildLevelPage()
{
    auto* page = new QWidget;
    auto* form = new QFormLayout;

    m_mapBox = new QComboBox;
    auto* model = qobject_cast<QStandardItemModel*>(m_mapBox->model());
    const QString lastMap = m_cfg.get("launcher_qt_map", "map2_s00");
    for (int i = 0; i < m_maps.size(); i++)
    {
        const QJsonObject m = m_maps.at(i).toObject();
        m_mapBox->addItem(m.value("id").toString() + "  -  " + m.value("name").toString(), m.value("id").toString());
        // Unused maps have no real start point (it is the origin), the
        // game boots into a void or crashes there.
        if (!m.value("usable").toBool() && model)
            model->item(i)->setEnabled(false);
        if (m.value("id").toString() == lastMap)
            m_mapBox->setCurrentIndex(i);
    }
    connect(m_mapBox, &QComboBox::currentIndexChanged, this, [this] { fillSpawns(); });

    m_spawnBox = new QComboBox;
    m_spawnBox->setMinimumContentsLength(40);
    m_spawnBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    fillSpawns();
    {
        const QString lastSpawn = m_cfg.get("launcher_qt_spawn", "default");
        const int     idx       = m_spawnBox->findData(lastSpawn);
        if (idx >= 0)
            m_spawnBox->setCurrentIndex(idx);
    }

    m_difficultyBox = new QComboBox;
    m_difficultyBox->addItem(tr("Easy"), "easy");
    m_difficultyBox->addItem(tr("Normal"), "normal");
    m_difficultyBox->addItem(tr("Hard"), "hard");
    m_difficultyBox->setCurrentIndex(qMax(0, m_difficultyBox->findData(m_cfg.get("launcher_qt_difficulty", "normal"))));

    form->addRow(tr("Map"), m_mapBox);
    form->addRow(tr("Spawn point"), m_spawnBox);
    form->addRow(tr("Difficulty"), m_difficultyBox);

    auto* note = new QLabel(tr("Starts a New Game directly in the chosen map with the starter loadout "
                               "(flashlight, radio, knife, handgun). Story flags start empty, so scripted "
                               "events may differ from a normal playthrough. Nothing here changes how the "
                               "game boots with Play."));
    note->setWordWrap(true);

    auto* start = new QPushButton(tr("Start in this level"));
    start->setEnabled(!m_gameDir.isEmpty());
    connect(start, &QPushButton::clicked, this, [this] {
        const QString map = m_mapBox->currentData().toString();
        const QString spawn = m_spawnBox->currentData().toString();
        const QString difficulty = m_difficultyBox->currentData().toString();
        m_cfg.set("launcher_qt_map", map);
        m_cfg.set("launcher_qt_spawn", spawn);
        m_cfg.set("launcher_qt_difficulty", difficulty);
        launch({ "-map", map, "-spawn", spawn, "-difficulty", difficulty });
    });

    auto* layout = new QVBoxLayout;
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addStretch(1);
    layout->addWidget(start, 0, Qt::AlignRight);
    page->setLayout(layout);
    return page;
}

void LauncherWindow::fillSpawns()
{
    m_spawnBox->clear();
    const QString id = m_mapBox->currentData().toString();
    for (const QJsonValue& v : m_maps)
    {
        const QJsonObject m = v.toObject();
        if (m.value("id").toString() != id)
            continue;
        for (const QJsonValue& s : m.value("spawns").toArray())
        {
            const QJsonObject sp = s.toObject();
            m_spawnBox->addItem(sp.value("label").toString(), sp.value("arg").toString());
        }
    }
}

QWidget* LauncherWindow::buildOptionPage(const QString& pageName)
{
    auto* page = new QWidget;
    auto* form = new QFormLayout;

    for (const OptionDef& o : OptionTable())
    {
        if (pageName != o.page)
            continue;

        const QString key   = o.key;
        const QString value = m_cfg.get(key, o.def);
        QWidget*      w     = nullptr;

        switch (o.kind)
        {
            case OptKind::Choice:
            {
                auto* box = new QComboBox;
                for (const OptChoice& c : o.choices)
                    box->addItem(tr(c.label), QString(c.value));
                int idx = box->findData(value);
                if (idx < 0)
                {
                    // Keep a hand-edited value the table does not know.
                    box->addItem(value, value);
                    idx = box->count() - 1;
                }
                box->setCurrentIndex(idx);
                m_writers.push_back([this, key, box] { m_cfg.set(key, box->currentData().toString()); });
                w = box;
                break;
            }
            case OptKind::Check:
            {
                auto* box = new QCheckBox;
                box->setChecked(value.toInt() != 0);
                m_writers.push_back([this, key, box] { m_cfg.set(key, box->isChecked() ? "1" : "0"); });
                w = box;
                break;
            }
            case OptKind::Integer:
            {
                auto* spin = new QSpinBox;
                spin->setRange(int(o.min), int(o.max));
                spin->setSingleStep(int(o.step));
                spin->setValue(value.toInt());
                m_writers.push_back([this, key, spin] { m_cfg.set(key, QString::number(spin->value())); });
                w = spin;
                break;
            }
            case OptKind::Decimal:
            {
                auto* spin = new QDoubleSpinBox;
                spin->setDecimals(2);
                spin->setRange(o.min, o.max);
                spin->setSingleStep(o.step);
                spin->setValue(value.toDouble());
                m_writers.push_back([this, key, spin] { m_cfg.set(key, QString::number(spin->value(), 'f', 2)); });
                w = spin;
                break;
            }
            case OptKind::Resolution:
            {
                auto*         box     = new QComboBox;
                const QString current = m_cfg.get("width", "640") + "x" + m_cfg.get("height", "480");
                const QSize   screen  = QGuiApplication::primaryScreen()
                                            ? QGuiApplication::primaryScreen()->size() * QGuiApplication::primaryScreen()->devicePixelRatio()
                                            : QSize(1920, 1080);
                const QList<QSize> sizes = { { 640, 480 }, { 960, 720 }, { 1280, 720 }, { 1280, 960 }, { 1600, 900 },
                                             { 1920, 1080 }, { 1920, 1200 }, { 2560, 1080 }, { 2560, 1440 },
                                             { 3440, 1440 }, { 3840, 2160 }, { 5120, 1440 }, { 5120, 2880 } };
                for (const QSize& s : sizes)
                    if (s.width() <= screen.width() && s.height() <= screen.height())
                        box->addItem(QString("%1 x %2").arg(s.width()).arg(s.height()),
                                     QString("%1x%2").arg(s.width()).arg(s.height()));
                int idx = box->findData(current);
                if (idx < 0)
                {
                    box->addItem(QString(current).replace('x', " x "), current);
                    idx = box->count() - 1;
                }
                box->setCurrentIndex(idx);
                m_writers.push_back([this, box] {
                    const QStringList wh = box->currentData().toString().split('x');
                    if (wh.size() == 2)
                    {
                        m_cfg.set("width", wh[0]);
                        m_cfg.set("height", wh[1]);
                    }
                });
                w = box;
                break;
            }
        }

        if (o.tip && *o.tip)
            w->setToolTip(tr(o.tip));
        form->addRow(tr(o.label), w);
    }

    page->setLayout(form);
    return page;
}

bool LauncherWindow::saveConfig()
{
    for (const auto& write : m_writers)
        write();
    return m_cfg.save();
}

void LauncherWindow::launch(const QStringList& args)
{
    if (!saveConfig())
    {
        QMessageBox::warning(this, windowTitle(), tr("Could not write %1.").arg(m_cfg.path()));
        return;
    }
    const QString exe = QDir(m_gameDir).filePath(kGameExe);
    if (!QProcess::startDetached(exe, args, m_gameDir))
    {
        QMessageBox::warning(this, windowTitle(), tr("Could not start %1.").arg(QDir::toNativeSeparators(exe)));
        return;
    }
    close();
}

void LauncherWindow::openToolsLauncher()
{
    saveConfig();
    QProcess::startDetached(QDir(m_gameDir).filePath(kToolsExe), {}, m_gameDir);
}
