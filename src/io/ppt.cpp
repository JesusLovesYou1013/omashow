#include "io/ppt.h"

#include "core/edit.h"
#include "core/imageasset.h"
#include "core/link.h"
#include "core/shape.h"
#include "core/table.h"
#include "core/textruns.h"
#include "io/cfb.h"
#include "render/textlayout.h"

#include <QBuffer>
#include <QDate>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QTransform>
#include <QtEndian>
#include <QtMath>

#include <cmath>
#include <functional>

#include <zlib.h>

namespace {

// ---- records ([MS-PPT] and [MS-ODRAW] record types)

enum RecordType : int {
    kDocument = 0x03E8, kDocumentAtom = 0x03E9, kSlide = 0x03EE, kSlideAtom = 0x03EF, kNotes = 0x03F0,
    kSlidePersistAtom = 0x03F3, kEnvironment = 0x03F2, kMainMaster = 0x03F8, kSlideShowSlideInfoAtom = 0x03F9,
    kExObjList = 0x0409, kPPDrawingGroup = 0x040B, kPPDrawing = 0x040C, kNamedShows = 0x0410, kNamedShow = 0x0411,
    kNamedShowSlidesAtom = 0x0412, kColorSchemeAtom = 0x07F0, kFontCollection = 0x07D5, kExObjRefAtom = 0x0BC1,
    kPlaceholderAtom = 0x0BC3, kOutlineTextRefAtom = 0x0F9E, kTextHeaderAtom = 0x0F9F, kTextCharsAtom = 0x0FA0,
    kStyleTextPropAtom = 0x0FA1, kTxMasterStyleAtom = 0x0FA3, kTextCFExceptionAtom = 0x0FA4, kTextPFExceptionAtom = 0x0FA5,
    kTextBytesAtom = 0x0FA8, kFontEntityAtom = 0x0FB7, kCString = 0x0FBA, kExHyperlinkAtom = 0x0FD3,
    kExHyperlink = 0x0FD7, kHeadersFooters = 0x0FD9, kHeadersFootersAtom = 0x0FDA, kTextInteractiveInfoAtom = 0x0FDF,
    kSlideListWithText = 0x0FF0, kAnimationInfo = 0x0FF1, kInteractiveInfo = 0x0FF2, kInteractiveInfoAtom = 0x0FF3,
    kUserEditAtom = 0x0FF5, kCurrentUserAtom = 0x0FF6, kExAviMovie = 0x1006, kExMCIMovie = 0x1007,
    kExWAVAudioEmbedded = 0x100E, kExWAVAudioLink = 0x100F, kExMIDIAudio = 0x100D, kExCDAudio = 0x100C,
    kProgTags = 0x1388, kProgBinaryTag = 0x138A, kBinaryTagData = 0x138B, kPersistDirectoryAtom = 0x1772,
    kComment10 = 0x2EE0, kComment10Atom = 0x2EE1,
    // Office Art
    kDggContainer = 0xF000, kBStoreContainer = 0xF001, kDgContainer = 0xF002, kSpgrContainer = 0xF003,
    kSpContainer = 0xF004, kFBSE = 0xF007, kFSPGR = 0xF009, kFSP = 0xF00A, kFOPT = 0xF00B,
    kClientTextbox = 0xF00D, kChildAnchor = 0xF00F, kClientAnchor = 0xF010, kClientData = 0xF011,
    kTertiaryFOPT = 0xF122, kExtTimeNode = 0xF144,
};

quint8 u8(const QByteArray &b, qint64 at) { return at >= 0 && at < b.size() ? quint8(b.at(at)) : 0; }
quint16 u16(const QByteArray &b, qint64 at) { return at >= 0 && at + 2 <= b.size() ? qFromLittleEndian<quint16>(b.constData() + at) : 0; }
qint16 s16(const QByteArray &b, qint64 at) { return qint16(u16(b, at)); }
quint32 u32(const QByteArray &b, qint64 at) { return at >= 0 && at + 4 <= b.size() ? qFromLittleEndian<quint32>(b.constData() + at) : 0; }
qint32 s32(const QByteArray &b, qint64 at) { return qint32(u32(b, at)); }

struct Rec {
    int ver = 0, instance = 0, type = 0;
    qint64 at = 0, len = 0;    // where its data starts, and how long it is
    bool valid = false;
    bool container() const { return ver == 0xF; }
    qint64 end() const { return at + len; }
};

Rec recordAt(const QByteArray &b, qint64 offset, qint64 limit) {
    Rec r;
    if (offset < 0 || offset + 8 > limit || limit > b.size()) return r;
    const quint16 vi = u16(b, offset);
    r.ver = vi & 0xF;
    r.instance = vi >> 4;
    r.type = u16(b, offset + 2);
    r.len = u32(b, offset + 4);
    r.at = offset + 8;
    if (r.len > limit - r.at) return r;
    r.valid = true;
    return r;
}

QVector<Rec> children(const QByteArray &b, qint64 from, qint64 to) {
    QVector<Rec> out;
    for (qint64 at = from; at + 8 <= to;) {
        const Rec r = recordAt(b, at, to);
        if (!r.valid) break;
        out.append(r);
        at = r.end();
    }
    return out;
}
QVector<Rec> children(const QByteArray &b, const Rec &parent) { return children(b, parent.at, parent.end()); }

Rec first(const QByteArray &b, const Rec &parent, int type, int instance = -1) {
    for (const auto &r : children(b, parent))
        if (r.type == type && (instance < 0 || r.instance == instance)) return r;
    return Rec();
}

// Every record of a type anywhere inside, through containers and tag blobs.
void collect(const QByteArray &b, const Rec &parent, int type, QVector<Rec> &out, int depth = 0) {
    if (depth > 24) return;
    for (const auto &r : children(b, parent)) {
        if (r.type == type) out.append(r);
        if (r.container() || r.type == kBinaryTagData) collect(b, r, type, out, depth + 1);
    }
}

QString utf16(const QByteArray &b, qint64 at, qint64 bytes) {
    if (at < 0 || bytes <= 0 || at + bytes > b.size()) return {};
    QString s = QString::fromUtf16(reinterpret_cast<const char16_t *>(b.constData() + at), bytes / 2);
    const int nul = s.indexOf(QChar(0));
    if (nul >= 0) s.truncate(nul);
    return s;
}
QString cstring(const QByteArray &b, const Rec &r) { return r.valid ? utf16(b, r.at, r.len) : QString(); }

// ---- Office Art properties

struct Props {
    QHash<int, quint32> value;
    QHash<int, QByteArray> complex;
    bool has(int pid) const { return value.contains(pid); }
    quint32 get(int pid, quint32 fallback) const { return value.value(pid, fallback); }
    qint32 sget(int pid, qint32 fallback) const { return value.contains(pid) ? qint32(value.value(pid)) : fallback; }
    qreal fixed(int pid, qreal fallback) const { return value.contains(pid) ? qint32(value.value(pid)) / 65536.0 : fallback; }
    // A switch in a set of them: its own bit, used only when its "use" bit (16 above) is set.
    bool flag(int pid, quint32 bit, bool fallback) const {
        if (!value.contains(pid)) return fallback;
        const quint32 v = value.value(pid);
        return (v & (bit << 16)) ? (v & bit) != 0 : fallback;
    }
    QString text(int pid) const { const auto d = complex.value(pid); return utf16(d, 0, d.size()); }
    void over(const Props &other) {
        for (auto it = other.value.cbegin(); it != other.value.cend(); ++it) {
            // Switch sets combine: the newer one's used bits win, the rest stay.
            if ((it.key() & 0x3F) == 0x3F && value.contains(it.key())) {
                const quint32 used = it.value() >> 16;
                value[it.key()] = (value[it.key()] & ~(used | (used << 16))) | it.value();
            } else value.insert(it.key(), it.value());
        }
        for (auto it = other.complex.cbegin(); it != other.complex.cend(); ++it) complex.insert(it.key(), it.value());
    }
};

void readProps(const QByteArray &b, const Rec &r, Props &into) {
    if (!r.valid) return;
    const int count = r.instance;
    qint64 fixedEnd = r.at + qint64(count) * 6;
    if (fixedEnd > r.end()) return;
    qint64 extra = fixedEnd;
    for (int i = 0; i < count; ++i) {
        const quint16 id = u16(b, r.at + i * 6);
        const quint32 op = u32(b, r.at + i * 6 + 2);
        const int pid = id & 0x3FFF;
        into.value.insert(pid, op);
        if (id & 0x8000) {
            qint64 size = op;
            // An array (points, segments, guides…) whose stated size leaves out its own
            // six-byte header, as many writers have it: measured from the header instead.
            static const QSet<int> arrays = {0x145, 0x146, 0x151, 0x152, 0x153, 0x156, 0x157, 0x197, 0x1CF};
            if (arrays.contains(pid) && extra + 6 <= r.end()) {
                const int elements = u16(b, extra);
                int each = u16(b, extra + 4);
                if (each == 0xFFF0) each = pid == 0x145 ? 4 : 2;
                if (qint64(elements) * each + 6 == qint64(op) + 6) size = op + 6;
            }
            const qint64 take = qMin<qint64>(size, r.end() - extra);
            if (take > 0) into.complex.insert(pid, b.mid(extra, take));
            extra += qMax<qint64>(0, take);
        }
    }
}

// ---- text styles ([MS-PPT] TextPFException / TextCFException)

enum PfBit : int {
    PfHasBullet = 0x1, PfBulletChar = 0x80, PfAlign = 0x800, PfLineSpacing = 0x1000, PfSpaceBefore = 0x2000,
    PfSpaceAfter = 0x4000, PfLeftMargin = 0x100, PfIndent = 0x400, PfTabSize = 0x8000, PfDirection = 0x200000,
};
enum CfBit : int {
    CfBold = 0x1, CfItalic = 0x2, CfUnderline = 0x4, CfShadow = 0x10, CfFont = 0x10000, CfSize = 0x20000,
    CfColor = 0x40000, CfPosition = 0x80000,
};

struct Pf { QHash<int, qint32> v; void over(const Pf &o) { for (auto it = o.v.cbegin(); it != o.v.cend(); ++it) v.insert(it.key(), it.value()); } };
struct Cf { QHash<int, qint64> v; void over(const Cf &o) { for (auto it = o.v.cbegin(); it != o.v.cend(); ++it) v.insert(it.key(), it.value()); } };

bool readPf(const QByteArray &b, qint64 &p, qint64 end, Pf &out) {
    if (p + 4 > end) return false;
    const quint32 m = u32(b, p);
    p += 4;
    const auto take16 = [&](int key, bool isSigned) {
        if (p + 2 > end) return false;
        if (key) out.v.insert(key, isSigned ? s16(b, p) : u16(b, p));
        p += 2;
        return true;
    };
    if (m & 0xF) {
        if (p + 2 > end) return false;
        const quint16 flags = u16(b, p);
        if (m & 0x1) out.v.insert(PfHasBullet, flags & 0x1);
        p += 2;
    }
    if ((m & 0x80) && !take16(PfBulletChar, false)) return false;
    if ((m & 0x10) && !take16(0, false)) return false;
    if ((m & 0x40) && !take16(0, true)) return false;
    if (m & 0x20) { if (p + 4 > end) return false; p += 4; }
    if ((m & 0x800) && !take16(PfAlign, false)) return false;
    if ((m & 0x1000) && !take16(PfLineSpacing, true)) return false;
    if ((m & 0x2000) && !take16(PfSpaceBefore, true)) return false;
    if ((m & 0x4000) && !take16(PfSpaceAfter, true)) return false;
    if ((m & 0x100) && !take16(PfLeftMargin, true)) return false;
    if ((m & 0x400) && !take16(PfIndent, true)) return false;
    if ((m & 0x8000) && !take16(PfTabSize, true)) return false;
    if (m & 0x100000) {
        if (p + 2 > end) return false;
        const int count = u16(b, p);
        p += 2 + qint64(count) * 4;
        if (p > end) return false;
    }
    if ((m & 0x10000) && !take16(0, false)) return false;
    if ((m & 0xE0000) && !take16(0, false)) return false;
    if ((m & 0x200000) && !take16(PfDirection, false)) return false;
    return true;
}

bool readCf(const QByteArray &b, qint64 &p, qint64 end, Cf &out) {
    if (p + 4 > end) return false;
    const quint32 m = u32(b, p);
    p += 4;
    if (m & 0xFFFF) {
        if (p + 2 > end) return false;
        const quint16 style = u16(b, p);
        for (const int bit : {CfBold, CfItalic, CfUnderline, CfShadow})
            if (m & bit) out.v.insert(bit, (style & bit) ? 1 : 0);
        p += 2;
    }
    const auto take = [&](quint32 bit, int key, int bytes, bool isSigned) {
        if (!(m & bit)) return true;
        if (p + bytes > end) return false;
        if (key) out.v.insert(key, bytes == 4 ? qint64(u32(b, p)) : isSigned ? qint64(s16(b, p)) : qint64(u16(b, p)));
        p += bytes;
        return true;
    };
    return take(0x10000, CfFont, 2, false) && take(0x200000, 0, 2, false) && take(0x400000, 0, 2, false) &&
           take(0x800000, 0, 2, false) && take(0x20000, CfSize, 2, false) && take(0x40000, CfColor, 4, false) &&
           take(0x80000, CfPosition, 2, true) && take(0x100000, 0, 4, false) && take(0x1000000, 0, 2, false) &&
           take(0x2000000, 0, 2, false) && take(0x4000000, 0, 4, false);
}

// The levels of a master's text styles, by text type (title 0, body 1, notes 2,
// other 4, centred body 5, centred title 6, half body 7, quarter body 8).
struct MasterStyles {
    Pf pf[9][5];
    Cf cf[9][5];
    bool known[9] = {};
};

void readMasterStyle(const QByteArray &b, const Rec &r, MasterStyles &into) {
    if (r.instance < 0 || r.instance > 8) return;
    qint64 p = r.at;
    const int levels = qMin<int>(u16(b, p), 5);
    p += 2;
    for (int i = 0; i < levels; ++i) {
        int level = i;
        if (r.instance >= 5) { level = qBound(0, int(u16(b, p)), 4); p += 2; }
        Pf pf; Cf cf;
        if (!readPf(b, p, r.end(), pf) || !readCf(b, p, r.end(), cf)) return;
        into.pf[r.instance][level] = pf;
        into.cf[r.instance][level] = cf;
    }
    into.known[r.instance] = true;
}

// ---- warnings and placing

struct Warnings {
    QMap<QString, QList<int>> where;
    void add(const QString &message, int slide) {
        auto &list = where[message];
        if (!list.contains(slide)) list.append(slide);
    }
    QStringList lines() const {
        QStringList out;
        for (auto it = where.cbegin(); it != where.cend(); ++it) {
            QStringList numbers;
            for (int n : it.value()) if (n > 0) numbers.append(QString::number(n));
            if (numbers.isEmpty()) out.append(it.key());
            else out.append(QStringLiteral("%1 (slide%2 %3)").arg(it.key(), numbers.size() > 1 ? QStringLiteral("s") : QString(), numbers.join(QStringLiteral(", "))));
        }
        return out;
    }
};

// Where an object sits: its box before turning, the turn about its middle,
// and whether it is mirrored.
struct Placement {
    QRectF rect;
    qreal rotation = 0;
    bool flipH = false, flipV = false;
    Placement inner(const QRectF &local) const {
        Placement p = *this;
        const QPointF offset = local.center() - rect.center();
        QTransform t;
        t.rotate(rotation);
        const QPointF centre = rect.center() + t.map(offset);
        p.rect = QRectF(centre.x() - local.width() / 2, centre.y() - local.height() / 2, local.width(), local.height());
        return p;
    }
};

// The look of a stretch of words, and of a paragraph.
struct Look {
    QString family;
    qreal size = 18;        // points
    bool bold = false, italic = false, underline = false, shadow = false;
    QColor color = Qt::black;
    int baseline = 0;
};
struct ParaLook {
    int align = 0;
    qreal lineHeight = 100;
    qreal before = 0, after = 0;   // points
    bool bullet = false;
    QChar bulletChar = QChar(0x2022);
    int level = 0;
};

struct TextOut {
    bool empty = true;
    QString text;
    Look box;
    ParaLook para;
    int listStyle = 0;
    QVector<TextRun> runs;
    QString link;
};

// One shape's words as the file keeps them.
struct TextGroup {
    int type = 4;
    QString chars;
    Rec style;                 // StyleTextPropAtom
    QVector<Rec> links;        // InteractiveInfo containers, in order
    bool valid = false;
};

struct HyperlinkTarget { QString target, location; };

struct Reader {
    QByteArray stream;         // "PowerPoint Document"
    QByteArray pictures;       // "Pictures"
    QHash<quint32, quint32> persist;
    QStringList fonts;
    struct Blip { qint64 offset = -1; QByteArray embedded; };
    QVector<Blip> blips;
    QHash<int, SceneObject> pictureCache;
    QSet<int> badPictures;
    Props drawingDefaults;
    Pf defaultPf;
    Cf defaultCf;
    MasterStyles documentStyles;
    Document doc;
    Warnings warnings;
    qreal k = 1;               // slide units per master unit (576 an inch)
    qreal upt = 1;             // slide units per point
    QHash<quint32, QString> slideIdOf;      // the file's slide id → ours
    QHash<quint32, int> slideNumberOf;
    QHash<quint32, HyperlinkTarget> hyperlinks;
    QHash<quint32, int> mediaKinds;          // ExObj id → record type
    QString footerText, dateText;
    bool animations = false;

