#include "io/cfb.h"

#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <functional>

namespace {

constexpr char kSignature[8] = {char(0xD0), char(0xCF), char(0x11), char(0xE0), char(0xA1), char(0xB1), char(0x1A), char(0xE1)};
constexpr quint32 kFree = 0xFFFFFFFF;
constexpr quint32 kEnd = 0xFFFFFFFE;
constexpr quint32 kFatSector = 0xFFFFFFFD;
constexpr quint32 kDifatSector = 0xFFFFFFFC;
constexpr quint32 kNone = 0xFFFFFFFF;   // no sibling or child in the directory
constexpr int kHeaderDifat = 109;
constexpr quint32 kMiniCutoff = 4096;
constexpr int kMiniSector = 64;
constexpr int kSector = 512;            // what is written: version 3
constexpr int kDirEntry = 128;

enum EntryType : quint8 { Empty = 0, Storage = 1, Stream = 2, Root = 5 };

quint32 u32(const QByteArray &b, qint64 at) { return qFromLittleEndian<quint32>(b.constData() + at); }
quint16 u16(const QByteArray &b, qint64 at) { return qFromLittleEndian<quint16>(b.constData() + at); }
void put32(QByteArray &b, qint64 at, quint32 v) { qToLittleEndian<quint32>(v, b.data() + at); }
void put16(QByteArray &b, qint64 at, quint16 v) { qToLittleEndian<quint16>(v, b.data() + at); }

// Directory order: shorter names first, then by upper-case UTF-16.
bool before(const QString &a, const QString &b) {
    if (a.size() != b.size()) return a.size() < b.size();
    return a.toUpper() < b.toUpper();
}

} // namespace

