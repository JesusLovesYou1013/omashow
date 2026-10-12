#include "io/pptxwriter.h"

#include "anim/presentation.h"
#include "core/chart.h"
#include "core/design.h"
#include "core/fonts.h"
#include "core/mediaasset.h"
#include "core/shape.h"
#include "core/table.h"
#include "core/textruns.h"
#include "io/zip.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QPainter>
#include <QSaveFile>
#include <QUrl>
#include <QSet>
#include <QRegularExpression>
#include <QUuid>
#include <QXmlStreamWriter>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

const QString nsA = QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/main");
const QString nsP = QStringLiteral("http://schemas.openxmlformats.org/presentationml/2006/main");
const QString nsR = QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships");
const QString nsC = QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/chart");
const QString nsP14 = QStringLiteral("http://schemas.microsoft.com/office/powerpoint/2010/main");
const QString relBase = QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/");
const QString relMedia = QStringLiteral("http://schemas.microsoft.com/office/2007/relationships/media");

// ---------------------------------------------------------------------------
// A thin hand on QXmlStreamWriter: prefixed names written as plain names, so
// the markup reads like the specification does.

class Xml {
public:
    Xml() : m_writer(&m_bytes) { m_writer.writeStartDocument(QStringLiteral("1.0"), true); }
    Xml &start(const char *tag) { m_writer.writeStartElement(QString::fromLatin1(tag)); return *this; }
    Xml &attr(const char *name, const QString &value) { m_writer.writeAttribute(QString::fromLatin1(name), value); return *this; }
    Xml &attr(const char *name, const char *value) { return attr(name, QString::fromLatin1(value)); }
    Xml &attr(const char *name, qint64 value) { return attr(name, QString::number(value)); }
    Xml &attr(const char *name, int value) { return attr(name, QString::number(value)); }
    Xml &text(const QString &words) { m_writer.writeCharacters(words); return *this; }
    Xml &end() { m_writer.writeEndElement(); return *this; }
    Xml &empty(const char *tag) { m_writer.writeEmptyElement(QString::fromLatin1(tag)); return *this; }
    // ANIM-INTEROP: a fragment kept from PowerPoint, written as it is.
    Xml &raw(const QString &xml) { m_writer.writeCharacters(QString()); m_writer.device()->write(xml.toUtf8()); return *this; }
    QByteArray finish() { m_writer.writeEndDocument(); return m_bytes; }
private:
    QByteArray m_bytes;
    QXmlStreamWriter m_writer;
};

// A relationships part being assembled beside its owner.
struct Rels {
    struct Rel { QString id, type, target; bool external = false; };
    QVector<Rel> rels;
    QString add(const QString &type, const QString &target, bool external = false) {
        for (const auto &r : rels) if (r.type == type && r.target == target && r.external == external) return r.id;
        Rel rel;
        rel.id = QStringLiteral("rId%1").arg(rels.size() + 1);
        rel.type = type; rel.target = target; rel.external = external;
        rels.append(rel);
        return rel.id;
    }
    QByteArray bytes() const {
        Xml x;
        x.start("Relationships").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
        for (const auto &r : rels) {
            x.start("Relationship").attr("Id", r.id).attr("Type", r.type).attr("Target", r.target);
            if (r.external) x.attr("TargetMode", "External");
            x.end();
        }
        x.end();
        return x.finish();
    }
};

QString hex(const QColor &c) {
    return QStringLiteral("%1%2%3").arg(c.red(), 2, 16, QLatin1Char('0')).arg(c.green(), 2, 16, QLatin1Char('0')).arg(c.blue(), 2, 16, QLatin1Char('0')).toUpper();
}

// <a:srgbClr val="…"><a:alpha val="…"/></a:srgbClr>
void writeColor(Xml &x, const QColor &c, qreal opacity = 1.0) {
    x.start("a:srgbClr").attr("val", hex(c));
    const qreal alpha = c.alphaF() * opacity;
    if (alpha < 0.999) x.start("a:alpha").attr("val", qRound(alpha * 100000)).end();
    x.end();
}

void writeSolidFill(Xml &x, const QColor &c, qreal opacity = 1.0) {
    x.start("a:solidFill");
    writeColor(x, c, opacity);
    x.end();
}

// ---------------------------------------------------------------------------

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

const char *presetFor(int kind) {
    static const char *names[] = {"rect", "ellipse", "triangle", "diamond", "pentagon", "hexagon", "star5", "heart",
                                  "rightArrow", "chevron", "wedgeRectCallout", "parallelogram", "can", "line", "mathPlus",
                                  "mathMultiply", "leftArrow", "leftRightArrow", "flowChartDocument", "trapezoid",
                                  "flowChartTerminator", "cloud", "arc", "donut"};
    return kind >= 0 && kind < 24 ? names[kind] : nullptr;
}

const char *dashFor(int style) {
    switch (style) {
    case 1: return "dash";
    case 2: return "sysDot";
    case 3: return "dashDot";
    case 4: return "lgDashDotDot";
    default: return "solid";
    }
}

const char *patternFor(int style) {
    // Qt::Dense1Pattern … Qt::DiagCrossPattern, in the order the model keeps.
    switch (style) {
    case 2: return "pct90"; case 3: return "pct75"; case 4: return "pct60"; case 5: return "pct50";
    case 6: return "pct30"; case 7: return "pct20"; case 8: return "pct10";
    case 9: return "horz"; case 10: return "vert"; case 11: return "smGrid";
    case 12: return "wdUpDiag"; case 13: return "wdDnDiag"; case 14: return "diagCross";
    default: return "pct50";
    }
}

// 0 → A, 25 → Z, 26 → AA: how a spreadsheet names its columns.
QString columnLetters(int index) {
    QString out;
    do { out.prepend(QChar('A' + index % 26)); index = index / 26 - 1; } while (index >= 0);
    return out;
}

struct Writer {
    const Document &doc;
    Warnings warnings;
    QVector<Zip::Entry> entries;
    std::shared_ptr<Workers::Job> job;

    qreal e = 1;      // EMU per slide unit
    qint64 cx = 12192000, cy = 6858000;

    QStringList overrides;   // content-type overrides, one line each
    QSet<QString> defaults;  // extensions with a default content type

    // Media are shared: one part per picture identity.
    QHash<QString, QString> mediaByIdentity;
    int mediaCount = 0, chartCount = 0, commentCount = 0;

    explicit Writer(const Document &document) : doc(document) {
        e = 12192000.0 / qMax(1.0, doc.size.width());
        cx = 12192000;
        cy = qRound64(doc.size.height() * e);
    }

    bool canceled() const { return job && job->canceled; }
    qint64 emu(qreal units) const { return qRound64(units * e); }
    int hundredthsPt(qreal units) const { return qMax(100, qRound(units * e / 12700.0 * 100)); }

    void part(const QString &path, const QByteArray &data, const char *contentType) {
        entries.append({path, data, true});
        if (contentType) overrides.append(QStringLiteral("<Override PartName=\"/%1\" ContentType=\"%2\"/>").arg(path, QString::fromLatin1(contentType)));
    }
    void binary(const QString &path, const QByteArray &data, const QString &extension, const QString &mime) {
        entries.append({path, data, !(extension == QLatin1String("png") || extension == QLatin1String("jpeg") || extension == QLatin1String("mp4"))});
        if (!defaults.contains(extension)) {
            defaults.insert(extension);
            overrides.append(QStringLiteral("<Default Extension=\"%1\" ContentType=\"%2\"/>").arg(extension, mime));
        }
    }
    static QString relsPath(const QString &partPath) {
        const int slash = partPath.lastIndexOf(QLatin1Char('/'));
        return partPath.left(slash + 1) + QStringLiteral("_rels/") + partPath.mid(slash + 1) + QStringLiteral(".rels");
    }

    // ---- theme

    QByteArray themeXml(const QString &name) const {
        Xml x;
        x.start("a:theme").attr("xmlns:a", nsA).attr("name", name);
        x.start("a:themeElements");
        x.start("a:clrScheme").attr("name", name);
        const auto c = [&](const char *tag, const QColor &color) { x.start(tag); x.start("a:srgbClr").attr("val", hex(color)).end(); x.end(); };
        const auto fg = doc.theme.colors.value(QStringLiteral("foreground"), QColor(Qt::white));
        const auto bg = doc.theme.colors.value(QStringLiteral("background"), QColor(Qt::black));
        const auto muted = doc.theme.colors.value(QStringLiteral("muted"), fg);
        const auto accent = doc.theme.colors.value(QStringLiteral("accent"), QColor(0x27, 0xc2, 0xff));
        c("a:dk1", fg); c("a:lt1", bg); c("a:dk2", muted); c("a:lt2", bg.lighter(110));
        const auto palette = Chart::palette(doc.theme, 0);
        for (int i = 0; i < 6; ++i) c(QByteArray("a:accent" + QByteArray::number(i + 1)).constData(), i < palette.size() ? palette[i] : accent);
        c("a:hlink", accent); c("a:folHlink", muted);
        x.end();
        x.start("a:fontScheme").attr("name", name);
        for (const auto *tag : {"a:majorFont", "a:minorFont"}) {
            x.start(tag);
            x.start("a:latin").attr("typeface", doc.theme.fonts.value(QString::fromLatin1(tag) == QLatin1String("a:majorFont") ? QStringLiteral("heading") : QStringLiteral("body"), QStringLiteral("Inter"))).end();
            x.start("a:ea").attr("typeface", "").end();
            x.start("a:cs").attr("typeface", "").end();
            x.end();
        }
        x.end();
        // The format scheme every theme must carry, in its plainest form.
        x.start("a:fmtScheme").attr("name", name);
        x.start("a:fillStyleLst");
        for (int i = 0; i < 3; ++i) { x.start("a:solidFill"); x.start("a:schemeClr").attr("val", "phClr").end(); x.end(); }
        x.end();
        x.start("a:lnStyleLst");
        for (int w : {6350, 12700, 19050}) {
            x.start("a:ln").attr("w", w).attr("cap", "flat").attr("cmpd", "sng").attr("algn", "ctr");
            x.start("a:solidFill"); x.start("a:schemeClr").attr("val", "phClr").end(); x.end();
            x.start("a:prstDash").attr("val", "solid").end();
            x.end();
        }
        x.end();
        x.start("a:effectStyleLst");
        for (int i = 0; i < 3; ++i) { x.start("a:effectStyle"); x.empty("a:effectLst"); x.end(); }
        x.end();
        x.start("a:bgFillStyleLst");
        for (int i = 0; i < 3; ++i) { x.start("a:solidFill"); x.start("a:schemeClr").attr("val", "phClr").end(); x.end(); }
        x.end();
        x.end(); // fmtScheme
        x.end(); // themeElements
        x.end();
        return x.finish();
    }

    // ---- shapes

    struct SlideCtx {
        QString partPath;
        Rels *rels = nullptr;
        int number = 0;               // 1-based slide number, 0 for masters and layouts
        int nextId = 2;
        QHash<QString, int> spidByObject;
    };

    void writeXfrm(Xml &x, const QRectF &rect, qreal rotation, const char *tag = "a:xfrm") const {
        x.start(tag);
        if (!qFuzzyIsNull(rotation)) x.attr("rot", qRound64(std::fmod(rotation, 360.0) * 60000));
        x.start("a:off").attr("x", emu(rect.x())).attr("y", emu(rect.y())).end();
        x.start("a:ext").attr("cx", qMax<qint64>(0, emu(rect.width()))).attr("cy", qMax<qint64>(0, emu(rect.height()))).end();
        x.end();
    }

    void writeCustomGeometry(Xml &x, const SceneObject &o) const {
        const auto path = Shape::path(o);
        const QRectF r = o.rect;
        const qreal w = qMax(1.0, r.width()), h = qMax(1.0, r.height());
        x.start("a:custGeom");
        x.empty("a:avLst"); x.empty("a:gdLst"); x.empty("a:ahLst"); x.empty("a:cxnLst");
        x.start("a:rect").attr("l", "0").attr("t", "0").attr("r", "r").attr("b", "b").end();
        x.start("a:pathLst");
        x.start("a:path").attr("w", emu(w)).attr("h", emu(h));
        if (o.fillStyle == 5) x.attr("fill", "none");
        const auto pt = [&](qreal px, qreal py) {
            x.start("a:pt").attr("x", emu(px - r.x())).attr("y", emu(py - r.y())).end();
        };
        for (int i = 0; i < path.elementCount(); ++i) {
            const auto el = path.elementAt(i);
            if (el.isMoveTo()) {
                x.start("a:moveTo"); pt(el.x, el.y); x.end();
            } else if (el.isLineTo()) {
                x.start("a:lnTo"); pt(el.x, el.y); x.end();
            } else if (el.isCurveTo() && i + 2 < path.elementCount()) {
                const auto b = path.elementAt(i + 1), c = path.elementAt(i + 2);
                x.start("a:cubicBezTo"); pt(el.x, el.y); pt(b.x, b.y); pt(c.x, c.y); x.end();
                i += 2;
            }
        }
        if (o.fillStyle != 5 && !o.connector && path.elementCount() > 1) x.empty("a:close");
        x.end(); // path
        x.end(); // pathLst
        x.end(); // custGeom
    }

    void writeFill(Xml &x, const SceneObject &o, int slide) {
        switch (o.fillStyle) {
        case 0: writeSolidFill(x, o.fill, o.opacity); break;
        case 1: case 2: {
            x.start("a:gradFill").attr("rotWithShape", "1");
            x.start("a:gsLst");
            x.start("a:gs").attr("pos", 0); writeColor(x, o.fill, o.opacity); x.end();
            x.start("a:gs").attr("pos", 100000); writeColor(x, o.fillSecondary, o.opacity); x.end();
            x.end();
            if (o.fillStyle == 1) x.start("a:lin").attr("ang", qRound64(std::fmod(std::fmod(o.fillAngle, 360.0) + 360.0, 360.0) * 60000)).attr("scaled", "0").end();
            else { x.start("a:path").attr("path", "circle"); x.start("a:fillToRect").attr("l", 50000).attr("t", 50000).attr("r", 50000).attr("b", 50000).end(); x.end(); }
            x.end();
            break;
        }
        case 3:
            x.start("a:pattFill").attr("prst", patternFor(o.patternStyle));
            x.start("a:fgClr"); writeColor(x, o.fill, o.opacity); x.end();
            x.start("a:bgClr"); writeColor(x, o.fillSecondary, o.opacity); x.end();
            x.end();
            break;
        case 4:
            warnings.add(QStringLiteral("A shape filled with a picture was given a plain fill"), slide);
            writeSolidFill(x, o.fill, o.opacity);
            break;
        default: x.empty("a:noFill"); break;
        }
    }