    struct MasterInfo {
        int index = -1;
        QString layoutId;
        QColor scheme[8];
        MasterStyles styles;
        bool backgroundObject = false;
    };
    QHash<quint32, MasterInfo> masters;      // by the file's master id
    quint32 firstMaster = 0;

    struct SlideEntry { quint32 persistId = 0, slideId = 0; QVector<TextGroup> texts; };

    // What one shape needs to know about where it is.
    struct Ctx {
        int slide = 0;                       // 1-based; 0 on a master
        const QColor *scheme = nullptr;
        const MasterStyles *styles = nullptr;
        const QVector<TextGroup> *outline = nullptr;
        QVector<SceneObject> *objects = nullptr;
        bool onMaster = false;
        // What a master's placeholders become.
        QVector<SceneObject> *placeholders = nullptr;
        MasterFields *fields = nullptr;
        QString notes;
        bool wantNotes = false;
    };

    qreal points(qreal pt) const { return pt * upt; }
    qreal emu(qreal value) const { return value / 1587.5 * k; }   // 914400 EMU = 576 master units

    // ---- colours

    QColor schemeColour(const Ctx &ctx, int index) const {
        if (ctx.scheme && index >= 0 && index < 8 && ctx.scheme[index].isValid()) return ctx.scheme[index];
        static const QColor fallback[8] = {Qt::white, Qt::black, Qt::gray, Qt::black, QColor(0x00, 0xB8, 0xFF), QColor(0x00, 0x99, 0x66), QColor(0xCC, 0x00, 0x00), QColor(0x99, 0x00, 0x99)};
        return fallback[qBound(0, index, 7)];
    }

    // An Office Art colour: RGB, a scheme entry, or another of the shape's colours.
    QColor artColour(quint32 c, const Ctx &ctx, const Props &p, int depth = 0) const {
        const quint8 flags = quint8(c >> 24);
        if (flags & 0x08) return schemeColour(ctx, int(c & 0xFF));
        if (flags & 0x10) {
            const int which = int(c & 0xFF);
            if (depth < 2) {
                if (which == 0xF0) return artColour(p.get(0x181, 0xFFFFFF), ctx, p, depth + 1);
                if (which == 0xF1) return artColour(p.get(0x1C0, 0), ctx, p, depth + 1);
                if (which == 0xF3) return artColour(p.get(0x183, 0xFFFFFF), ctx, p, depth + 1);
            }
            return Qt::black;
        }
        return QColor(int(c & 0xFF), int((c >> 8) & 0xFF), int((c >> 16) & 0xFF));
    }

    // A text colour: RGB, or a scheme entry.
    QColor textColour(quint32 c, const Ctx &ctx) const {
        const quint8 index = quint8(c >> 24);
        if (index == 0xFE) return QColor(int(c & 0xFF), int((c >> 8) & 0xFF), int((c >> 16) & 0xFF));
        if (index < 8) return schemeColour(ctx, index);
        return schemeColour(ctx, 1);
    }

    // ---- pictures

    bool pictureInto(SceneObject &o, int pib, int slide) {
        if (pib <= 0 || pib > blips.size()) {
            warnings.add(QStringLiteral("A picture's file was missing from the deck"), slide);
            return false;
        }
        if (pictureCache.contains(pib)) { ImageAsset::copyData(o, pictureCache.value(pib)); return true; }
        if (badPictures.contains(pib)) return false;
        const Blip &blip = blips.at(pib - 1);
        const QByteArray &source = blip.embedded.isEmpty() ? pictures : blip.embedded;
        const qint64 offset = blip.embedded.isEmpty() ? blip.offset : 0;
        const Rec r = recordAt(source, offset, source.size());
        QByteArray bytes;
        QString why;
        if (r.valid) {
            const bool twoIds = r.instance & 1;
            const qint64 skip = twoIds ? 32 : 16;
            switch (r.type) {
            case 0xF01D: case 0xF02A: case 0xF01E: case 0xF029:   // JPEG, CMYK JPEG, PNG, TIFF
                bytes = source.mid(r.at + skip + 1, r.len - skip - 1);
                break;
            case 0xF01F: {                                       // DIB: a BMP without its file header
                const QByteArray dib = source.mid(r.at + skip + 1, r.len - skip - 1);
                const quint32 headerSize = u32(dib, 0);
                const int bits = u16(dib, 14);
                const quint32 used = u32(dib, 32);
                quint32 palette = (used ? used : (bits <= 8 ? (1u << bits) : 0)) * 4;
                if (u32(dib, 16) == 3 && headerSize == 40) palette += 12;
                QByteArray file("BM");
                file.append(QByteArray(12, '\0'));
                qToLittleEndian<quint32>(quint32(14 + dib.size()), file.data() + 2);
                qToLittleEndian<quint32>(14 + headerSize + palette, file.data() + 10);
                bytes = file + dib;
                break;
            }
            case 0xF01A: case 0xF01B: case 0xF01C:
                why = QStringLiteral("metafile");
                break;
            default:
                break;
            }
        }
        QString error;
        if (why.isEmpty() && !bytes.isEmpty() && ImageAsset::decode(o, bytes, &error)) {
            SceneObject keep;
            keep.type = ObjectType::Image;
            ImageAsset::copyData(keep, o);
            pictureCache.insert(pib, keep);
            return true;
        }
        badPictures.insert(pib);
        if (why == QLatin1String("metafile"))
            warnings.add(QStringLiteral("A picture in a Windows or Mac drawing format (WMF, EMF or PICT) was left out"), slide);
        else
            warnings.add(QStringLiteral("A picture could not be read%1").arg(error.isEmpty() ? QString() : QStringLiteral(": ") + error), slide);
        return false;
    }

