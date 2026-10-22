#pragma once

// A compound file ("OLE2", [MS-CFB]): the container of PowerPoint 97–2003
// decks (.ppt, .pps, .pot) and WPS Presentation's (.dps, .dpt). A small file
// system in one file: storages (folders) holding streams.
//
// Written in-house for the same reason as zip.*: no new dependency for one
// container. Reading checks every sector number and chain length, so a broken
// or hostile file is refused rather than looped over. Writing makes version 3
// files (512-byte sectors), the kind every Office reads, with small streams
// in the mini stream as the format asks.

#include <QByteArray>
#include <QHash>
#include <QStringList>
#include <QVector>

namespace Cfb {

// Starts every compound file.
bool looksLike(const QByteArray &raw);

struct Entry {
    QString path;       // "PowerPoint Document", "ObjectPool/_1234/Ole"
    QByteArray data;
};

// Empty when the streams would not fit (the same limit as reading).
// `rootClass` is the root storage's CLSID, 16 bytes, or empty.
QByteArray write(const QVector<Entry> &streams, const QByteArray &rootClass = {});

constexpr qint64 kMaxFileBytes = 512LL * 1024 * 1024;

class Reader {
public:
    explicit Reader(const QByteArray &raw);

    bool isValid() const { return m_valid; }
    QString error() const { return m_error; }
    bool contains(const QString &path) const { return m_streams.contains(path); }
    QStringList paths() const { return m_streams.keys(); }
    QByteArray read(const QString &path) const { return m_streams.value(path); }

private:
    QHash<QString, QByteArray> m_streams;
    bool m_valid = false;
    QString m_error;
};

} // namespace Cfb