namespace Cfb {

bool looksLike(const QByteArray &raw) {
    return raw.size() >= 512 && raw.startsWith(QByteArray::fromRawData(kSignature, 8));
}

Reader::Reader(const QByteArray &raw) {
    if (!looksLike(raw)) { m_error = QStringLiteral("not a compound file"); return; }
    if (raw.size() > kMaxFileBytes) { m_error = QStringLiteral("too large"); return; }
    const int shift = u16(raw, 0x1E);
    if (shift != 9 && shift != 12) { m_error = QStringLiteral("unknown sector size"); return; }
    const int miniShift = u16(raw, 0x20);
    if (miniShift != 6) { m_error = QStringLiteral("unknown mini sector size"); return; }
    const qint64 sectorSize = qint64(1) << shift;
    const int perSector = int(sectorSize / 4);
    const qint64 sectorCount = qMax<qint64>(0, (raw.size() - sectorSize + sectorSize - 1) / sectorSize);
    const auto offsetOf = [&](quint32 sector) { return (qint64(sector) + 1) * sectorSize; };
    const auto sectorOk = [&](quint32 sector) { return qint64(sector) < sectorCount && offsetOf(sector) + sectorSize <= raw.size() + sectorSize; };
    // The file may end inside its last sector: read what is there.
    const auto sectorBytes = [&](quint32 sector) {
        QByteArray s = raw.mid(offsetOf(sector), sectorSize);
        if (s.size() < sectorSize) s.append(QByteArray(sectorSize - s.size(), '\0'));
        return s;
    };

    // Where the FAT is: 109 places in the header, then a chain of DIFAT sectors.
    const quint32 fatCount = u32(raw, 0x2C);
    if (fatCount > quint32(sectorCount)) { m_error = QStringLiteral("broken allocation table"); return; }
    QVector<quint32> fatSectors;
    for (int i = 0; i < kHeaderDifat && fatSectors.size() < int(fatCount); ++i) fatSectors.append(u32(raw, 0x4C + i * 4));
    quint32 difat = u32(raw, 0x44);
    QSet<quint32> seen;
    while (fatSectors.size() < int(fatCount) && difat != kEnd && difat != kFree) {
        if (!sectorOk(difat) || seen.contains(difat)) { m_error = QStringLiteral("broken allocation table"); return; }
        seen.insert(difat);
        const auto s = sectorBytes(difat);
        for (int i = 0; i < perSector - 1 && fatSectors.size() < int(fatCount); ++i) fatSectors.append(u32(s, i * 4));
        difat = u32(s, (perSector - 1) * 4);
    }
    QVector<quint32> fat;
    fat.reserve(fatSectors.size() * perSector);
    for (const auto sector : fatSectors) {
        if (!sectorOk(sector)) { m_error = QStringLiteral("broken allocation table"); return; }
        const auto s = sectorBytes(sector);
        for (int i = 0; i < perSector; ++i) fat.append(u32(s, i * 4));
    }

    // A chain of sectors, each visited once and no longer than the table.
    const auto chain = [&](quint32 start, const QVector<quint32> &table, QVector<quint32> *out) {
        out->clear();
        QSet<quint32> visited;
        for (quint32 at = start; at != kEnd && at != kFree; at = table.value(int(at), kFree)) {
            if (int(at) >= table.size() || visited.contains(at)) return false;
            visited.insert(at);
            out->append(at);
        }
        return true;
    };
    const auto readChain = [&](quint32 start, qint64 size, QByteArray *out) {
        QVector<quint32> sectors;
        if (!chain(start, fat, &sectors)) return false;
        if (qint64(sectors.size()) * sectorSize < size) return false;
        out->clear();
        out->reserve(int(qMin<qint64>(size, qint64(sectors.size()) * sectorSize)));
        for (const auto sector : sectors) {
            if (!sectorOk(sector)) return false;
            out->append(sectorBytes(sector));
            if (out->size() >= size) break;
        }
        out->truncate(int(size));
        return true;
    };

    // The directory.
    QByteArray dir;
    {
        QVector<quint32> sectors;
        if (!chain(u32(raw, 0x30), fat, &sectors) || sectors.isEmpty()) { m_error = QStringLiteral("broken directory"); return; }
        for (const auto sector : sectors) {
            if (!sectorOk(sector)) { m_error = QStringLiteral("broken directory"); return; }
            dir.append(sectorBytes(sector));
        }
    }
    struct Dir { QString name; quint8 type; quint32 left, right, child, start; qint64 size; };
    QVector<Dir> entries;
    const bool version3 = u16(raw, 0x1A) == 3;
    for (qint64 at = 0; at + kDirEntry <= dir.size(); at += kDirEntry) {
        Dir d;
        const int nameBytes = qBound(0, int(u16(dir, at + 0x40)), 64);
        d.name = QString::fromUtf16(reinterpret_cast<const char16_t *>(dir.constData() + at), qMax(0, nameBytes / 2 - 1));
        d.type = quint8(dir.at(at + 0x42));
        d.left = u32(dir, at + 0x44);
        d.right = u32(dir, at + 0x48);
        d.child = u32(dir, at + 0x4C);
        d.start = u32(dir, at + 0x74);
        d.size = version3 ? qint64(u32(dir, at + 0x78)) : qint64(qFromLittleEndian<quint64>(dir.constData() + at + 0x78));
        entries.append(d);
    }
    if (entries.isEmpty() || entries.first().type != Root) { m_error = QStringLiteral("no root storage"); return; }

    // Small streams live in the mini stream, cut into 64-byte pieces.
    const quint32 cutoff = u32(raw, 0x38);
    QByteArray mini;
    if (entries.first().size > 0 && !readChain(entries.first().start, qMin<qint64>(entries.first().size, kMaxFileBytes), &mini)) {
        m_error = QStringLiteral("broken mini stream");
        return;
    }
    QVector<quint32> miniFat;
    {
        QByteArray table;
        const quint32 first = u32(raw, 0x3C);
        if (first != kEnd && first != kFree) {
            QVector<quint32> sectors;
            if (!chain(first, fat, &sectors)) { m_error = QStringLiteral("broken mini allocation table"); return; }
            for (const auto sector : sectors) {
                if (!sectorOk(sector)) { m_error = QStringLiteral("broken mini allocation table"); return; }
                table.append(sectorBytes(sector));
            }
        }
        for (qint64 at = 0; at + 4 <= table.size(); at += 4) miniFat.append(u32(table, at));
    }
    const auto readMini = [&](quint32 start, qint64 size, QByteArray *out) {
        QVector<quint32> sectors;
        if (!chain(start, miniFat, &sectors)) return false;
        if (qint64(sectors.size()) * kMiniSector < size) return false;
        out->clear();
        for (const auto sector : sectors) {
            const qint64 at = qint64(sector) * kMiniSector;
            if (at + kMiniSector > mini.size() + kMiniSector) return false;
            out->append(mini.mid(at, kMiniSector));
            if (out->size() >= size) break;
        }
        if (out->size() < size) return false;
        out->truncate(int(size));
        return true;
    };

    // Walk the tree: siblings left and right, a storage's children below it.
    QSet<quint32> visited;
    std::function<bool(quint32, const QString &, int)> walk = [&](quint32 index, const QString &prefix, int depth) {
        if (index == kNone) return true;
        if (int(index) >= entries.size() || visited.contains(index) || depth > 64) return false;
        visited.insert(index);
        const auto &d = entries.at(int(index));
        if (!walk(d.left, prefix, depth + 1) || !walk(d.right, prefix, depth + 1)) return false;
        const QString path = prefix + d.name;
        if (d.type == Storage) return walk(d.child, path + QLatin1Char('/'), depth + 1);
        if (d.type != Stream) return true;
        if (d.size > kMaxFileBytes) return false;
        QByteArray data;
        const bool ok = d.size == 0 ? true : d.size < qint64(cutoff) ? readMini(d.start, d.size, &data) : readChain(d.start, d.size, &data);
        if (!ok) return false;
        m_streams.insert(path, data);
        return true;
    };
    if (!walk(entries.first().child, QString(), 0)) { m_error = QStringLiteral("broken directory"); m_streams.clear(); return; }
    m_valid = true;
}

QByteArray write(const QVector<Entry> &streams, const QByteArray &rootClass) {
    // The tree of storages and streams the paths describe.
    struct Node { QString name; quint8 type = Stream; QByteArray data; QVector<int> children; quint32 start = kEnd;
                  quint32 left = kNone, right = kNone, child = kNone; bool red = false; };
    QVector<Node> nodes(1);
    nodes[0].name = QStringLiteral("Root Entry");
    nodes[0].type = Root;
    qint64 total = 0;
    for (const auto &stream : streams) {
        total += stream.data.size();
        if (total > kMaxFileBytes) return {};
        const auto parts = stream.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) return {};
        int parent = 0;
        for (int i = 0; i < parts.size(); ++i) {
            const auto name = parts.at(i).left(31);
            int found = -1;
            for (const int c : nodes.at(parent).children) if (nodes.at(c).name.compare(name, Qt::CaseInsensitive) == 0) found = c;
            if (found < 0) {
                Node n;
                n.name = name;
                n.type = i + 1 < parts.size() ? Storage : Stream;
                nodes.append(n);
                found = nodes.size() - 1;
                nodes[parent].children.append(found);
            }
            parent = found;
        }
        nodes[parent].data = stream.data;
    }