    // ---- fills, outlines, shadows

    struct Fill {
        int style = 5;
        QColor color, second;
        qreal angle = 0;
        int pattern = 9;
        int pib = 0;
        bool tiled = false;
    };

    Fill fillOf(const Props &p, const Ctx &ctx, bool filledByDefault = true) {
        Fill f;
        if (!p.flag(0x1BF, 0x10, filledByDefault)) return f;
        const int type = int(p.get(0x180, 0));
        const qreal opacity = qBound(0.0, p.fixed(0x182, 1.0), 1.0);
        const qreal backOpacity = qBound(0.0, p.fixed(0x184, 1.0), 1.0);
        QColor base = artColour(p.get(0x181, 0xFFFFFF), ctx, p);
        QColor back = artColour(p.get(0x183, 0xFFFFFF), ctx, p);
        base.setAlphaF(opacity);
        back.setAlphaF(backOpacity);
        switch (type) {
        case 0:
            f.style = 0;
            f.color = base;
            break;
        case 1:   // a pattern: an 8 by 8 picture of two colours
            f.style = 3;
            f.color = base;
            f.second = back;
            f.pattern = 12;
            warnings.add(QStringLiteral("A pattern fill was drawn with a stand-in pattern"), ctx.slide);
            break;
        case 2: case 3:
            f.pib = int(p.get(0x186, 0));
            f.tiled = type == 2;
            f.style = f.pib > 0 ? 4 : 0;
            f.color = base;
            break;
        case 4: case 7: case 8: {   // shading along a line
            f.style = 1;
            // The angle turns anticlockwise from left-to-right; the focus says where the
            // back colour sits: 0 at the start, 100 at the end, 50 in the middle.
            const qreal angle = p.fixed(0x18B, 0);
            const int focus = p.sget(0x18C, 0);
            f.angle = std::fmod(std::fmod(90.0 - angle, 360.0) + 360.0, 360.0);
            if (focus <= -50 || focus >= 50) { f.color = base; f.second = back; }
            else { f.color = back; f.second = base; }
            if (std::abs(focus) > 0 && std::abs(focus) < 100)
                warnings.add(QStringLiteral("A gradient running out from the middle was shown running one way"), ctx.slide);
            break;
        }
        case 5: case 6:   // shading out from a point, or the shape's own outline
            f.style = 2;
            f.color = back;
            f.second = base;
            break;
        case 9:   // the slide's background: shown through
            f.style = 5;
            break;
        default:
            f.style = 0;
            f.color = base;
            break;
        }
        return f;
    }

    void applyPictureFill(SceneObject &o, const Fill &f, int slide) {
        SceneObject pic;
        pic.type = ObjectType::Image;
        if (!pictureInto(pic, f.pib, slide)) { o.fillStyle = 0; o.fill = f.color.isValid() ? f.color : QColor(Qt::white); return; }
        o.fillStyle = 4;
        ImageAsset::copyData(o, pic);
        o.imageMode = 2;
        if (f.tiled && !pic.image.isNull() && o.rect.width() > 1 && o.rect.height() > 1) {
            // Tiles at the picture's own size (96 dots an inch), made into one picture.
            const QSizeF natural = ImageAsset::size(pic);
            const qreal tw = natural.width() / 96.0 * 576 * k, th = natural.height() / 96.0 * 576 * k;
            if (tw > 1 && th > 1 && (tw < o.rect.width() * 0.95 || th < o.rect.height() * 0.95)) {
                const qreal density = qBound(0.25, pic.image.width() / tw, 4.0);
                QImage out(qMax(1, qRound(o.rect.width() * density)), qMax(1, qRound(o.rect.height() * density)), QImage::Format_ARGB32_Premultiplied);
                out.fill(Qt::transparent);
                QPainter painter(&out);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                const QImage tile = pic.image.scaled(qMax(1, qRound(tw * density)), qMax(1, qRound(th * density)), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                painter.fillRect(out.rect(), QBrush(tile));
                painter.end();
                QByteArray png;
                QBuffer buffer(&png);
                buffer.open(QIODevice::WriteOnly);
                out.save(&buffer, "PNG");
                SceneObject made;
                made.type = ObjectType::Image;
                QString error;
                if (ImageAsset::decode(made, png, &error)) ImageAsset::copyData(o, made);
            }
        }
    }

    void applyFill(SceneObject &o, const Fill &f, int slide) {
        o.fillToken.clear();
        o.fillStyle = f.style;
        if (f.style == 4) { applyPictureFill(o, f, slide); return; }
        if (f.color.isValid()) o.fill = f.color;
        if (f.second.isValid()) o.fillSecondary = f.second;
        o.fillAngle = f.angle;
        o.patternStyle = f.pattern;
    }

    void applyStroke(SceneObject &o, const Props &p, const Ctx &ctx, bool lineByDefault = true) {
        if (!p.flag(0x1FF, 0x8, lineByDefault)) { o.strokeWidth = 0; return; }
        QColor c = artColour(p.get(0x1C0, 0x08000001), ctx, p);
        c.setAlphaF(qBound(0.0, p.fixed(0x1C1, 1.0), 1.0));
        o.strokeColor = c;
        o.strokeWidth = qMax(0.5, emu(p.get(0x1CB, 9525)));
        const int dash = int(p.get(0x1CE, 0));
        // Solid; dashes; dots; dash-dot; dash-dot-dot (OmaShow's order).
        static const int styles[] = {0, 1, 2, 3, 4, 2, 1, 2, 1, 3, 1, 3, 4};
        o.strokeStyle = dash >= 0 && dash < int(sizeof styles / sizeof *styles) ? styles[dash] : 0;
        const int join = int(p.get(0x1D6, 2));
        o.strokeJoin = join == 0 ? 1 : join == 1 ? 0 : 2;
        const int cap = int(p.get(0x1D7, 2));
        o.strokeCap = cap == 0 ? 2 : cap == 1 ? 1 : 0;
    }

    void applyShadow(SceneObject &o, const Props &p, const Ctx &ctx) {
        if (!p.flag(0x23F, 0x2, false)) return;
        QColor c = artColour(p.get(0x201, 0x808080), ctx, p);
        c.setAlphaF(qBound(0.0, p.fixed(0x204, 1.0), 1.0));
        o.shadowEnabled = true;
        o.shadowColor = c;
        o.shadowX = emu(p.sget(0x205, 25400));
        o.shadowY = emu(p.sget(0x206, 25400));
        o.shadowBlur = 0;
    }

    static void describe(SceneObject &o, const Props &p) {
        const QString name = p.text(0x380), description = p.text(0x381);
        if (!name.isEmpty()) o.altTitle = name;
        if (!description.isEmpty()) o.altText = description;
    }

    static bool shows(const SceneObject &box) { return box.fillStyle != 5 || box.strokeWidth > 0; }

    // ---- links

    QString linkOf(const Rec &interactive) {
        const Rec atom = first(stream, interactive, kInteractiveInfoAtom);
        if (!atom.valid) return {};
        const int action = u8(stream, atom.at + 8);
        const quint32 linkId = u32(stream, atom.at + 4);
        if (action != 4) return {};
        const HyperlinkTarget target = hyperlinks.value(linkId);
        if (!target.location.isEmpty()) {
            // "slide id,slide number,title"
            const quint32 slideId = target.location.section(QLatin1Char(','), 0, 0).toUInt();
            if (slideIdOf.contains(slideId)) return QStringLiteral("#slide:") + slideIdOf.value(slideId);
        }
        return target.target;
    }

    void applyLink(SceneObject &o, const QString &href, int slide) {
        if (href.isEmpty()) return;
        if (href.startsWith(QLatin1String("#slide:"))) { o.linkKind = 3; o.linkTarget = href.mid(7); }
        else if (href.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive)) { o.linkKind = 2; o.linkTarget = href.mid(7); }
        else if (href.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) { o.linkKind = 1; o.linkTarget = href; }
        if (o.linkKind && !Links::validate(o.linkKind, o.linkTarget, doc, true).isEmpty()) { o.linkKind = 0; o.linkTarget.clear(); }
        if (!o.linkKind) warnings.add(QStringLiteral("A link OmaShow cannot follow was left off: %1").arg(href.left(80)), slide);
    }

    // ---- text

    TextGroup textGroup(const QVector<Rec> &recs, int &i) const {
        TextGroup g;
        if (i >= recs.size() || recs[i].type != kTextHeaderAtom) return g;
        g.type = int(u32(stream, recs[i].at));
        g.valid = true;
        for (++i; i < recs.size(); ++i) {
            const Rec &r = recs[i];
            if (r.type == kTextHeaderAtom || r.type == kSlidePersistAtom) break;
            if (r.type == kTextCharsAtom) g.chars = QString::fromUtf16(reinterpret_cast<const char16_t *>(stream.constData() + r.at), r.len / 2);
            else if (r.type == kTextBytesAtom) g.chars = QString::fromLatin1(stream.constData() + r.at, r.len);
            else if (r.type == kStyleTextPropAtom) g.style = r;
            else if (r.type == kInteractiveInfo && r.instance == 0) g.links.append(r);
        }
        return g;
    }

    static int baseType(int type) {
        if (type == 0 || type == 6) return 0;
        if (type == 1 || type == 5 || type == 7 || type == 8) return 1;
        if (type == 2) return 2;
        return 4;
    }

    // The look a level of a text type has before its own runs say otherwise.
    void styleFor(const Ctx &ctx, int type, int level, Pf &pf, Cf &cf) const {
        pf = defaultPf;
        cf = defaultCf;
        level = qBound(0, level, 4);
        const auto layer = [&](const MasterStyles &s, int t) {
            if (t < 0 || t > 8 || !s.known[t]) return;
            pf.over(s.pf[t][0]); cf.over(s.cf[t][0]);
            if (level > 0) { pf.over(s.pf[t][level]); cf.over(s.cf[t][level]); }
        };
        const int base = baseType(type);
        layer(documentStyles, base);
        if (ctx.styles) {
            layer(*ctx.styles, base);
            if (type >= 5 && type <= 8) layer(*ctx.styles, type);
        }
    }

    Look lookOf(const Cf &cf, const Ctx &ctx) const {
        Look l;
        const int font = int(cf.v.value(CfFont, 0));
        l.family = fonts.value(font, fonts.value(0, QStringLiteral("Arial")));
        l.size = cf.v.contains(CfSize) ? qBound(1.0, qreal(cf.v.value(CfSize)), 4000.0) : 18;
        l.bold = cf.v.value(CfBold, 0);
        l.italic = cf.v.value(CfItalic, 0);
        l.underline = cf.v.value(CfUnderline, 0);
        l.shadow = cf.v.value(CfShadow, 0);
        l.color = cf.v.contains(CfColor) ? textColour(quint32(cf.v.value(CfColor)), ctx) : schemeColour(ctx, 1);
        const int position = int(cf.v.value(CfPosition, 0));
        l.baseline = position > 0 ? 1 : position < 0 ? 2 : 0;
        return l;
    }