    void writeLine(Xml &x, const SceneObject &o) const {
        if (o.strokeWidth <= 0) { x.start("a:ln"); x.empty("a:noFill"); x.end(); return; }
        x.start("a:ln").attr("w", qMax<qint64>(1, emu(o.strokeWidth)));
        x.attr("cap", o.strokeCap == 0 ? "flat" : o.strokeCap == 2 ? "sq" : "rnd");
        writeSolidFill(x, o.strokeColor, o.opacity);
        x.start("a:prstDash").attr("val", dashFor(o.strokeStyle)).end();
        if (o.strokeJoin == 0) x.empty("a:miter"); else if (o.strokeJoin == 2) x.empty("a:bevel"); else x.empty("a:round");
        if (o.connector) {
            if (o.connectorArrowStart) x.start("a:headEnd").attr("type", "triangle").end();
            if (o.connectorArrowEnd) x.start("a:tailEnd").attr("type", "triangle").end();
        }
        x.end();
    }

    void writeEffects(Xml &x, const SceneObject &o) const {
        if (!o.shadowEnabled) return;
        x.start("a:effectLst");
        x.start("a:outerShdw").attr("blurRad", 38100).attr("dist", emu(std::hypot(o.shadowX, o.shadowY)))
            .attr("dir", qRound64(std::fmod(qRadiansToDegrees(std::atan2(o.shadowY, o.shadowX)) + 360.0, 360.0) * 60000))
            .attr("algn", "tl").attr("rotWithShape", "0");
        writeColor(x, o.shadowColor);
        x.end();
        x.end();
    }

    void writeNonVisual(Xml &x, const char *tag, const char *cNvTag, const SceneObject &o, SlideCtx &ctx, const QString &fallbackName, bool textBox = false) {
        const int id = ctx.nextId++;
        ctx.spidByObject.insert(o.id, id);
        x.start(tag);
        x.start("p:cNvPr").attr("id", id).attr("name", o.altTitle.isEmpty() ? fallbackName : o.altTitle);
        if (!o.altText.isEmpty()) x.attr("descr", o.altText);
        if (o.hidden) x.attr("hidden", "1");
        x.end();
        x.start(cNvTag);
        if (textBox) x.attr("txBox", "1");
        x.end();
        x.empty("p:nvPr");
        x.end();
    }

    // ---- text

    void writeRunProperties(Xml &x, const char *tag, const SceneObject &o, const TextRun *run, const QString &linkRel) const {
        QString lang = run && !run->language.isEmpty() ? run->language : !o.language.isEmpty() ? o.language : !doc.language.isEmpty() ? doc.language : QStringLiteral("en_GB");
        lang.replace(QLatin1Char('_'), QLatin1Char('-'));
        x.start(tag).attr("lang", lang);
        const qreal size = run && run->fontSize > 0 ? run->fontSize : o.fontSize;
        x.attr("sz", hundredthsPt(size));
        const bool bold = run && run->weight ? run->weight >= 600 : o.fontWeight >= 600;
        const bool italic = run && run->italic ? run->italic == 1 : o.italic;
        const bool underline = run && run->underline ? run->underline == 1 : o.underline;
        x.attr("b", bold ? "1" : "0").attr("i", italic ? "1" : "0").attr("u", underline ? "sng" : "none");
        const bool strike = run && run->strike ? run->strike == 1 : o.strike;
        if (strike) x.attr("strike", "sngStrike");
        else if (run && run->strike == 2) x.attr("strike", "noStrike");
        if (run && run->baseline) x.attr("baseline", run->baseline == 1 ? 30000 : -25000);
        if (o.uppercase) x.attr("cap", "all");
        if (!qFuzzyIsNull(o.letterSpacing)) x.attr("spc", qRound(o.letterSpacing * e / 12700.0 * 100));
        const QColor color = run && run->color.isValid() ? run->color : o.textColor;
        writeSolidFill(x, color, o.opacity);
        const QString family = run && !run->fontFamily.isEmpty() ? run->fontFamily : o.fontFamily;
        x.start("a:latin").attr("typeface", family).end();
        if (!linkRel.isEmpty()) {
            x.start("a:hlinkClick").attr("r:id", linkRel);
            if (o.linkKind == 3) x.attr("action", "ppaction://hlinksldjump");
            x.end();
        }
        x.end();
    }

    // The words of a text object, or of a table cell, as a txBody.
    void writeTextBody(Xml &x, const char *tag, const SceneObject &o, SlideCtx &ctx, bool inTable) {
        x.start(tag);
        x.start("a:bodyPr").attr("wrap", "square").attr("lIns", inTable ? 45720 : 0).attr("tIns", inTable ? 45720 : 0).attr("rIns", inTable ? 45720 : 0).attr("bIns", inTable ? 45720 : 0)
            .attr("rtlCol", o.direction == 2 ? "1" : "0")
            .attr("anchor", o.verticalAlign == 1 ? "ctr" : o.verticalAlign == 2 ? "b" : "t");
        if (o.columns > 1) x.attr("numCol", o.columns);
        if (o.textFit == 1) x.empty("a:normAutofit"); else x.empty("a:noAutofit");
        x.end();
        x.empty("a:lstStyle");

        QString linkRel;
        if (!inTable && o.linkKind >= 1 && o.linkKind <= 3 && !o.linkTarget.isEmpty()) {
            if (o.linkKind == 3) {
                int index = -1;
                for (int i = 0; i < doc.slides.size(); ++i) if (doc.slides[i].id == o.linkTarget) index = i;
                if (index >= 0) linkRel = ctx.rels->add(relBase + QStringLiteral("slide"), QStringLiteral("../slides/slide%1.xml").arg(index + 1));
            } else {
                const auto target = o.linkKind == 2 ? QStringLiteral("mailto:") + o.linkTarget : o.linkTarget;
                linkRel = ctx.rels->add(relBase + QStringLiteral("hyperlink"), target, true);
            }
        }
        if (o.textKind == 1) warnings.add(QStringLiteral("An equation was written as its source text"), ctx.number);
        if (o.textKind == 2) warnings.add(QStringLiteral("Words on a path were written as an ordinary text box"), ctx.number);

        const auto runs = TextRuns::tidy(o.runs, o.text.size());
        const auto paragraphs = o.text.split(QLatin1Char('\n'));
        int position = 0;
        int number = o.listStart;
        for (const auto &paragraph : paragraphs) {
            int level = 0;
            QString words = paragraph;
            if (o.listStyle > 0) { while (words.startsWith(QLatin1Char('\t'))) { words.remove(0, 1); ++level; } }
            const int skipped = paragraph.size() - words.size();
            x.start("a:p");
            x.start("a:pPr");
            if (o.listStyle > 0) {
                x.attr("marL", qint64(342900) * (level + 1)).attr("indent", -342900).attr("lvl", qMin(8, level));
            }
            x.attr("algn", o.textAlign == 1 ? "ctr" : o.textAlign == 2 ? "r" : o.textAlign == 3 ? "just" : "l");
            if (!qFuzzyCompare(o.lineHeight, 100.0) && o.lineHeight > 0) { x.start("a:lnSpc"); x.start("a:spcPct").attr("val", qRound(o.lineHeight * 1000)).end(); x.end(); }
            if (o.paragraphSpacing > 0) { x.start("a:spcBef"); x.start("a:spcPts").attr("val", qRound(o.paragraphSpacing * e / 12700.0 * 100)).end(); x.end(); }
            switch (o.listStyle) {
            case 0: x.empty("a:buNone"); break;
            case 1: x.start("a:buChar").attr("char", level == 0 ? QString(QChar(0x2022)) : level == 1 ? QString(QChar(0x25E6)) : QString(QChar(0x25AA))).end(); break;
            case 2: x.start("a:buAutoNum").attr("type", "arabicPeriod").attr("startAt", number).end(); break;
            case 3: x.start("a:buChar").attr("char", QString(QChar(0x25E6))).end(); break;
            case 4: x.start("a:buChar").attr("char", QString(QChar(0x25AA))).end(); break;
            case 5: x.start("a:buAutoNum").attr("type", "alphaLcPeriod").attr("startAt", number).end(); break;
            case 6: x.start("a:buAutoNum").attr("type", "alphaUcPeriod").attr("startAt", number).end(); break;
            case 7: x.start("a:buAutoNum").attr("type", "romanLcPeriod").attr("startAt", number).end(); break;
            case 8: x.start("a:buAutoNum").attr("type", "romanUcPeriod").attr("startAt", number).end(); break;
            default: x.empty("a:buNone"); break;
            }
            x.end(); // pPr
            // Runs: split the paragraph where the formatting changes.
            const int start = position + skipped, end = position + paragraph.size();
            int at = start;
            while (at < end) {
                const TextRun *run = TextRuns::at(runs, at);
                int stop = end;
                if (run) stop = qMin(end, run->start + run->length);
                for (const auto &r : runs) if (r.start > at && r.start < stop) stop = r.start;
                x.start("a:r");
                writeRunProperties(x, "a:rPr", o, run, linkRel);
                x.start("a:t").text(o.text.mid(at, stop - at)).end();
                x.end();
                at = stop;
            }
            writeRunProperties(x, "a:endParaRPr", o, nullptr, QString());
            x.end(); // p
            position += paragraph.size() + 1;
        }
        x.end();
    }

    void writeTextObject(Xml &x, const SceneObject &o, SlideCtx &ctx) {
        x.start("p:sp");
        writeNonVisual(x, "p:nvSpPr", "p:cNvSpPr", o, ctx, QStringLiteral("Text %1").arg(ctx.nextId), true);
        x.start("p:spPr");
        writeXfrm(x, o.rect, o.rotation);
        x.start("a:prstGeom").attr("prst", "rect"); x.empty("a:avLst"); x.end();
        x.empty("a:noFill");
        x.end();
        writeTextBody(x, "p:txBody", o, ctx, false);
        x.end();
    }

    void writeShape(Xml &x, const SceneObject &o, SlideCtx &ctx) {
        x.start("p:sp");
        writeNonVisual(x, "p:nvSpPr", "p:cNvSpPr", o, ctx, QStringLiteral("Shape %1").arg(ctx.nextId));
        x.start("p:spPr");
        writeXfrm(x, o.rect, o.rotation);
        const bool rounded = o.shapeKind == 0 && o.cornerRadius > 0;
        const char *preset = o.connector || o.shapeKind == 99 ? nullptr : rounded ? "roundRect" : presetFor(o.shapeKind);
        if (preset) {
            x.start("a:prstGeom").attr("prst", preset);
            x.start("a:avLst");
            if (rounded) {
                // The rounding is a fraction of the shorter side; a pill is 50000.
                const qreal shorter = qMax(1.0, qMin(o.rect.width(), o.rect.height()));
                x.start("a:gd").attr("name", "adj").attr("fmla", QStringLiteral("val %1").arg(qBound(0, qRound(o.cornerRadius / shorter * 100000), 50000))).end();
            }
            x.end();
            x.end();
        } else writeCustomGeometry(x, o);
        writeFill(x, o, ctx.number);
        writeLine(x, o);
        writeEffects(x, o);
        x.end();
        x.end();
    }

    // ---- pictures and films

    QString mediaPart(const QString &identity, const QByteArray &data, const QString &extension, const QString &mime) {
        const auto key = identity.isEmpty() ? QString::number(++mediaCount) + extension : identity + extension;
        auto it = mediaByIdentity.find(key);
        if (it != mediaByIdentity.end()) return *it;
        const auto path = QStringLiteral("ppt/media/media%1.%2").arg(++mediaCount).arg(extension);
        binary(path, data, extension, mime);
        mediaByIdentity.insert(key, path);
        return path;
    }

    static QByteArray pngOf(const QImage &image) {
        QByteArray out;
        QBuffer buffer(&out);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        return out;
    }

    void writePictureCommon(Xml &x, const SceneObject &o, SlideCtx &ctx, const QString &blipRel, bool film) {
        x.start("p:blipFill");
        x.start("a:blip").attr("r:embed", blipRel);
        if (!film) {
            if (o.opacity < 0.999) x.start("a:alphaModFix").attr("amt", qRound(o.opacity * 100000)).end();
            if (qFuzzyIsNull(o.imageSaturation)) x.empty("a:grayscl");
            if (!qFuzzyIsNull(o.imageBrightness) || !qFuzzyCompare(o.imageContrast, 1.0))
                x.start("a:lum").attr("bright", qRound(qBound(-1.0, o.imageBrightness, 1.0) * 100000)).attr("contrast", qRound(qBound(-1.0, o.imageContrast - 1.0, 1.0) * 100000)).end();
            if (o.imageTintAmount > 0) warnings.add(QStringLiteral("A picture's tint was left off"), ctx.number);
        }
        x.end();
        const QRectF crop = o.imageCrop;
        if (!film && (crop.x() > 0 || crop.y() > 0 || crop.right() < 1 || crop.bottom() < 1)) {
            x.start("a:srcRect").attr("l", qRound(crop.x() * 100000)).attr("t", qRound(crop.y() * 100000))
                .attr("r", qRound((1 - crop.right()) * 100000)).attr("b", qRound((1 - crop.bottom()) * 100000)).end();
        }
        x.start("a:stretch"); x.empty("a:fillRect"); x.end();
        x.end();
        x.start("p:spPr");
        writeXfrm(x, o.rect, o.rotation);
        const char *mask = o.imageMask == 1 ? "ellipse" : o.imageMask == 2 ? "roundRect" : o.imageMask == 3 ? "hexagon" : o.imageMask == 4 ? "heart" : "rect";
        x.start("a:prstGeom").attr("prst", mask); x.empty("a:avLst"); x.end();
        if (o.strokeWidth > 0) writeLine(x, o);
        writeEffects(x, o);
        x.end();
    }

