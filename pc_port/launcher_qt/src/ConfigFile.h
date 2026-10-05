#pragma once

#include <QHash>
#include <QSet>
#include <QString>

// config.cfg reader/writer with the same contract as the C# launcher's
// ConfigManager and the game's own writer: only keys set through set() are
// rewritten, in place; comments, order, unknown keys and bind lines pass
// through untouched. Missing keys are appended under a header.
class ConfigFile
{
public:
    bool load(const QString& path);
    bool save();

    QString get(const QString& key, const QString& fallback = QString()) const;
    bool has(const QString& key) const { return m_values.contains(key); }
    void set(const QString& key, const QString& value);

    const QString& path() const { return m_path; }

private:
    static bool parseLine(const QString& line, QString* key, QString* value);

    QString                 m_path;
    QHash<QString, QString> m_values;
    QSet<QString>           m_dirty;
};