    ParaLook paraOf(const Pf &pf, const Look &look, int level) const {
        ParaLook p;
        const int align = int(pf.v.value(PfAlign, 0));
        p.align = align == 1 ? 1 : align == 2 ? 2 : align >= 3 ? 3 : 0;
        const qint32 spacing = pf.v.value(PfLineSpacing, 100);
        // A share of a line, or (negative) a height in master units (8 a point).
        if (spacing >= 0) p.lineHeight = qBound(50.0, qreal(spacing), 400.0);
        else if (look.size > 0) p.lineHeight = qBound(50.0, (-spacing / 8.0) / (look.size * 1.17) * 100, 400.0);
        const auto space = [&](qint32 v) { return v >= 0 ? v / 100.0 * look.size * 1.2 : -v / 8.0; };
        p.before = space(pf.v.value(PfSpaceBefore, 0));
        p.after = space(pf.v.value(PfSpaceAfter, 0));
        p.bullet = pf.v.value(PfHasBullet, 0);
        if (pf.v.contains(PfBulletChar) && pf.v.value(PfBulletChar) > 0) p.bulletChar = QChar(ushort(pf.v.value(PfBulletChar)));
        p.level = level;
        return p;
    }

    static int markerOf(const ParaLook &p) {
        if (!p.bullet) return 0;
        const ushort u = p.bulletChar.unicode();
        if (u == 0x25CB || u == 0x25E6 || u == 0x006F || u == 0x2218) return 3;
        if (u == 0x25A0 || u == 0x25AA || u == 0x25FC || u == 0x00A7 || u == 0x2751 || u == 0x2752 || u == 0x00A8 || u == 0x00A7 || u == 0xF0A7) return 4;
        return 1;
    }

    TextOut readText(const TextGroup &g, const Ctx &ctx) {
        TextOut out;
        if (!g.valid || g.chars.trimmed().isEmpty()) return out;
        QString chars = g.chars;
        const int n = chars.size();
        // Paragraph and character runs, each covering the text and one more.
        struct PfRun { int start, length, level; Pf pf; };
        struct CfRun { int start, length; Cf cf; };
        QVector<PfRun> pfRuns;
        QVector<CfRun> cfRuns;
        if (g.style.valid) {
            qint64 p = g.style.at;
            const qint64 end = g.style.end();
            int at = 0;
            while (at < n + 1 && p + 6 <= end) {
                const int count = int(qMin<quint32>(u32(stream, p), quint32(n + 1 - at)));
                const int level = qBound(0, int(u16(stream, p + 4)), 4);
                p += 6;
                Pf pf;
                if (!readPf(stream, p, end, pf)) break;
                pfRuns.append({at, qMax(1, count), level, pf});
                at += qMax(1, count);
            }
            at = 0;
            while (at < n + 1 && p + 4 <= end) {
                const int count = int(qMin<quint32>(u32(stream, p), quint32(n + 1 - at)));
                p += 4;
                Cf cf;
                if (!readCf(stream, p, end, cf)) break;
                cfRuns.append({at, qMax(1, count), cf});
                at += qMax(1, count);
            }
        }
        const auto pfAt = [&](int i) -> const PfRun * {
            for (const auto &r : pfRuns) if (i >= r.start && i < r.start + r.length) return &r;
            return pfRuns.isEmpty() ? nullptr : &pfRuns.last();
        };
        const auto cfAt = [&](int i) -> const Cf * {
            for (const auto &r : cfRuns) if (i >= r.start && i < r.start + r.length) return &r.cf;
            return nullptr;
        };

        struct Para { QString text; ParaLook look; QVector<QPair<int, Look>> pieces; Look first; };
        QVector<Para> paras;
        int from = 0;
        for (int i = 0; i <= n; ++i) {
            if (i < n && chars.at(i) != QLatin1Char('\r')) continue;
            Para para;
            const PfRun *pr = pfAt(from);
            const int level = pr ? pr->level : 0;
            Pf pf; Cf base;
            styleFor(ctx, g.type, level, pf, base);
            if (pr) pf.over(pr->pf);
            const auto lookAt = [&](int at) {
                Cf cf = base;
                if (const Cf *own = cfAt(at)) cf.over(*own);
                return lookOf(cf, ctx);
            };
            para.first = lookAt(from);
            para.look = paraOf(pf, para.first, level);
            for (int j = from; j < i; ++j) {
                QChar c = chars.at(j);
                if (c == QChar(0x0B)) c = QLatin1Char('\n');
                else if (c.unicode() < 0x20 && c != QLatin1Char('\t')) continue;
                const Look look = lookAt(j);
                const bool same = !para.pieces.isEmpty() && [&] {
                    const Look &l = para.pieces.last().second;
                    return l.family == look.family && qFuzzyCompare(l.size, look.size) && l.bold == look.bold && l.italic == look.italic &&
                           l.underline == look.underline && l.color == look.color && l.baseline == look.baseline;
                }();
                if (!same) para.pieces.append({int(para.text.size()), look});
                para.text += c;
            }
            paras.append(para);
            from = i + 1;
        }
        if (paras.isEmpty()) return out;
        out.empty = false;
        const Para &firstPara = paras.first();
        out.box = firstPara.pieces.isEmpty() ? firstPara.first : firstPara.pieces.first().second;
        out.para = firstPara.look;
        bool allListed = true, anyListed = false;
        for (const auto &p : paras) {
            if (p.text.trimmed().isEmpty()) continue;
            const int marker = markerOf(p.look);
            if (marker > 0) { if (!anyListed) out.listStyle = marker; anyListed = true; }
            else allListed = false;
        }
        if (anyListed && !allListed) {
            out.listStyle = 0;
            warnings.add(QStringLiteral("List markers in a box mixing list items and plain paragraphs were left off"), ctx.slide);
        }
        QVector<TextRun> runs;
        for (int i = 0; i < paras.size(); ++i) {
            const auto &p = paras[i];
            if (i) out.text += QLatin1Char('\n');
            if (out.listStyle > 0) out.text += QString(qMin(p.look.level, 8), QLatin1Char('\t'));
            const int base0 = out.text.size();
            for (int j = 0; j < p.pieces.size(); ++j) {
                const int a = p.pieces[j].first, b = j + 1 < p.pieces.size() ? p.pieces[j + 1].first : p.text.size();
                if (b <= a) continue;
                const Look &look = p.pieces[j].second;
                TextRun run;
                run.start = base0 + a;
                run.length = b - a;
                if (look.family != out.box.family) run.fontFamily = look.family;
                if (!qFuzzyCompare(look.size, out.box.size)) run.fontSize = points(look.size);
                if (look.bold != out.box.bold) run.weight = look.bold ? 700 : 400;
                if (look.italic != out.box.italic) run.italic = look.italic ? 1 : 2;
                if (look.underline != out.box.underline) run.underline = look.underline ? 1 : 2;
                run.baseline = look.baseline;
                if (look.color != out.box.color) run.color = look.color;
                if (!run.fontFamily.isEmpty() || run.fontSize > 0 || run.weight || run.italic || run.underline || run.baseline || run.color.isValid())
                    runs.append(run);
            }
            out.text += p.text;
        }
        if (out.text.size() > Table::maxText) {
            out.text.truncate(Table::maxText);
            warnings.add(QStringLiteral("A very long text was cut short"), ctx.slide);
        }
        out.runs = TextRuns::tidy(runs, out.text.size());
        for (const auto &link : g.links) {
            out.link = linkOf(link);
            if (!out.link.isEmpty()) break;
        }
        return out;
    }

    static QString plainText(const QString &chars) {
        QString s = chars;
        s.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        s.replace(QChar(0x0B), QLatin1Char('\n'));
        return s.trimmed();
    }

    SceneObject textObject(const TextOut &text, const Props &p, const Placement &place, const Ctx &ctx) {
        SceneObject o;
        o.id = Edit::newId(QStringLiteral("text"));
        o.type = ObjectType::Text;
        o.rect = place.rect;
        o.rotation = place.rotation;
        o.text = text.text;
        const Look &b = text.box;
        o.fontFamily = b.family.isEmpty() ? QStringLiteral("Arial") : b.family;
        o.fontSize = points(b.size);
        o.fontWeight = b.bold ? 700 : 400;
        o.italic = b.italic;
        o.underline = b.underline;
        o.textColor = b.color;
        o.textAlign = text.para.align;
        o.lineHeight = text.para.lineHeight;
        o.paragraphSpacing = points(qMax(text.para.before, text.para.after));
        o.listStyle = text.listStyle;
        o.runs = text.runs;
        const int anchor = int(p.get(0x87, 0));
        o.verticalAlign = anchor % 3 == 0 ? 0 : anchor % 3 == 1 ? 1 : 2;
        if (anchor > 9) o.verticalAlign = 0;
        o.textFit = p.flag(0xBF, 0x2, false) ? 3 : 2;
        o.fillStyle = 5;
        o.strokeWidth = 0;
        o.fillToken.clear(); o.textColorToken.clear(); o.fontToken.clear();
        if (b.shadow) {
            o.shadowEnabled = true;
            o.shadowColor = QColor(0, 0, 0, 128);
            o.shadowX = o.shadowY = qMax(1.0, points(1.5));
        }
        if (!ctx.onMaster && p.get(0x88, 0) != 0) warnings.add(QStringLiteral("Words running up or down the page were set across"), ctx.slide);
        applyLink(o, text.link, ctx.slide);
        return o;
    }

    // The part of a box words sit in: less its margins.
    QRectF padded(const QRectF &r, const Props &p) const {
        const qreal l = emu(p.sget(0x81, 91440)), t = emu(p.sget(0x82, 45720));
        const qreal rr = emu(p.sget(0x83, 91440)), b = emu(p.sget(0x84, 45720));
        QRectF out = r;
        if (r.width() > l + rr + 1) { out.setLeft(r.left() + l); out.setRight(r.right() - rr); }
        if (r.height() > t + b + 1) { out.setTop(r.top() + t); out.setBottom(r.bottom() - b); }
        return out;
    }

    // ---- shapes