    void writePicture(Xml &x, const SceneObject &o, SlideCtx &ctx) {
        QByteArray data = o.imageData;
        QString extension = o.imageFormat.toLower(), mime;
        if (extension == QLatin1String("jpg")) extension = QStringLiteral("jpeg");
        if (extension == QLatin1String("svg") || data.isEmpty()) {
            // A vector picture goes as a bitmap of itself at a generous size.
            QImage raster = o.image;
            if (!o.vectorPicture.isNull() && !o.vectorSize.isEmpty()) {
                const qreal scale = qMin(4.0, 2048.0 / qMax(1.0, qMax(o.vectorSize.width(), o.vectorSize.height())));
                raster = QImage((o.vectorSize * scale).toSize(), QImage::Format_ARGB32_Premultiplied);
                raster.fill(Qt::transparent);
                QPainter painter(&raster);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.scale(scale, scale);
                painter.drawPicture(0, 0, o.vectorPicture);
            }
            if (raster.isNull()) { warnings.add(QStringLiteral("A picture had no data to write"), ctx.number); return; }
            data = pngOf(raster);
            extension = QStringLiteral("png");
            if (o.imageFormat.toLower() == QLatin1String("svg")) warnings.add(QStringLiteral("A vector picture was written as a bitmap"), ctx.number);
        }
        mime = extension == QLatin1String("png") ? QStringLiteral("image/png") : extension == QLatin1String("jpeg") ? QStringLiteral("image/jpeg")
             : extension == QLatin1String("gif") ? QStringLiteral("image/gif") : extension == QLatin1String("webp") ? QStringLiteral("image/webp") : QStringLiteral("application/octet-stream");
        const auto path = mediaPart(o.imageId + QLatin1Char('/') + extension, data, extension, mime);
        const auto rel = ctx.rels->add(relBase + QStringLiteral("image"), QStringLiteral("../") + path.mid(4));
        x.start("p:pic");
        writeNonVisual(x, "p:nvPicPr", "p:cNvPicPr", o, ctx, QStringLiteral("Picture %1").arg(ctx.nextId));
        writePictureCommon(x, o, ctx, rel, false);
        x.end();
    }

    void writeMedia(Xml &x, const SceneObject &o, SlideCtx &ctx) {
        // The poster is the first frame; the film itself rides beside it.
        QImage poster = MediaAsset::frameAt(o, qMax(0.0, o.mediaTrimStart));
        if (poster.isNull()) { poster = QImage(64, 36, QImage::Format_RGB32); poster.fill(QColor(30, 30, 30)); }
        const auto posterPath = mediaPart(o.mediaId + QStringLiteral("/poster"), pngOf(poster), QStringLiteral("png"), QStringLiteral("image/png"));
        const auto posterRel = ctx.rels->add(relBase + QStringLiteral("image"), QStringLiteral("../") + posterPath.mid(4));
        QString linkRel, embedRel;
        if (!o.mediaData.isEmpty()) {
            auto extension = QFileInfo(o.mediaName).suffix().toLower();
            if (extension.isEmpty()) extension = o.mediaContainer.section(QLatin1Char(','), 0, 0).toLower();
            if (extension.isEmpty() || extension.size() > 5) extension = o.mediaVideo ? QStringLiteral("mp4") : QStringLiteral("m4a");
            const QString mime = extension == QLatin1String("mp4") || extension == QLatin1String("m4v") ? QStringLiteral("video/mp4")
                               : extension == QLatin1String("mov") ? QStringLiteral("video/quicktime")
                               : extension == QLatin1String("webm") ? QStringLiteral("video/webm")
                               : extension == QLatin1String("mkv") ? QStringLiteral("video/x-matroska")
                               : extension == QLatin1String("mp3") ? QStringLiteral("audio/mpeg")
                               : extension == QLatin1String("wav") ? QStringLiteral("audio/wav")
                               : extension == QLatin1String("m4a") ? QStringLiteral("audio/mp4")
                               : extension == QLatin1String("ogg") || extension == QLatin1String("oga") ? QStringLiteral("audio/ogg")
                               : extension == QLatin1String("flac") ? QStringLiteral("audio/flac")
                               : o.mediaVideo ? QStringLiteral("video/mp4") : QStringLiteral("audio/mpeg");
            const auto path = mediaPart(o.mediaId + QLatin1Char('/') + extension, o.mediaData, extension, mime);
            linkRel = ctx.rels->add(relBase + (o.mediaVideo ? QStringLiteral("video") : QStringLiteral("audio")), QStringLiteral("../") + path.mid(4));
            embedRel = ctx.rels->add(relMedia, QStringLiteral("../") + path.mid(4));
        } else if (!o.mediaPath.isEmpty()) {
            linkRel = ctx.rels->add(relBase + (o.mediaVideo ? QStringLiteral("video") : QStringLiteral("audio")), QUrl::fromLocalFile(o.mediaPath).toString(), true);
            warnings.add(QStringLiteral("A linked film points at its file on this computer; embed it to travel"), ctx.number);
        }
        if (o.mediaTrimStart > 0 || o.mediaTrimEnd > 0) warnings.add(QStringLiteral("A film's trim was left off; the whole file plays"), ctx.number);
        x.start("p:pic");
        const int id = ctx.nextId++;
        ctx.spidByObject.insert(o.id, id);
        x.start("p:nvPicPr");
        x.start("p:cNvPr").attr("id", id).attr("name", o.altTitle.isEmpty() ? (o.mediaName.isEmpty() ? QStringLiteral("Film %1").arg(id) : o.mediaName) : o.altTitle);
        if (!o.altText.isEmpty()) x.attr("descr", o.altText);
        if (!linkRel.isEmpty()) { x.start("a:hlinkClick").attr("r:id", "").attr("action", "ppaction://media").end(); }
        x.end();
        x.start("p:cNvPicPr"); x.start("a:picLocks").attr("noChangeAspect", "1").end(); x.end();
        x.start("p:nvPr");
        if (!linkRel.isEmpty()) {
            x.start(o.mediaVideo ? "a:videoFile" : "a:audioFile").attr("r:link", linkRel).end();
            if (!embedRel.isEmpty()) {
                x.start("p:extLst"); x.start("p:ext").attr("uri", "{DAA4B4D4-6D71-4841-9C94-3DE7FCFB9230}");
                x.start("p14:media").attr("xmlns:p14", nsP14).attr("r:embed", embedRel).end();
                x.end(); x.end();
            }
        }
        x.end();
        x.end();
        writePictureCommon(x, o, ctx, posterRel, true);
        x.end();
    }

    // ---- tables

    void writeTable(Xml &x, const SceneObject &o, SlideCtx &ctx) {
        const auto &t = o.table;
        const int rows = t.rows.size(), columns = t.columns.size();
        if (rows < 1 || columns < 1) return;
        x.start("p:graphicFrame");
        writeNonVisual(x, "p:nvGraphicFramePr", "p:cNvGraphicFramePr", o, ctx, QStringLiteral("Table %1").arg(ctx.nextId));
        writeXfrm(x, o.rect, 0, "p:xfrm");
        if (!qFuzzyIsNull(o.rotation)) warnings.add(QStringLiteral("A turned table was written upright"), ctx.number);
        x.start("a:graphic");
        x.start("a:graphicData").attr("uri", "http://schemas.openxmlformats.org/drawingml/2006/table");
        x.start("a:tbl");
        x.start("a:tblPr");
        if (t.headerRows > 0) x.attr("firstRow", "1");
        if (t.headerColumns > 0) x.attr("firstCol", "1");
        if (t.banded) x.attr("bandRow", "1");
        x.end();
        qreal columnTotal = 0, rowTotal = 0;
        for (qreal c : t.columns) columnTotal += c;
        for (qreal r : t.rows) rowTotal += r;
        x.start("a:tblGrid");
        for (qreal c : t.columns) x.start("a:gridCol").attr("w", qMax<qint64>(1, emu(o.rect.width() * c / qMax(0.0001, columnTotal)))).end();
        x.end();
        for (int r = 0; r < rows; ++r) {
            x.start("a:tr").attr("h", qMax<qint64>(1, emu(o.rect.height() * t.rows[r] / qMax(0.0001, rowTotal))));
            for (int c = 0; c < columns; ++c) {
                const auto &cell = t.cells[r * columns + c];
                x.start("a:tc");
                if (cell.rowSpan == 0 && cell.columnSpan == 0) {
                    // Covered: say which way, by finding the anchor.
                    const int anchorIndex = Table::anchor(t, r, c);
                    const int ar = anchorIndex / columns, ac = anchorIndex % columns;
                    if (ac != c) x.attr("hMerge", "1");
                    if (ar != r) x.attr("vMerge", "1");
                } else {
                    if (cell.columnSpan > 1) x.attr("gridSpan", cell.columnSpan);
                    if (cell.rowSpan > 1) x.attr("rowSpan", cell.rowSpan);
                }
                SceneObject words = Table::textObject(o, r, c);
                words.id = cell.id;
                words.runs.clear();
                words.linkKind = 0;
                if (cell.rowSpan == 0 && cell.columnSpan == 0) words.text.clear();
                writeTextBody(x, "a:txBody", words, ctx, true);
                x.start("a:tcPr").attr("marL", emu(t.padding)).attr("marR", emu(t.padding)).attr("marT", emu(t.padding)).attr("marB", emu(t.padding))
                    .attr("anchor", words.verticalAlign == 1 ? "ctr" : words.verticalAlign == 2 ? "b" : "t");
                const QColor border = QColor(cell.style.value(QStringLiteral("borderColor"), t.borderColor.name(QColor::HexArgb)).toString());
                const qreal width = cell.style.value(QStringLiteral("borderWidth"), t.borderWidth).toDouble();
                for (const auto *side : {"a:lnL", "a:lnR", "a:lnT", "a:lnB"}) {
                    x.start(side).attr("w", qMax<qint64>(0, emu(width)));
                    if (width > 0 && border.isValid() && border.alpha() > 0) writeSolidFill(x, border); else x.empty("a:noFill");
                    x.end();
                }
                if (words.fill.isValid() && words.fill.alpha() > 0) writeSolidFill(x, words.fill); else x.empty("a:noFill");
                x.end();
                x.end(); // tc
            }
            x.end(); // tr
        }
        x.end(); x.end(); x.end();
        x.end();
    }

    // ---- charts

    static bool number(const QString &text, const QString &locale, double *value) {
        QString cleaned = text.trimmed();
        cleaned.remove(QLatin1Char('%')).remove(QStringLiteral("£")).remove(QStringLiteral("€")).remove(QLatin1Char('$'));
        bool ok = false;
        double v = QLocale(locale).toDouble(cleaned, &ok);
        if (!ok) { v = QLocale::c().toDouble(cleaned.remove(QLatin1Char(',')), &ok); }
        if (ok) *value = v;
        return ok;
    }