    // Each storage's children as a balanced tree, coloured to be a valid
    // red-black tree: every level is full but the last, whose nodes are red.
    for (auto &node : nodes) {
        if (node.children.isEmpty()) continue;
        auto kids = node.children;
        std::sort(kids.begin(), kids.end(), [&](int a, int b) { return before(nodes.at(a).name, nodes.at(b).name); });
        QHash<int, int> depth;
        std::function<int(int, int, int)> build = [&](int from, int to, int d) -> int {
            if (from > to) return -1;
            const int mid = (from + to) / 2;
            const int at = kids.at(mid);
            depth.insert(at, d);
            const int l = build(from, mid - 1, d + 1), r = build(mid + 1, to, d + 1);
            nodes[at].left = l < 0 ? kNone : quint32(l);
            nodes[at].right = r < 0 ? kNone : quint32(r);
            return at;
        };
        node.child = quint32(build(0, kids.size() - 1, 0));
        int deepest = 0;
        for (const int d : std::as_const(depth)) deepest = qMax(deepest, d);
        const bool perfect = kids.size() == (1 << (deepest + 1)) - 1;
        for (auto it = depth.cbegin(); it != depth.cend(); ++it) nodes[it.key()].red = !perfect && it.value() == deepest;
    }

    // Where everything goes.
    QByteArray miniStream;
    QVector<quint32> miniFat;
    for (auto &node : nodes) {
        if (node.type != Stream || node.data.isEmpty() || node.data.size() >= int(kMiniCutoff)) continue;
        const int pieces = (node.data.size() + kMiniSector - 1) / kMiniSector;
        node.start = quint32(miniFat.size());
        for (int i = 0; i < pieces; ++i) miniFat.append(i + 1 < pieces ? quint32(miniFat.size() + 1) : kEnd);
        QByteArray padded = node.data;
        padded.append(QByteArray(pieces * kMiniSector - padded.size(), '\0'));
        miniStream.append(padded);
    }
    const auto sectorsFor = [](qint64 bytes) { return int((bytes + kSector - 1) / kSector); };
    const int miniFatSectors = sectorsFor(qint64(miniFat.size()) * 4);
    const int dirSectors = sectorsFor(qint64(nodes.size()) * kDirEntry);
    const int miniStreamSectors = sectorsFor(miniStream.size());
    int dataSectors = miniFatSectors + dirSectors + miniStreamSectors;
    for (const auto &node : nodes)
        if (node.type == Stream && node.data.size() >= int(kMiniCutoff)) dataSectors += sectorsFor(node.data.size());
    int fatSectors = 0, difatSectors = 0;
    for (;;) {
        const int all = dataSectors + fatSectors + difatSectors;
        const int needFat = (all + kSector / 4 - 1) / (kSector / 4);
        const int needDifat = needFat > kHeaderDifat ? (needFat - kHeaderDifat + kSector / 4 - 2) / (kSector / 4 - 1) : 0;
        if (needFat == fatSectors && needDifat == difatSectors) break;
        fatSectors = needFat;
        difatSectors = needDifat;
    }
    const int sectorTotal = fatSectors + difatSectors + dataSectors;
    QVector<quint32> fat(fatSectors * (kSector / 4), kFree);
    int next = 0;
    for (int i = 0; i < fatSectors; ++i) fat[next++] = kFatSector;
    const int firstDifat = next;
    for (int i = 0; i < difatSectors; ++i) fat[next++] = kDifatSector;
    const auto allocate = [&](int count) {
        if (count == 0) return kEnd;
        const int first = next;
        for (int i = 0; i < count; ++i, ++next) fat[next] = i + 1 < count ? quint32(next + 1) : kEnd;
        return quint32(first);
    };
    const quint32 miniFatStart = allocate(miniFatSectors);
    const quint32 dirStart = allocate(dirSectors);
    const quint32 miniStreamStart = allocate(miniStreamSectors);
    nodes[0].start = miniStreamStart;
    for (auto &node : nodes)
        if (node.type == Stream && node.data.size() >= int(kMiniCutoff)) node.start = allocate(sectorsFor(node.data.size()));
    Q_ASSERT(next == sectorTotal);