    // A preset shape's outline in its own 21600 square, or a kind OmaShow draws itself.
    bool preset(int type, const Props &p, int *kind, qreal *corner, QPainterPath *path) const {
        const auto adj = [&](int i, qreal fallback) { return qreal(p.sget(0x147 + i, qint32(fallback))); };
        const auto poly = [&](std::initializer_list<QPointF> pts) {
            QPainterPath out;
            bool firstPoint = true;
            for (const auto &pt : pts) { if (firstPoint) out.moveTo(pt); else out.lineTo(pt); firstPoint = false; }
            out.closeSubpath();
            *path = out;
            return true;
        };
        const qreal W = 21600;
        switch (type) {
        case 1: case 202: case 109: *kind = 0; return true;
        case 2: case 176: *kind = 0; *corner = qBound(0.0, adj(0, 3600) / W, 0.5); return true;
        case 3: case 120: *kind = 1; return true;
        case 4: case 110: *kind = 3; return true;
        case 5: { const qreal a = adj(0, 10800); return poly({{a, 0}, {W, W}, {0, W}}); }
        case 6: return poly({{0, 0}, {W, W}, {0, W}});
        case 7: { const qreal a = adj(0, 5400); return poly({{a, 0}, {W, 0}, {W - a, W}, {0, W}}); }
        case 8: { const qreal a = adj(0, 5400); return poly({{0, 0}, {W, 0}, {W - a, W}, {a, W}}); }
        case 9: { const qreal a = adj(0, 5400); return poly({{a, 0}, {W - a, 0}, {W, W / 2}, {W - a, W}, {a, W}, {0, W / 2}}); }
        case 10: { const qreal a = adj(0, 6326); return poly({{a, 0}, {W - a, 0}, {W, a}, {W, W - a}, {W - a, W}, {a, W}, {0, W - a}, {0, a}}); }
        case 11: { const qreal a = adj(0, 5400); return poly({{a, 0}, {W - a, 0}, {W - a, a}, {W, a}, {W, W - a}, {W - a, W - a}, {W - a, W}, {a, W}, {a, W - a}, {0, W - a}, {0, a}, {a, a}}); }
        case 12: *kind = 6; return true;
        case 13: { const qreal a = adj(0, 16200), b = adj(1, 5400); return poly({{0, b}, {a, b}, {a, 0}, {W, W / 2}, {a, W}, {a, W - b}, {0, W - b}}); }
        case 66: { const qreal a = adj(0, 5400), b = adj(1, 5400); return poly({{W, b}, {a, b}, {a, 0}, {0, W / 2}, {a, W}, {a, W - b}, {W, W - b}}); }
        case 67: { const qreal a = adj(0, 16200), b = adj(1, 5400); return poly({{b, 0}, {b, a}, {0, a}, {W / 2, W}, {W, a}, {W - b, a}, {W - b, 0}}); }
        case 68: { const qreal a = adj(0, 5400), b = adj(1, 5400); return poly({{b, W}, {b, a}, {0, a}, {W / 2, 0}, {W, a}, {W - b, a}, {W - b, W}}); }
        case 69: { const qreal a = adj(0, 4320), b = adj(1, 5400); return poly({{0, W / 2}, {a, 0}, {a, b}, {W - a, b}, {W - a, 0}, {W, W / 2}, {W - a, W}, {W - a, W - b}, {a, W - b}, {a, W}}); }
        case 15: { const qreal a = adj(0, 16200); return poly({{0, 0}, {a, 0}, {W, W / 2}, {a, W}, {0, W}}); }
        case 55: { const qreal a = adj(0, 16200); return poly({{0, 0}, {a, 0}, {W, W / 2}, {a, W}, {0, W}, {W - a, W / 2}}); }
        case 56: *kind = 4; return true;
        case 74: *kind = 7; return true;
        case 22: *kind = 12; return true;
        case 23: *kind = 23; return true;
        case 61: case 62: *kind = 10; return true;
        case 116: *kind = 20; return true;
        case 114: *kind = 18; return true;
        case 111: return poly({{W * 0.2, 0}, {W, 0}, {W * 0.8, W}, {0, W}});
        default: return false;
        }
    }

    // A freeform's outline, in its own coordinate box.
    QPainterPath freeform(const Props &p, QRectF *space) const {
        QPainterPath path;
        const QByteArray v = p.complex.value(0x145), s = p.complex.value(0x146);
        *space = QRectF(QPointF(p.sget(0x140, 0), p.sget(0x141, 0)), QPointF(p.sget(0x142, 21600), p.sget(0x143, 21600)));
        if (v.size() < 6) return path;
        const int count = u16(v, 0);
        int size = u16(v, 4);
        if (size == 0xFFF0) size = 4;
        if (size != 4 && size != 8) return path;
        QVector<QPointF> pts;
        for (int i = 0; i < count && 6 + qint64(i + 1) * size <= v.size(); ++i) {
            const qint64 at = 6 + qint64(i) * size;
            pts.append(size == 8 ? QPointF(s32(v, at), s32(v, at + 4)) : QPointF(s16(v, at), s16(v, at + 2)));
        }
        if (pts.isEmpty()) return path;
        int next = 0;
        const auto take = [&]() { return next < pts.size() ? pts[next++] : pts.last(); };
        if (s.size() < 6) {
            path.moveTo(take());
            while (next < pts.size()) path.lineTo(take());
            return path;
        }
        const int segments = u16(s, 0);
        int segSize = u16(s, 4);
        if (segSize == 0xFFF0) segSize = 2;
        if (segSize != 2) return path;
        bool open = true;
        for (int i = 0; i < segments && 6 + qint64(i + 1) * 2 <= s.size(); ++i) {
            const quint16 seg = u16(s, 6 + i * 2);
            const int kind = seg >> 13;
            int n = seg & 0x1FFF;
            switch (kind) {
            case 0:   // lines
                if (n == 0) n = 1;
                for (int j = 0; j < n; ++j) { if (open && path.elementCount() == 0) path.moveTo(take()); else path.lineTo(take()); }
                break;
            case 1:   // curves
                if (n == 0) n = 1;
                for (int j = 0; j < n; ++j) { const QPointF a = take(), b = take(), c = take(); if (path.elementCount() == 0) path.moveTo(a); path.cubicTo(a, b, c); }
                break;
            case 2: path.moveTo(take()); open = true; break;
            case 3: path.closeSubpath(); break;
            case 4: break;
            case 5: case 6: next += (seg & 0xFF); break;   // escapes: skip their points
            default: break;
            }
        }
        return path;
    }

    // Where a shape sits on the slide, given its box in the space it is drawn in.
    Placement place(const QRectF &local, const QTransform &toSlide, qreal rotation, bool flipH, bool flipV) const {
        QRectF r = local.normalized();
        const qreal turn = std::fmod(std::fmod(rotation, 360.0) + 360.0, 360.0);
        // Turned near upright, the box is kept as it looks: swap its sides back.
        if ((turn >= 45 && turn < 135) || (turn >= 225 && turn < 315)) {
            const QPointF c = r.center();
            r = QRectF(c.x() - r.height() / 2, c.y() - r.width() / 2, r.height(), r.width());
        }
        Placement p;
        const QPointF o = toSlide.map(QPointF(0, 0));
        const QPointF ax = toSlide.map(QPointF(1, 0)) - o, ay = toSlide.map(QPointF(0, 1)) - o;
        const qreal sx = std::hypot(ax.x(), ax.y()), sy = std::hypot(ay.x(), ay.y());
        const QPointF c = toSlide.map(r.center());
        p.rect = QRectF(c.x() - r.width() * sx / 2, c.y() - r.height() * sy / 2, r.width() * sx, r.height() * sy);
        const qreal frameTurn = qRadiansToDegrees(std::atan2(ax.y(), ax.x()));
        const bool mirrored = toSlide.determinant() < 0;
        p.rotation = rotation + frameTurn;
        if (std::abs(p.rotation) < 1e-6) p.rotation = 0;
        p.flipH = flipH != mirrored;
        p.flipV = flipV;
        return p;
    }

    struct Shape {
        Rec container;
        int type = 0;
        quint32 spid = 0, flags = 0;
        Props props;
        QRectF anchor;
        bool hasAnchor = false;
        Rec clientData, textbox;
        int placeholder = -1;
    };

    Shape shapeOf(const Rec &sp) const {
        Shape s;
        s.container = sp;
        s.props = drawingDefaults;
        for (const auto &r : children(stream, sp)) {
            if (r.type == kFSP) { s.type = r.instance; s.spid = u32(stream, r.at); s.flags = u32(stream, r.at + 4); }
            else if (r.type == kFOPT || r.type == kTertiaryFOPT) { Props own; readProps(stream, r, own); s.props.over(own); }
            else if (r.type == kClientAnchor) {
                if (r.len >= 16) s.anchor = QRectF(QPointF(s32(stream, r.at + 4), s32(stream, r.at)), QPointF(s32(stream, r.at + 8), s32(stream, r.at + 12)));
                else s.anchor = QRectF(QPointF(s16(stream, r.at + 2), s16(stream, r.at)), QPointF(s16(stream, r.at + 4), s16(stream, r.at + 6)));
                s.hasAnchor = true;
            } else if (r.type == kChildAnchor && r.len >= 16) {
                s.anchor = QRectF(QPointF(s32(stream, r.at), s32(stream, r.at + 4)), QPointF(s32(stream, r.at + 8), s32(stream, r.at + 12)));
                s.hasAnchor = true;
            } else if (r.type == kClientData) {
                s.clientData = r;
                const Rec ph = first(stream, r, kPlaceholderAtom);
                if (ph.valid) s.placeholder = u8(stream, ph.at + 4);
            } else if (r.type == kClientTextbox) s.textbox = r;
        }
        return s;
    }

    TextGroup textOf(const Shape &s, const Ctx &ctx) const {
        if (s.textbox.valid) {
            const auto recs = children(stream, s.textbox);
            for (int i = 0; i < recs.size(); ++i) {
                if (recs[i].type == kTextHeaderAtom) return textGroup(recs, i);
                if (recs[i].type == kOutlineTextRefAtom && ctx.outline) {
                    const int index = s32(stream, recs[i].at);
                    if (index >= 0 && index < ctx.outline->size()) return ctx.outline->at(index);
                }
            }
        }
        if (s.clientData.valid) {
            const Rec ref = first(stream, s.clientData, kOutlineTextRefAtom);
            if (ref.valid && ctx.outline) {
                const int index = s32(stream, ref.at);
                if (index >= 0 && index < ctx.outline->size()) return ctx.outline->at(index);
            }
        }
        return {};
    }

    void add(QVector<SceneObject> made, const QStringList &groups, Ctx &ctx, const QString &link, bool hidden) {
        if (made.isEmpty()) return;
        QStringList own = groups;
        if (made.size() > 1) own.append(Edit::newId(QStringLiteral("group")));
        for (auto &o : made) {
            o.groups = own;
            o.hidden = hidden;
            if (!link.isEmpty() && !o.linkKind) applyLink(o, link, ctx.slide);
            ctx.objects->append(o);
        }
    }