    void writeChart(Xml &x, const SceneObject &o, SlideCtx &ctx) {
        const auto layout = Chart::layout(o);
        const auto &t = o.table;
        const int columns = t.columns.size(), rows = t.rows.size();
        if (columns < 2 || rows < 2) { warnings.add(QStringLiteral("A chart without data was left out"), ctx.number); return; }
        const int kind = o.chart.kind;
        const bool scatter = kind == 9, pie = kind == 7 || kind == 8;
        const auto chartPath = QStringLiteral("ppt/charts/chart%1.xml").arg(++chartCount);
        const auto rel = ctx.rels->add(relBase + QStringLiteral("chart"), QStringLiteral("../") + chartPath.mid(4));

        Xml c;
        c.start("c:chartSpace").attr("xmlns:c", nsC).attr("xmlns:a", nsA).attr("xmlns:r", nsR);
        c.start("c:roundedCorners").attr("val", "0").end();
        c.start("c:chart");
        if (!o.chart.title.trimmed().isEmpty()) {
            c.start("c:title"); c.start("c:tx"); c.start("c:rich"); c.empty("a:bodyPr"); c.empty("a:lstStyle");
            c.start("a:p"); c.start("a:r"); c.start("a:rPr").attr("lang", "en-GB").attr("sz", hundredthsPt(o.fontSize * 1.2)).attr("b", "0"); writeSolidFill(c, o.textColor); c.end();
            c.start("a:t").text(o.chart.title).end(); c.end(); c.end();
            c.end(); c.end(); c.start("c:overlay").attr("val", "0").end(); c.end();
            c.start("c:autoTitleDeleted").attr("val", "0").end();
        } else c.start("c:autoTitleDeleted").attr("val", "1").end();
        c.start("c:plotArea");
        c.empty("c:layout");
        const char *element = scatter ? "c:scatterChart" : pie ? (kind == 8 ? "c:doughnutChart" : "c:pieChart")
                            : kind == 4 ? "c:lineChart" : kind == 5 || kind == 6 ? "c:areaChart" : "c:barChart";
        c.start(element);
        if (scatter) c.start("c:scatterStyle").attr("val", "lineMarker").end();
        if (kind <= 3) { c.start("c:barDir").attr("val", kind == 1 || kind == 3 ? "bar" : "col").end(); c.start("c:grouping").attr("val", kind >= 2 ? "stacked" : "clustered").end(); }
        else if (kind == 4 || kind == 5 || kind == 6) c.start("c:grouping").attr("val", kind == 6 ? "stacked" : "standard").end();
        c.start("c:varyColors").attr("val", pie ? "1" : "0").end();

        const auto colorFor = [&](int series, int category) {
            const auto &cell = t.cells[pie ? (category + 1) * columns : series + 1];
            if (o.chart.seriesColors.contains(cell.id)) return o.chart.seriesColors.value(cell.id);
            const auto palette = o.chart.resolvedColors.isEmpty() ? Chart::palette(doc.theme, o.chart.palette) : o.chart.resolvedColors;
            return palette.isEmpty() ? QColor(Qt::gray) : palette[(pie ? category : series) % palette.size()];
        };
        const int seriesCount = columns - 1, categoryCount = rows - 1;
        for (int s = 0; s < seriesCount; ++s) {
            c.start("c:ser");
            c.start("c:idx").attr("val", s).end(); c.start("c:order").attr("val", s).end();
            c.start("c:tx"); c.start("c:strRef"); c.start("c:f").text(QStringLiteral("Sheet1!$%1$1").arg(columnLetters(s + 1))).end();
            c.start("c:strCache"); c.start("c:ptCount").attr("val", 1).end(); c.start("c:pt").attr("idx", 0); c.start("c:v").text(layout.series.value(s)).end(); c.end(); c.end();
            c.end(); c.end();
            if (!pie) {
                c.start("c:spPr");
                if (kind == 4 || scatter) { c.start("a:ln").attr("w", 28575); writeSolidFill(c, colorFor(s, 0)); c.end(); }
                else writeSolidFill(c, colorFor(s, 0));
                c.end();
                if (kind == 4 || scatter) { c.start("c:marker"); c.start("c:symbol").attr("val", "circle").end(); c.start("c:size").attr("val", 6).end(); c.end(); }
            } else {
                for (int k = 0; k < categoryCount; ++k) {
                    c.start("c:dPt"); c.start("c:idx").attr("val", k).end(); c.start("c:bubble3D").attr("val", "0").end();
                    c.start("c:spPr"); writeSolidFill(c, colorFor(s, k)); c.end(); c.end();
                }
            }
            if (o.chart.labels) {
                c.start("c:dLbls");
                for (const auto *flag : {"c:showLegendKey", "c:showVal", "c:showCatName", "c:showSerName", "c:showPercent", "c:showBubbleSize"})
                    c.start(flag).attr("val", QLatin1String(flag) == QLatin1String("c:showVal") ? "1" : "0").end();
                c.end();
            }
            if (scatter) {
                for (const auto *axis : {"c:xVal", "c:yVal"}) {
                    const int column = QLatin1String(axis) == QLatin1String("c:xVal") ? 0 : s + 1;
                    c.start(axis); c.start("c:numRef"); c.start("c:f").text(QStringLiteral("Sheet1!$%1$2:$%1$%2").arg(columnLetters(column)).arg(rows)).end();
                    c.start("c:numCache"); c.start("c:formatCode").text(QStringLiteral("General")).end(); c.start("c:ptCount").attr("val", categoryCount).end();
                    for (int k = 0; k < categoryCount; ++k) {
                        double v;
                        if (!number(t.cells[(k + 1) * columns + column].text, o.chart.locale, &v)) continue;
                        c.start("c:pt").attr("idx", k); c.start("c:v").text(QString::number(v, 'g', 15)).end(); c.end();
                    }
                    c.end(); c.end(); c.end();
                }
            } else {
                c.start("c:cat"); c.start("c:strRef"); c.start("c:f").text(QStringLiteral("Sheet1!$A$2:$A$%1").arg(rows)).end();
                c.start("c:strCache"); c.start("c:ptCount").attr("val", categoryCount).end();
                for (int k = 0; k < categoryCount; ++k) { c.start("c:pt").attr("idx", k); c.start("c:v").text(layout.categories.value(k)).end(); c.end(); }
                c.end(); c.end(); c.end();
                c.start("c:val"); c.start("c:numRef"); c.start("c:f").text(QStringLiteral("Sheet1!$%1$2:$%1$%2").arg(columnLetters(s + 1)).arg(rows)).end();
                c.start("c:numCache"); c.start("c:formatCode").text(QStringLiteral("General")).end(); c.start("c:ptCount").attr("val", categoryCount).end();
                for (int k = 0; k < categoryCount; ++k) {
                    double v;
                    if (!number(t.cells[(k + 1) * columns + s + 1].text, o.chart.locale, &v)) continue;
                    c.start("c:pt").attr("idx", k); c.start("c:v").text(QString::number(v, 'g', 15)).end(); c.end();
                }
                c.end(); c.end(); c.end();
            }
            if (kind == 4 || scatter) c.start("c:smooth").attr("val", "0").end();
            c.end(); // ser
        }
        if (kind >= 2 && kind <= 3) { c.start("c:gapWidth").attr("val", 150).end(); c.start("c:overlap").attr("val", 100).end(); }
        else if (kind <= 1) c.start("c:gapWidth").attr("val", 150).end();
        if (kind == 8) c.start("c:holeSize").attr("val", 50).end();
        if (!pie) { c.start("c:axId").attr("val", 111).end(); c.start("c:axId").attr("val", 222).end(); }
        c.end(); // kind element
        if (!pie) {
            const auto axisTitle = [&](const QString &words) {
                if (words.trimmed().isEmpty()) return;
                c.start("c:title"); c.start("c:tx"); c.start("c:rich"); c.empty("a:bodyPr"); c.empty("a:lstStyle");
                c.start("a:p"); c.start("a:r"); c.start("a:rPr").attr("lang", "en-GB").attr("sz", hundredthsPt(o.fontSize)); writeSolidFill(c, o.textColor); c.end();
                c.start("a:t").text(words).end(); c.end(); c.end(); c.end(); c.end();
                c.start("c:overlay").attr("val", "0").end(); c.end();
            };
            const bool horizontal = kind == 1 || kind == 3;
            c.start(scatter ? "c:valAx" : "c:catAx");
            c.start("c:axId").attr("val", 111).end();
            c.start("c:scaling"); c.start("c:orientation").attr("val", "minMax").end(); c.end();
            c.start("c:delete").attr("val", "0").end();
            c.start("c:axPos").attr("val", horizontal ? "l" : "b").end();
            axisTitle(o.chart.xTitle);
            c.start("c:numFmt").attr("formatCode", "General").attr("sourceLinked", "1").end();
            c.start("c:majorTickMark").attr("val", "out").end(); c.start("c:minorTickMark").attr("val", "none").end();
            c.start("c:tickLblPos").attr("val", "nextTo").end();
            c.start("c:crossAx").attr("val", 222).end();
            c.start("c:crosses").attr("val", "autoZero").end();
            if (!scatter) { c.start("c:auto").attr("val", "1").end(); c.start("c:lblAlgn").attr("val", "ctr").end(); c.start("c:lblOffset").attr("val", 100).end(); }
            else c.start("c:crossBetween").attr("val", "midCat").end();
            c.end();
            c.start("c:valAx");
            c.start("c:axId").attr("val", 222).end();
            c.start("c:scaling");
            if (o.chart.logarithmic) c.start("c:logBase").attr("val", 10).end();
            c.start("c:orientation").attr("val", "minMax").end();
            if (o.chart.manualY) { c.start("c:max").attr("val", QString::number(o.chart.maximumY)).end(); c.start("c:min").attr("val", QString::number(o.chart.minimumY)).end(); }
            c.end();
            c.start("c:delete").attr("val", "0").end();
            c.start("c:axPos").attr("val", horizontal ? "b" : "l").end();
            if (o.chart.grid) c.empty("c:majorGridlines");
            axisTitle(o.chart.yTitle);
            c.start("c:numFmt").attr("formatCode", "General").attr("sourceLinked", "1").end();
            c.start("c:majorTickMark").attr("val", "out").end(); c.start("c:minorTickMark").attr("val", "none").end();
            c.start("c:tickLblPos").attr("val", "nextTo").end();
            c.start("c:crossAx").attr("val", 111).end();
            c.start("c:crosses").attr("val", "autoZero").end();
            c.start("c:crossBetween").attr("val", scatter ? "midCat" : "between").end();
            c.end();
        }
        c.end(); // plotArea
        if (o.chart.legend) {
            c.start("c:legend"); c.start("c:legendPos").attr("val", "b").end(); c.start("c:overlay").attr("val", "0").end(); c.end();
        }
        c.start("c:plotVisOnly").attr("val", "1").end();
        c.start("c:dispBlanksAs").attr("val", "gap").end();
        c.end(); // chart
        c.start("c:txPr"); c.empty("a:bodyPr"); c.empty("a:lstStyle");
        c.start("a:p"); c.start("a:pPr"); c.start("a:defRPr").attr("sz", hundredthsPt(o.fontSize)); writeSolidFill(c, o.textColor); c.start("a:latin").attr("typeface", o.fontFamily).end(); c.end(); c.end();
        c.empty("a:endParaRPr"); c.end(); c.end();
        c.end(); // chartSpace
        part(chartPath, c.finish(), "application/vnd.openxmlformats-officedocument.drawingml.chart+xml");

        x.start("p:graphicFrame");
        writeNonVisual(x, "p:nvGraphicFramePr", "p:cNvGraphicFramePr", o, ctx, QStringLiteral("Chart %1").arg(ctx.nextId));
        writeXfrm(x, o.rect, 0, "p:xfrm");
        x.start("a:graphic");
        x.start("a:graphicData").attr("uri", nsC);
        x.start("c:chart").attr("xmlns:c", nsC).attr("r:id", rel).end();
        x.end(); x.end();
        x.end();
    }

    void writeObjects(Xml &x, const QVector<SceneObject> &objects, SlideCtx &ctx) {
        for (const auto &o : objects) {
            if (canceled()) return;
            switch (o.type) {
            case ObjectType::Rect: writeShape(x, o, ctx); break;
            case ObjectType::Text: writeTextObject(x, o, ctx); break;
            case ObjectType::Image: writePicture(x, o, ctx); break;
            case ObjectType::Media: writeMedia(x, o, ctx); break;
            case ObjectType::Table: writeTable(x, o, ctx); break;
            case ObjectType::Chart: writeChart(x, o, ctx); break;
            }
        }
    }

    void startTree(Xml &x) const {
        x.start("p:spTree");
        x.start("p:nvGrpSpPr"); x.start("p:cNvPr").attr("id", 1).attr("name", "").end(); x.empty("p:cNvGrpSpPr"); x.empty("p:nvPr"); x.end();
        x.start("p:grpSpPr"); x.start("a:xfrm");
        x.start("a:off").attr("x", 0).attr("y", 0).end(); x.start("a:ext").attr("cx", 0).attr("cy", 0).end();
        x.start("a:chOff").attr("x", 0).attr("y", 0).end(); x.start("a:chExt").attr("cx", 0).attr("cy", 0).end();
        x.end(); x.end();
    }

    void writeBackground(Xml &x, const QColor &color) const {
        x.start("p:bg"); x.start("p:bgPr"); writeSolidFill(x, color); x.empty("a:effectLst"); x.end(); x.end();
    }

    // ---- masters and layouts

    struct MasterOut { QString path; int index; };
    QHash<QString, int> masterIndex;   // Master.id → 1-based
    QHash<QString, int> layoutIndex;   // SlideLayout.id → 1-based
    qint64 nextLayoutId = 2147483649LL;

    void writeMaster(const Master &m, int index, const QVector<int> &layoutIndices) {
        const auto path = QStringLiteral("ppt/slideMasters/slideMaster%1.xml").arg(index);
        Rels rels;
        Xml x;
        x.start("p:sldMaster").attr("xmlns:a", nsA).attr("xmlns:r", nsR).attr("xmlns:p", nsP);
        x.start("p:cSld").attr("name", m.name);
        writeBackground(x, doc.theme.colors.value(m.backgroundToken, m.background));
        startTree(x);
        SlideCtx ctx; ctx.partPath = path; ctx.rels = &rels;
        QVector<SceneObject> objects;
        for (const auto &o : m.objects) objects.append(Design::themed(doc.theme, o));
        writeObjects(x, objects, ctx);
        x.end(); x.end();
        x.start("p:clrMap").attr("bg1", "lt1").attr("tx1", "dk1").attr("bg2", "lt2").attr("tx2", "dk2").attr("accent1", "accent1").attr("accent2", "accent2")
            .attr("accent3", "accent3").attr("accent4", "accent4").attr("accent5", "accent5").attr("accent6", "accent6").attr("hlink", "hlink").attr("folHlink", "folHlink").end();
        x.start("p:sldLayoutIdLst");
        for (int li : layoutIndices) {
            const auto rel = rels.add(relBase + QStringLiteral("slideLayout"), QStringLiteral("../slideLayouts/slideLayout%1.xml").arg(li));
            x.start("p:sldLayoutId").attr("id", nextLayoutId++).attr("r:id", rel).end();
        }
        x.end();
        x.start("p:txStyles");
        for (const auto *tag : {"p:titleStyle", "p:bodyStyle", "p:otherStyle"}) {
            x.start(tag);
            x.start("a:lvl1pPr").attr("algn", "l");
            x.start("a:defRPr").attr("sz", QLatin1String(tag) == QLatin1String("p:titleStyle") ? 4400 : 2400);
            x.start("a:solidFill"); x.start("a:schemeClr").attr("val", "tx1").end(); x.end();
            x.start("a:latin").attr("typeface", QLatin1String(tag) == QLatin1String("p:titleStyle") ? "+mj-lt" : "+mn-lt").end();
            x.end(); x.end(); x.end();
        }
        x.end();
        x.end();
        rels.add(relBase + QStringLiteral("theme"), QStringLiteral("../theme/theme1.xml"));
        part(path, x.finish(), "application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml");
        part(relsPath(path), rels.bytes(), nullptr);
    }