    QByteArray out(qint64(sectorTotal + 1) * kSector, '\0');
    const auto sectorAt = [&](quint32 sector) { return (qint64(sector) + 1) * kSector; };
    const auto place = [&](quint32 start, const QByteArray &bytes) {
        if (start != kEnd) std::copy(bytes.cbegin(), bytes.cend(), out.begin() + sectorAt(start));
    };

    // Header.
    std::copy(kSignature, kSignature + 8, out.begin());
    put16(out, 0x18, 0x003E);
    put16(out, 0x1A, 3);
    put16(out, 0x1C, 0xFFFE);
    put16(out, 0x1E, 9);
    put16(out, 0x20, 6);
    put32(out, 0x2C, quint32(fatSectors));
    put32(out, 0x30, dirStart);
    put32(out, 0x38, kMiniCutoff);
    put32(out, 0x3C, miniFatStart);
    put32(out, 0x40, quint32(miniFatSectors));
    put32(out, 0x44, difatSectors ? quint32(firstDifat) : kEnd);
    put32(out, 0x48, quint32(difatSectors));
    for (int i = 0; i < kHeaderDifat; ++i) put32(out, 0x4C + i * 4, i < fatSectors ? quint32(i) : kFree);
    // DIFAT sectors: the rest of the FAT's sector numbers, each ending with the next.
    for (int d = 0; d < difatSectors; ++d) {
        const qint64 at = sectorAt(quint32(firstDifat + d));
        for (int i = 0; i < kSector / 4 - 1; ++i) {
            const int fatIndex = kHeaderDifat + d * (kSector / 4 - 1) + i;
            put32(out, at + i * 4, fatIndex < fatSectors ? quint32(fatIndex) : kFree);
        }
        put32(out, at + kSector - 4, d + 1 < difatSectors ? quint32(firstDifat + d + 1) : kEnd);
    }
    for (int i = 0; i < fat.size(); ++i) put32(out, (i / (kSector / 4) + 1) * qint64(kSector) + (i % (kSector / 4)) * 4, fat.at(i));

