#include "ConfigFile.h"

#include <QFile>
#include <QStringList>

// The game treats both '#' and ';' as comment leaders (pc_config.c).
bool ConfigFile::parseLine(const QString& line, QString* key, QString* value)
{
    const QString t = line.trimmed();
    if (t.isEmpty() || t.startsWith('#') || t.startsWith(';'))
        return false;
    const int eq = t.indexOf('=');
    if (eq <= 0)
        return false;
    *key   = t.left(eq).trimmed();
    *value = t.mid(eq + 1).trimmed();
    return !key->isEmpty();
}

bool ConfigFile::load(const QString& path)
{
    m_path = path;
    m_values.clear();
    m_dirty.clear();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
    for (QString line : lines)
    {
        if (line.endsWith('\r'))
            line.chop(1);
        QString k, v;
        if (parseLine(line, &k, &v))
            m_values.insert(k, v);
    }
    return true;
}

QString ConfigFile::get(const QString& key, const QString& fallback) const
{
    return m_values.value(key, fallback);
}

void ConfigFile::set(const QString& key, const QString& value)
{
    if (m_values.value(key) == value && m_values.contains(key))
        return;
    m_values.insert(key, value);
    m_dirty.insert(key);
}

bool ConfigFile::save()
{
    if (m_dirty.isEmpty())
        return true;

    // Re-read from disk: the game writes keys at runtime (flashlight, fog,
    // map...), and those edits must survive a launcher save.
    QByteArray raw;
    {
        QFile f(m_path);
        if (f.open(QIODevice::ReadOnly))
            raw = f.readAll();
    }
    const bool  crlf  = raw.contains("\r\n");
    QStringList lines = QString::fromUtf8(raw).split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();

    QSet<QString> written;
    for (QString& line : lines)
    {
        if (line.endsWith('\r'))
            line.chop(1);
        QString k, v;
        if (parseLine(line, &k, &v) && m_dirty.contains(k))
        {
            line = k + " = " + m_values.value(k);
            written.insert(k);
        }
    }

    QStringList missing;
    for (const QString& k : m_dirty)
        if (!written.contains(k))
            missing << k;
    if (!missing.isEmpty())
    {
        missing.sort();
        lines << QString() << QStringLiteral("## Qt launcher");
        for (const QString& k : missing)
            lines << k + " = " + m_values.value(k);
    }

    QFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QString eol = crlf ? QStringLiteral("\r\n") : QStringLiteral("\n");
    f.write((lines.join(eol) + eol).toUtf8());
    m_dirty.clear();
    return true;
}