    void writeLayout(const SlideLayout &layout, int index, int master) {
        const auto path = QStringLiteral("ppt/slideLayouts/slideLayout%1.xml").arg(index);
        Rels rels;
        rels.add(relBase + QStringLiteral("slideMaster"), QStringLiteral("../slideMasters/slideMaster%1.xml").arg(master));
        Xml x;
        x.start("p:sldLayout").attr("xmlns:a", nsA).attr("xmlns:r", nsR).attr("xmlns:p", nsP).attr("preserve", "1");
        x.start("p:cSld").attr("name", layout.name);
        startTree(x);
        SlideCtx ctx; ctx.partPath = path; ctx.rels = &rels;
        int bodies = 0;
        for (const auto &placeholder : layout.placeholders) {
            const bool title = placeholder.id == QLatin1String("title");
            if (!title && placeholder.id.startsWith(QLatin1String("body"))) ++bodies;
            SceneObject p = Design::themed(doc.theme, placeholder);
            const int id = ctx.nextId++;
            x.start("p:sp");
            x.start("p:nvSpPr");
            x.start("p:cNvPr").attr("id", id).attr("name", title ? QStringLiteral("Title") : QStringLiteral("Body %1").arg(bodies)).end();
            x.start("p:cNvSpPr"); x.start("a:spLocks").attr("noGrp", "1").end(); x.end();
            x.start("p:nvPr");
            x.start("p:ph");
            if (title) x.attr("type", "title"); else x.attr("idx", bodies);
            x.end();
            x.end(); x.end();
            x.start("p:spPr"); writeXfrm(x, p.rect, p.rotation); x.end();
            p.linkKind = 0;
            writeTextBody(x, "p:txBody", p, ctx, false);
            x.end();
        }
        x.end(); x.end();
        x.start("p:clrMapOvr"); x.empty("a:masterClrMapping"); x.end();
        x.end();
        part(path, x.finish(), "application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml");
        part(relsPath(path), rels.bytes(), nullptr);
    }

    // ---- notes

    void writeNotesMaster() {
        Rels rels;
        rels.add(relBase + QStringLiteral("theme"), QStringLiteral("../theme/theme2.xml"));
        Xml x;
        x.start("p:notesMaster").attr("xmlns:a", nsA).attr("xmlns:r", nsR).attr("xmlns:p", nsP);
        x.start("p:cSld");
        writeBackground(x, QColor(Qt::white));
        startTree(x);
        x.start("p:sp"); x.start("p:nvSpPr"); x.start("p:cNvPr").attr("id", 2).attr("name", "Slide Image Placeholder 1").end();
        x.start("p:cNvSpPr"); x.start("a:spLocks").attr("noGrp", "1").attr("noRot", "1").attr("noChangeAspect", "1").end(); x.end();
        x.start("p:nvPr"); x.start("p:ph").attr("type", "sldImg").attr("idx", 2).end(); x.end(); x.end();
        x.start("p:spPr"); x.start("a:xfrm"); x.start("a:off").attr("x", 685800).attr("y", 1143000).end(); x.start("a:ext").attr("cx", 5486400).attr("cy", 3086100).end(); x.end();
        x.start("a:prstGeom").attr("prst", "rect"); x.empty("a:avLst"); x.end(); x.empty("a:noFill"); x.end(); x.end();
        x.start("p:sp"); x.start("p:nvSpPr"); x.start("p:cNvPr").attr("id", 3).attr("name", "Notes Placeholder 2").end();
        x.start("p:cNvSpPr"); x.start("a:spLocks").attr("noGrp", "1").end(); x.end();
        x.start("p:nvPr"); x.start("p:ph").attr("type", "body").attr("idx", 1).end(); x.end(); x.end();
        x.start("p:spPr"); x.start("a:xfrm"); x.start("a:off").attr("x", 685800).attr("y", 4343400).end(); x.start("a:ext").attr("cx", 5486400).attr("cy", 4114800).end(); x.end();
        x.start("a:prstGeom").attr("prst", "rect"); x.empty("a:avLst"); x.end(); x.end();
        x.start("p:txBody"); x.empty("a:bodyPr"); x.empty("a:lstStyle"); x.start("a:p"); x.start("a:r"); x.start("a:rPr").attr("lang", "en-GB").end(); x.start("a:t").text(QStringLiteral("Notes")).end(); x.end(); x.end(); x.end();
        x.end();
        x.end(); x.end();
        x.start("p:clrMap").attr("bg1", "lt1").attr("tx1", "dk1").attr("bg2", "lt2").attr("tx2", "dk2").attr("accent1", "accent1").attr("accent2", "accent2")
            .attr("accent3", "accent3").attr("accent4", "accent4").attr("accent5", "accent5").attr("accent6", "accent6").attr("hlink", "hlink").attr("folHlink", "folHlink").end();
        x.start("p:notesStyle"); x.start("a:lvl1pPr"); x.start("a:defRPr").attr("sz", 1200); x.start("a:solidFill"); x.start("a:schemeClr").attr("val", "tx1").end(); x.end(); x.start("a:latin").attr("typeface", "+mn-lt").end(); x.end(); x.end(); x.end();
        x.end();
        part(QStringLiteral("ppt/notesMasters/notesMaster1.xml"), x.finish(), "application/vnd.openxmlformats-officedocument.presentationml.notesMaster+xml");
        part(relsPath(QStringLiteral("ppt/notesMasters/notesMaster1.xml")), rels.bytes(), nullptr);
    }

    void writeNotes(int number, const QString &notes) {
        const auto path = QStringLiteral("ppt/notesSlides/notesSlide%1.xml").arg(number);
        Rels rels;
        rels.add(relBase + QStringLiteral("notesMaster"), QStringLiteral("../notesMasters/notesMaster1.xml"));
        rels.add(relBase + QStringLiteral("slide"), QStringLiteral("../slides/slide%1.xml").arg(number));
        Xml x;
        x.start("p:notes").attr("xmlns:a", nsA).attr("xmlns:r", nsR).attr("xmlns:p", nsP);
        x.start("p:cSld");
        startTree(x);
        x.start("p:sp"); x.start("p:nvSpPr"); x.start("p:cNvPr").attr("id", 2).attr("name", "Slide Image Placeholder 1").end();
        x.start("p:cNvSpPr"); x.start("a:spLocks").attr("noGrp", "1").attr("noRot", "1").attr("noChangeAspect", "1").end(); x.end();
        x.start("p:nvPr"); x.start("p:ph").attr("type", "sldImg").end(); x.end(); x.end();
        x.empty("p:spPr"); x.end();
        x.start("p:sp"); x.start("p:nvSpPr"); x.start("p:cNvPr").attr("id", 3).attr("name", "Notes Placeholder 2").end();
        x.start("p:cNvSpPr"); x.start("a:spLocks").attr("noGrp", "1").end(); x.end();
        x.start("p:nvPr"); x.start("p:ph").attr("type", "body").attr("idx", 1).end(); x.end(); x.end();
        x.empty("p:spPr");
        x.start("p:txBody"); x.empty("a:bodyPr"); x.empty("a:lstStyle");
        for (const auto &paragraph : notes.split(QLatin1Char('\n'))) {
            x.start("a:p"); x.start("a:r"); x.start("a:rPr").attr("lang", "en-GB").end(); x.start("a:t").text(paragraph).end(); x.end(); x.end();
        }
        x.end(); x.end();
        x.end(); x.end();
        x.start("p:clrMapOvr"); x.empty("a:masterClrMapping"); x.end();
        x.end();
        part(path, x.finish(), "application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml");
        part(relsPath(path), rels.bytes(), nullptr);
    }

    // ---- comments

    QStringList authors;
    QVector<int> authorLastIdx;
    int authorOf(const QString &name) {
        const auto who = name.isEmpty() ? QStringLiteral("Unknown") : name;
        if (!authors.contains(who)) { authors.append(who); authorLastIdx.append(0); }
        return authors.indexOf(who);
    }

    void writeComments(int number, const Slide &slide, Rels &rels) {
        QVector<const Comment *> mine;
        for (const auto &c : doc.comments) if (c.slideId == slide.id) mine.append(&c);
        if (mine.isEmpty()) return;
        const auto path = QStringLiteral("ppt/comments/comment%1.xml").arg(++commentCount);
        Xml x;
        x.start("p:cmLst").attr("xmlns:a", nsA).attr("xmlns:r", nsR).attr("xmlns:p", nsP);
        for (const auto *c : mine) {
            const int author = authorOf(c->author);
            x.start("p:cm").attr("authorId", author).attr("idx", ++authorLastIdx[author]);
            if (!c->created.isEmpty()) x.attr("dt", c->created);
            QPointF where(10, 10);
            if (const auto *o = slide.find(c->objectId)) where = QPointF(o->rect.x() * e / 12700.0, o->rect.y() * e / 12700.0);
            x.start("p:pos").attr("x", qRound64(where.x())).attr("y", qRound64(where.y())).end();
            QString words = c->text;
            if (!c->parentId.isEmpty()) words = QStringLiteral("Re: ") + words;
            if (c->resolved) words += QStringLiteral(" [resolved]");
            x.start("p:text").text(words).end();
            x.end();
        }
        x.end();
        part(path, x.finish(), "application/vnd.openxmlformats-officedocument.presentationml.comments+xml");
        rels.add(relBase + QStringLiteral("comments"), QStringLiteral("../") + path.mid(4));
    }

    void writeAuthors() {
        if (authors.isEmpty()) return;
        Xml x;
        x.start("p:cmAuthorLst").attr("xmlns:a", nsA).attr("xmlns:r", nsR).attr("xmlns:p", nsP);
        for (int i = 0; i < authors.size(); ++i) {
            QString initials;
            for (const auto &word : authors[i].split(QLatin1Char(' '), Qt::SkipEmptyParts)) initials += word.at(0).toUpper();
            x.start("p:cmAuthor").attr("id", i).attr("name", authors[i]).attr("initials", initials.left(3)).attr("lastIdx", qMax(1, authorLastIdx.value(i))).attr("clrIdx", i % 8).end();
        }
        x.end();
        part(QStringLiteral("ppt/commentAuthors.xml"), x.finish(), "application/vnd.openxmlformats-officedocument.presentationml.commentAuthors+xml");
    }

    // ---- transitions and builds

    void writeTransition(Xml &x, int index, const Slide &slide) {
        const int kind = Presentation::transitionKind(doc, index);
        const qreal seconds = Presentation::transitionSeconds(doc, index);
        const bool advance = slide.advanceAfter >= 0;
        if (kind == 0 && !advance) return;
        x.start("p:transition").attr("spd", seconds < 0.6 ? "fast" : seconds < 0.9 ? "med" : "slow");
        if (advance) x.attr("advTm", qRound64(slide.advanceAfter * 1000));
        const char *dir = slide.transitionDirection == 1 ? "r" : slide.transitionDirection == 2 ? "u" : slide.transitionDirection == 3 ? "d" : "l";
        if (kind == Presentation::Fade) x.empty("p:fade");
        else if (kind == Presentation::Push) x.start("p:push").attr("dir", dir).end();
        else if (kind == Presentation::Cover) x.start("p:cover").attr("dir", dir).end();
        else if (kind == Presentation::Uncover) x.start("p:pull").attr("dir", dir).end();
        else if (kind == Presentation::FadeThroughBlack) x.start("p:fade").attr("thruBlk", "1").end();
        else if (kind == Presentation::Zoom) x.empty("p:zoom");
        else if (kind == Presentation::Whirl) x.empty("p:newsflash");
        else if (kind == Presentation::Morph) { x.empty("p:fade"); warnings.add(QStringLiteral("The morph transition was written as a fade"), index + 1); }
        x.end();
    }

    // ANIM-INTEROP: a kept PowerPoint fragment with the new shape ids, fresh
    // node ids and (for the effect itself) the current timing. Empty when an
    // object it names is gone.
    QString placeIds(QString xml, const SlideCtx &ctx) const {
        static const QRegularExpression ref(QStringLiteral("@\\{([^}]*)\\}"));
        QString out; int last = 0;
        for (auto it = ref.globalMatch(xml); it.hasNext();) {
            const auto m = it.next();
            const int spid = ctx.spidByObject.value(m.captured(1), 0);
            if (!spid) return QString();
            out += xml.mid(last, m.capturedStart() - last) + QString::number(spid);
            last = m.capturedEnd();
        }
        return out + xml.mid(last);
    }
    static QString renumber(const QString &xml, int &id) {
        static const QRegularExpression cTn(QStringLiteral("<p:cTn( [^>]*?)? id=\"\\d+\""));
        QString out; int last = 0;
        for (auto it = cTn.globalMatch(xml); it.hasNext();) {
            const auto m = it.next();
            out += xml.mid(last, m.capturedStart() - last) + QStringLiteral("<p:cTn") + m.captured(1) + QStringLiteral(" id=\"%1\"").arg(id++);
            last = m.capturedEnd();
        }
        return out + xml.mid(last);
    }
    // Sets attributes on the first tag matching `tagStart` (empty value: removes).
    static QString setAttrs(const QString &xml, int at, const QList<QPair<QString, QString>> &values) {
        const int close = xml.indexOf(QLatin1Char('>'), at);
        if (close < 0) return xml;
        QString tag = xml.mid(at, close - at);
        const bool selfClosing = tag.endsWith(QLatin1Char('/'));
        if (selfClosing) tag.chop(1);
        for (const auto &[k, v] : values) {
            const QRegularExpression re(QStringLiteral(" %1=\"[^\"]*\"").arg(QRegularExpression::escape(k)));
            tag.remove(re);
            if (!v.isEmpty()) tag += QStringLiteral(" %1=\"%2\"").arg(k, v);
        }
        return xml.left(at) + tag + (selfClosing ? QStringLiteral("/") : QString()) + xml.mid(close);
    }
    static QString scaleDurations(QString xml, qreal ratio) {
        if (std::abs(ratio - 1.0) < 1e-6) return xml;
        static const QRegularExpression dur(QStringLiteral(" (dur|delay)=\"(\\d+)\""));
        QString out; int last = 0;
        for (auto it = dur.globalMatch(xml); it.hasNext();) {
            const auto m = it.next();
            const qint64 v = m.captured(2).toLongLong();
            // Parts keep their place in the effect: lengths and inner delays scale together.
            out += xml.mid(last, m.capturedStart() - last) + QStringLiteral(" %1=\"%2\"").arg(m.captured(1)).arg(v <= 1 ? v : qMax<qint64>(1, qRound64(v * ratio)));
            last = m.capturedEnd();
        }
        return out + xml.mid(last);
    }