    QByteArray miniTable(miniFatSectors * kSector, '\0');
    for (int i = 0; i < miniTable.size() / 4; ++i) put32(miniTable, i * 4, i < miniFat.size() ? miniFat.at(i) : kFree);
    place(miniFatStart, miniTable);
    place(miniStreamStart, miniStream);

    QByteArray dir(dirSectors * kSector, '\0');
    for (int i = 0; i < dirSectors * kSector / kDirEntry; ++i) {
        const qint64 at = qint64(i) * kDirEntry;
        put32(dir, at + 0x44, kNone);
        put32(dir, at + 0x48, kNone);
        put32(dir, at + 0x4C, kNone);
        if (i >= nodes.size()) continue;
        const auto &node = nodes.at(i);
        const auto name = node.name.toStdU16String();
        std::copy(reinterpret_cast<const char *>(name.data()), reinterpret_cast<const char *>(name.data() + name.size()), dir.begin() + at);
        put16(dir, at + 0x40, quint16((name.size() + 1) * 2));
        dir[at + 0x42] = char(node.type);
        dir[at + 0x43] = char(node.red ? 0 : 1);
        put32(dir, at + 0x44, node.left);
        put32(dir, at + 0x48, node.right);
        put32(dir, at + 0x4C, node.child);
        if (i == 0 && rootClass.size() == 16) std::copy(rootClass.cbegin(), rootClass.cend(), dir.begin() + at + 0x50);
        if (node.type == Root) {
            put32(dir, at + 0x74, miniStream.isEmpty() ? kEnd : node.start);
            put32(dir, at + 0x78, quint32(miniStream.size()));
        } else if (node.type == Stream) {
            put32(dir, at + 0x74, node.data.isEmpty() ? kEnd : node.start);
            put32(dir, at + 0x78, quint32(node.data.size()));
        }
    }
    place(dirStart, dir);
    for (const auto &node : nodes)
        if (node.type == Stream && node.data.size() >= int(kMiniCutoff)) place(node.start, node.data);
    return out;
}

} // namespace Cfb