    void shape(const Shape &s, const QTransform &toSlide, const QStringList &groups, Ctx &ctx) {
        if (s.flags & 0x8) return;   // deleted
        if (!s.hasAnchor) return;
        const Props &p = s.props;
        const bool hidden = p.flag(0x3BF, 0x2, false);
        const Placement at = place(s.anchor, toSlide, p.fixed(0x4, 0), s.flags & 0x40, s.flags & 0x80);
        QString link;
        bool media = false;
        if (s.clientData.valid) {
            const Rec click = first(stream, s.clientData, kInteractiveInfo, 0);
            if (click.valid) link = linkOf(click);
            if (first(stream, s.clientData, kAnimationInfo).valid) animations = true;
            const Rec ref = first(stream, s.clientData, kExObjRefAtom);
            if (ref.valid) {
                const int kind = mediaKinds.value(u32(stream, ref.at), 0);
                media = kind != 0;
                if (media) warnings.add(QStringLiteral("A film or sound was left out"), ctx.slide);
                else warnings.add(QStringLiteral("An embedded object (a chart, a spreadsheet…) came in as its picture"), ctx.slide);
            }
        }

        // A master's placeholders: words to follow, not things to draw.
        if (ctx.onMaster && s.placeholder > 0) {
            if (ctx.fields && (s.placeholder == 7 || s.placeholder == 8 || s.placeholder == 9)) return;
            if (ctx.placeholders && (s.placeholder == 1 || s.placeholder == 2 || s.placeholder == 3 || s.placeholder == 4)) {
                const QString id = s.placeholder == 1 || s.placeholder == 3 ? QStringLiteral("title") : QStringLiteral("body");
                for (const auto &existing : *ctx.placeholders) if (existing.id == id) return;
                TextGroup sample;
                sample.valid = true;
                sample.type = s.placeholder == 1 ? 0 : s.placeholder == 3 ? 6 : s.placeholder == 4 ? 5 : 1;
                sample.chars = id == QLatin1String("title") ? QStringLiteral("Title") : QStringLiteral("Body");
                TextOut text = readText(sample, ctx);
                if (id == QLatin1String("body")) text.listStyle = markerOf(text.para);
                SceneObject o = textObject(text, p, at.inner(padded(at.rect, p)), ctx);
                o.id = id;
                ctx.placeholders->append(o);
            }
            return;
        }
        // A slide's date, number and footer are the master's to draw.
        if (!ctx.onMaster && (s.placeholder == 7 || s.placeholder == 8 || s.placeholder == 9)) return;

        if (ctx.wantNotes && s.placeholder == 12) {
            const TextGroup g = textOf(s, ctx);
            if (g.valid) ctx.notes = plainText(g.chars);
            return;
        }
        if (ctx.wantNotes) return;

        // Pictures.
        const int pib = int(p.get(0x104, 0));
        if (s.type == 75 || (pib > 0 && (s.flags & 0x10))) {
            SceneObject o;
            o.id = Edit::newId(QStringLiteral("image"));
            o.type = ObjectType::Image;
            if (!pictureInto(o, pib, ctx.slide)) return;
            o.rect = at.rect;
            o.rotation = at.rotation;
            o.fillStyle = 5;
            o.imageMode = 2;
            applyStroke(o, p, ctx, false);
            applyShadow(o, p, ctx);
            describe(o, p);
            const qreal t = p.fixed(0x100, 0), b = p.fixed(0x101, 0), l = p.fixed(0x102, 0), r = p.fixed(0x103, 0);
            if ((t > 0 || b > 0 || l > 0 || r > 0) && t >= 0 && b >= 0 && l >= 0 && r >= 0 && l + r < 1 && t + b < 1)
                o.imageCrop = QRectF(l, t, 1 - l - r, 1 - t - b);
            const qreal contrast = p.fixed(0x108, 1.0);
            o.imageContrast = qBound(0.0, contrast, 2.0);
            o.imageBrightness = qBound(-1.0, p.sget(0x109, 0) / 32768.0, 1.0);
            if (p.flag(0x13F, 0x4, false)) o.imageSaturation = 0;
            if (p.flag(0x13F, 0x2, false)) { o.imageSaturation = 0; warnings.add(QStringLiteral("A black-and-white picture was shown in greys"), ctx.slide); }
            if (at.flipH || at.flipV) warnings.add(QStringLiteral("A mirrored picture was shown unmirrored"), ctx.slide);
            add({o}, groups, ctx, link, hidden);
            return;
        }
        if (media) return;

        // Lines and connectors.
        if (s.type == 20 || (s.type >= 32 && s.type <= 40)) {
            QPointF a = at.rect.topLeft(), b = at.rect.bottomRight();
            if (at.flipH) std::swap(a.rx(), b.rx());
            if (at.flipV) std::swap(a.ry(), b.ry());
            if (!qFuzzyIsNull(at.rotation)) {
                QTransform turn;
                turn.translate(at.rect.center().x(), at.rect.center().y());
                turn.rotate(at.rotation);
                turn.translate(-at.rect.center().x(), -at.rect.center().y());
                a = turn.map(a); b = turn.map(b);
            }
            SceneObject o;
            o.id = Edit::newId(QStringLiteral("connector"));
            o.connector = true;
            o.connectorStart = a;
            o.connectorEnd = b;
            o.connectorRoute = s.type >= 33 && s.type <= 36 ? 1 : s.type >= 37 ? 2 : 0;
            o.connectorArrowStart = p.get(0x1D0, 0) != 0;
            o.connectorArrowEnd = p.get(0x1D1, 0) != 0;
            applyStroke(o, p, ctx);
            if (o.strokeWidth <= 0) return;
            applyShadow(o, p, ctx);
            describe(o, p);
            QPainterPath path;
            path.moveTo(a);
            path.lineTo(b);
            ::Shape::assignPath(o, path);
            o.fillStyle = 5;
            add({o}, groups, ctx, link, hidden);
            return;
        }

        // Everything else: an outline, then words.
        QVector<SceneObject> made;
        SceneObject box;
        box.id = Edit::newId(QStringLiteral("shape"));
        box.type = ObjectType::Rect;
        box.rect = at.rect;
        box.rotation = at.rotation;
        const bool textBox = s.type == 202;
        applyFill(box, fillOf(p, ctx, !textBox), ctx.slide);
        // A mirrored shape's shading is mirrored with it.
        if (box.fillStyle == 1 && (at.flipH || at.flipV)) {
            qreal angle = box.fillAngle;
            if (at.flipH) angle = 180 - angle;
            if (at.flipV) angle = -angle;
            box.fillAngle = std::fmod(std::fmod(angle, 360.0) + 360.0, 360.0);
        }
        applyStroke(box, p, ctx, !textBox);
        applyShadow(box, p, ctx);
        describe(box, p);
        int kind = 0;
        qreal corner = 0;
        QPainterPath outline;
        QRectF space(0, 0, 21600, 21600);
        bool known = true;
        if (s.type == 0 || p.complex.contains(0x145)) outline = freeform(p, &space);
        else if (!preset(s.type, p, &kind, &corner, &outline)) {
            known = false;
            if (shows(box)) warnings.add(QStringLiteral("A shape OmaShow doesn't draw was shown as a rectangle"), ctx.slide);
        }
        if (shows(box)) {
            if (!outline.isEmpty()) {
                // Into the unturned box, mirrored as the shape is, then placed.
                QTransform t;
                t.translate(at.rect.left(), at.rect.top());
                t.scale(at.rect.width() / qMax(1.0, space.width()), at.rect.height() / qMax(1.0, space.height()));
                t.translate(-space.left(), -space.top());
                QTransform mirror;
                mirror.translate(space.center().x(), space.center().y());
                mirror.scale(at.flipH ? -1 : 1, at.flipV ? -1 : 1);
                mirror.translate(-space.center().x(), -space.center().y());
                const QPainterPath world = (mirror * t).map(outline);
                if (s.type == 0 && !p.flag(0x17F, 0x1, true)) box.fillStyle = 5;
                const QRectF before = box.rect;
                ::Shape::assignPath(box, world);
                QRectF bounds = world.boundingRect();
                if (bounds.width() < .001) bounds.setWidth(1);
                if (bounds.height() < .001) bounds.setHeight(1);
                Placement inner = at.inner(bounds);
                box.rect = inner.rect;
                box.rotation = inner.rotation;
                if (box.fillStyle == 4 && box.rect.size() != before.size()) applyFill(box, fillOf(p, ctx, !textBox), ctx.slide);
            } else {
                box.shapeKind = kind;
                if (corner > 0) box.cornerRadius = qMin(at.rect.width(), at.rect.height()) * corner;
                if (!known) box.shapeKind = 0;
                // OmaShow's own kinds don't mirror: a mirrored one becomes its outline.
                if ((at.flipH || at.flipV) && known && kind != 0 && kind != 1 && kind != 3) {
                    const QPointF c = box.rect.center();
                    QTransform mirror;
                    mirror.translate(c.x(), c.y());
                    mirror.scale(at.flipH ? -1 : 1, at.flipV ? -1 : 1);
                    mirror.translate(-c.x(), -c.y());
                    const QPainterPath path = mirror.map(::Shape::path(box));
                    const qreal rotation = box.rotation;
                    const QRectF keep = box.rect;
                    if (::Shape::assignPath(box, path)) { box.rotation = rotation; box.rect = keep; }
                }
            }
            made.append(box);
        }
        const TextGroup g = textOf(s, ctx);
        const TextOut text = readText(g, ctx);
        if (!text.empty) {
            SceneObject words = textObject(text, p, at.inner(padded(at.rect, p)), ctx);
            if (made.isEmpty()) { describe(words, p); applyShadow(words, p, ctx); }
            made.append(words);
        }
        add(made, groups, ctx, link, hidden);
    }

    // A group: its own shape first (its box in its parent, and the space its
    // children are placed in), then the children.
    void group(const Rec &spgr, const QTransform &parent, const QStringList &groups, Ctx &ctx, bool top, int depth) {
        if (depth > 32) return;
        const auto recs = children(stream, spgr);
        QTransform toSlide = parent;
        QStringList own = groups;
        int start = 0;
        if (!recs.isEmpty() && recs.first().type == kSpContainer) {
            const Shape g = shapeOf(recs.first());
            start = 1;
            if (!top) {
                QRectF space;
                for (const auto &r : children(stream, recs.first()))
                    if (r.type == kFSPGR && r.len >= 16)
                        space = QRectF(QPointF(s32(stream, r.at), s32(stream, r.at + 4)), QPointF(s32(stream, r.at + 8), s32(stream, r.at + 12))).normalized();
                if (g.hasAnchor && space.width() > 0 && space.height() > 0) {
                    QRectF box = g.anchor.normalized();
                    const qreal turn = std::fmod(std::fmod(g.props.fixed(0x4, 0), 360.0) + 360.0, 360.0);
                    if ((turn >= 45 && turn < 135) || (turn >= 225 && turn < 315)) {
                        const QPointF c = box.center();
                        box = QRectF(c.x() - box.height() / 2, c.y() - box.width() / 2, box.height(), box.width());
                    }
                    QTransform local;
                    local.translate(box.center().x(), box.center().y());
                    local.rotate(g.props.fixed(0x4, 0));
                    local.scale((g.flags & 0x40 ? -1 : 1) * box.width() / space.width(), (g.flags & 0x80 ? -1 : 1) * box.height() / space.height());
                    local.translate(-space.center().x(), -space.center().y());
                    toSlide = local * parent;
                }
                own.append(Edit::newId(QStringLiteral("group")));
                if (g.props.has(0x39F) || g.props.has(0x3A0))
                    warnings.add(QStringLiteral("A table came in as separate shapes"), ctx.slide);
            }
        }
        for (int i = start; i < recs.size(); ++i) {
            if (recs[i].type == kSpContainer) shape(shapeOf(recs[i]), toSlide, own, ctx);
            else if (recs[i].type == kSpgrContainer) group(recs[i], toSlide, own, ctx, false, depth + 1);
        }
    }