    void writeBuilds(Xml &x, int number, const Slide &slide, const SlideCtx &ctx) {
        // One PowerPoint effect each. A Reveal of a whole text by paragraph is
        // one effect per paragraph, each straight after the last (ANIM-INTEROP).
        struct Emit { const BuildStep *step; int spid; BuildTrigger trigger; qreal delay; qreal duration; int from, to; bool raw; int group; };
        // Groups PowerPoint already numbered (kept effects and triggers) keep their numbers.
        static const QRegularExpression grpRe(QStringLiteral("presetClass=[^>]*?grpId=\"(\\d+)\"|grpId=\"(\\d+)\"[^>]*?presetClass="));
        QHash<int, QSet<int>> taken;
        QHash<QPair<int, int>, QString> keptBuild;
        const auto rawGroup = [&](const QString &xml) { const auto m = grpRe.match(xml); return m.hasMatch() ? (m.captured(1).isEmpty() ? m.captured(2) : m.captured(1)).toInt() : -1; };
        for (const auto &s : slide.timeline.steps) {
            const int sp = ctx.spidByObject.value(s.targetId, 0);
            if (!sp || s.pptxXml.isEmpty() || s.pptxSig != s.interopSig()) continue;
            const int g = rawGroup(s.pptxXml);
            if (g >= 0) { taken[sp].insert(g); if (s.pptxBuild != QLatin1String("-")) keptBuild.insert({sp, g}, s.pptxBuild); }
        }
        QVector<QPair<int, int>> triggerGroups;
        for (const auto &entry : slide.pptxInteractiveBuilds) {
            const auto parts = entry.split(QLatin1Char('|'));
            const int sp = parts.size() >= 3 ? ctx.spidByObject.value(parts.at(0), 0) : 0;
            if (!sp) continue;
            const int g = parts.at(1).toInt();
            taken[sp].insert(g); triggerGroups.append({sp, g});
            if (parts.at(2) != QLatin1String("-")) keptBuild.insert({sp, g}, parts.mid(2).join(QLatin1Char('|')));
        }
        const auto freeGroup = [&](int sp) { int g = 0; while (taken[sp].contains(g)) ++g; taken[sp].insert(g); return g; };
        QVector<Emit> emits;
        bool timedLater = false, legacyApproximated = false, floatSideways = false, pathless = false;
        bool keptRangeGone = false, orientLost = false, timedEarly = false;
        QStringList convertedNames;
        QHash<int, int> kindOf;             // spid -> 0 shape, 1 text box, 2 other
        QSet<QPair<int, int>> paragraphGroups;
        QHash<QPair<int, int>, int> paragraphRun, paragraphRunNext;   // (spid, phase) -> group, next paragraph
        for (const auto &o : slide.objects) if (const int sp = ctx.spidByObject.value(o.id, 0)) kindOf[sp] = o.type == ObjectType::Rect ? 0 : o.type == ObjectType::Text ? 1 : 2;
        const auto paragraphsOf = [&](const QString &id) { for (const auto &o : slide.objects) if (o.id == id) return int(o.text.split(QLatin1Char('\n')).size()); return 1; };
        for (const auto &s : slide.timeline.steps) {
            if (s.effect == Effect::None || s.effect == Effect::Media) continue;
            const int spid = ctx.spidByObject.value(s.targetId, 0);
            if (!spid) continue;
            const bool rangeGone = s.rangeFrom >= 0 && s.rangeTo > paragraphsOf(s.targetId);
            if (!s.pptxXml.isEmpty() && rangeGone) keptRangeGone = true;
            const bool raw = !s.pptxXml.isEmpty() && s.pptxSig == s.interopSig() && !rangeGone && !placeIds(s.pptxXml, ctx).isEmpty();
            if (!raw && !s.pptxWas.isEmpty()) convertedNames << s.pptxWas;
            if (!raw && s.effect == Effect::Path && s.orient) orientLost = true;
            if (!raw && s.effect == Effect::Path && s.motionPath.isEmpty()) {
                const auto *guide = slide.find(s.pathId);
                if (!guide || Shape::worldPath(*guide).isEmpty()) { pathless = true; continue; }
            }
            if (!raw && (s.effect == Effect::Move || s.effect == Effect::Scale || s.effect == Effect::Spin)) legacyApproximated = true;
            if (!raw && s.effect == Effect::Float && (s.direction == 1 || s.direction == 2)) floatSideways = true;
            const bool paragraphPart = s.effect == Effect::Reveal && s.unit == 0 && s.rangeFrom >= 0;
            const auto partKey = qMakePair(spid, int(s.phase));
            const int group = raw ? rawGroup(s.pptxXml)
                            : paragraphPart && paragraphRun.contains(partKey) && paragraphRunNext.value(partKey) == s.rangeFrom ? paragraphRun.value(partKey) : freeGroup(spid);
            if (paragraphPart) { paragraphRun[partKey] = group; paragraphRunNext[partKey] = s.rangeTo; } else paragraphRun.remove(partKey);
            if (!raw && s.effect == Effect::Reveal && s.unit == 0 && s.rangeFrom < 0) {
                const int n = qMax(1, paragraphsOf(s.targetId));
                for (int k = 0; k < n; ++k)
                    emits.append({&s, spid, k == 0 ? s.trigger : BuildTrigger::AfterPrevious, k == 0 ? s.delay : 0.0, s.duration / n, k, k + 1, false, group});
                paragraphGroups.insert({spid, group});
                continue;
            }
            if (s.effect == Effect::Reveal && s.unit == 0) paragraphGroups.insert({spid, group});
            // A range past the text's last paragraph reveals the whole text instead.
            emits.append({&s, spid, s.trigger, s.delay, s.duration, rangeGone ? -1 : s.rangeFrom, rangeGone ? -1 : s.rangeTo, raw, group});
        }
        if (floatSideways) warnings.add(QStringLiteral("A float in from the side was written as a float up (PowerPoint floats only up or down)"), number);
        if (legacyApproximated) warnings.add(QStringLiteral("An older move, scale or spin build was written as fly in, zoom or spin"), number);
        if (pathless) warnings.add(QStringLiteral("A build along a path with no path chosen was left out"), number);
        if (keptRangeGone) warnings.add(QStringLiteral("A PowerPoint text animation whose paragraphs are gone was written as OmaShow's own"), number);
        if (orientLost) warnings.add(QStringLiteral("Turning with the path has no PowerPoint equivalent; the object keeps its angle there"), number);
        if (!convertedNames.isEmpty()) {
            convertedNames.removeDuplicates();
            warnings.add(QStringLiteral("PowerPoint effects changed here are written as OmaShow's nearest: %1").arg(convertedNames.join(QStringLiteral(", "))), number);
        }
        QStringList interactive;
        int id = 1;
        if (emits.isEmpty() && slide.pptxInteractive.isEmpty()) return;
        x.start("p:timing");
        x.start("p:tnLst"); x.start("p:par");
        x.start("p:cTn").attr("id", id++).attr("dur", "indefinite").attr("restart", "never").attr("nodeType", "tmRoot");
        x.start("p:childTnLst");
        const bool main = !emits.isEmpty();
        if (main) {
        x.start("p:seq").attr("concurrent", "1").attr("nextAc", "seek");
        x.start("p:cTn").attr("id", id++).attr("dur", "indefinite").attr("nodeType", "mainSeq");
        x.start("p:childTnLst");
        }
        const auto cond = [&](const char *delay) { x.start("p:stCondLst"); x.start("p:cond").attr("delay", delay).end(); x.end(); };
        const auto groupCond = [&](bool automatic) {
            x.start("p:stCondLst");
            x.start("p:cond").attr("delay", "indefinite").end();
            if (automatic) { x.start("p:cond").attr("evt", "onBegin").attr("delay", "0"); x.start("p:tn").attr("val", 2).end(); x.end(); }
            x.end();
        };
        const auto target = [&](int spid, int from, int to) {
            x.start("p:tgtEl"); x.start("p:spTgt").attr("spid", spid);
            if (from >= 0) { x.start("p:txEl"); x.start("p:pRg").attr("st", from).attr("end", qMax(from, to - 1)).end(); x.end(); }
            x.end(); x.end(); };
        // PowerPoint's structure (and OmaShow's Timeline::resolvedSteps): a click
        // opens a group; within it a run starts with the click and with each
        // After previous; With previous joins the run. Delays are the effect's own.
        bool groupOpen = false, runOpen = false, clicked = false;
        qreal sinceClick = 0, runStart = 0;
        for (int i = 0; i < emits.size(); ++i) {
            const auto &em = emits[i];
            const auto &s = *em.step;
            const int spid = em.spid;
            // A first animation that starts by itself starts with the slide.
            const bool automatic = i == 0 && em.trigger != BuildTrigger::OnClick;
            const bool atTime = em.trigger == BuildTrigger::Absolute;
            // At a time = With previous plus a delay (PowerPoint's own form); exact
            // unless a click came before it.
            if (atTime && clicked) timedLater = true;
            const bool newClick = i == 0 || em.trigger == BuildTrigger::OnClick;
            if (newClick && !automatic) clicked = true;
            if (newClick) {
                if (runOpen) { x.end(); x.end(); x.end(); runOpen = false; }
                if (groupOpen) { x.end(); x.end(); x.end(); }
                x.start("p:par"); x.start("p:cTn").attr("id", id++).attr("fill", "hold"); groupCond(automatic); x.start("p:childTnLst");
                groupOpen = true;
                sinceClick = runStart = 0;
            }
            const qreal runAt = newClick ? 0.0 : em.trigger == BuildTrigger::AfterPrevious ? sinceClick : -1;
            if (runAt >= 0) {
                if (runOpen) { x.end(); x.end(); x.end(); }
                x.start("p:par"); x.start("p:cTn").attr("id", id++).attr("fill", "hold"); cond(QByteArray::number(qRound64(runAt * 1000)).constData()); x.start("p:childTnLst");
                runOpen = true; runStart = runAt;
            }
            const qint64 ms = qMax<qint64>(1, qRound64(em.duration * 1000));
            if (atTime && s.start + em.delay < runStart - 1e-6) timedEarly = true;
            const qreal ownDelay = atTime ? qMax<qreal>(0.0, s.start + em.delay - runStart) : em.delay;
            const qint64 delay = qRound64(ownDelay * 1000);
            const char *nodeType = newClick && !automatic ? "clickEffect" : em.trigger == BuildTrigger::AfterPrevious ? "afterEffect" : "withEffect";
            const QString repeat = s.repeat < 0 ? QStringLiteral("indefinite") : s.repeat > 1 ? QString::number(qRound64(s.repeat * 1000)) : QString();
            const qreal span = em.duration * (s.autoReverse ? 2.0 : 1.0) * (s.repeat > 1 ? s.repeat : 1.0);
            sinceClick = qMax(sinceClick, runStart + ownDelay + span);
            if (em.raw) {
                // As PowerPoint wrote it, with today's ids, start, delay, length and repeats.
                QString xml = renumber(placeIds(s.pptxXml, ctx), id);
                const int at = xml.indexOf(QStringLiteral("presetClass="));
                const int tag = at < 0 ? -1 : xml.lastIndexOf(QStringLiteral("<p:cTn"), at);
                if (tag >= 0) {
                    QList<QPair<QString, QString>> set{{QStringLiteral("nodeType"), QString::fromLatin1(nodeType)}, {QStringLiteral("repeatCount"), repeat},
                                                       {QStringLiteral("autoRev"), s.autoReverse ? QStringLiteral("1") : QString()}};
                    // Fill only changes when rewinding was switched on or off.
                    const QString head = xml.mid(tag, xml.indexOf(QLatin1Char('>'), tag) - tag);
                    const bool removes = head.contains(QStringLiteral("fill=\"remove\""));
                    if (s.rewind != removes) set.append({QStringLiteral("fill"), s.rewind ? QStringLiteral("remove") : QStringLiteral("hold")});
                    // The curve, only when it was changed here: the reader plays PowerPoint's
                    // own (effect node, else its first eased part); approximations play ours.
                    QEasingCurve::Type was = QEasingCurve::Linear;
                    int exactA = -1, exactD = -1;   // the file's own amounts, when the curve came from it
                    if (s.effect != Effect::Reveal) {
                        if (!s.pptxName.isEmpty() || s.effect == Effect::Appear) was = Effects::defaultEasing(s.effect);
                        else {
                            static const QRegularExpression ad(QStringLiteral(" (accel|decel)=\"(\\d+)\""));
                            static const QRegularExpression easedTag(QStringLiteral("<p:cTn [^>]*(accel|decel)=\"\\d+\"[^>]*>"));
                            QString where = head;
                            if (!ad.match(head).hasMatch()) {
                                const auto m = easedTag.match(xml, xml.indexOf(QLatin1Char('>'), tag));
                                where = m.hasMatch() ? m.captured(0) : QString();
                            }
                            qreal accel = 0, decel = 0;
                            for (auto it = ad.globalMatch(where); it.hasNext();) { const auto m = it.next(); (m.captured(1) == QLatin1String("accel") ? accel : decel) = m.captured(2).toDouble() / 100000.0; }
                            was = Effects::easingFromPpt(accel, decel);
                            exactA = qRound(accel * 100000); exactD = qRound(decel * 100000);
                        }
                    }
                    int a0 = 0, d0 = 0, a1 = 0, d1 = 0;
                    Effects::pptFromEasing(was, a0, d0); Effects::pptFromEasing(s.easing, a1, d1);
                    if (exactA >= 0) { a0 = exactA; d0 = exactD; }
                    if (s.accel >= 0 && s.decel >= 0) { a1 = qRound(s.accel * 100000); d1 = qRound(s.decel * 100000); }
                    // Unchanged in OmaShow terms (the preset the file's amounts read as): leave the file's own.
                    else if (exactA >= 0 && s.easing == was) { a1 = a0; d1 = d0; }
                    if (a0 != a1 || d0 != d1)
                    {
                        set.append({QStringLiteral("accel"), a1 ? QString::number(a1) : QString()});
                        set.append({QStringLiteral("decel"), d1 ? QString::number(d1) : QString()});
                    }
                    xml = setAttrs(xml, tag, set);
                    const int st = xml.indexOf(QStringLiteral("<p:stCondLst>"), tag), stEnd = xml.indexOf(QStringLiteral("</p:stCondLst>"), tag);
                    const int firstChild = xml.indexOf(QLatin1Char('>'), tag) + 1;
                    const QString own = QStringLiteral("<p:stCondLst><p:cond delay=\"%1\"/></p:stCondLst>").arg(delay);
                    if (st == firstChild && stEnd > st) xml = xml.left(st) + own + xml.mid(stEnd + 14);
                    else xml.insert(firstChild, own);
                    if (s.pptxDuration > 0 && s.effect != Effect::Appear) {
                        const int body = xml.indexOf(QStringLiteral("<p:childTnLst>"), tag);
                        if (body > 0) xml = xml.left(body) + scaleDurations(xml.mid(body), s.duration / s.pptxDuration);
                    }
                }
                x.raw(xml);
                continue;
            }
            const bool out = s.phase == BuildPhase::Out;
            const bool emphasis = Effects::isEmphasis(s.effect);
            const bool motion = s.effect == Effect::Path;
            const bool reveal = s.effect == Effect::Reveal;
            const char *presetClass = motion ? "path" : emphasis ? "emph" : out ? "exit" : "entr";
            const int presetId = motion ? 0 : s.effect == Effect::Rotate ? 8 : s.effect == Effect::Pulse ? 26 : emphasis ? 6
                               : reveal ? (s.duration <= 0 ? 1 : 10) : s.effect == Effect::Appear ? 1 : s.effect == Effect::Fly || s.effect == Effect::Move ? 2
                               : s.effect == Effect::Rise || s.effect == Effect::Float ? (s.effect == Effect::Float && s.direction == 3 ? 47 : 42)
                               : s.effect == Effect::Wipe ? 22 : s.effect == Effect::Scale || s.effect == Effect::Zoom ? 53
                               : s.effect == Effect::Spin || s.effect == Effect::Turn ? 49 : 10;
            static const int subtypes[4] = {4, 8, 2, 1};   // from bottom, left, right, top
            // Zoom is PowerPoint's "Zoom, In" (subtype 16).
            const int subtype = s.effect == Effect::Fly || s.effect == Effect::Wipe ? subtypes[qBound(0, s.direction, 3)]
                              : presetId == 53 ? 16 : 0;
            x.start("p:par");
            x.start("p:cTn").attr("id", id++).attr("presetID", presetId).attr("presetClass", presetClass).attr("presetSubtype", subtype);
            {
                // The step's curve as PowerPoint's Smooth start / Smooth end.
                int accel = 0, decel = 0;
                Effects::pptFromEasing(s.easing, accel, decel);
                if (s.accel >= 0 && s.decel >= 0) { accel = qRound(s.accel * 100000); decel = qRound(s.decel * 100000); }
                if (accel) x.attr("accel", accel);
                if (decel) x.attr("decel", decel);
            }
            if (!repeat.isEmpty()) x.attr("repeatCount", repeat);
            if (s.autoReverse) x.attr("autoRev", "1");
            x.attr("fill", s.rewind ? "remove" : "hold").attr("grpId", em.group).attr("nodeType", nodeType);
            cond(QByteArray::number(delay).constData());
            const int from = reveal && s.unit == 0 ? em.from : -1, to = em.to;
            qint64 each = ms;
            if (reveal && s.unit != 0) {
                // By word or letter: PowerPoint iterates; each unit its share, one
                // after another (the iterate element comes before childTnLst).
                const QString text = [&]{ for (const auto &o : slide.objects) if (o.id == s.targetId) return o.text; return QString(); }();
                const int n = qMax(1, s.unit == 2 ? int(text.size()) : int(text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size()));
                each = qMax<qint64>(1, qRound64(em.duration * 1000 / n));
                x.start("p:iterate").attr("type", s.unit == 2 ? "lt" : "wd");
                if (s.duration <= 0) x.start("p:tmAbs").attr("val", 0).end(); else x.start("p:tmPct").attr("val", 100000).end();
                x.end();
            }
            x.start("p:childTnLst");
            if (reveal && s.unit != 0) {
                if (!out) {
                    x.start("p:set"); x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", 1).attr("fill", "hold"); cond("0"); x.end(); target(spid, -1, -1);
                    x.start("p:attrNameLst"); x.start("p:attrName").text(QStringLiteral("style.visibility")).end(); x.end(); x.end();
                    x.start("p:to"); x.start("p:strVal").attr("val", "visible").end(); x.end(); x.end();
                }
                if (s.duration > 0) {
                    x.start("p:animEffect").attr("transition", out ? "out" : "in").attr("filter", "fade");
                    x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", each).end(); target(spid, -1, -1); x.end(); x.end();
                }
                if (out) {
                    x.start("p:set"); x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", 1).attr("fill", "hold"); cond(QByteArray::number(qMax<qint64>(0, each - 1)).constData()); x.end(); target(spid, -1, -1);
                    x.start("p:attrNameLst"); x.start("p:attrName").text(QStringLiteral("style.visibility")).end(); x.end(); x.end();
                    x.start("p:to"); x.start("p:strVal").attr("val", "hidden").end(); x.end(); x.end();
                }
            } else if (motion) {
                QString path = s.motionPath;
                if (path.isEmpty()) {
                    // A guide object's outline: written from where the object starts
                    // (writeSlide places it there) to where it lands.
                    QPainterPath route = Shape::worldPath(*slide.find(s.pathId));
                    if (s.pathReverse) route = route.toReversed();
                    path = MotionPath::write(route, route.pointAtPercent(0), doc.size);
                }
                x.start("p:animMotion").attr("origin", "layout").attr("path", path).attr("pathEditMode", "relative").attr("ptsTypes", "");
                x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", ms).attr("fill", "hold").end(); target(spid, -1, -1);
                x.start("p:attrNameLst"); x.start("p:attrName").text(QStringLiteral("ppt_x")).end(); x.start("p:attrName").text(QStringLiteral("ppt_y")).end(); x.end(); x.end();
                x.end();
            } else if (s.effect == Effect::Grow || s.effect == Effect::Shrink) {
                const qreal by = s.amount > 0 ? s.amount : s.effect == Effect::Grow ? 1.25 : 0.8;
                x.start("p:animScale"); x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", ms).attr("fill", "hold").end(); target(spid, -1, -1); x.end();
                x.start("p:by").attr("x", qRound(by * 100000)).attr("y", qRound(by * 100000)).end();
                x.start("p:to").attr("x", qRound(by * 100000)).attr("y", qRound(by * 100000)).end();
                x.end();
            } else if (s.effect == Effect::Pulse) {
                // PowerPoint's Pulse (26) swells and settles; OmaShow's swell.
                const int swell = qRound(100000 + qMax(0.01, s.amount > 0 ? s.amount : 0.15) * 100000);
                x.start("p:animScale"); x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", qMax<qint64>(1, ms / 2)).attr("autoRev", "1").attr("fill", "hold").end(); target(spid, -1, -1); x.end();
                x.start("p:by").attr("x", swell).attr("y", swell).end();
                x.end();
            } else if (s.effect == Effect::Rotate) {
                x.start("p:animRot").attr("by", qRound64((s.amount != 0 ? s.amount : 360) * 60000));
                x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", ms).attr("fill", "hold").end(); target(spid, -1, -1);
                x.start("p:attrNameLst"); x.start("p:attrName").text(QStringLiteral("r")).end(); x.end(); x.end(); x.end();
            } else {
                if (!out) {
                    x.start("p:set"); x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", 1).attr("fill", "hold"); cond("0"); x.end(); target(spid, from, to);
                    x.start("p:attrNameLst"); x.start("p:attrName").text(QStringLiteral("style.visibility")).end(); x.end(); x.end();
                    x.start("p:to"); x.start("p:strVal").attr("val", "visible").end(); x.end(); x.end();
                }
                // Fly travels without fading and Appear has no transition at all.
                static const char *wipes[4] = {"wipe(down)", "wipe(left)", "wipe(right)", "wipe(up)"};
                const char *filter = s.effect == Effect::Wipe ? wipes[qBound(0, s.direction, 3)]
                                   : s.effect == Effect::Fly || s.effect == Effect::Appear || (reveal && s.duration <= 0) ? nullptr : "fade";
                if (filter) {
                    x.start("p:animEffect").attr("transition", out ? "out" : "in").attr("filter", filter);
                    x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", ms).end(); target(spid, from, to); x.end(); x.end();
                }
                const auto keyframes = [&](const char *axis, const QString &away) {
                    const QString rest = QStringLiteral("#") + QString::fromLatin1(axis);
                    x.start("p:anim").attr("calcmode", "lin").attr("valueType", "num");
                    x.start("p:cBhvr").attr("additive", "base"); x.start("p:cTn").attr("id", id++).attr("dur", ms).attr("fill", "hold").end(); target(spid, -1, -1);
                    x.start("p:attrNameLst"); x.start("p:attrName").text(QString::fromLatin1(axis)).end(); x.end(); x.end();
                    x.start("p:tavLst");
                    x.start("p:tav").attr("tm", 0); x.start("p:val"); x.start("p:strVal").attr("val", out ? rest : away).end(); x.end(); x.end();
                    x.start("p:tav").attr("tm", 100000); x.start("p:val"); x.start("p:strVal").attr("val", out ? away : rest).end(); x.end(); x.end();
                    x.end(); x.end();
                };
                if (s.effect == Effect::Fly) {
                    const int d = qBound(0, s.direction, 3);
                    if (d == 0) keyframes("ppt_y", QStringLiteral("1+#ppt_h/2"));
                    else if (d == 3) keyframes("ppt_y", QStringLiteral("0-#ppt_h/2"));
                    else if (d == 1) keyframes("ppt_x", QStringLiteral("0-#ppt_w/2"));
                    else keyframes("ppt_x", QStringLiteral("1+#ppt_w/2"));
                } else if (s.effect == Effect::Float) {
                    // PowerPoint's Float In drifts a tenth of the slide height.
                    const int d = qBound(0, s.direction, 3);
                    const bool xAxis = d == 1 || d == 2;
                    const qreal rel = d == 1 || d == 3 ? -0.1 : 0.1;
                    keyframes(xAxis ? "ppt_x" : "ppt_y", QStringLiteral("#") + (xAxis ? "ppt_x" : "ppt_y") + QStringLiteral("+") + QString::number(rel, 'f', 4));
                } else if (s.effect == Effect::Zoom) {
                    const int from = qRound((s.amount > 0 && s.amount < 1 ? s.amount : 0.3) * 100000);
                    x.start("p:animScale"); x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", ms).attr("fill", "hold").end(); target(spid, -1, -1); x.end();
                    x.start("p:from").attr("x", out ? 100000 : from).attr("y", out ? 100000 : from).end();
                    x.start("p:to").attr("x", out ? from : 100000).attr("y", out ? from : 100000).end();
                    x.end();
                } else if (s.effect == Effect::Turn) {
                    x.start("p:animRot").attr("by", qRound64((s.amount != 0 ? s.amount : 360) * 60000 * (out ? 1 : -1)));
                    x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", ms).attr("fill", "hold").end(); target(spid, -1, -1);
                    x.start("p:attrNameLst"); x.start("p:attrName").text(QStringLiteral("r")).end(); x.end(); x.end(); x.end();
                } else if (s.effect == Effect::Rise || s.effect == Effect::Move) {
                    const qreal fromX = s.effect == Effect::Move ? s.amountX : 0, fromY = s.effect == Effect::Move ? s.amountY : doc.size.height() * 0.1;
                    for (const auto *axis : {"ppt_x", "ppt_y"}) {
                        const bool xAxis = QLatin1String(axis) == QLatin1String("ppt_x");
                        const qreal offset = (xAxis ? fromX : fromY) / (xAxis ? doc.size.width() : doc.size.height());
                        if (qFuzzyIsNull(offset)) continue;
                        const QString rest = QStringLiteral("#") + QString::fromLatin1(axis);
                        keyframes(axis, rest + QStringLiteral("+") + QString::number(offset, 'f', 4));
                    }
                } else if (s.effect == Effect::Scale) {
                    x.start("p:animScale"); x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", ms).attr("fill", "hold").end(); target(spid, -1, -1); x.end();
                    x.start("p:from").attr("x", out ? 100000 : 10000).attr("y", out ? 100000 : 10000).end();
                    x.start("p:to").attr("x", out ? 10000 : 100000).attr("y", out ? 10000 : 100000).end();
                    x.end();
                } else if (s.effect == Effect::Spin) {
                    x.start("p:animRot").attr("by", qRound64((s.amount > 0 ? s.amount : 360) * 60000));
                    x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", ms).attr("fill", "hold").end(); target(spid, -1, -1);
                    x.start("p:attrNameLst"); x.start("p:attrName").text(QStringLiteral("r")).end(); x.end(); x.end(); x.end();
                }
                if (out) {
                    x.start("p:set"); x.start("p:cBhvr"); x.start("p:cTn").attr("id", id++).attr("dur", 1).attr("fill", "hold"); cond(QByteArray::number(qint64(ms - 1)).constData()); x.end(); target(spid, from, to);
                    x.start("p:attrNameLst"); x.start("p:attrName").text(QStringLiteral("style.visibility")).end(); x.end(); x.end();
                    x.start("p:to"); x.start("p:strVal").attr("val", "hidden").end(); x.end(); x.end();
                }
            }
            x.end(); // childTnLst
            x.end(); // cTn
            x.end(); // par (effect)
        }
        if (runOpen) { x.end(); x.end(); x.end(); }
        if (groupOpen) { x.end(); x.end(); x.end(); }
        if (timedEarly) warnings.add(QStringLiteral("An animation set to start at a time earlier than the one before it ends was written to start with it"), number);
        if (timedLater) warnings.add(QStringLiteral("An animation set to start at a time after a click was written as With previous, timed from that click"), number);
        if (main) {
        x.end(); // childTnLst of mainSeq
        x.end(); // cTn mainSeq
        x.start("p:prevCondLst"); x.start("p:cond").attr("evt", "onPrev").attr("delay", "0"); x.start("p:tgtEl"); x.empty("p:sldTgt"); x.end(); x.end(); x.end();
        x.start("p:nextCondLst"); x.start("p:cond").attr("evt", "onNext").attr("delay", "0"); x.start("p:tgtEl"); x.empty("p:sldTgt"); x.end(); x.end(); x.end();
        x.end(); // seq
        }
        // PowerPoint triggers kept from the import, with today's ids and their own build groups.
        for (const auto &kept : slide.pptxInteractive) {
            QString xml = placeIds(kept, ctx);
            if (xml.isEmpty()) { warnings.add(QStringLiteral("A PowerPoint trigger animation on an object that is gone was left out"), number); continue; }
            xml = renumber(xml, id);
            x.raw(xml);
        }
        x.end(); // childTnLst root
        x.end(); // cTn root
        x.end(); // par
        x.end(); // tnLst
        // One entry per shape and build group, as PowerPoint lists them: text by
        // paragraph says so; a plain shape animates with its background.
        // Kept effects say what their entry was; ordered by shape, then group, as PowerPoint does.
        QVector<QPair<int, int>> builds;
        for (const auto &em : emits) if (em.group >= 0 && !builds.contains(QPair<int, int>(em.spid, em.group)) && (!em.raw || keptBuild.contains({em.spid, em.group}))) builds.append({em.spid, em.group});
        for (const auto &g : triggerGroups) if (!builds.contains(g) && keptBuild.contains(g)) builds.append(g);
        std::sort(builds.begin(), builds.end());
        QString list;
        for (const auto &[sp, g] : builds) {
            QString attrs;
            if (keptBuild.contains({sp, g})) attrs = keptBuild.value({sp, g});
            else if (kindOf.value(sp, 2) == 2) continue;
            else if (paragraphGroups.contains({sp, g})) attrs = QStringLiteral("build=\"p\"");
            else if (kindOf.value(sp) == 0) attrs = QStringLiteral("animBg=\"1\"");
            list += QStringLiteral("<p:bldP spid=\"%1\" grpId=\"%2\"%3/>").arg(sp).arg(g).arg(attrs.isEmpty() ? QString() : QLatin1Char(' ') + attrs);
        }
        if (!list.isEmpty()) x.raw(QStringLiteral("<p:bldLst>") + list + QStringLiteral("</p:bldLst>"));
        x.end(); // timing
    }