    // A drawing: its shapes, and the background shape if it has one.
    Props drawing(const Rec &container, Ctx &ctx, bool *hasBackground) {
        Props background;
        *hasBackground = false;
        const Rec pp = first(stream, container, kPPDrawing);
        const Rec dg = pp.valid ? first(stream, pp, kDgContainer) : Rec();
        if (!dg.valid) return background;
        QTransform units;
        units.scale(k, k);
        for (const auto &r : children(stream, dg)) {
            if (r.type == kSpgrContainer) group(r, units, {}, ctx, true, 0);
            else if (r.type == kSpContainer) {
                const Shape s = shapeOf(r);
                if (s.flags & 0x400) { background = s.props; *hasBackground = true; }
            }
        }
        return background;
    }

    SceneObject backgroundBox(const Fill &fill, int slide) {
        SceneObject o;
        o.id = Edit::newId(QStringLiteral("background"));
        o.type = ObjectType::Rect;
        o.rect = QRectF(QPointF(0, 0), doc.size);
        o.strokeWidth = 0;
        o.locked = true;
        applyFill(o, fill, slide);
        return o;
    }

    void schemeOf(const Rec &container, QColor *into) const {
        // The scheme in use; LibreOffice writes a master's only as the first of its list.
        Rec atom = first(stream, container, kColorSchemeAtom, 1);
        if (!atom.valid) atom = first(stream, container, kColorSchemeAtom);
        if (!atom.valid || atom.len < 32) return;
        for (int i = 0; i < 8; ++i) {
            const quint32 c = u32(stream, atom.at + i * 4);
            into[i] = QColor(int(c & 0xFF), int((c >> 8) & 0xFF), int((c >> 16) & 0xFF));
        }
    }

    Rec persistRecord(quint32 id) const {
        if (!persist.contains(id)) return Rec();
        return recordAt(stream, persist.value(id), stream.size());
    }

    // ---- masters

    void readMaster(const SlideEntry &entry) {
        const Rec r = persistRecord(entry.persistId);
        if (!r.valid || (r.type != kMainMaster && r.type != kSlide)) return;
        MasterInfo info;
        const Rec atom = first(stream, r, kSlideAtom);
        // A title master follows its main master's styles and colours.
        if (r.type == kSlide && atom.valid && masters.contains(u32(stream, atom.at + 12))) {
            const MasterInfo &main = masters[u32(stream, atom.at + 12)];
            info.styles = main.styles;
            for (int i = 0; i < 8; ++i) info.scheme[i] = main.scheme[i];
        }
        schemeOf(r, info.scheme);
        for (const auto &c : children(stream, r))
            if (c.type == kTxMasterStyleAtom) readMasterStyle(stream, c, info.styles);
        Master master;
        master.id = Edit::newId(QStringLiteral("master"));
        master.name = r.type == kSlide ? QStringLiteral("Title Master") : QStringLiteral("Master %1").arg(doc.masters.size() + 1);
        master.backgroundToken.clear();
        SlideLayout layout;
        layout.id = Edit::newId(QStringLiteral("layout"));
        layout.masterId = master.id;
        layout.name = master.name;
        Ctx ctx;
        ctx.scheme = info.scheme;
        ctx.styles = &info.styles;
        ctx.objects = &master.objects;
        ctx.onMaster = true;
        ctx.placeholders = &layout.placeholders;
        ctx.fields = &master.fields;
        bool hasBackground = false;
        const Props back = drawing(r, ctx, &hasBackground);
        master.background = info.scheme[0].isValid() ? info.scheme[0] : QColor(Qt::white);
        if (hasBackground) {
            const Fill fill = fillOf(back, ctx);
            if (fill.style == 0) master.background = fill.color;
            else if (fill.style != 5) { master.objects.prepend(backgroundBox(fill, 0)); info.backgroundObject = true; }
        }
        for (auto &o : master.objects) o.locked = true;
        info.index = doc.masters.size();
        info.layoutId = layout.id;
        doc.masters.append(master);
        doc.layouts.append(layout);
        masters.insert(entry.slideId, info);
        if (!firstMaster) firstMaster = entry.slideId;
    }

    // ---- slides

    void transition(const Rec &container, Slide &slide, int number) {
        const Rec info = first(stream, container, kSlideShowSlideInfoAtom);
        if (!info.valid || info.len < 16) return;
        const qint32 time = s32(stream, info.at);
        const int direction = u8(stream, info.at + 8);
        const int effect = u8(stream, info.at + 9);
        const quint16 flags = u16(stream, info.at + 10);
        const int speed = u8(stream, info.at + 12);
        if (flags & 0x4) slide.skipped = true;
        if ((flags & 0x400) && time >= 0) slide.advanceAfter = time / 1000.0;
        // Left, up, right, down: where the new slide moves to.
        const auto from = [&]() { return direction == 1 ? 2 : direction == 2 ? 1 : direction == 3 ? 3 : 0; };
        switch (effect) {
        case 0: slide.transition = 0; break;
        case 6: slide.transition = direction == 1 ? 6 : 1; break;
        case 23: slide.transition = 1; break;
        case 4: slide.transition = 4; slide.transitionDirection = from(); break;
        case 7: slide.transition = 5; slide.transitionDirection = from(); break;
        case 20: slide.transition = 2; slide.transitionDirection = from(); break;
        case 11: slide.transition = 7; break;
        default:
            slide.transition = 1;
            warnings.add(QStringLiteral("A transition OmaShow doesn't have was shown as a fade"), number);
            break;
        }
        slide.transitionSeconds = speed == 0 ? 2 : speed == 1 ? 1 : 0.5;
    }

    void readSlide(const SlideEntry &entry, int number, const QHash<quint32, SlideEntry> &notesById) {
        const Rec r = persistRecord(entry.persistId);
        if (!r.valid || r.type != kSlide) return;
        Slide slide;
        slide.id = slideIdOf.value(entry.slideId, Edit::newId(QStringLiteral("slide")));
        const Rec atom = first(stream, r, kSlideAtom);
        const quint32 masterId = atom.valid ? u32(stream, atom.at + 12) : 0;
        const quint32 notesId = atom.valid ? u32(stream, atom.at + 16) : 0;
        const quint16 flags = atom.valid ? u16(stream, atom.at + 20) : 0x7;
        const quint32 useMaster = masters.contains(masterId) ? masterId : firstMaster;
        const MasterInfo info = masters.value(useMaster);
        Master *master = info.index >= 0 ? &doc.masters[info.index] : nullptr;
        slide.layoutId = info.layoutId;
        slide.showMasterObjects = flags & 0x1;
        transition(r, slide, number);

        QColor scheme[8];
        for (int i = 0; i < 8; ++i) scheme[i] = info.scheme[i];
        if (!(flags & 0x2)) schemeOf(r, scheme);

        Ctx ctx;
        ctx.slide = number;
        ctx.scheme = scheme;
        ctx.styles = &info.styles;
        ctx.outline = &entry.texts;
        ctx.objects = &slide.objects;

        QVector<SceneObject> shapes;
        ctx.objects = &shapes;
        bool hasBackground = false;
        const Props back = drawing(r, ctx, &hasBackground);
        bool ownBackground = false;
        slide.background = master ? master->background : QColor(Qt::white);
        if (!(flags & 0x4) && hasBackground) {
            const Fill fill = fillOf(back, ctx);
            if (fill.style == 0) { slide.background = fill.color; slide.backgroundOverride = true; }
            else if (fill.style != 5) {
                slide.backgroundOverride = true;
                slide.objects.append(backgroundBox(fill, number));
                ownBackground = true;
            }
        }
        // The master's shapes draw over the slide's background in OmaShow, so a
        // slide with a background of its own over a master whose background is
        // a shape brings the master's other shapes onto itself.
        if (master && slide.showMasterObjects && (ownBackground || (slide.backgroundOverride && info.backgroundObject))) {
            for (int i = info.backgroundObject ? 1 : 0; i < master->objects.size(); ++i) {
                SceneObject copy = master->objects[i];
                copy.id = Edit::newId(QStringLiteral("shape"));
                copy.locked = true;
                slide.objects.append(copy);
            }
            slide.showMasterObjects = false;
        }
        slide.objects += shapes;

        // Number, date and footer, as the slide asks for them.
        const Rec hf = first(stream, r, kHeadersFooters);
        const Rec hfAtom = hf.valid ? first(stream, hf, kHeadersFootersAtom) : Rec();
        if (hfAtom.valid && master) {
            const quint16 shown = u16(stream, hfAtom.at + 2);
            const bool date = shown & 0x1, num = shown & 0x8, footer = shown & 0x20;
            slide.showMasterFields = date || num || footer;
            auto &f = master->fields;
            if (num) f.showNumber = true;
            if (footer) { f.showFooter = true; if (f.footer.isEmpty()) f.footer = footerText; }
            if (date) { f.showDate = true; if ((shown & 0x4) && f.date.isEmpty()) f.date = dateText; }
        } else slide.showMasterFields = false;

        // Words for the speaker.
        if (notesById.contains(notesId)) {
            const SlideEntry &n = notesById[notesId];
            const Rec nr = persistRecord(n.persistId);
            if (nr.valid && nr.type == kNotes) {
                Ctx nctx;
                nctx.slide = number;
                nctx.scheme = scheme;
                nctx.outline = &n.texts;
                QVector<SceneObject> none;
                nctx.objects = &none;
                nctx.wantNotes = true;
                bool unused = false;
                drawing(nr, nctx, &unused);
                slide.notes = nctx.notes;
            }
        }

        // Comments, and whether it has builds.
        QVector<Rec> comments;
        collect(stream, r, kComment10, comments);
        for (const auto &c : comments) {
            Comment comment;
            comment.id = Edit::newId(QStringLiteral("comment"));
            comment.slideId = slide.id;
            comment.author = cstring(stream, first(stream, c, kCString, 0)).trimmed();
            if (comment.author.isEmpty()) comment.author = QStringLiteral("Unknown");
            comment.text = cstring(stream, first(stream, c, kCString, 1));
            comment.text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            const Rec when = first(stream, c, kComment10Atom);
            if (when.valid && when.len >= 20) {
                const QDate day(u16(stream, when.at + 4), u16(stream, when.at + 6), u16(stream, when.at + 10));
                const QTime time(u16(stream, when.at + 12), u16(stream, when.at + 14), u16(stream, when.at + 16));
                if (day.isValid()) comment.created = QDateTime(day, time.isValid() ? time : QTime(0, 0)).toString(Qt::ISODate);
            }
            doc.comments.append(comment);
        }
        QVector<Rec> timing;
        collect(stream, r, kExtTimeNode, timing);
        if (!timing.isEmpty()) animations = true;
        doc.slides.append(slide);
    }

    // ---- the whole file