    // ---- slides

    void writeSlide(int index) {
        const auto &slide = doc.slides[index];
        const int number = index + 1;
        const auto path = QStringLiteral("ppt/slides/slide%1.xml").arg(number);
        Rels rels;
        const auto *layout = Design::layout(doc, slide.layoutId);
        int layoutNumber = layoutIndex.value(slide.layoutId, 0);
        if (!layoutNumber) layoutNumber = layoutIndex.isEmpty() ? 1 : layoutIndex.cbegin().value();
        rels.add(relBase + QStringLiteral("slideLayout"), QStringLiteral("../slideLayouts/slideLayout%1.xml").arg(layoutNumber));
        Q_UNUSED(layout);

        const Slide resolved = Design::resolve(doc, index);
        Xml x;
        x.start("p:sld").attr("xmlns:a", nsA).attr("xmlns:r", nsR).attr("xmlns:p", nsP);
        if (slide.skipped) x.attr("show", "0");
        if (!slide.showMasterObjects) x.attr("showMasterSp", "0");
        x.start("p:cSld");
        writeBackground(x, resolved.background);
        startTree(x);
        SlideCtx ctx; ctx.partPath = path; ctx.rels = &rels; ctx.number = number;
        QVector<SceneObject> objects;
        // ANIM-INTEROP: PowerPoint moves an object along a path from where it is;
        // an OmaShow guide path lands it where it is. So an object whose first
        // path follows a guide is written where that path starts.
        QHash<QString, QPointF> pathStart;
        for (const auto &s : slide.timeline.steps) {
            if (s.effect != Effect::Path || !s.motionPath.isEmpty() || pathStart.contains(s.targetId) ||
                (!s.pptxXml.isEmpty() && s.pptxSig == s.interopSig())) continue;
            const auto *guide = slide.find(s.pathId);
            if (!guide) continue;
            const QPainterPath route = Shape::worldPath(*guide);
            if (route.isEmpty()) continue;
            pathStart.insert(s.targetId, route.pointAtPercent(s.pathReverse ? 1.0 : 0.0) - route.pointAtPercent(s.pathReverse ? 0.0 : 1.0));
        }
        for (auto o : resolved.objects) {
            // The master's own shapes are on the master; fields are written as words.
            if (o.id.startsWith(QLatin1String("@master/"))) continue;
            if (pathStart.contains(o.id)) o.rect.translate(pathStart.value(o.id));
            objects.append(o);
        }
        writeObjects(x, objects, ctx);
        x.end(); // spTree
        x.end(); // cSld
        x.start("p:clrMapOvr"); x.empty("a:masterClrMapping"); x.end();
        writeTransition(x, index, slide);
        writeBuilds(x, number, slide, ctx);
        x.end();
        if (!slide.notes.trimmed().isEmpty()) {
            writeNotes(number, slide.notes);
            rels.add(relBase + QStringLiteral("notesSlide"), QStringLiteral("../notesSlides/notesSlide%1.xml").arg(number));
        }
        writeComments(number, slide, rels);
        part(path, x.finish(), "application/vnd.openxmlformats-officedocument.presentationml.slide+xml");
        part(relsPath(path), rels.bytes(), nullptr);
    }