    bool run(const QByteArray &raw, QString *error) {
        Cfb::Reader file(raw);
        if (!file.isValid()) { *error = QStringLiteral("this is not a PowerPoint 97–2003 deck: %1.").arg(file.error()); return false; }
        if (!file.contains(QStringLiteral("PowerPoint Document"))) {
            if (file.contains(QStringLiteral("PP40")) || file.contains(QStringLiteral("PP97_DUALSTORAGE")))
                *error = QStringLiteral("this deck is from PowerPoint 95 or older, which OmaShow cannot open.");
            else
                *error = QStringLiteral("this is not a PowerPoint deck: it has no presentation inside.");
            return false;
        }
        if (file.contains(QStringLiteral("EncryptedSummary"))) {
            *error = QStringLiteral("this presentation is protected by a password, which OmaShow cannot open.");
            return false;
        }
        stream = file.read(QStringLiteral("PowerPoint Document"));
        pictures = file.read(QStringLiteral("Pictures"));

        // The latest edit, then every older one, newest first.
        const QByteArray user = file.read(QStringLiteral("Current User"));
        qint64 edit = -1;
        const Rec current = recordAt(user, 0, user.size());
        if (current.valid && current.type == kCurrentUserAtom) {
            if (u32(user, current.at + 4) == 0xF3D1C4DF) {
                *error = QStringLiteral("this presentation is protected by a password, which OmaShow cannot open.");
                return false;
            }
            edit = u32(user, current.at + 8);
        }
        if (edit < 0 || !recordAt(stream, edit, stream.size()).valid || recordAt(stream, edit, stream.size()).type != kUserEditAtom) {
            // No pointer to it: the last edit record in the stream.
            edit = -1;
            for (qint64 at = 0; at + 8 <= stream.size();) {
                const Rec r = recordAt(stream, at, stream.size());
                if (!r.valid) break;
                if (r.type == kUserEditAtom) edit = at;
                at = r.end();
            }
        }
        if (edit < 0) { *error = QStringLiteral("this PowerPoint deck is damaged: it has no record of its last save."); return false; }
        QVector<qint64> edits;
        QSet<qint64> seen;
        quint32 documentRef = 0;
        for (qint64 at = edit; at > 0 && !seen.contains(at) && edits.size() < 10000;) {
            const Rec r = recordAt(stream, at, stream.size());
            if (!r.valid || r.type != kUserEditAtom || r.len < 28) break;
            seen.insert(at);
            edits.append(at);
            if (edits.size() == 1) {
                documentRef = u32(stream, r.at + 16);
                if (r.len >= 32 && u32(stream, r.at + 28) != 0) {
                    *error = QStringLiteral("this presentation is protected by a password, which OmaShow cannot open.");
                    return false;
                }
            }
            at = u32(stream, r.at + 8);
            if (at == 0) break;
        }
        for (int i = edits.size() - 1; i >= 0; --i) {
            const Rec ue = recordAt(stream, edits[i], stream.size());
            const Rec dir = recordAt(stream, u32(stream, ue.at + 12), stream.size());
            if (!dir.valid || dir.type != kPersistDirectoryAtom) continue;
            for (qint64 p = dir.at; p + 4 <= dir.end();) {
                const quint32 head = u32(stream, p);
                const quint32 id = head & 0xFFFFF, count = head >> 20;
                p += 4;
                for (quint32 j = 0; j < count && p + 4 <= dir.end(); ++j, p += 4) persist.insert(id + j, u32(stream, p));
            }
        }
        const Rec document = persistRecord(documentRef);
        if (!document.valid || document.type != kDocument) { *error = QStringLiteral("this PowerPoint deck is damaged: its contents could not be found."); return false; }

        // Size.
        const Rec da = first(stream, document, kDocumentAtom);
        qreal w = 5760, h = 4320;
        if (da.valid) { w = s32(stream, da.at); h = s32(stream, da.at + 4); }
        if (w <= 0 || h <= 0) { w = 5760; h = 4320; }
        const qreal ratio = qBound(0.125, h / w, 8.0);
        doc.size = QSizeF(1920, qRound(1920 * ratio));
        k = 1920 / w;
        upt = 8 * k;

        // Fonts, default text, links, films.
        const Rec env = first(stream, document, kEnvironment);
        if (env.valid) {
            const Rec fc = first(stream, env, kFontCollection);
            if (fc.valid)
                for (const auto &f : children(stream, fc))
                    if (f.type == kFontEntityAtom) {
                        while (fonts.size() < f.instance) fonts.append(QString());
                        const QString name = utf16(stream, f.at, qMin<qint64>(64, f.len));
                        if (fonts.size() == f.instance) fonts.append(name); else fonts[f.instance] = name;
                    }
            for (const auto &c : children(stream, env)) {
                if (c.type == kTxMasterStyleAtom) readMasterStyle(stream, c, documentStyles);
                else if (c.type == kTextCFExceptionAtom) { qint64 p = c.at; readCf(stream, p, c.end(), defaultCf); }
                else if (c.type == kTextPFExceptionAtom) { qint64 p = c.at + 2; readPf(stream, p, c.end(), defaultPf); }
            }
        }
        if (!defaultCf.v.contains(CfSize)) defaultCf.v.insert(CfSize, 18);
        const Rec objects = first(stream, document, kExObjList);
        if (objects.valid)
            for (const auto &o : children(stream, objects)) {
                if (o.type == kExHyperlink) {
                    const Rec idAtom = first(stream, o, kExHyperlinkAtom);
                    if (!idAtom.valid) continue;
                    HyperlinkTarget t;
                    t.target = cstring(stream, first(stream, o, kCString, 1));
                    t.location = cstring(stream, first(stream, o, kCString, 3));
                    hyperlinks.insert(u32(stream, idAtom.at), t);
                } else if (o.type == kExAviMovie || o.type == kExMCIMovie || o.type == kExWAVAudioEmbedded ||
                           o.type == kExWAVAudioLink || o.type == kExMIDIAudio || o.type == kExCDAudio) {
                    // Their first atom starts with the object's id.
                    QVector<Rec> atoms;
                    collect(stream, o, 0x0FC1, atoms);   // ExObjRefAtom-like ExMediaAtom
                    for (const auto &a : atoms) mediaKinds.insert(u32(stream, a.at), o.type);
                }
            }
        // Footer and fixed date text for the slides.
        for (const auto &c : children(stream, document))
            if (c.type == kHeadersFooters && c.instance == 3) {
                footerText = cstring(stream, first(stream, c, kCString, 2));
                dateText = cstring(stream, first(stream, c, kCString, 0));
            }

        // Pictures and the deck's default shape look.
        const Rec dgg0 = first(stream, document, kPPDrawingGroup);
        const Rec dgg = dgg0.valid ? first(stream, dgg0, kDggContainer) : Rec();
        if (dgg.valid) {
            for (const auto &c : children(stream, dgg)) {
                if (c.type == kFOPT) readProps(stream, c, drawingDefaults);
                if (c.type != kBStoreContainer) continue;
                for (const auto &e : children(stream, c)) {
                    if (e.type != kFBSE) continue;
                    Blip blip;
                    blip.offset = u32(stream, e.at + 28);
                    const int nameBytes = u8(stream, e.at + 33);
                    const qint64 inside = e.at + 36 + nameBytes;
                    if (e.len > 36 + nameBytes + 8) blip.embedded = stream.mid(inside, e.end() - inside);
                    if (u32(stream, e.at + 24) == 0 && blip.embedded.isEmpty()) blip.offset = -1;
                    blips.append(blip);
                }
            }
        }

        // The lists of masters, slides and notes, with their outline words.
        QVector<SlideEntry> masterList, slideList, notesList;
        for (const auto &c : children(stream, document)) {
            if (c.type != kSlideListWithText) continue;
            QVector<SlideEntry> *into = c.instance == 0 ? &slideList : c.instance == 1 ? &masterList : c.instance == 2 ? &notesList : nullptr;
            if (!into) continue;
            const auto recs = children(stream, c);
            for (int i = 0; i < recs.size();) {
                if (recs[i].type == kSlidePersistAtom) {
                    SlideEntry e;
                    e.persistId = u32(stream, recs[i].at);
                    e.slideId = u32(stream, recs[i].at + 12);
                    into->append(e);
                    ++i;
                } else if (recs[i].type == kTextHeaderAtom && !into->isEmpty()) {
                    into->last().texts.append(textGroup(recs, i));
                } else ++i;
            }
        }
        int number = 0;
        for (const auto &e : slideList) {
            slideIdOf.insert(e.slideId, Edit::newId(QStringLiteral("slide")));
            slideNumberOf.insert(e.slideId, ++number);
        }

        for (const auto &e : masterList) {
            const Rec r = persistRecord(e.persistId);
            if (r.valid && r.type == kMainMaster) readMaster(e);
        }
        for (const auto &e : masterList) {
            const Rec r = persistRecord(e.persistId);
            if (r.valid && r.type == kSlide) readMaster(e);
        }
        if (doc.masters.isEmpty()) {
            Master plain;
            plain.id = Edit::newId(QStringLiteral("master"));
            plain.name = QStringLiteral("Default");
            plain.background = Qt::white;
            plain.backgroundToken.clear();
            SlideLayout layout;
            layout.id = Edit::newId(QStringLiteral("layout"));
            layout.masterId = plain.id;
            layout.name = plain.name;
            doc.masters.append(plain);
            doc.layouts.append(layout);
            MasterInfo info;
            info.index = 0;
            info.layoutId = layout.id;
            masters.insert(0, info);
        }

        QHash<quint32, SlideEntry> notesById;
        for (const auto &e : notesList) notesById.insert(e.slideId, e);
        doc.transition = 0;
        doc.transitionDuration = 0.5;
        number = 0;
        for (const auto &e : slideList) readSlide(e, ++number, notesById);
        if (doc.slides.isEmpty()) {
            // A template with no slides yet: one empty slide on its first master.
            Slide slide;
            slide.id = Edit::newId(QStringLiteral("slide"));
            slide.layoutId = doc.layouts.first().id;
            slide.background = doc.masters.first().background;
            doc.slides.append(slide);
        }

        // Custom shows.
        const Rec shows = first(stream, document, kNamedShows);
        if (shows.valid)
            for (const auto &s : children(stream, shows)) {
                if (s.type != kNamedShow) continue;
                CustomShow show;
                show.id = Edit::newId(QStringLiteral("show"));
                show.name = cstring(stream, first(stream, s, kCString, 0));
                const Rec list = first(stream, s, kNamedShowSlidesAtom);
                for (qint64 p = list.at; list.valid && p + 4 <= list.end(); p += 4) {
                    const QString id = slideIdOf.value(u32(stream, p));
                    if (!id.isEmpty()) show.slideIds.append(id);
                }
                if (!show.slideIds.isEmpty()) doc.shows.append(show);
            }

        // The deck's palette and type: from the first master.
        if (!doc.masters.isEmpty()) {
            doc.theme.name = QStringLiteral("Imported");
            doc.theme.colors[QStringLiteral("background")] = doc.masters.first().background;
            if (!doc.layouts.isEmpty())
                for (const auto &p : doc.layouts.first().placeholders) {
                    if (p.id == QLatin1String("title")) { doc.theme.colors[QStringLiteral("foreground")] = p.textColor; doc.theme.fonts[QStringLiteral("heading")] = p.fontFamily; }
                    if (p.id == QLatin1String("body")) doc.theme.fonts[QStringLiteral("body")] = p.fontFamily;
                }
        }
        if (animations) warnings.add(QStringLiteral("Builds (animations) were left out for now"), 0);
        return true;
    }
};

} // namespace

namespace Ppt {

Result read(const QByteArray &raw) {
    Result result;
    Reader reader;
    QString error;
    if (!reader.run(raw, &error)) { result.error = error; return result; }
    result.document = reader.doc;
    result.warnings = reader.warnings.lines();
    result.ok = true;
    return result;
}

Result load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        Result result;
        result.error = QStringLiteral("%1 could not be read.").arg(QFileInfo(path).fileName());
        return result;
    }
    return read(file.readAll());
}

} // namespace Ppt