    // ---- the deck

    QByteArray run() {
        Document copy = doc;
        Q_UNUSED(copy);
        // Masters and layouts, numbered; every layout belongs to a master.
        QVector<QVector<int>> layoutsOfMaster(doc.masters.size());
        int layoutNumber = 0;
        for (const auto &layout : doc.layouts) {
            int master = 0;
            for (int i = 0; i < doc.masters.size(); ++i) if (doc.masters[i].id == layout.masterId) master = i;
            ++layoutNumber;
            layoutIndex.insert(layout.id, layoutNumber);
            if (!doc.masters.isEmpty()) layoutsOfMaster[master].append(layoutNumber);
        }
        if (doc.masters.isEmpty()) {
            Master plain;
            plain.id = QStringLiteral("plain");
            plain.name = QStringLiteral("Plain");
            plain.background = doc.theme.colors.value(QStringLiteral("background"), QColor(Qt::white));
            plain.backgroundToken.clear();
            QVector<int> mine;
            if (doc.layouts.isEmpty()) {
                SlideLayout blank; blank.id = QStringLiteral("blank"); blank.name = QStringLiteral("Blank"); blank.masterId = plain.id;
                writeLayout(blank, 1, 1);
                layoutIndex.insert(blank.id, 1);
                mine.append(1);
            } else for (int i = 1; i <= doc.layouts.size(); ++i) mine.append(i);
            writeMaster(plain, 1, mine);
        } else {
            for (int i = 0; i < doc.masters.size(); ++i) {
                if (layoutsOfMaster[i].isEmpty()) {
                    // PowerPoint needs a layout under every master.
                    SlideLayout blank; blank.id = QStringLiteral("blank-") + doc.masters[i].id; blank.name = QStringLiteral("Blank"); blank.masterId = doc.masters[i].id;
                    writeLayout(blank, ++layoutNumber, i + 1);
                    layoutIndex.insert(blank.id, layoutNumber);
                    layoutsOfMaster[i].append(layoutNumber);
                }
                writeMaster(doc.masters[i], i + 1, layoutsOfMaster[i]);
            }
        }
        for (const auto &layout : doc.layouts) {
            int master = 0;
            for (int i = 0; i < doc.masters.size(); ++i) if (doc.masters[i].id == layout.masterId) master = i;
            writeLayout(layout, layoutIndex.value(layout.id), master + 1);
        }
        part(QStringLiteral("ppt/theme/theme1.xml"), themeXml(doc.theme.name.isEmpty() ? QStringLiteral("OmaShow") : doc.theme.name), "application/vnd.openxmlformats-officedocument.theme+xml");
        part(QStringLiteral("ppt/theme/theme2.xml"), themeXml(QStringLiteral("Notes")), "application/vnd.openxmlformats-officedocument.theme+xml");
        writeNotesMaster();

        for (int i = 0; i < doc.slides.size(); ++i) {
            if (canceled()) return QByteArray();
            writeSlide(i);
        }
        writeAuthors();

        // The presentation part ties it together.
        Rels rels;
        Xml x;
        x.start("p:presentation").attr("xmlns:a", nsA).attr("xmlns:r", nsR).attr("xmlns:p", nsP).attr("saveSubsetFonts", "1");
        x.start("p:sldMasterIdLst");
        const int masterCount = doc.masters.isEmpty() ? 1 : doc.masters.size();
        for (int i = 1; i <= masterCount; ++i) {
            const auto rel = rels.add(relBase + QStringLiteral("slideMaster"), QStringLiteral("slideMasters/slideMaster%1.xml").arg(i));
            x.start("p:sldMasterId").attr("id", 2147483648LL + i - 1).attr("r:id", rel).end();
        }
        x.end();
        x.start("p:notesMasterIdLst");
        x.start("p:notesMasterId").attr("r:id", rels.add(relBase + QStringLiteral("notesMaster"), QStringLiteral("notesMasters/notesMaster1.xml"))).end();
        x.end();
        QStringList slideRels;
        x.start("p:sldIdLst");
        for (int i = 0; i < doc.slides.size(); ++i) {
            const auto rel = rels.add(relBase + QStringLiteral("slide"), QStringLiteral("slides/slide%1.xml").arg(i + 1));
            slideRels.append(rel);
            x.start("p:sldId").attr("id", 256 + i).attr("r:id", rel).end();
        }
        x.end();
        x.start("p:sldSz").attr("cx", cx).attr("cy", cy).end();
        x.start("p:notesSz").attr("cx", 6858000).attr("cy", 9144000).end();
        if (!doc.shows.isEmpty()) {
            x.start("p:custShowLst");
            int showId = 0;
            for (const auto &show : doc.shows) {
                x.start("p:custShow").attr("name", show.name).attr("id", showId++);
                x.start("p:sldLst");
                for (const auto &slideId : show.slideIds)
                    for (int i = 0; i < doc.slides.size(); ++i)
                        if (doc.slides[i].id == slideId) x.start("p:sld").attr("r:id", slideRels[i]).end();
                x.end(); x.end();
            }
            x.end();
        }
        x.start("p:defaultTextStyle");
        x.start("a:defPPr"); x.start("a:defRPr").attr("lang", "en-GB").end(); x.end();
        x.start("a:lvl1pPr").attr("marL", 0).attr("algn", "l"); x.start("a:defRPr").attr("sz", 1800); x.start("a:latin").attr("typeface", "+mn-lt").end(); x.end(); x.end();
        x.end();
        if (!doc.sections.isEmpty()) {
            // Every slide belongs somewhere; the ones before any section share a nameless one.
            struct Group { QString name; QVector<int> slides; };
            QVector<Group> groups;
            for (int i = 0; i < doc.slides.size(); ++i) {
                QString name;
                for (const auto &s : doc.sections) if (s.id == doc.slides[i].sectionId) name = s.name;
                if (groups.isEmpty() || groups.last().name != name) groups.append({name, {}});
                groups.last().slides.append(i);
            }
            x.start("p:extLst"); x.start("p:ext").attr("uri", "{521415D9-36F7-43E2-AB2F-B90AF26B5E84}");
            x.start("p14:sectionLst").attr("xmlns:p14", nsP14);
            for (const auto &g : groups) {
                x.start("p14:section").attr("name", g.name.isEmpty() ? QStringLiteral("Untitled Section") : g.name).attr("id", QUuid::createUuid().toString(QUuid::WithBraces).toUpper());
                x.start("p14:sldIdLst");
                for (int i : g.slides) x.start("p14:sldId").attr("id", 256 + i).end();
                x.end(); x.end();
            }
            x.end(); x.end(); x.end();
        }
        x.end();
        if (!authors.isEmpty()) rels.add(relBase + QStringLiteral("commentAuthors"), QStringLiteral("commentAuthors.xml"));
        rels.add(relBase + QStringLiteral("theme"), QStringLiteral("theme/theme1.xml"));
        part(QStringLiteral("ppt/presentation.xml"), x.finish(), "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml");
        part(relsPath(QStringLiteral("ppt/presentation.xml")), rels.bytes(), nullptr);

        Rels root;
        root.add(relBase + QStringLiteral("officeDocument"), QStringLiteral("ppt/presentation.xml"));
        entries.prepend({QStringLiteral("_rels/.rels"), root.bytes(), true});

        QByteArray types = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                           "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                           "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                           "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
        for (const auto &line : overrides) types += line.toUtf8();
        types += "</Types>";
        entries.prepend({QStringLiteral("[Content_Types].xml"), types, true});
        return Zip::write(entries);
    }
};

} // namespace

namespace PptxWriter {

QByteArray bytes(const Document &document, QStringList *warnings, const std::shared_ptr<Workers::Job> &job) {
    Writer writer(document);
    writer.job = job;
    const auto out = writer.run();
    if (warnings) *warnings = writer.warnings.lines();
    return out;
}

Report write(const Document &document, const QString &path, const std::shared_ptr<Workers::Job> &job) {
    Report report;
    if (document.slides.isEmpty()) { report.error = QStringLiteral("The deck has no slides to write."); return report; }
    QStringList warnings;
    const auto data = bytes(document, &warnings, job);
    if (job && job->canceled) { report.error = QStringLiteral("Cancelled."); return report; }
    if (data.isEmpty()) { report.error = QStringLiteral("The deck could not be written as PowerPoint."); return report; }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        report.error = QStringLiteral("%1 could not be written.").arg(QFileInfo(path).fileName());
        return report;
    }
    report.ok = true;
    report.bytes = data.size();
    report.lines = warnings;
    report.lines.prepend(QStringLiteral("Wrote %1 slides as PowerPoint.").arg(document.slides.size()));
    // The deck names its typefaces and cannot carry them: say which, so the
    // person it goes to can be told.
    const auto families = Fonts::families(document);
    if (!families.isEmpty()) report.lines.append(QStringLiteral("It asks for the typefaces %1; anyone opening it needs them installed to see it as designed.").arg(families.keys().join(QStringLiteral(", "))));
    return report;
}

} // namespace PptxWriter
