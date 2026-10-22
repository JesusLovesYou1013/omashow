#include "io/odp.h"

#include "core/edit.h"
#include "core/imageasset.h"
#include "core/link.h"
#include "core/mediaasset.h"
#include "core/shape.h"
#include "core/svgasset.h"
#include "core/table.h"
#include "core/textruns.h"
#include "io/metafile.h"
#include "io/zip.h"
#include "render/textlayout.h"

#include <QBuffer>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTransform>
#include <QUrl>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QtMath>

#include <cmath>
#include <functional>
#include <map>

namespace {

// ---------------------------------------------------------------------------
// A tree of the XML that keeps text where it stands among the elements (an
// ODF paragraph mixes words, spans, spaces and tabs in order). Names carry the
// conventional prefix for their namespace whatever prefix the file used.

struct El {
    QString tag;                     // "draw:frame"; empty for words
    QHash<QString, QString> at;      // "svg:x" → "2cm"
    QVector<El> kids;
    QString text;                    // the words of a text node

    bool isText() const { return tag.isEmpty(); }
    const El *child(const QString &name) const {
        for (const auto &k : kids) if (k.tag == name) return &k;
        return nullptr;
    }
    const El *child(const char *name) const { return child(QLatin1String(name)); }
    QVector<const El *> all(const char *name) const {
        QVector<const El *> out;
        for (const auto &k : kids) if (k.tag == QLatin1String(name)) out.append(&k);
        return out;
    }
    QString a(const char *name, const QString &fallback = QString()) const { return at.value(QLatin1String(name), fallback); }
    bool has(const char *name) const { return at.contains(QLatin1String(name)); }
    // Every word below, as written.
    QString words() const {
        if (isText()) return text;
        QString out;
        for (const auto &k : kids) out += k.words();
        return out;
    }
    void collect(const char *name, QVector<const El *> &out) const {
        for (const auto &k : kids) {
            if (k.tag == QLatin1String(name)) out.append(&k);
            k.collect(name, out);
        }
    }
};

QString prefixOf(QStringView uri) {
    static const QHash<QString, QString> known = {
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:office:1.0"), QStringLiteral("office")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:style:1.0"), QStringLiteral("style")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:text:1.0"), QStringLiteral("text")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:table:1.0"), QStringLiteral("table")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:drawing:1.0"), QStringLiteral("draw")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0"), QStringLiteral("fo")},
        {QStringLiteral("http://www.w3.org/1999/xlink"), QStringLiteral("xlink")},
        {QStringLiteral("http://purl.org/dc/elements/1.1/"), QStringLiteral("dc")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:meta:1.0"), QStringLiteral("meta")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0"), QStringLiteral("number")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0"), QStringLiteral("svg")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:chart:1.0"), QStringLiteral("chart")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:dr3d:1.0"), QStringLiteral("dr3d")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:form:1.0"), QStringLiteral("form")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:presentation:1.0"), QStringLiteral("presentation")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:smil-compatible:1.0"), QStringLiteral("smil")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:animation:1.0"), QStringLiteral("anim")},
        {QStringLiteral("http://openoffice.org/2009/office"), QStringLiteral("officeooo")},
        {QStringLiteral("http://openoffice.org/2010/draw"), QStringLiteral("drawooo")},
        {QStringLiteral("urn:org:documentfoundation:names:experimental:office:xmlns:loext:1.0"), QStringLiteral("loext")},
        {QStringLiteral("http://www.w3.org/XML/1998/namespace"), QStringLiteral("xml")},
        {QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:manifest:1.0"), QStringLiteral("manifest")},
    };
    return known.value(uri.toString(), QStringLiteral("?"));
}

QString qualified(QStringView uri, QStringView local) {
    if (uri.isEmpty()) return local.toString();
    return prefixOf(uri) + QLatin1Char(':') + local;
}

// Words matter only inside text elements; elsewhere whitespace is layout.
bool keepsWords(const QString &tag) {
    return tag.startsWith(QLatin1String("text:")) || tag == QLatin1String("dc:creator") || tag == QLatin1String("dc:date") ||
           tag == QLatin1String("office:binary-data") || tag == QLatin1String("svg:title") || tag == QLatin1String("svg:desc") ||
           tag.startsWith(QLatin1String("presentation:")) || tag == QLatin1String("meta:generator");
}

bool parseXml(const QByteArray &bytes, El *root, QString *error) {
    QXmlStreamReader xml(bytes);
    QVector<El *> stack;
    *root = El();
    bool started = false;
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::StartElement: {
            if (stack.size() > 256) { if (error) *error = QStringLiteral("nested too deeply"); return false; }
            El *node;
            if (!started) { node = root; started = true; }
            else { stack.last()->kids.append(El()); node = &stack.last()->kids.last(); }
            node->tag = qualified(xml.namespaceUri(), xml.name());
            for (const auto &attribute : xml.attributes())
                node->at.insert(qualified(attribute.namespaceUri(), attribute.name()), attribute.value().toString());
            stack.append(node);
            break;
        }
        case QXmlStreamReader::EndElement:
            if (!stack.isEmpty()) stack.removeLast();
            break;
        case QXmlStreamReader::Characters:
            if (!stack.isEmpty() && (keepsWords(stack.last()->tag) || !xml.isWhitespace())) {
                auto &kids = stack.last()->kids;
                if (!kids.isEmpty() && kids.last().isText()) kids.last().text += xml.text();
                else { El words; words.text = xml.text().toString(); kids.append(words); }
            }
            break;
        default: break;
        }
    }
    if (xml.hasError()) {
        if (error) *error = xml.errorString();
        return false;
    }
    return started;
}

// Pictures drawn in Inkscape carry the editor's own notes (its namespaces)
// and blur filters, which OmaShow's SVG check refuses and its renderer would
// not draw. The drawing without them, for that check to see as usual; the
// bytes as they were when they cannot be read.
QByteArray plainSvg(const QByteArray &bytes) {
    static const QString svgNs = QStringLiteral("http://www.w3.org/2000/svg");
    static const QString xlinkNs = QStringLiteral("http://www.w3.org/1999/xlink");
    static const QString xmlNs = QStringLiteral("http://www.w3.org/XML/1998/namespace");
    QXmlStreamReader in(bytes);
    QByteArray out;
    QXmlStreamWriter w(&out);
    bool root = true;
    while (!in.atEnd()) {
        const auto token = in.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) return bytes;
        if (token == QXmlStreamReader::StartDocument) w.writeStartDocument();
        else if (token == QXmlStreamReader::StartElement) {
            const QString ns = in.namespaceUri().toString(), name = in.name().toString();
            if ((!ns.isEmpty() && ns != svgNs) || name == QLatin1String("filter") || name == QLatin1String("metadata")) {
                in.skipCurrentElement();
                continue;
            }
            if (root) { w.writeDefaultNamespace(svgNs); w.writeNamespace(xlinkNs, QStringLiteral("xlink")); }
            w.writeStartElement(svgNs, name);
            root = false;
            for (const auto &a : in.attributes()) {
                const QString ans = a.namespaceUri().toString(), an = a.name().toString();
                if (an == QLatin1String("filter")) continue;
                if (ans.isEmpty()) {
                    QString value = a.value().toString();
                    if (an == QLatin1String("style")) {
                        static const QRegularExpression filter(QStringLiteral("(^|;)\\s*filter\\s*:[^;]*"));
                        value.remove(filter);
                    }
                    w.writeAttribute(an, value);
                } else if (ans == xlinkNs || ans == xmlNs) w.writeAttribute(ans, an, a.value().toString());
            }
        } else if (token == QXmlStreamReader::EndElement) w.writeEndElement();
        else if (token == QXmlStreamReader::Characters) {
            if (in.isCDATA()) w.writeCDATA(in.text().toString()); else w.writeCharacters(in.text().toString());
        } else if (token == QXmlStreamReader::EndDocument) w.writeEndDocument();
    }
    return in.hasError() ? bytes : out;
}

// ---------------------------------------------------------------------------
// Values.

// A length in centimetres; NaN when it is none.
qreal centimetres(const QString &value) {
    static const QRegularExpression re(QStringLiteral("^\\s*(-?[0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?)\\s*([a-z%]*)\\s*$"));
    const auto m = re.match(value);
    if (!m.hasMatch()) return qQNaN();
    const qreal n = m.captured(1).toDouble();
    const QString unit = m.captured(2);
    if (unit == QLatin1String("cm")) return n;
    if (unit == QLatin1String("mm")) return n / 10;
    if (unit == QLatin1String("in") || unit == QLatin1String("inch")) return n * 2.54;
    if (unit == QLatin1String("pt")) return n * 2.54 / 72;
    if (unit == QLatin1String("pc")) return n * 2.54 / 6;
    if (unit == QLatin1String("px")) return n * 2.54 / 96;
    if (unit.isEmpty()) return n / 1000;   // 1/100 mm, as in old files
    return qQNaN();
}

qreal percentOf(const QString &value, qreal fallback) {
    QString v = value.trimmed();
    if (!v.endsWith(QLatin1Char('%'))) return fallback;
    v.chop(1);
    bool ok = false;
    const qreal n = v.toDouble(&ok);
    return ok ? n / 100.0 : fallback;
}

// An angle in degrees: "45deg", "0.5rad", "50grad", or bare (tenths of a
// degree in LibreOffice's gradients, degrees elsewhere).
qreal degrees(const QString &value, bool bareTenths) {
    QString v = value.trimmed();
    bool ok = false;
    if (v.endsWith(QLatin1String("deg"))) { v.chop(3); return v.toDouble(); }
    if (v.endsWith(QLatin1String("grad"))) { v.chop(4); return v.toDouble() * 0.9; }
    if (v.endsWith(QLatin1String("rad"))) { v.chop(3); return qRadiansToDegrees(v.toDouble()); }
    const qreal n = v.toDouble(&ok);
    return ok ? (bareTenths ? n / 10.0 : n) : 0;
}

QColor colourOf(const QString &value) {
    if (value.isEmpty() || value == QLatin1String("transparent")) return {};
    QColor c(value.trimmed());
    return c.isValid() ? c : QColor();
}

bool truthy(const QString &value) { return value == QLatin1String("true") || value == QLatin1String("1"); }

// "PT00H00M04.5S", "PT4S", "P0D"…
qreal isoSeconds(const QString &value) {
    static const QRegularExpression re(QStringLiteral("^P(?:([0-9.]+)D)?(?:T(?:([0-9.]+)H)?(?:([0-9.]+)M)?(?:([0-9.]+)S)?)?$"));
    const auto m = re.match(value.trimmed());
    if (!m.hasMatch()) return -1;
    return m.captured(1).toDouble() * 86400 + m.captured(2).toDouble() * 3600 + m.captured(3).toDouble() * 60 + m.captured(4).toDouble();
}

// "2.5s", "500ms", "0:00:02.5"
qreal smilSeconds(const QString &value) {
    QString v = value.trimmed();
    if (v.endsWith(QLatin1String("ms"))) { v.chop(2); return v.toDouble() / 1000.0; }
    if (v.endsWith(QLatin1Char('s'))) { v.chop(1); return v.toDouble(); }
    if (v.contains(QLatin1Char(':'))) {
        qreal total = 0;
        for (const auto &part : v.split(QLatin1Char(':'))) total = total * 60 + part.toDouble();
        return total;
    }
    bool ok = false;
    const qreal n = v.toDouble(&ok);
    return ok ? n : -1;
}

using Props = QHash<QString, QString>;

// Layers properties, nearer last; a font size given as a percentage is of the
// size underneath it.
void layer(Props &into, const Props &over) {
    // A colour of its own outranks "the window's text colour" from further down.
    if (over.contains(QStringLiteral("fo:color")) && !over.contains(QStringLiteral("style:use-window-font-color")))
        into.remove(QStringLiteral("style:use-window-font-color"));
    for (auto it = over.cbegin(); it != over.cend(); ++it) {
        if (it.key() == QLatin1String("fo:font-size") && it.value().endsWith(QLatin1Char('%'))) {
            const qreal under = centimetres(into.value(it.key(), QStringLiteral("18pt")));
            const qreal scaled = (std::isnan(under) ? 18 * 2.54 / 72 : under) * percentOf(it.value(), 1);
            into.insert(it.key(), QString::number(scaled * 72 / 2.54, 'f', 3) + QStringLiteral("pt"));
            continue;
        }
        into.insert(it.key(), it.value());
    }
}

Props attributesOf(const El *el) { return el ? el->at : Props(); }

// ---------------------------------------------------------------------------
// Styles: where an object's look comes from. Content and master pages each
// see their own automatic styles and the common ones.

struct Styles {
    enum Scope { Content, Masters };
    QHash<QString, const El *> common, autoContent, autoMasters;   // "family\nname"
    QHash<QString, const El *> defaults;                           // by family
    QHash<QString, const El *> named;                              // "draw:gradient\nname"
    QHash<QString, const El *> listsCommon, listsContent, listsMasters;
    QHash<QString, QString> fonts;                                  // font-face name → family
    QHash<QString, const El *> pageLayouts;

    static QString key(const QString &family, const QString &name) { return family + QLatin1Char('\n') + name; }

    void addStyles(const El *container, QHash<QString, const El *> &into, QHash<QString, const El *> &lists, bool commonPart) {
        if (!container) return;
        for (const auto &k : container->kids) {
            if (k.tag == QLatin1String("style:style")) into.insert(key(k.a("style:family"), k.a("style:name")), &k);
            else if (k.tag == QLatin1String("text:list-style")) lists.insert(k.a("style:name"), &k);
            else if (k.tag == QLatin1String("style:page-layout")) pageLayouts.insert(k.a("style:name"), &k);
            else if (commonPart && k.tag == QLatin1String("style:default-style")) defaults.insert(k.a("style:family"), &k);
            else if (commonPart && k.has("draw:name")) named.insert(k.tag + QLatin1Char('\n') + k.a("draw:name"), &k);
        }
    }
    void addFonts(const El *decls) {
        if (!decls) return;
        for (const auto *face : decls->all("style:font-face")) {
            QString family = face->a("svg:font-family");
            if (family.size() > 1 && (family.startsWith(QLatin1Char('\'')) || family.startsWith(QLatin1Char('"')))) family = family.mid(1, family.size() - 2);
            if (family.isEmpty()) family = face->a("style:name");
            fonts.insert(face->a("style:name"), family);
        }
    }

    const El *find(Scope scope, const QString &family, const QString &name) const {
        if (name.isEmpty()) return nullptr;
        const QString k = key(family, name);
        const auto &automatic = scope == Content ? autoContent : autoMasters;
        if (const El *s = automatic.value(k)) return s;
        return common.value(k);
    }
    const El *list(Scope scope, const QString &name) const {
        if (name.isEmpty()) return nullptr;
        if (const El *s = (scope == Content ? listsContent : listsMasters).value(name)) return s;
        return listsCommon.value(name);
    }

    // One kind of properties ("style:graphic-properties"…) as the style says
    // with everything it inherits; `withDefault` starts from the family's
    // default style.
    Props props(Scope scope, const QString &family, const QString &name, const char *element, bool withDefault = true) const {
        QVector<const El *> chain;
        const El *s = find(scope, family, name);
        QSet<const El *> seen;
        while (s && !seen.contains(s) && chain.size() < 32) {
            seen.insert(s);
            chain.prepend(s);
            s = find(scope, family, s->a("style:parent-style-name"));
        }
        Props out;
        if (withDefault) {
            const El *d = defaults.value(family);
            if (!d && family == QLatin1String("presentation")) d = defaults.value(QStringLiteral("graphic"));
            if (d) layer(out, attributesOf(d->child(element)));
        }
        for (const auto *style : chain) layer(out, attributesOf(style->child(element)));
        return out;
    }
    // The nearest text:list-style a graphic or presentation style carries.
    const El *listOf(Scope scope, const QString &family, const QString &name) const {
        const El *s = find(scope, family, name);
        QSet<const El *> seen;
        while (s && !seen.contains(s)) {
            seen.insert(s);
            if (const El *g = s->child("style:graphic-properties"))
                if (const El *l = g->child("text:list-style")) return l;
            s = find(scope, family, s->a("style:parent-style-name"));
        }
        return nullptr;
    }
    QString family(const Props &text) const {
        const QString name = text.value(QStringLiteral("style:font-name"));
        if (!name.isEmpty()) return fonts.value(name, name);
        QString direct = text.value(QStringLiteral("fo:font-family"));
        if (direct.size() > 1 && (direct.startsWith(QLatin1Char('\'')) || direct.startsWith(QLatin1Char('"')))) direct = direct.mid(1, direct.size() - 2);
        return direct;
    }
};

// ---------------------------------------------------------------------------
// Enhanced geometry: a custom shape's outline from its own path, formulas
// and adjustments, as LibreOffice draws it.

struct Geometry {
    qreal vx = 0, vy = 0, vw = 21600, vh = 21600;
    QVector<qreal> modifiers;
    QHash<QString, int> equationIndex;
    QStringList formulas;
    mutable QVector<qreal> values;
    mutable QVector<int> state;     // 0 not yet, 1 being worked out, 2 known
    qreal logWidth = 0, logHeight = 0;
    qreal stretchX = 0, stretchY = 0;

    void read(const El *g, qreal widthCm, qreal heightCm) {
        if (!g) return;
        const auto box = g->a("svg:viewBox").simplified().split(QLatin1Char(' '));
        if (box.size() == 4) {
            vx = box[0].toDouble(); vy = box[1].toDouble();
            vw = box[2].toDouble(); vh = box[3].toDouble();
        }
        // Shapes brought in from PowerPoint leave the viewBox empty and give
        // the size their path is drawn in here instead.
        const auto sub = g->a("drawooo:sub-view-size").simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if ((vw <= 0 || vh <= 0) && sub.size() >= 2) { vx = vy = 0; vw = sub[0].toDouble(); vh = sub[1].toDouble(); }
        if (vw <= 0) vw = 21600;
        if (vh <= 0) vh = 21600;
        for (const auto &m : g->a("draw:modifiers").simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts)) modifiers.append(m.toDouble());
        for (const auto *eq : g->all("draw:equation")) {
            equationIndex.insert(eq->a("draw:name"), formulas.size());
            formulas.append(eq->a("draw:formula"));
        }
        values.fill(0, formulas.size());
        state.fill(0, formulas.size());
        logWidth = widthCm * 1000;
        logHeight = heightCm * 1000;
        stretchX = g->a("draw:path-stretchpoint-x", QStringLiteral("0")).toDouble();
        stretchY = g->a("draw:path-stretchpoint-y", QStringLiteral("0")).toDouble();
    }

    qreal keyword(const QString &word, bool *ok) const {
        *ok = true;
        if (word == QLatin1String("left")) return vx;
        if (word == QLatin1String("top")) return vy;
        if (word == QLatin1String("right")) return vx + vw;
        if (word == QLatin1String("bottom")) return vy + vh;
        if (word == QLatin1String("width")) return vw;
        if (word == QLatin1String("height")) return vh;
        if (word == QLatin1String("xstretch")) return stretchX;
        if (word == QLatin1String("ystretch")) return stretchY;
        if (word == QLatin1String("hasstroke") || word == QLatin1String("hasfill")) return 1;
        if (word == QLatin1String("logwidth")) return logWidth;
        if (word == QLatin1String("logheight")) return logHeight;
        if (word == QLatin1String("pi")) return M_PI;
        *ok = false;
        return 0;
    }

    qreal equation(int index) const {
        if (index < 0 || index >= formulas.size()) return 0;
        if (state[index] == 2) return values[index];
        if (state[index] == 1) return 0;   // a loop: nothing sensible to give
        state[index] = 1;
        int at = 0;
        const qreal v = expression(formulas[index], at);
        values[index] = std::isfinite(v) ? v : 0;
        state[index] = 2;
        return values[index];
    }

    // A parameter in a path or a formula: a number, ?equation, $modifier or keyword.
    qreal parameter(const QString &token) const {
        if (token.startsWith(QLatin1Char('?'))) return equation(equationIndex.value(token.mid(1), -1));
        if (token.startsWith(QLatin1Char('$'))) return modifiers.value(token.mid(1).toInt());
        bool ok = false;
        const qreal n = token.toDouble(&ok);
        if (ok) return n;
        const qreal k = keyword(token, &ok);
        return ok ? k : 0;
    }

    // ---- formulas: + - * / with functions, by recursive descent

    static void skip(const QString &s, int &i) { while (i < s.size() && s[i].isSpace()) ++i; }

    qreal expression(const QString &s, int &i) const {
        qreal v = term(s, i);
        for (;;) {
            skip(s, i);
            if (i < s.size() && s[i] == QLatin1Char('+')) { ++i; v += term(s, i); }
            else if (i < s.size() && s[i] == QLatin1Char('-')) { ++i; v -= term(s, i); }
            else return v;
        }
    }
    qreal term(const QString &s, int &i) const {
        qreal v = unary(s, i);
        for (;;) {
            skip(s, i);
            if (i < s.size() && s[i] == QLatin1Char('*')) { ++i; v *= unary(s, i); }
            else if (i < s.size() && s[i] == QLatin1Char('/')) { ++i; const qreal d = unary(s, i); v = qFuzzyIsNull(d) ? 0 : v / d; }
            else return v;
        }
    }
    qreal unary(const QString &s, int &i) const {
        skip(s, i);
        if (i < s.size() && s[i] == QLatin1Char('-')) { ++i; return -unary(s, i); }
        if (i < s.size() && s[i] == QLatin1Char('+')) { ++i; return unary(s, i); }
        return primary(s, i);
    }
    QVector<qreal> arguments(const QString &s, int &i) const {
        QVector<qreal> out;
        skip(s, i);
        if (i >= s.size() || s[i] != QLatin1Char('(')) return out;
        ++i;
        for (;;) {
            out.append(expression(s, i));
            skip(s, i);
            if (i < s.size() && s[i] == QLatin1Char(',')) { ++i; continue; }
            if (i < s.size() && s[i] == QLatin1Char(')')) ++i;
            return out;
        }
    }
    qreal primary(const QString &s, int &i) const {
        skip(s, i);
        if (i >= s.size()) return 0;
        const QChar c = s[i];
        if (c == QLatin1Char('(')) {
            ++i;
            const qreal v = expression(s, i);
            skip(s, i);
            if (i < s.size() && s[i] == QLatin1Char(')')) ++i;
            return v;
        }
        if (c.isDigit() || c == QLatin1Char('.')) {
            const int from = i;
            while (i < s.size() && (s[i].isDigit() || s[i] == QLatin1Char('.'))) ++i;
            if (i < s.size() && (s[i] == QLatin1Char('e') || s[i] == QLatin1Char('E')) && i + 1 < s.size() &&
                (s[i + 1].isDigit() || s[i + 1] == QLatin1Char('-') || s[i + 1] == QLatin1Char('+'))) {
                i += 2;
                while (i < s.size() && s[i].isDigit()) ++i;
            }
            return s.mid(from, i - from).toDouble();
        }
        if (c == QLatin1Char('?') || c == QLatin1Char('$')) {
            const int from = i++;
            while (i < s.size() && (s[i].isLetterOrNumber() || s[i] == QLatin1Char('_'))) ++i;
            return parameter(s.mid(from, i - from));
        }
        if (c.isLetter()) {
            const int from = i;
            while (i < s.size() && (s[i].isLetterOrNumber() || s[i] == QLatin1Char('_'))) ++i;
            const QString word = s.mid(from, i - from);
            bool ok = false;
            const qreal k = keyword(word, &ok);
            if (ok) return k;
            const auto args = arguments(s, i);
            const qreal x = args.value(0), y = args.value(1);
            if (word == QLatin1String("abs")) return std::abs(x);
            if (word == QLatin1String("sqrt")) return x > 0 ? std::sqrt(x) : 0;
            if (word == QLatin1String("sin")) return std::sin(x);
            if (word == QLatin1String("cos")) return std::cos(x);
            if (word == QLatin1String("tan")) return std::tan(x);
            if (word == QLatin1String("atan")) return std::atan(x);
            if (word == QLatin1String("atan2")) return std::atan2(x, y);
            if (word == QLatin1String("min")) return qMin(x, y);
            if (word == QLatin1String("max")) return qMax(x, y);
            if (word == QLatin1String("if")) return x > 0 ? y : args.value(2);
            return 0;
        }
        ++i;   // something unreadable: step over it
        return 0;
    }

    // ---- the path

    struct Piece { QPainterPath path; bool fill = true, stroke = true; };

    // In viewBox coordinates; one piece per N-ended stretch.
    QVector<Piece> pieces(const QString &source) const {
        // Tokens: command letters, and parameters.
        QStringList tokens;
        {
            int i = 0;
            const QString s = source;
            while (i < s.size()) {
                const QChar c = s[i];
                if (c.isSpace() || c == QLatin1Char(',')) { ++i; continue; }
                if (c == QLatin1Char('?') || c == QLatin1Char('$')) {
                    const int from = i++;
                    while (i < s.size() && (s[i].isLetterOrNumber() || s[i] == QLatin1Char('_'))) ++i;
                    tokens.append(s.mid(from, i - from));
                } else if (c.isDigit() || c == QLatin1Char('.') || c == QLatin1Char('-') || c == QLatin1Char('+')) {
                    const int from = i++;
                    while (i < s.size() && (s[i].isDigit() || s[i] == QLatin1Char('.') ||
                                            ((s[i] == QLatin1Char('e') || s[i] == QLatin1Char('E')) && i + 1 < s.size() && (s[i + 1].isDigit() || s[i + 1] == QLatin1Char('-')))))
                        i += (s[i] == QLatin1Char('e') || s[i] == QLatin1Char('E')) ? 2 : 1;
                    tokens.append(s.mid(from, i - from));
                } else if (c.isLetter()) {
                    const int from = i;
                    while (i < s.size() && s[i].isLetter()) ++i;
                    const QString word = s.mid(from, i - from);
                    if (word.size() == 1) tokens.append(word);
                    else {
                        // A keyword, unless it is commands run together ("ZN").
                        bool ok = false;
                        keyword(word, &ok);
                        if (ok) tokens.append(word);
                        else for (const QChar w : word) tokens.append(QString(w));
                    }
                } else ++i;
            }
        }
        const auto isCommand = [](const QString &t) {
            return t.size() == 1 && QStringLiteral("MLCZNFSTUABWVXYQG").contains(t[0]);
        };
        QVector<Piece> out;
        Piece current;
        QPointF point, start;
        bool open = false;
        int i = 0;
        QString command;
        bool quadrantX = true;
        const auto number = [&]() -> qreal { return i < tokens.size() && !isCommand(tokens[i]) ? parameter(tokens[i++]) : 0; };
        const auto hasNumber = [&]() { return i < tokens.size() && !isCommand(tokens[i]); };
        // x before y: the order arguments are worked out in is not fixed.
        const auto pair = [&]() { const qreal x = number(); const qreal y = number(); return QPointF(x, y); };
        const auto ensureStart = [&]() { if (!open) { current.path.moveTo(point); start = point; open = true; } };
        // An arc in Qt's terms: angles anticlockwise from three o'clock, as
        // the eye sees them.
        const auto qtAngle = [](const QPointF &c, qreal rx, qreal ry, const QPointF &p) {
            return qRadiansToDegrees(std::atan2(-(p.y() - c.y()) / qMax(1e-9, ry), (p.x() - c.x()) / qMax(1e-9, rx)));
        };
        const auto arc = [&](const QRectF &r, const QPointF &from, const QPointF &to, bool clockwise, bool move) {
            const QPointF c = r.center();
            const qreal rx = r.width() / 2, ry = r.height() / 2;
            const qreal a0 = qtAngle(c, rx, ry, from), a1 = qtAngle(c, rx, ry, to);
            qreal sweep = a1 - a0;
            if (clockwise) { while (sweep >= 0) sweep -= 360; }
            else { while (sweep <= 0) sweep += 360; }
            if (qFuzzyCompare(qAbs(sweep), 360.0) && QLineF(from, to).length() > 1e-6) sweep = clockwise ? sweep + 360 : sweep - 360;
            if (move || !open) { QPainterPath tmp; tmp.arcMoveTo(r, a0); current.path.moveTo(tmp.currentPosition()); start = tmp.currentPosition(); open = true; }
            current.path.arcTo(r, a0, sweep);
            point = current.path.currentPosition();
        };
        while (i < tokens.size()) {
            if (isCommand(tokens[i])) command = tokens[i++];
            else if (command.isEmpty()) { ++i; continue; }
            const QChar cmd = command.isEmpty() ? QChar() : command[0];
            if (cmd == QLatin1Char('M')) {
                const qreal x = number(), y = number();
                point = QPointF(x, y);
                current.path.moveTo(point);
                start = point;
                open = true;
                command = QStringLiteral("L");   // more pairs are lines
                quadrantX = true;
            } else if (cmd == QLatin1Char('L')) {
                if (!hasNumber()) { command.clear(); continue; }
                const qreal x = number(), y = number();
                ensureStart();
                point = QPointF(x, y);
                current.path.lineTo(point);
            } else if (cmd == QLatin1Char('C')) {
                if (!hasNumber()) { command.clear(); continue; }
                const QPointF c1 = pair(), c2 = pair(), p = pair();
                ensureStart();
                current.path.cubicTo(c1, c2, p);
                point = p;
            } else if (cmd == QLatin1Char('Q')) {
                if (!hasNumber()) { command.clear(); continue; }
                const QPointF c1 = pair(), p = pair();
                ensureStart();
                current.path.quadTo(c1, p);
                point = p;
            } else if (cmd == QLatin1Char('Z')) {
                current.path.closeSubpath();
                point = start;
                open = false;
                command.clear();
            } else if (cmd == QLatin1Char('N')) {
                if (!current.path.isEmpty()) out.append(current);
                current = Piece();
                open = false;
                command.clear();
            } else if (cmd == QLatin1Char('F')) {
                current.fill = false;
                command.clear();
            } else if (cmd == QLatin1Char('S')) {
                current.stroke = false;
                command.clear();
            } else if (cmd == QLatin1Char('T') || cmd == QLatin1Char('U')) {
                if (!hasNumber()) { command.clear(); continue; }
                const qreal cx = number(), cy = number(), rx = number(), ry = number(), t0 = number(), t1 = number();
                const QRectF r(cx - rx, cy - ry, 2 * rx, 2 * ry);
                qreal sweep = t1 - t0;
                while (sweep < 0) sweep += 360;
                if (qFuzzyIsNull(sweep) && !qFuzzyCompare(t0, t1)) sweep = 360;
                if (qFuzzyIsNull(sweep)) sweep = 360;
                if (cmd == QLatin1Char('U') || !open) {
                    QPainterPath tmp; tmp.arcMoveTo(r, t0);
                    if (cmd == QLatin1Char('U')) { current.path.moveTo(tmp.currentPosition()); }
                    else { ensureStart(); current.path.lineTo(tmp.currentPosition()); }
                    start = tmp.currentPosition();
                    open = true;
                }
                current.path.arcTo(r, t0, sweep);
                point = current.path.currentPosition();
            } else if (cmd == QLatin1Char('A') || cmd == QLatin1Char('B') || cmd == QLatin1Char('W') || cmd == QLatin1Char('V')) {
                if (!hasNumber()) { command.clear(); continue; }
                const QPointF p1 = pair(), p2 = pair(), from = pair(), to = pair();
                const QRectF r = QRectF(p1, p2).normalized();
                // A and W carry on from the pen; B and V start afresh.
                const bool move = cmd == QLatin1Char('B') || cmd == QLatin1Char('V');
                const bool clockwise = cmd == QLatin1Char('W') || cmd == QLatin1Char('V');
                if (!move && open) {
                    QPainterPath tmp; tmp.arcMoveTo(r, qtAngle(r.center(), r.width() / 2, r.height() / 2, from));
                    current.path.lineTo(tmp.currentPosition());
                }
                arc(r, from, to, clockwise, move);
            } else if (cmd == QLatin1Char('X') || cmd == QLatin1Char('Y')) {
                if (!hasNumber()) { command.clear(); quadrantX = true; continue; }
                const QPointF to = pair();
                ensureStart();
                const bool horizontalFirst = cmd == QLatin1Char('X') ? quadrantX : !quadrantX;
                const QPointF c = horizontalFirst ? QPointF(point.x(), to.y()) : QPointF(to.x(), point.y());
                const qreal rx = std::abs(horizontalFirst ? to.x() - point.x() : point.x() - to.x());
                const qreal ry = std::abs(horizontalFirst ? point.y() - to.y() : to.y() - point.y());
                if (rx < 1e-9 || ry < 1e-9) current.path.lineTo(to);
                else {
                    const QRectF r(c.x() - rx, c.y() - ry, 2 * rx, 2 * ry);
                    const qreal a0 = qtAngle(c, rx, ry, point), a1 = qtAngle(c, rx, ry, to);
                    qreal sweep = a1 - a0;
                    while (sweep > 180) sweep -= 360;
                    while (sweep < -180) sweep += 360;
                    current.path.arcTo(r, a0, sweep);
                }
                point = to;
                quadrantX = !quadrantX;
            } else if (cmd == QLatin1Char('G')) {
                if (!hasNumber()) { command.clear(); continue; }
                // OOXML's arcTo: radii, then start and swing as the eye sees
                // them, clockwise on the page.
                const qreal wr = number(), hr = number(), st = number(), sw = number();
                ensureStart();
                const auto parametric = [&](qreal visual) {
                    const qreal t = qDegreesToRadians(visual);
                    return std::atan2(wr * std::sin(t), hr * std::cos(t));
                };
                const qreal ts = parametric(st);
                qreal te = parametric(st + sw);
                if (sw > 0) { while (te < ts) te += 2 * M_PI; if (sw >= 360) te = ts + 2 * M_PI; }
                else if (sw < 0) { while (te > ts) te -= 2 * M_PI; if (sw <= -360) te = ts - 2 * M_PI; }
                else te = ts;
                const QPointF c(point.x() - wr * std::cos(ts), point.y() - hr * std::sin(ts));
                const QRectF r(c.x() - wr, c.y() - hr, 2 * wr, 2 * hr);
                if (wr > 1e-9 && hr > 1e-9) current.path.arcTo(r, -qRadiansToDegrees(ts), -qRadiansToDegrees(te - ts));
                point = current.path.currentPosition();
            } else {
                ++i;
            }
        }
        if (!current.path.isEmpty()) out.append(current);
        return out;
    }

    // Where the text goes, in viewBox coordinates: the first text area.
    QRectF textArea(const QString &areas) const {
        const auto parts = areas.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 4) return QRectF(vx, vy, vw, vh);
        return QRectF(QPointF(parameter(parts[0]), parameter(parts[1])), QPointF(parameter(parts[2]), parameter(parts[3]))).normalized();
    }
};

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

// Where an object sits: its box before turning, and the turn about its middle.
struct Placement {
    QRectF rect;
    qreal rotation = 0;   // degrees, clockwise as OmaShow turns
    bool valid = false;
    // A box inside this one (in the unturned box's own terms), placed so it
    // turns with it about the same middle.
    Placement inner(const QRectF &local) const {
        Placement p;
        p.valid = valid;
        p.rotation = rotation;
        const QPointF offset = local.center() - rect.center();
        QTransform t;
        t.rotate(rotation);
        const QPointF centre = rect.center() + t.map(offset);
        p.rect = QRectF(centre.x() - local.width() / 2, centre.y() - local.height() / 2, local.width(), local.height());
        return p;
    }
};

// One look for a stretch of words, as the styles under it say.
struct RunLook {
    QString family;
    qreal size = 18;          // points
    int weight = 400;
    bool italic = false, underline = false, strike = false, caps = false;
    int baseline = 0;         // 1 raised, 2 lowered
    QColor color = Qt::black;
    qreal spacing = 0;        // letter spacing, centimetres
    QString language;
    QString link;
    bool shadow = false;
};

struct ParaLook {
    int align = 0;            // left, centre, right, justify
    qreal lineHeight = 100;   // percent
    qreal before = 0, after = 0, indent = 0, tabs = 0;   // centimetres
    int direction = 0;
};

struct Paragraph {
    QString text;
    QVector<QPair<int, RunLook>> pieces;   // start in `text`, look
    ParaLook para;
    RunLook first;                          // the look of an empty paragraph
    int level = 0;                          // list nesting, 0 outermost
    int marker = 0;                         // 0 none, else OmaShow's list style
    int startAt = 1;
};

struct Reader {
    Zip::Reader *zip = nullptr;
    El content, styles, flat;
    const El *docContent = nullptr, *docStyles = nullptr;
    Styles st;
    Document doc;
    Warnings warnings;
    qreal k = 1;              // slide units per centimetre
    QString scratchDir;
    QHash<QString, QString> slideIdByName;   // page name → slide id
    QHash<QString, QString> objectByShapeId; // draw:id / xml:id → object id, per page
    struct PendingConnector { QString objectId, startShape, endShape; };
    QVector<PendingConnector> pendingConnectors;

    // What one object needs to know about where it is.
    struct Ctx {
        Styles::Scope scope = Styles::Content;
        int slide = 0;                        // 1-based; 0 on a master
        QString masterName;
        QVector<SceneObject> *objects = nullptr;
        bool onMaster = false;
    };

    qreal units(qreal cm) const { return cm * k; }
    qreal length(const El *el, const char *name, qreal fallback = 0) const {
        const qreal v = centimetres(el->a(name));
        return std::isnan(v) ? fallback : v * k;
    }
    qreal points(qreal pt) const { return pt * 2.54 / 72 * k; }

    QByteArray part(const QString &href) const {
        QString path = href;
        while (path.startsWith(QLatin1String("./"))) path = path.mid(2);
        if (path.startsWith(QLatin1Char('/'))) path = path.mid(1);
        if (!zip) return {};
        if (zip->contains(path)) return zip->read(path);
        const QString decoded = QUrl::fromPercentEncoding(path.toUtf8());
        return zip->contains(decoded) ? zip->read(decoded) : QByteArray();
    }

    // ---- placing

    Placement placementOf(const El *el) const {
        Placement p;
        const qreal w = centimetres(el->a("svg:width")), h = centimetres(el->a("svg:height"));
        if (std::isnan(w) || std::isnan(h)) return p;
        QRectF local(0, 0, units(w), units(h));
        const QString transform = el->a("draw:transform");
        if (transform.isEmpty()) {
            const qreal x = centimetres(el->a("svg:x")), y = centimetres(el->a("svg:y"));
            p.rect = QRectF(std::isnan(x) ? 0 : units(x), std::isnan(y) ? 0 : units(y), local.width(), local.height());
            p.valid = true;
            return p;
        }
        // Applied in order to the box at the origin: rotate turns anticlockwise
        // on the page, translate moves.
        QTransform t;
        static const QRegularExpression op(QStringLiteral("([a-zA-Z]+)\\s*\\(([^)]*)\\)"));
        auto it = op.globalMatch(transform);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString name = m.captured(1);
            const auto args = m.captured(2).simplified().split(QRegularExpression(QStringLiteral("[ ,]+")), Qt::SkipEmptyParts);
            QTransform step;
            if (name == QLatin1String("rotate")) step.rotateRadians(-args.value(0).toDouble());
            else if (name == QLatin1String("translate")) {
                const qreal dx = centimetres(args.value(0)), dy = args.size() > 1 ? centimetres(args.value(1)) : 0;
                step.translate(std::isnan(dx) ? 0 : units(dx), std::isnan(dy) ? 0 : units(dy));
            } else if (name == QLatin1String("scale")) step.scale(args.value(0).toDouble(), args.size() > 1 ? args.value(1).toDouble() : args.value(0).toDouble());
            else if (name == QLatin1String("skewX")) step.shear(std::tan(-args.value(0).toDouble()), 0);
            else if (name == QLatin1String("skewY")) step.shear(0, std::tan(-args.value(0).toDouble()));
            else if (name == QLatin1String("matrix") && args.size() == 6) {
                const qreal e = centimetres(args[4]), f = centimetres(args[5]);
                step = QTransform(args[0].toDouble(), args[1].toDouble(), args[2].toDouble(), args[3].toDouble(),
                                  std::isnan(e) ? 0 : units(e), std::isnan(f) ? 0 : units(f));
            }
            t = t * step;
        }
        const QPointF origin = t.map(QPointF(0, 0));
        const QPointF across = t.map(QPointF(local.width(), 0)) - origin;
        const QPointF centre = t.map(local.center());
        p.rotation = qRadiansToDegrees(std::atan2(across.y(), across.x()));
        if (std::abs(p.rotation) < 1e-6) p.rotation = 0;
        // A box with an x offset in the file as well as a transform: LibreOffice adds it.
        const qreal x = centimetres(el->a("svg:x")), y = centimetres(el->a("svg:y"));
        const QPointF extra(std::isnan(x) ? 0 : units(x), std::isnan(y) ? 0 : units(y));
        p.rect = QRectF(centre.x() + extra.x() - local.width() / 2, centre.y() + extra.y() - local.height() / 2, local.width(), local.height());
        p.valid = true;
        return p;
    }

    // ---- looks

    // Every property group of an object's own style, nearest last.
    struct ObjectStyle {
        Props graphic, paragraph, text;
        QString family, name;
    };
    ObjectStyle objectStyle(const El *el, const Ctx &ctx) const {
        ObjectStyle s;
        if (el->has("presentation:style-name")) { s.family = QStringLiteral("presentation"); s.name = el->a("presentation:style-name"); }
        else { s.family = QStringLiteral("graphic"); s.name = el->a("draw:style-name"); }
        s.graphic = st.props(ctx.scope, s.family, s.name, "style:graphic-properties");
        s.paragraph = st.props(ctx.scope, s.family, s.name, "style:paragraph-properties");
        s.text = st.props(ctx.scope, s.family, s.name, "style:text-properties");
        // A whole-object paragraph style (fields on masters).
        if (el->has("draw:text-style-name")) {
            layer(s.paragraph, st.props(ctx.scope, QStringLiteral("paragraph"), el->a("draw:text-style-name"), "style:paragraph-properties", false));
            layer(s.text, st.props(ctx.scope, QStringLiteral("paragraph"), el->a("draw:text-style-name"), "style:text-properties", false));
        }
        return s;
    }

    const El *named(const char *kind, const QString &name) const {
        return st.named.value(QLatin1String(kind) + QLatin1Char('\n') + name);
    }

    // A picture by href or inline, decoded into the object; false when it could not be.
    bool pictureInto(SceneObject &o, const El *image, int slide, bool quiet = false) {
        QByteArray bytes;
        QString suffix;
        if (const El *binary = image->child("office:binary-data")) bytes = QByteArray::fromBase64(binary->words().toLatin1());
        else {
            const QString href = image->a("xlink:href");
            if (href.isEmpty()) return false;
            if (href.contains(QLatin1String("://")) && !href.startsWith(QLatin1String("file:"))) {
                if (!quiet) warnings.add(QStringLiteral("A picture linked from the web was left out"), slide);
                return false;
            }
            if (href.startsWith(QLatin1String("file:")) || QDir::isAbsolutePath(href) || href.startsWith(QLatin1String("../"))) {
                const QString local = href.startsWith(QLatin1String("file:")) ? QUrl(href).toLocalFile() : href;
                QFile file(local);
                if (file.open(QIODevice::ReadOnly) && file.size() < 256LL * 1024 * 1024) bytes = file.readAll();
                if (bytes.isEmpty()) {
                    if (!quiet) warnings.add(QStringLiteral("A picture linked from outside the deck was left out"), slide);
                    return false;
                }
            } else bytes = part(href);
            suffix = QFileInfo(href).suffix().toLower();
        }
        if (bytes.isEmpty()) {
            if (!quiet) warnings.add(QStringLiteral("A picture's file was missing from the deck"), slide);
            return false;
        }
        QString error;
        const QString mime = image->a("draw:mime-type");
        const bool svg = suffix == QLatin1String("svg") || mime.contains(QLatin1String("svg")) ||
                         bytes.left(256).contains("<svg") || (bytes.startsWith("<?xml") && bytes.left(1024).contains("<svg"));
        if (svg && (SvgAsset::decode(o, bytes, &error) || SvgAsset::decode(o, plainSvg(bytes), &error))) return true;
        if (Metafile::recognise(bytes)) {
            const QByteArray svg = Metafile::toSvg(bytes, &error);
            if (!svg.isEmpty() && SvgAsset::decode(o, svg, &error)) return true;
        }
        if (bytes.startsWith("VCLMTF") || suffix == QLatin1String("svm") || suffix == QLatin1String("wmf") || suffix == QLatin1String("emf") ||
            bytes.startsWith(QByteArray("\xd7\xcd\xc6\x9a", 4)) || (bytes.size() > 44 && bytes.mid(40, 4) == " EMF")) {
            if (!quiet) warnings.add(QStringLiteral("A picture in a Windows or StarOffice drawing format was left out"), slide);
            return false;
        }
        if (ImageAsset::decode(o, bytes, &error)) return true;
        if (!quiet) warnings.add(QStringLiteral("A picture could not be read: %1").arg(error.isEmpty() ? suffix : error), slide);
        return false;
    }

    // The fill a set of graphic (or drawing-page) properties describes.
    struct Fill {
        int style = 5;          // OmaShow's fillStyle
        QColor color, second;
        qreal angle = 0;
        int pattern = 9;
        SceneObject picture;    // a picture fill's picture
        bool tiled = false;
        qreal tileW = 0, tileH = 0;
    };
    Fill fillOf(const Props &g, int slide, const QString &fallbackFill = QStringLiteral("none")) {
        Fill f;
        const QString kind = g.value(QStringLiteral("draw:fill"), fallbackFill);
        qreal opacity = percentOf(g.value(QStringLiteral("draw:opacity"), QStringLiteral("100%")), 1);
        // A transparency that fades across the fill: OmaShow gives it its mean.
        if (const El *fade = named("draw:opacity", g.value(QStringLiteral("draw:opacity-name"))))
            opacity = (percentOf(fade->a("draw:start", QStringLiteral("100%")), 1) + percentOf(fade->a("draw:end", QStringLiteral("100%")), 1)) / 2;
        QColor base = colourOf(g.value(QStringLiteral("draw:fill-color"), QStringLiteral("#729fcf")));
        if (!base.isValid()) base = QColor(0x72, 0x9f, 0xcf);
        if (g.contains(QStringLiteral("draw:opacity-name")))
            warnings.add(QStringLiteral("A fill that fades out gradually was given one even transparency, its average"), slide);
        const auto alpha = [&](QColor c) { c.setAlphaF(qBound(0.0, c.alphaF() * opacity, 1.0)); return c; };
        if (kind == QLatin1String("solid")) {
            f.style = 0;
            f.color = alpha(base);
        } else if (kind == QLatin1String("gradient")) {
            const El *gr = named("draw:gradient", g.value(QStringLiteral("draw:fill-gradient-name")));
            if (!gr) { f.style = 0; f.color = alpha(base); return f; }
            const QString style = gr->a("draw:style", QStringLiteral("linear"));
            QColor from = colourOf(gr->a("draw:start-color", QStringLiteral("#000000")));
            QColor to = colourOf(gr->a("draw:end-color", QStringLiteral("#ffffff")));
            // A colour at less than full intensity is that much darker.
            const auto dim = [](QColor c, qreal intensity) {
                if (intensity >= 0.999) return c;
                return QColor::fromRgbF(c.redF() * intensity, c.greenF() * intensity, c.blueF() * intensity, c.alphaF());
            };
            from = dim(from.isValid() ? from : QColor(Qt::black), percentOf(gr->a("draw:start-intensity", QStringLiteral("100%")), 1));
            to = dim(to.isValid() ? to : QColor(Qt::white), percentOf(gr->a("draw:end-intensity", QStringLiteral("100%")), 1));
            // Many stops: the first and the last.
            const auto stops = gr->all("loext:gradient-stop");
            if (stops.size() > 2) warnings.add(QStringLiteral("A gradient of more than two colours was shown with its first and last"), slide);
            if (style == QLatin1String("linear") || style == QLatin1String("axial")) {
                f.style = 1;
                // ODF turns anticlockwise from top-to-bottom; OmaShow clockwise from left-to-right.
                const qreal angle = degrees(gr->a("draw:angle", QStringLiteral("0")), true);
                f.angle = std::fmod(std::fmod(90.0 - angle, 360.0) + 360.0, 360.0);
                f.color = alpha(from);
                f.second = alpha(to);
                if (style == QLatin1String("axial")) {
                    // Outer colour to inner and back: shown as the first half.
                    f.color = alpha(from); f.second = alpha(to);
                    warnings.add(QStringLiteral("A gradient running out from the middle was shown running one way"), slide);
                }
            } else {
                // Radial, ellipsoid, square, rectangular: from the outside (start) in.
                f.style = 2;
                f.color = alpha(to);
                f.second = alpha(from);
                if (style != QLatin1String("radial") && style != QLatin1String("ellipsoid"))
                    warnings.add(QStringLiteral("A %1 gradient was shown as a round one").arg(style), slide);
            }
        } else if (kind == QLatin1String("hatch")) {
            const El *h = named("draw:hatch", g.value(QStringLiteral("draw:fill-hatch-name")));
            f.style = 3;
            f.color = alpha(h ? colourOf(h->a("draw:color", QStringLiteral("#000000"))) : QColor(Qt::black));
            f.second = truthy(g.value(QStringLiteral("draw:fill-hatch-solid"))) ? alpha(base) : QColor(0, 0, 0, 0);
            const QString hs = h ? h->a("draw:style", QStringLiteral("single")) : QStringLiteral("single");
            qreal turn = h ? std::fmod(std::fmod(degrees(h->a("draw:rotation", QStringLiteral("0")), true), 180.0) + 180.0, 180.0) : 0;
            // OmaShow's patterns: horizontal 7, vertical 8, cross 9, "/" 10, "\" 11, diagonal cross 12.
            if (hs == QLatin1String("single")) f.pattern = turn < 22.5 || turn >= 157.5 ? 7 : turn < 67.5 ? 10 : turn < 112.5 ? 8 : 11;
            else f.pattern = (turn >= 22.5 && turn < 67.5) || (turn >= 112.5 && turn < 157.5) ? 12 : 9;
        } else if (kind == QLatin1String("bitmap")) {
            const El *img = named("draw:fill-image", g.value(QStringLiteral("draw:fill-image-name")));
            SceneObject pic;
            pic.type = ObjectType::Image;
            if (img && pictureInto(pic, img, slide)) {
                f.style = 4;
                f.picture = pic;
                const QString repeat = g.value(QStringLiteral("style:repeat"), QStringLiteral("repeat"));
                f.tiled = repeat == QLatin1String("repeat");
                const qreal tw = centimetres(g.value(QStringLiteral("draw:fill-image-width"))), th = centimetres(g.value(QStringLiteral("draw:fill-image-height")));
                f.tileW = std::isnan(tw) ? 0 : units(tw);
                f.tileH = std::isnan(th) ? 0 : units(th);
                f.color = QColor(255, 255, 255, 0);
            } else {
                f.style = 0;
                f.color = alpha(base);
            }
        }
        return f;
    }

    // A picture fill as OmaShow draws one: the picture over the box, tiles
    // made into one picture the size of the box.
    void applyPictureFill(SceneObject &o, const Fill &f) {
        o.fillStyle = 4;
        ImageAsset::copyData(o, f.picture);
        o.imageMode = 2;
        if (f.tiled && !f.picture.image.isNull() && o.rect.width() > 1 && o.rect.height() > 1) {
            const QSizeF natural = ImageAsset::size(f.picture);
            const qreal tw = f.tileW > 0 ? f.tileW : natural.width() * 2.54 / 96 * k / 2.54 * 2.54;
            const qreal th = f.tileH > 0 ? f.tileH : natural.height() * 2.54 / 96 * k / 2.54 * 2.54;
            if (tw > 1 && th > 1 && (tw < o.rect.width() * 0.95 || th < o.rect.height() * 0.95)) {
                const qreal density = qBound(0.25, f.picture.image.width() / tw, 4.0);
                QImage out(qMax(1, qRound(o.rect.width() * density)), qMax(1, qRound(o.rect.height() * density)), QImage::Format_ARGB32_Premultiplied);
                out.fill(Qt::transparent);
                QPainter painter(&out);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                const QImage tile = f.picture.image.scaled(qMax(1, qRound(tw * density)), qMax(1, qRound(th * density)), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
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

    void applyFill(SceneObject &o, const Fill &f) {
        o.fillStyle = f.style;
        if (f.style == 4) { applyPictureFill(o, f); return; }
        if (f.color.isValid()) o.fill = f.color;
        if (f.second.isValid()) o.fillSecondary = f.second;
        o.fillAngle = f.angle;
        o.patternStyle = f.pattern;
    }

    int dashOf(const QString &name) const {
        const El *dash = named("draw:stroke-dash", name);
        if (!dash) return 1;
        const int dots1 = dash->a("draw:dots1", QStringLiteral("1")).toInt(), dots2 = dash->a("draw:dots2", QStringLiteral("0")).toInt();
        const auto longish = [](const QString &v) {
            if (v.isEmpty()) return false;
            if (v.endsWith(QLatin1Char('%'))) return percentOf(v, 1) > 1.5;
            const qreal cm = centimetres(v);
            return !std::isnan(cm) && cm > 0.05;
        };
        const bool long1 = longish(dash->a("draw:dots1-length")), long2 = longish(dash->a("draw:dots2-length"));
        if (dots2 > 0 && dots1 > 0) {
            if (long1 != long2) return dots2 >= 2 || dots1 >= 2 ? 4 : 3;
            return long1 ? 1 : 2;
        }
        return long1 ? 1 : 2;
    }

    void applyStroke(SceneObject &o, const Props &g, const QString &fallback = QStringLiteral("solid")) {
        const QString kind = g.value(QStringLiteral("draw:stroke"), fallback);
        if (kind == QLatin1String("none")) { o.strokeWidth = 0; return; }
        QColor c = colourOf(g.value(QStringLiteral("svg:stroke-color"), QStringLiteral("#3465a4")));
        if (!c.isValid()) c = QColor(0x34, 0x65, 0xa4);
        c.setAlphaF(qBound(0.0, c.alphaF() * percentOf(g.value(QStringLiteral("svg:stroke-opacity"), QStringLiteral("100%")), 1), 1.0));
        o.strokeColor = c;
        const qreal w = centimetres(g.value(QStringLiteral("svg:stroke-width"), QStringLiteral("0cm")));
        // Width 0 is the thinnest line the screen can draw.
        o.strokeWidth = std::isnan(w) || w <= 0 ? qMax(1.0, units(0.026)) : units(w);
        o.strokeStyle = kind == QLatin1String("dash") ? dashOf(g.value(QStringLiteral("draw:stroke-dash"))) : 0;
        const QString join = g.value(QStringLiteral("draw:stroke-linejoin"), QStringLiteral("round"));
        o.strokeJoin = join == QLatin1String("miter") ? 0 : join == QLatin1String("bevel") ? 2 : 1;
        const QString cap = g.value(QStringLiteral("svg:stroke-linecap"), QStringLiteral("butt"));
        o.strokeCap = cap == QLatin1String("round") ? 1 : cap == QLatin1String("square") ? 2 : 0;
    }

    void applyShadow(SceneObject &o, const Props &g) {
        if (g.value(QStringLiteral("draw:shadow")) != QLatin1String("visible")) return;
        o.shadowEnabled = true;
        QColor c = colourOf(g.value(QStringLiteral("draw:shadow-color"), QStringLiteral("#808080")));
        if (!c.isValid()) c = QColor(128, 128, 128);
        c.setAlphaF(percentOf(g.value(QStringLiteral("draw:shadow-opacity"), QStringLiteral("100%")), 1));
        o.shadowColor = c;
        const qreal x = centimetres(g.value(QStringLiteral("draw:shadow-offset-x"), QStringLiteral("0.2cm")));
        const qreal y = centimetres(g.value(QStringLiteral("draw:shadow-offset-y"), QStringLiteral("0.2cm")));
        o.shadowX = std::isnan(x) ? 0 : units(x);
        o.shadowY = std::isnan(y) ? 0 : units(y);
        const qreal blur = centimetres(g.value(QStringLiteral("loext:shadow-blur"), QStringLiteral("0cm")));
        o.shadowBlur = std::isnan(blur) ? 0 : units(blur);
    }

    void describe(SceneObject &o, const El *el) const {
        if (const El *title = el->child("svg:title")) o.altTitle = title->words().trimmed().left(200);
        if (o.altTitle.isEmpty()) o.altTitle = el->a("draw:name").left(200);
        if (const El *desc = el->child("svg:desc")) o.altText = desc->words().trimmed().left(2000);
        const QString display = el->a("draw:display");
        if (display == QLatin1String("none") || display == QLatin1String("printer")) o.hidden = true;
        if (truthy(el->a("draw:protect")) || el->a("draw:protect").contains(QLatin1String("position"))) o.locked = true;
    }

    void remember(const El *el, const QString &objectId) {
        for (const char *name : {"draw:id", "xml:id"})
            if (el->has(name)) objectByShapeId.insert(el->a(name), objectId);
    }

    // ---- text

    RunLook runLook(const Props &t) const {
        RunLook r;
        r.family = st.family(t);
        const qreal size = centimetres(t.value(QStringLiteral("fo:font-size")));
        if (!std::isnan(size) && size > 0) r.size = size * 72 / 2.54;
        const QString weight = t.value(QStringLiteral("fo:font-weight"), QStringLiteral("normal"));
        r.weight = weight == QLatin1String("bold") ? 700 : weight == QLatin1String("normal") ? 400 : qBound(100, weight.toInt(), 900);
        const QString style = t.value(QStringLiteral("fo:font-style"));
        r.italic = style == QLatin1String("italic") || style == QLatin1String("oblique");
        const QString under = t.value(QStringLiteral("style:text-underline-style"), QStringLiteral("none"));
        r.underline = !under.isEmpty() && under != QLatin1String("none");
        const QString strike = t.value(QStringLiteral("style:text-line-through-style"), QStringLiteral("none"));
        r.strike = !strike.isEmpty() && strike != QLatin1String("none");
        r.caps = t.value(QStringLiteral("fo:text-transform")) == QLatin1String("uppercase");
        const QString position = t.value(QStringLiteral("style:text-position")).trimmed();
        if (position.startsWith(QLatin1String("super")) || (position.endsWith(QLatin1Char('%')) && percentOf(position.section(QLatin1Char(' '), 0, 0), 0) > 0)) r.baseline = 1;
        else if (position.startsWith(QLatin1String("sub")) || percentOf(position.section(QLatin1Char(' '), 0, 0), 0) < 0) r.baseline = 2;
        QColor c = truthy(t.value(QStringLiteral("style:use-window-font-color"))) ? QColor(Qt::black) : colourOf(t.value(QStringLiteral("fo:color"), QStringLiteral("#000000")));
        if (!c.isValid()) c = Qt::black;
        if (t.contains(QStringLiteral("loext:opacity"))) c.setAlphaF(qBound(0.0, percentOf(t.value(QStringLiteral("loext:opacity")), 1), 1.0));
        r.color = c;
        const qreal spacing = centimetres(t.value(QStringLiteral("fo:letter-spacing")));
        r.spacing = std::isnan(spacing) ? 0 : spacing;
        const QString language = t.value(QStringLiteral("fo:language"));
        if (!language.isEmpty() && language != QLatin1String("zxx") && language != QLatin1String("none")) {
            r.language = language;
            const QString country = t.value(QStringLiteral("fo:country"));
            if (!country.isEmpty() && country != QLatin1String("none")) r.language += QLatin1Char('_') + country;
        }
        const QString shadow = t.value(QStringLiteral("fo:text-shadow"), QStringLiteral("none"));
        r.shadow = !shadow.isEmpty() && shadow != QLatin1String("none");
        return r;
    }

    ParaLook paraLook(const Props &p, qreal sizePt) const {
        ParaLook l;
        const QString align = p.value(QStringLiteral("fo:text-align"), QStringLiteral("start"));
        l.align = align == QLatin1String("center") ? 1 : (align == QLatin1String("end") || align == QLatin1String("right")) ? 2 : align == QLatin1String("justify") ? 3 : 0;
        const QString height = p.value(QStringLiteral("fo:line-height"), QStringLiteral("100%"));
        if (height.endsWith(QLatin1Char('%'))) l.lineHeight = percentOf(height, 1) * 100;
        else {
            // A fixed height, as a share of the type's own line (about 1.17 of its size).
            const qreal cm = centimetres(height);
            if (!std::isnan(cm) && cm > 0 && sizePt > 0) l.lineHeight = cm / (sizePt * 2.54 / 72 * 1.17) * 100;
        }
        const qreal atLeast = centimetres(p.value(QStringLiteral("style:line-height-at-least")));
        if (!std::isnan(atLeast) && atLeast > 0 && sizePt > 0) l.lineHeight = qMax(l.lineHeight, atLeast / (sizePt * 2.54 / 72 * 1.17) * 100);
        const qreal spacing = centimetres(p.value(QStringLiteral("style:line-spacing")));
        if (!std::isnan(spacing) && sizePt > 0) l.lineHeight += spacing / (sizePt * 2.54 / 72 * 1.17) * 100;
        l.lineHeight = qBound(50.0, l.lineHeight, 400.0);
        const auto cmOr0 = [&](const char *name) { const qreal v = centimetres(p.value(QLatin1String(name))); return std::isnan(v) ? 0.0 : v; };
        l.before = cmOr0("fo:margin-top");
        l.after = cmOr0("fo:margin-bottom");
        l.indent = cmOr0("fo:margin-left");
        l.tabs = cmOr0("style:tab-stop-distance");
        const QString mode = p.value(QStringLiteral("style:writing-mode"));
        l.direction = mode.startsWith(QLatin1String("rl")) ? 2 : 0;
        return l;
    }

    // Where a text's look comes from before its own paragraph and span styles.
    struct TextBase {
        Styles::Scope scope = Styles::Content;
        Props para, text;
        const El *listStyle = nullptr;   // the object's own, if any
        QString outlineStem;             // "Piano-outline" for an outline box
        QString family;                  // the object's style family
    };

    TextBase textBase(const El *el, const Ctx &ctx) const {
        TextBase b;
        b.scope = ctx.scope;
        const ObjectStyle os = objectStyle(el, ctx);
        b.family = os.family;
        b.para = os.paragraph;
        b.text = os.text;
        b.listStyle = st.listOf(ctx.scope, os.family, os.name);
        // An outline box draws each level from its master's outline styles.
        const El *s = st.find(ctx.scope, os.family, os.name);
        QSet<const El *> seen;
        while (s && !seen.contains(s)) {
            seen.insert(s);
            const QString name = s->a("style:name");
            if (name.endsWith(QLatin1String("-outline1"))) { b.outlineStem = name.left(name.size() - 1); break; }
            s = st.find(ctx.scope, os.family, s->a("style:parent-style-name"));
        }
        return b;
    }

    // OmaShow's marker for one level of a list style; 0 for none.
    static int markerOf(const El *list, int level, int *start) {
        if (!list) return 1;
        const El *found = nullptr;
        for (const auto &k : list->kids) {
            if (!k.tag.startsWith(QLatin1String("text:list-level-style"))) continue;
            if (k.a("text:level", QStringLiteral("1")).toInt() == level + 1) { found = &k; break; }
            if (!found) found = &k;
        }
        if (!found) return 1;
        if (found->tag == QLatin1String("text:list-level-style-number")) {
            if (start) *start = qMax(1, found->a("text:start-value", QStringLiteral("1")).toInt());
            const QString format = found->a("style:num-format");
            if (format.isEmpty()) return 0;
            if (format == QLatin1String("a")) return 5;
            if (format == QLatin1String("A")) return 6;
            if (format == QLatin1String("i")) return 7;
            if (format == QLatin1String("I")) return 8;
            return 2;
        }
        if (found->tag == QLatin1String("text:list-level-style-bullet")) {
            const QString c = found->a("text:bullet-char");
            if (c.isEmpty()) return 0;
            const ushort u = c.at(0).unicode();
            if (u == 0x25CB || u == 0x25E6 || u == 0x006F || u == 0x2218) return 3;   // ○ ◦ o ∘
            if (u == 0x25A0 || u == 0x25AA || u == 0x25FC || u == 0x00A7 || u == 0x2751 || u == 0x2752) return 4;   // ■ ▪ ◼ § ❑ ❒
            return 1;
        }
        return 1;   // a picture bullet
    }

    struct Walk {
        QVector<Paragraph> out;
        QString link;
        bool linkMixed = false;
    };

    // The words of a paragraph, run by run.
    void inlines(const El &el, const Props &textProps, const TextBase &base, Paragraph &p, bool &space, Walk &walk, const QString &link) const {
        for (const auto &k : el.kids) {
            if (k.isText()) {
                QString words;
                for (const QChar c : k.text) {
                    if (c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
                        if (!space) { words += QLatin1Char(' '); space = true; }
                    } else { words += c; space = false; }
                }
                if (!words.isEmpty()) { p.pieces.append({p.text.size(), runLook(textProps)}); p.pieces.last().second.link = link; p.text += words; }
                continue;
            }
            const QString &tag = k.tag;
            if (tag == QLatin1String("text:span")) {
                Props t = textProps;
                layer(t, st.props(base.scope, QStringLiteral("text"), k.a("text:style-name"), "style:text-properties", false));
                inlines(k, t, base, p, space, walk, link);
            } else if (tag == QLatin1String("text:a")) {
                QString href = k.a("xlink:href");
                if (!href.isEmpty()) {
                    if (walk.link.isEmpty()) walk.link = href;
                    else if (walk.link != href) walk.linkMixed = true;
                }
                Props t = textProps;
                layer(t, st.props(base.scope, QStringLiteral("text"), k.a("text:style-name"), "style:text-properties", false));
                inlines(k, t, base, p, space, walk, href);
            } else if (tag == QLatin1String("text:s")) {
                const int count = qBound(1, k.a("text:c", QStringLiteral("1")).toInt(), 1000);
                p.pieces.append({p.text.size(), runLook(textProps)});
                p.text += QString(count, QLatin1Char(' '));
                space = false;
            } else if (tag == QLatin1String("text:tab")) {
                p.pieces.append({p.text.size(), runLook(textProps)});
                p.text += QLatin1Char('\t');
                space = false;
            } else if (tag == QLatin1String("text:line-break")) {
                p.pieces.append({p.text.size(), runLook(textProps)});
                p.text += QChar(0x2028);
                space = true;
            } else if (tag == QLatin1String("office:annotation") || tag == QLatin1String("officeooo:annotation") ||
                       tag == QLatin1String("office:annotation-end") || tag.startsWith(QLatin1String("text:bookmark")) ||
                       tag == QLatin1String("text:soft-page-break") || tag.startsWith(QLatin1String("draw:")) ||
                       tag == QLatin1String("text:note") || tag.startsWith(QLatin1String("text:reference-mark"))) {
                continue;
            } else {
                // Fields and anything else: the words they show.
                Props t = textProps;
                if (k.has("text:style-name")) layer(t, st.props(base.scope, QStringLiteral("text"), k.a("text:style-name"), "style:text-properties", false));
                inlines(k, t, base, p, space, walk, link);
            }
        }
    }

    void paragraph(const El &el, const TextBase &base, int level, int marker, int startAt, Walk &walk) const {
        Paragraph p;
        p.level = level;
        p.marker = marker;
        p.startAt = startAt;
        Props para = base.para, text = base.text;
        if (marker >= 0 && !base.outlineStem.isEmpty() && level > 0) {
            const QString name = base.outlineStem + QString::number(qMin(level + 1, 9));
            layer(para, st.props(base.scope, QStringLiteral("presentation"), name, "style:paragraph-properties", false));
            layer(text, st.props(base.scope, QStringLiteral("presentation"), name, "style:text-properties", false));
        }
        const QString styleName = el.a("text:style-name");
        layer(para, st.props(base.scope, QStringLiteral("paragraph"), styleName, "style:paragraph-properties", false));
        layer(text, st.props(base.scope, QStringLiteral("paragraph"), styleName, "style:text-properties", false));
        p.first = runLook(text);
        p.para = paraLook(para, p.first.size);
        bool space = true;
        inlines(el, text, base, p, space, walk, QString());
        // An empty paragraph takes the look of its empty span, as its height does.
        if (p.pieces.isEmpty())
            for (const auto &k : el.kids)
                if (k.tag == QLatin1String("text:span")) {
                    Props t = text;
                    layer(t, st.props(base.scope, QStringLiteral("text"), k.a("text:style-name"), "style:text-properties", false));
                    p.first = runLook(t);
                    break;
                }
        // Trailing white space collapses away, as at the start.
        int end = p.text.size();
        while (end > 0 && p.text.at(end - 1) == QLatin1Char(' ') && space) { --end; }
        if (end < p.text.size()) {
            p.text.truncate(end);
            while (!p.pieces.isEmpty() && p.pieces.last().first >= end && end > 0) p.pieces.removeLast();
        }
        walk.out.append(p);
    }

    void blocks(const El &container, const TextBase &base, int level, const El *list, bool listed, Walk &walk) const {
        for (const auto &k : container.kids) {
            if (k.tag == QLatin1String("text:p") || k.tag == QLatin1String("text:h")) {
                int start = 1;
                const int marker = listed ? markerOf(list ? list : base.listStyle, level, &start) : -1;
                paragraph(k, base, level, marker, start, walk);
            } else if (k.tag == QLatin1String("text:list")) {
                const El *own = st.list(base.scope, k.a("text:style-name"));
                const El *use = own ? own : list;
                const int next = listed ? level + 1 : 0;
                for (const auto &item : k.kids) {
                    if (item.tag == QLatin1String("text:list-item")) blocks(item, base, next, use, true, walk);
                    else if (item.tag == QLatin1String("text:list-header")) {
                        Walk inner;
                        blocks(item, base, next, use, false, inner);
                        for (auto &p : inner.out) { p.level = next; walk.out.append(p); }
                        if (!inner.link.isEmpty() && walk.link.isEmpty()) walk.link = inner.link;
                    }
                }
            } else if (k.tag == QLatin1String("text:section") || k.tag == QLatin1String("text:list-item")) {
                blocks(k, base, level, list, listed, walk);
            }
        }
    }

    // The words of a text box or a shape's text, as OmaShow holds them.
    struct TextOut {
        bool empty = true;
        QString text;
        RunLook box;
        ParaLook para;
        int listStyle = 0, listStart = 1;
        QVector<TextRun> runs;
        QString link;
        bool linkMixed = false;
    };

    TextOut readText(const El *container, const TextBase &base, int slide) {
        TextOut out;
        if (!container) return out;
        Walk walk;
        blocks(*container, base, 0, nullptr, false, walk);
        if (walk.out.isEmpty()) return out;
        bool anyWords = false;
        for (const auto &p : walk.out) if (!p.text.trimmed().isEmpty()) anyWords = true;
        if (!anyWords) return out;
        out.empty = false;
        out.link = walk.link;
        out.linkMixed = walk.linkMixed;
        // The box's look: the first paragraph's, empty or not.
        const auto &first = walk.out.first();
        out.box = first.pieces.isEmpty() ? first.first : first.pieces.first().second;
        out.para = walk.out.first().para;
        // A list box: every paragraph with words is an item. OmaShow's marker
        // is one for the box; the first item's says which.
        bool allListed = true, anyListed = false;
        for (const auto &p : walk.out) {
            if (p.text.trimmed().isEmpty()) continue;
            if (p.marker > 0) { if (!anyListed) { out.listStyle = p.marker; out.listStart = p.startAt; } anyListed = true; }
            else allListed = false;
        }
        if (anyListed && !allListed) {
            out.listStyle = 0;
            warnings.add(QStringLiteral("List markers in a box mixing list items and plain paragraphs were left off"), slide);
        }
        // Words, then runs where they differ from the box.
        QVector<TextRun> runs;
        for (int i = 0; i < walk.out.size(); ++i) {
            const auto &p = walk.out[i];
            if (i) out.text += QLatin1Char('\n');
            if (out.listStyle > 0) out.text += QString(qMin(p.level, 8), QLatin1Char('\t'));
            const int base0 = out.text.size();
            for (int j = 0; j < p.pieces.size(); ++j) {
                const int from = p.pieces[j].first, to = j + 1 < p.pieces.size() ? p.pieces[j + 1].first : p.text.size();
                if (to <= from) continue;
                const RunLook &look = p.pieces[j].second;
                TextRun run;
                run.start = base0 + from;
                run.length = to - from;
                if (look.family != out.box.family) run.fontFamily = look.family;
                if (!qFuzzyCompare(look.size, out.box.size)) run.fontSize = points(look.size);
                if (look.weight != out.box.weight) run.weight = look.weight;
                if (look.italic != out.box.italic) run.italic = look.italic ? 1 : 2;
                if (look.underline != out.box.underline) run.underline = look.underline ? 1 : 2;
                if (look.strike != out.box.strike) run.strike = look.strike ? 1 : 2;
                run.baseline = look.baseline;
                if (look.color != out.box.color) run.color = look.color;
                if (look.language != out.box.language && !look.language.isEmpty()) run.language = look.language;
                if (!run.fontFamily.isEmpty() || run.fontSize > 0 || run.weight || run.italic || run.underline || run.strike ||
                    run.baseline || run.color.isValid() || !run.language.isEmpty())
                    runs.append(run);
            }
            out.text += p.text;
        }
        if (out.text.size() > Table::maxText) {
            out.text.truncate(Table::maxText);
            warnings.add(QStringLiteral("A very long text was cut short"), slide);
        }
        out.runs = TextRuns::tidy(runs, out.text.size());
        return out;
    }

    // A link as OmaShow keeps one; empty when it cannot follow it.
    void applyLink(SceneObject &o, const QString &href, int slide) {
        if (href.isEmpty()) return;
        if (href.startsWith(QLatin1Char('#'))) {
            const QString target = slideIdByName.value(QUrl::fromPercentEncoding(href.mid(1).toUtf8()));
            if (!target.isEmpty()) { o.linkKind = 3; o.linkTarget = target; }
        } else if (href.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive)) { o.linkKind = 2; o.linkTarget = href.mid(7); }
        else if (href.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) { o.linkKind = 1; o.linkTarget = href; }
        if (o.linkKind && !Links::validate(o.linkKind, o.linkTarget, doc, true).isEmpty()) { o.linkKind = 0; o.linkTarget.clear(); }
        if (!o.linkKind) warnings.add(QStringLiteral("A link OmaShow cannot follow was left off: %1").arg(href.left(80)), slide);
    }

    // A text object from what readText found, in `rect` (the box less its
    // insets), with the graphic style's box-wide settings.
    SceneObject textObject(const TextOut &text, const Props &graphic, const Props &glow, const Placement &place, bool frame, int slide) {
        SceneObject o;
        o.id = Edit::newId(QStringLiteral("text"));
        o.type = ObjectType::Text;
        o.rect = place.rect;
        o.rotation = place.rotation;
        o.text = text.text;
        const auto &b = text.box;
        o.fontFamily = b.family.isEmpty() ? QStringLiteral("Liberation Sans") : b.family;
        o.fontSize = points(b.size);
        o.fontWeight = b.weight;
        o.italic = b.italic;
        o.underline = b.underline;
        o.strike = b.strike;
        o.uppercase = b.caps;
        o.textColor = b.color;
        o.letterSpacing = units(b.spacing);
        if (!b.language.isEmpty()) o.language = b.language;
        o.textAlign = text.para.align;
        o.lineHeight = text.para.lineHeight;
        o.paragraphSpacing = units(qMax(text.para.after, text.para.before));
        if (text.para.tabs > 0 && o.text.contains(QLatin1Char('\t'))) o.tabStop = units(text.para.tabs);
        o.direction = text.para.direction;
        o.listStyle = text.listStyle;
        o.listStart = text.listStart;
        o.runs = text.runs;
        const QString vertical = graphic.value(QStringLiteral("draw:textarea-vertical-align"), frame ? QStringLiteral("top") : QStringLiteral("middle"));
        o.verticalAlign = vertical == QLatin1String("middle") ? 1 : vertical == QLatin1String("bottom") ? 2 : 0;
        const QString fit = graphic.value(QStringLiteral("draw:fit-to-size"));
        if (truthy(graphic.value(QStringLiteral("style:shrink-to-fit"))) || fit == QLatin1String("shrink-to-fit") || fit == QLatin1String("true")) {
            o.textFit = 1;
            o.fitKept = QStringLiteral("*");
        } else if (truthy(graphic.value(QStringLiteral("draw:auto-grow-height")))) o.textFit = 3;
        else o.textFit = 2;
        o.fillStyle = 5;
        o.strokeWidth = 0;
        o.fillToken.clear(); o.textColorToken.clear(); o.fontToken.clear();
        if (b.shadow) {
            o.shadowEnabled = true;
            o.shadowColor = QColor(0, 0, 0, 128);
            o.shadowX = o.shadowY = qMax(1.0, points(1.5));
        }
        const qreal glowRadius = centimetres(glow.value(QStringLiteral("loext:glow-text-radius")));
        if (!std::isnan(glowRadius) && glowRadius > 0) {
            QColor c = colourOf(glow.value(QStringLiteral("loext:glow-text-color"), QStringLiteral("#ffffff")));
            if (c.isValid()) {
                c.setAlphaF(qBound(0.0, 1.0 - percentOf(glow.value(QStringLiteral("loext:glow-text-transparency"), QStringLiteral("0%")), 0), 1.0));
                o.textGlowRadius = units(glowRadius);
                o.textGlowColor = c;
            }
        }
        if (text.linkMixed) warnings.add(QStringLiteral("A box with several links kept only its first"), slide);
        applyLink(o, text.link, slide);
        return o;
    }

    // The part of a box text sits in: less its padding.
    QRectF padded(const QRectF &r, const Props &g, bool frame) const {
        const auto pad = [&](const char *name, qreal fallback) {
            const qreal v = centimetres(g.value(QLatin1String(name)));
            return units(std::isnan(v) ? (g.contains(QStringLiteral("fo:padding")) ? qMax(0.0, centimetres(g.value(QStringLiteral("fo:padding")))) : fallback) : v);
        };
        const qreal side = frame ? 0.25 : 0.25, edge = frame ? 0.125 : 0.125;
        const qreal l = pad("fo:padding-left", side), rr = pad("fo:padding-right", side);
        const qreal t = pad("fo:padding-top", edge), b = pad("fo:padding-bottom", edge);
        QRectF out = r;
        if (r.width() > l + rr + 1) { out.setLeft(r.left() + l); out.setRight(r.right() - rr); }
        if (r.height() > t + b + 1) { out.setTop(r.top() + t); out.setBottom(r.bottom() - b); }
        return out;
    }

    // ---- shapes

    // A filled and outlined box for one object, before its outline is set.
    SceneObject boxFor(const El *el, const ObjectStyle &os, const QRectF &rect, qreal rotation, int slide) {
        SceneObject box;
        box.id = Edit::newId(QStringLiteral("shape"));
        box.type = ObjectType::Rect;
        box.rect = rect;
        box.rotation = rotation;
        box.fillToken.clear();
        applyFill(box, fillOf(os.graphic, slide));
        applyStroke(box, os.graphic, QStringLiteral("none"));
        applyShadow(box, os.graphic);
        describe(box, el);
        return box;
    }

    static bool shows(const SceneObject &box) { return box.fillStyle != 5 || box.strokeWidth > 0; }

    // An outline drawn in the unturned box's own terms, placed as `place` turns.
    void assignOutline(SceneObject &box, const QPainterPath &world, const Placement &place) {
        if (world.isEmpty()) return;
        QRectF bounds = world.boundingRect();
        if (bounds.width() < .001) bounds.setWidth(1);
        if (bounds.height() < .001) bounds.setHeight(1);
        const Placement at = place.inner(bounds);
        Shape::assignPath(box, world);
        box.rect = at.rect;
        box.rotation = at.rotation;
    }

    // Boxes and words for a shape, then remembered by its id. `outline`
    // pieces are in slide units of the unturned box; empty means "kind".
    QString addShape(const El *el, Ctx &ctx, const QStringList &groups, const Placement &place, int kind, qreal corner,
                 const QVector<Geometry::Piece> &outline, const QRectF &textArea, const El *textContainer, bool frame, const QString &link) {
        const ObjectStyle os = objectStyle(el, ctx);
        QVector<SceneObject> made;
        if (outline.isEmpty()) {
            SceneObject box = boxFor(el, os, place.rect, place.rotation, ctx.slide);
            box.shapeKind = kind;
            if (kind == 0 && corner > 0) box.cornerRadius = qMin(corner, qMin(place.rect.width(), place.rect.height()) / 2);
            if (shows(box)) made.append(box);
        } else {
            for (const auto &piece : outline) {
                SceneObject box = boxFor(el, os, place.rect, place.rotation, ctx.slide);
                if (!piece.fill) box.fillStyle = 5;
                if (!piece.stroke) box.strokeWidth = 0;
                if (!shows(box)) continue;
                const QRectF before = box.rect;
                assignOutline(box, piece.path, place);
                // A picture fill fills the shape's box, not the piece's.
                if (box.fillStyle == 4 && box.rect.size() != before.size()) applyFill(box, fillOf(os.graphic, ctx.slide));
                made.append(box);
            }
        }
        const TextOut text = readText(textContainer, textBase(el, ctx), ctx.slide);
        if (!text.empty) {
            const QRectF area = padded(textArea, os.graphic, frame);
            SceneObject words = textObject(text, os.graphic, os.text, place.inner(area), frame, ctx.slide);
            // With no box to cast it, the shadow is the words'.
            if (made.isEmpty()) { describe(words, el); applyShadow(words, os.graphic); }
            made.append(words);
        }
        if (made.isEmpty()) return {};
        QStringList own = groups;
        if (made.size() > 1) own.append(Edit::newId(QStringLiteral("group")));
        for (auto &o : made) {
            o.groups = own;
            if (!link.isEmpty() && !o.linkKind) applyLink(o, link, ctx.slide);
            ctx.objects->append(o);
        }
        remember(el, made.first().id);
        return made.first().id;
    }

    void customShape(const El *el, Ctx &ctx, const QStringList &groups, const QString &link) {
        const Placement place = placementOf(el);
        if (!place.valid) return;
        const El *g = el->child("draw:enhanced-geometry");
        const QString type = g ? g->a("draw:type") : QString();
        const QString source = g ? g->a("draw:enhanced-path") : QString();
        const qreal w = place.rect.width(), h = place.rect.height();
        Geometry geometry;
        geometry.read(g, w / k, h / k);
        QTransform toBox;
        toBox.translate(place.rect.x(), place.rect.y());
        if (g && truthy(g->a("draw:mirror-horizontal"))) { toBox.translate(w, 0); toBox.scale(-1, 1); }
        if (g && truthy(g->a("draw:mirror-vertical"))) { toBox.translate(0, h); toBox.scale(1, -1); }
        toBox.scale(w / geometry.vw, h / geometry.vh);
        toBox.translate(-geometry.vx, -geometry.vy);
        QRectF area = place.rect;
        if (g && g->has("draw:text-areas")) area = toBox.mapRect(geometry.textArea(g->a("draw:text-areas")));

        int kind = -1;
        qreal corner = 0;
        QVector<Geometry::Piece> pieces;
        const bool plainRect = type == QLatin1String("rectangle") || type == QLatin1String("ooxml-rect") || (type == QLatin1String("non-primitive") && source.isEmpty());
        if (plainRect || (type.isEmpty() && source.isEmpty())) kind = 0;
        else if ((type == QLatin1String("ellipse") || type == QLatin1String("ooxml-ellipse")) && !truthy(g->a("draw:mirror-horizontal"))) kind = 1;
        else if (!source.isEmpty()) {
            pieces = geometry.pieces(source);
            for (auto &piece : pieces) piece.path = toBox.map(piece.path);
            if (pieces.isEmpty()) kind = 0;
        } else if (type == QLatin1String("round-rectangle") || type == QLatin1String("ooxml-roundRect")) {
            kind = 0;
            corner = qMin(w, h) * qBound(0.0, geometry.modifiers.value(0, 3600) / qMin(geometry.vw, geometry.vh), 0.5);
        } else {
            kind = 0;
            warnings.add(QStringLiteral("The %1 shape was drawn as a rectangle").arg(type), ctx.slide);
        }
        addShape(el, ctx, groups, place, kind, corner, pieces, area, el, false, link);
    }

    void rectangle(const El *el, Ctx &ctx, const QStringList &groups, const QString &link) {
        const Placement place = placementOf(el);
        if (!place.valid) return;
        const qreal r = centimetres(el->a("draw:corner-radius"));
        addShape(el, ctx, groups, place, 0, std::isnan(r) ? 0 : units(r), {}, place.rect, el, false, link);
    }

    void ellipse(const El *el, Ctx &ctx, const QStringList &groups, const QString &link) {
        const Placement place = placementOf(el);
        if (!place.valid) return;
        const QString part = el->a("draw:kind", QStringLiteral("full"));
        if (part == QLatin1String("full")) { addShape(el, ctx, groups, place, 1, 0, {}, place.rect, el, false, link); return; }
        const qreal a0 = degrees(el->a("draw:start-angle", QStringLiteral("0")), false);
        qreal sweep = degrees(el->a("draw:end-angle", QStringLiteral("360")), false) - a0;
        while (sweep <= 0) sweep += 360;
        Geometry::Piece piece;
        const QRectF r = place.rect;
        if (part == QLatin1String("section")) { piece.path.moveTo(r.center()); piece.path.arcTo(r, a0, sweep); piece.path.closeSubpath(); }
        else { piece.path.arcMoveTo(r, a0); piece.path.arcTo(r, a0, sweep); if (part == QLatin1String("cut")) piece.path.closeSubpath(); else piece.fill = false; }
        addShape(el, ctx, groups, place, 1, 0, {piece}, place.rect, el, false, link);
    }

    // Points in a shape's viewBox, mapped into its box.
    QTransform viewBoxOf(const El *el, const QRectF &rect) const {
        const auto box = el->a("svg:viewBox").simplified().split(QLatin1Char(' '));
        qreal vx = 0, vy = 0, vw = rect.width() / k * 1000, vh = rect.height() / k * 1000;
        if (box.size() == 4) { vx = box[0].toDouble(); vy = box[1].toDouble(); vw = box[2].toDouble(); vh = box[3].toDouble(); }
        QTransform t;
        t.translate(rect.x(), rect.y());
        t.scale(vw > 0 ? rect.width() / vw : 1, vh > 0 ? rect.height() / vh : 1);
        t.translate(-vx, -vy);
        return t;
    }

    static QPainterPath pointsPath(const QString &points, bool close) {
        QPainterPath path;
        const auto pairs = points.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const auto &pair : pairs) {
            const auto xy = pair.split(QLatin1Char(','));
            if (xy.size() != 2) continue;
            const QPointF p(xy[0].toDouble(), xy[1].toDouble());
            if (path.elementCount() == 0) path.moveTo(p); else path.lineTo(p);
        }
        if (close && path.elementCount() > 2) path.closeSubpath();
        return path;
    }

    // SVG path data (M L H V C S Q T Z, absolute or relative). Arcs become lines.
    static QPainterPath svgPath(const QString &d) {
        QPainterPath path;
        QVector<QPair<QChar, QVector<qreal>>> commands;
        static const QRegularExpression token(QStringLiteral("([MmLlHhVvCcSsQqTtAaZz])|(-?(?:[0-9]+\\.?[0-9]*|\\.[0-9]+)(?:[eE][-+]?[0-9]+)?)"));
        auto it = token.globalMatch(d);
        while (it.hasNext()) {
            const auto m = it.next();
            if (!m.captured(1).isEmpty()) commands.append({m.captured(1).at(0), {}});
            else if (!commands.isEmpty()) commands.last().second.append(m.captured(2).toDouble());
        }
        QPointF at, start, control;
        QChar last;
        for (const auto &c : commands) {
            const QChar cmd = c.first;
            const bool rel = cmd.isLower();
            const QChar up = cmd.toUpper();
            const auto &v = c.second;
            const QPointF base = rel ? at : QPointF();
            if (up == QLatin1Char('Z')) { path.closeSubpath(); at = start; last = up; continue; }
            const int step = up == QLatin1Char('M') || up == QLatin1Char('L') || up == QLatin1Char('T') ? 2 : up == QLatin1Char('H') || up == QLatin1Char('V') ? 1
                           : up == QLatin1Char('C') ? 6 : up == QLatin1Char('S') || up == QLatin1Char('Q') ? 4 : 7;
            for (int i = 0; i + step <= v.size(); i += step) {
                const QPointF b = rel ? at : QPointF();
                if (up == QLatin1Char('M')) {
                    at = b + QPointF(v[i], v[i + 1]);
                    if (i == 0) { path.moveTo(at); start = at; } else path.lineTo(at);
                } else if (up == QLatin1Char('L') || up == QLatin1Char('T')) { at = b + QPointF(v[i], v[i + 1]); path.lineTo(at); }
                else if (up == QLatin1Char('H')) { at = QPointF(rel ? at.x() + v[i] : v[i], at.y()); path.lineTo(at); }
                else if (up == QLatin1Char('V')) { at = QPointF(at.x(), rel ? at.y() + v[i] : v[i]); path.lineTo(at); }
                else if (up == QLatin1Char('C')) {
                    const QPointF c1 = b + QPointF(v[i], v[i + 1]), c2 = b + QPointF(v[i + 2], v[i + 3]);
                    at = b + QPointF(v[i + 4], v[i + 5]);
                    path.cubicTo(c1, c2, at);
                    control = c2;
                } else if (up == QLatin1Char('S')) {
                    const QPointF c1 = (last == QLatin1Char('C') || last == QLatin1Char('S')) ? at * 2 - control : at;
                    const QPointF c2 = b + QPointF(v[i], v[i + 1]);
                    at = b + QPointF(v[i + 2], v[i + 3]);
                    path.cubicTo(c1, c2, at);
                    control = c2;
                } else if (up == QLatin1Char('Q')) {
                    const QPointF c1 = b + QPointF(v[i], v[i + 1]);
                    at = b + QPointF(v[i + 2], v[i + 3]);
                    path.quadTo(c1, at);
                } else {
                    at = b + QPointF(v[i + 5], v[i + 6]);
                    path.lineTo(at);
                }
                last = up;
            }
            Q_UNUSED(base);
        }
        return path;
    }

    bool hasMarkers(const Props &g, bool *start, bool *end) const {
        *start = !g.value(QStringLiteral("draw:marker-start")).isEmpty();
        *end = !g.value(QStringLiteral("draw:marker-end")).isEmpty();
        return *start || *end;
    }

    // A straight line with arrowheads: OmaShow draws arrows on connectors.
    QString arrowLine(const El *el, Ctx &ctx, const QStringList &groups, const QPointF &a, const QPointF &b, bool start, bool end) {
        const ObjectStyle os = objectStyle(el, ctx);
        SceneObject o;
        o.id = Edit::newId(QStringLiteral("connector"));
        o.connector = true;
        o.connectorStart = a;
        o.connectorEnd = b;
        o.connectorRoute = 0;
        o.connectorArrowStart = start;
        o.connectorArrowEnd = end;
        applyStroke(o, os.graphic, QStringLiteral("solid"));
        applyShadow(o, os.graphic);
        describe(o, el);
        QPainterPath path;
        path.moveTo(a);
        path.lineTo(b);
        Shape::assignPath(o, path);
        o.fillStyle = 5;
        o.groups = groups;
        ctx.objects->append(o);
        remember(el, o.id);
        return o.id;
    }

    void line(const El *el, Ctx &ctx, const QStringList &groups, const QString &link) {
        const auto at = [&](const char *name) { const qreal v = centimetres(el->a(name)); return std::isnan(v) ? 0.0 : units(v); };
        const QPointF a(at("svg:x1"), at("svg:y1")), b(at("svg:x2"), at("svg:y2"));
        const ObjectStyle os = objectStyle(el, ctx);
        bool start = false, end = false;
        if (hasMarkers(os.graphic, &start, &end)) { arrowLine(el, ctx, groups, a, b, start, end); return; }
        Placement place;
        place.valid = true;
        place.rect = QRectF(a, b).normalized();
        Geometry::Piece piece;
        piece.fill = false;
        piece.path.moveTo(a);
        piece.path.lineTo(b);
        addShape(el, ctx, groups, place, 0, 0, {piece}, place.rect, el, false, link);
    }

    void polyShape(const El *el, Ctx &ctx, const QStringList &groups, const QString &link) {
        const Placement place = placementOf(el);
        if (!place.valid) return;
        const bool polygon = el->tag == QLatin1String("draw:polygon");
        QPainterPath path = el->tag == QLatin1String("draw:path") ? svgPath(el->a("svg:d")) : pointsPath(el->a("draw:points"), polygon);
        if (path.isEmpty()) return;
        path = viewBoxOf(el, place.rect).map(path);
        const ObjectStyle os = objectStyle(el, ctx);
        bool start = false, end = false;
        if (!polygon && hasMarkers(os.graphic, &start, &end)) {
            if (path.elementCount() == 2 && !path.elementAt(1).isCurveTo()) {
                QTransform turn;
                turn.translate(place.rect.center().x(), place.rect.center().y());
                turn.rotate(place.rotation);
                turn.translate(-place.rect.center().x(), -place.rect.center().y());
                arrowLine(el, ctx, groups, turn.map(QPointF(path.elementAt(0))), turn.map(QPointF(path.elementAt(1))), start, end);
                return;
            }
            warnings.add(QStringLiteral("Arrowheads on a bent or curved line were left off"), ctx.slide);
        }
        Geometry::Piece piece;
        piece.path = path;
        piece.fill = polygon || el->tag == QLatin1String("draw:path");
        addShape(el, ctx, groups, place, 0, 0, {piece}, place.rect, el, false, link);
    }

    void connector(const El *el, Ctx &ctx, const QStringList &groups) {
        const auto at = [&](const char *name) { const qreal v = centimetres(el->a(name)); return std::isnan(v) ? 0.0 : units(v); };
        const QPointF a(at("svg:x1"), at("svg:y1")), b(at("svg:x2"), at("svg:y2"));
        const ObjectStyle os = objectStyle(el, ctx);
        bool start = false, end = false;
        hasMarkers(os.graphic, &start, &end);
        const QString id = arrowLine(el, ctx, groups, a, b, start, end);
        SceneObject &o = ctx.objects->last();
        const QString type = el->a("draw:type", QStringLiteral("standard"));
        o.connectorRoute = type == QLatin1String("standard") ? 1 : type == QLatin1String("curve") ? 2 : 0;
        if (type == QLatin1String("lines")) warnings.add(QStringLiteral("A connector of three straight lines was drawn as one"), ctx.slide);
        if (el->has("draw:start-shape") || el->has("draw:end-shape"))
            pendingConnectors.append({id, el->a("draw:start-shape"), el->a("draw:end-shape")});
    }

    // ---- pictures, films, tables and charts

    QString picture(const El *el, const QVector<const El *> &images, Ctx &ctx, const QStringList &groups, const Placement &place, const QString &link) {
        SceneObject o;
        o.id = Edit::newId(QStringLiteral("image"));
        o.type = ObjectType::Image;
        bool ok = false;
        for (int i = 0; i < images.size() && !ok; ++i) ok = pictureInto(o, images[i], ctx.slide, i + 1 < images.size());
        if (!ok) return {};
        const ObjectStyle os = objectStyle(el, ctx);
        const Props &g = os.graphic;
        o.rect = place.rect;
        o.rotation = place.rotation;
        o.groups = groups;
        o.fillStyle = 5;
        o.imageMode = 2;
        applyStroke(o, g, QStringLiteral("none"));
        applyShadow(o, g);
        describe(o, el);
        // The crop is in lengths off each edge of the picture at its own size.
        static const QRegularExpression clip(QStringLiteral("rect\\(\\s*([^,\\s]+)[,\\s]+([^,\\s]+)[,\\s]+([^,\\s]+)[,\\s]+([^,\\s)]+)\\s*\\)"));
        const auto m = clip.match(g.value(QStringLiteral("fo:clip")));
        if (m.hasMatch()) {
            const QSizeF px = ImageAsset::size(o);
            const qreal dpmX = o.image.dotsPerMeterX() > 0 ? o.image.dotsPerMeterX() : 96 / 0.0254;
            const qreal dpmY = o.image.dotsPerMeterY() > 0 ? o.image.dotsPerMeterY() : 96 / 0.0254;
            const qreal wcm = px.width() / dpmX * 100, hcm = px.height() / dpmY * 100;
            const auto cmOf = [](const QString &v) { const qreal c = centimetres(v); return std::isnan(c) ? 0.0 : c; };
            const qreal t = cmOf(m.captured(1)), r = cmOf(m.captured(2)), b = cmOf(m.captured(3)), l = cmOf(m.captured(4));
            if (wcm > 0 && hcm > 0 && (t > 0 || r > 0 || b > 0 || l > 0)) {
                const qreal fl = l / wcm, fr = r / wcm, ft = t / hcm, fb = b / hcm;
                if (fl + fr < 1 && ft + fb < 1) o.imageCrop = QRectF(fl, ft, 1 - fl - fr, 1 - ft - fb);
            }
        }
        const qreal opacity = percentOf(g.value(QStringLiteral("draw:image-opacity"), QStringLiteral("100%")), 1);
        if (opacity < 0.999) o.opacity = qBound(0.0, opacity, 1.0);
        const QString mode = g.value(QStringLiteral("draw:color-mode"), QStringLiteral("standard"));
        if (mode == QLatin1String("greyscale")) o.imageSaturation = 0;
        else if (mode == QLatin1String("mono") || mode == QLatin1String("watermark")) {
            o.imageSaturation = 0;
            warnings.add(QStringLiteral("A black-and-white or watermark picture was shown in greys"), ctx.slide);
        }
        o.imageBrightness = qBound(-1.0, percentOf(g.value(QStringLiteral("draw:luminance"), QStringLiteral("0%")), 0), 1.0);
        o.imageContrast = qBound(0.0, 1 + percentOf(g.value(QStringLiteral("draw:contrast"), QStringLiteral("0%")), 0), 2.0);
        if (!link.isEmpty()) applyLink(o, link, ctx.slide);
        ctx.objects->append(o);
        remember(el, o.id);
        return o.id;
    }

    QString media(const El *el, const El *plugin, Ctx &ctx, const QStringList &groups, const Placement &place) {
        const QString href = plugin->a("xlink:href");
        QByteArray bytes;
        QString name = QFileInfo(QUrl::fromPercentEncoding(href.toUtf8())).fileName();
        if (href.contains(QLatin1String("://")) && !href.startsWith(QLatin1String("file:"))) {
            warnings.add(QStringLiteral("A film linked from the web was left out"), ctx.slide);
            return {};
        }
        if (href.startsWith(QLatin1String("file:")) || QDir::isAbsolutePath(href) || href.startsWith(QLatin1String("../"))) {
            const QString local = href.startsWith(QLatin1String("file:")) ? QUrl(href).toLocalFile() : href;
            const auto result = MediaAsset::fromFile(local, false, {}, name);
            if (!result.ok()) { warnings.add(QStringLiteral("A linked film could not be found: %1").arg(name), ctx.slide); return {}; }
            SceneObject o = result.object;
            o.id = Edit::newId(QStringLiteral("media"));
            o.type = ObjectType::Media;
            o.rect = place.rect; o.rotation = place.rotation; o.groups = groups;
            describe(o, el);
            ctx.objects->append(o);
            remember(el, o.id);
            return o.id;
        }
        bytes = part(href);
        if (bytes.isEmpty() || scratchDir.isEmpty()) { warnings.add(QStringLiteral("A film's file was missing from the deck"), ctx.slide); return {}; }
        QTemporaryFile file(scratchDir + QStringLiteral("/omashow-import-XXXXXX.") + QFileInfo(name).suffix());
        if (!file.open()) return {};
        file.write(bytes);
        file.flush();
        const auto result = MediaAsset::fromFile(file.fileName(), true, {}, name);
        if (!result.ok()) { warnings.add(QStringLiteral("A film could not be embedded: %1").arg(result.error), ctx.slide); return {}; }
        SceneObject o = result.object;
        o.id = Edit::newId(QStringLiteral("media"));
        o.type = ObjectType::Media;
        o.mediaName = name;
        o.rect = place.rect; o.rotation = place.rotation; o.groups = groups;
        describe(o, el);
        ctx.objects->append(o);
        remember(el, o.id);
        return o.id;
    }

    QString table(const El *el, const El *tbl, Ctx &ctx, const QStringList &groups, const Placement &place) {
        // Columns and rows, with repeats spelled out.
        QVector<qreal> widths;
        for (const auto &k : tbl->kids) {
            const auto add = [&](const El &column) {
                const int repeat = qBound(1, column.a("table:number-columns-repeated", QStringLiteral("1")).toInt(), Table::maxColumns);
                const Props p = st.props(ctx.scope, QStringLiteral("table-column"), column.a("table:style-name"), "style:table-column-properties", false);
                const qreal w = centimetres(p.value(QStringLiteral("style:column-width")));
                for (int i = 0; i < repeat && widths.size() <= Table::maxColumns; ++i) widths.append(std::isnan(w) || w <= 0 ? 1 : w);
            };
            if (k.tag == QLatin1String("table:table-column")) add(k);
            else if (k.tag == QLatin1String("table:table-columns") || k.tag == QLatin1String("table:table-header-columns"))
                for (const auto *c : k.all("table:table-column")) add(*c);
        }
        QVector<const El *> rowEls;
        int headerRows = 0;
        for (const auto &k : tbl->kids) {
            if (k.tag == QLatin1String("table:table-row")) rowEls.append(&k);
            else if (k.tag == QLatin1String("table:table-rows") || k.tag == QLatin1String("table:table-header-rows")) {
                for (const auto *r : k.all("table:table-row")) rowEls.append(r);
                if (k.tag == QLatin1String("table:table-header-rows")) headerRows += k.all("table:table-row").size();
            }
        }
        int columns = widths.size();
        for (const auto *r : rowEls) {
            int n = 0;
            for (const auto &c : r->kids)
                if (c.tag == QLatin1String("table:table-cell") || c.tag == QLatin1String("table:covered-table-cell"))
                    n += qBound(1, c.a("table:number-columns-repeated", QStringLiteral("1")).toInt(), Table::maxColumns);
            columns = qMax(columns, n);
        }
        const int rows = rowEls.size();
        if (rows < 1 || columns < 1 || rows > Table::maxRows || columns > Table::maxColumns) {
            warnings.add(QStringLiteral("A table was too large to bring across"), ctx.slide);
            return {};
        }
        while (widths.size() < columns) widths.append(widths.isEmpty() ? 1 : widths.last());
        widths.resize(columns);
        SceneObject o;
        o.id = Edit::newId(QStringLiteral("table"));
        o.type = ObjectType::Table;
        o.table = Table::create(rows, columns);
        o.rect = place.rect; o.rotation = place.rotation; o.groups = groups;
        describe(o, el);
        const auto relative = [](QVector<qreal> &sizes) {
            qreal total = 0;
            for (qreal s : sizes) total += s;
            const qreal mean = sizes.isEmpty() || total <= 0 ? 1 : total / sizes.size();
            for (auto &s : sizes) s = qBound(0.05, s / mean, 20.0);
        };
        o.table.columns = widths;
        for (int r = 0; r < rows; ++r) {
            const Props p = st.props(ctx.scope, QStringLiteral("table-row"), rowEls[r]->a("table:style-name"), "style:table-row-properties", false);
            qreal h = centimetres(p.value(QStringLiteral("style:row-height")));
            if (std::isnan(h)) h = centimetres(p.value(QStringLiteral("style:min-row-height")));
            o.table.rows[r] = std::isnan(h) || h <= 0 ? 1 : h;
        }
        relative(o.table.columns);
        relative(o.table.rows);
        // The table's own look: what LibreOffice draws without a template.
        TextBase base;
        base.scope = ctx.scope;
        base.text = st.props(ctx.scope, QStringLiteral("graphic"), QString(), "style:text-properties");
        base.para = st.props(ctx.scope, QStringLiteral("graphic"), QString(), "style:paragraph-properties");
        const RunLook tableLook = runLook(base.text);
        o.fontSize = points(tableLook.size);
        o.fontFamily = tableLook.family.isEmpty() ? QStringLiteral("Liberation Sans") : tableLook.family;
        o.textColor = tableLook.color;
        o.verticalAlign = 0;
        o.table.padding = units(0.1);
        o.fill = QColor(0, 0, 0, 0);
        o.fillStyle = 0;
        o.fillToken.clear(); o.textColorToken.clear(); o.fontToken.clear();
        o.table.borderWidth = 0;
        const bool templated = tbl->has("table:template-name");
        bool explicitFills = false;
        for (int r = 0; r < rows; ++r) {
            int c = 0;
            for (const auto &cellEl : rowEls[r]->kids) {
                const bool covered = cellEl.tag == QLatin1String("table:covered-table-cell");
                if (!covered && cellEl.tag != QLatin1String("table:table-cell")) continue;
                const int repeat = qBound(1, cellEl.a("table:number-columns-repeated", QStringLiteral("1")).toInt(), Table::maxColumns);
                for (int n = 0; n < repeat && c < columns; ++n, ++c) {
                    auto &cell = o.table.cells[r * columns + c];
                    if (covered) { cell.rowSpan = 0; cell.columnSpan = 0; continue; }
                    if (cell.rowSpan == 0) continue;   // already covered by a span above
                    cell.rowSpan = qBound(1, cellEl.a("table:number-rows-spanned", QStringLiteral("1")).toInt(), rows - r);
                    cell.columnSpan = qBound(1, cellEl.a("table:number-columns-spanned", QStringLiteral("1")).toInt(), columns - c);
                    for (int rr = r; rr < r + cell.rowSpan; ++rr)
                        for (int cc = c; cc < c + cell.columnSpan; ++cc)
                            if (rr != r || cc != c) { auto &under = o.table.cells[rr * columns + cc]; under.rowSpan = 0; under.columnSpan = 0; }
                    const QString styleName = cellEl.a("table:style-name");
                    TextBase cellBase = base;
                    layer(cellBase.text, st.props(ctx.scope, QStringLiteral("table-cell"), styleName, "style:text-properties", false));
                    layer(cellBase.para, st.props(ctx.scope, QStringLiteral("table-cell"), styleName, "style:paragraph-properties", false));
                    const TextOut text = readText(&cellEl, cellBase, ctx.slide);
                    cell.text = text.text;
                    const RunLook look = text.empty ? runLook(cellBase.text) : text.box;
                    if (!qFuzzyCompare(look.size, tableLook.size)) cell.style.insert(QStringLiteral("fontSize"), points(look.size));
                    if (look.weight != 400) cell.style.insert(QStringLiteral("fontWeight"), look.weight);
                    if (look.italic) cell.style.insert(QStringLiteral("italic"), true);
                    if (look.underline) cell.style.insert(QStringLiteral("underline"), true);
                    if (look.color != tableLook.color) cell.style.insert(QStringLiteral("textColor"), look.color.name(QColor::HexArgb));
                    if (!look.family.isEmpty() && look.family != o.fontFamily) cell.style.insert(QStringLiteral("fontFamily"), look.family);
                    if (!text.empty && text.para.align) cell.style.insert(QStringLiteral("textAlign"), text.para.align);
                    // Cell fills and edges: graphic properties (Impress) or cell properties (the standard).
                    const Props graphic = st.props(ctx.scope, QStringLiteral("table-cell"), styleName, "style:graphic-properties", false);
                    const Props props = st.props(ctx.scope, QStringLiteral("table-cell"), styleName, "style:table-cell-properties", false);
                    if (graphic.contains(QStringLiteral("draw:fill"))) {
                        const Fill f = fillOf(graphic, ctx.slide);
                        explicitFills = true;
                        cell.style.insert(QStringLiteral("fill"), f.style == 0 && f.color.isValid() ? f.color.name(QColor::HexArgb) : QStringLiteral("#00000000"));
                    } else if (props.contains(QStringLiteral("fo:background-color"))) {
                        const QColor f = colourOf(props.value(QStringLiteral("fo:background-color")));
                        explicitFills = true;
                        cell.style.insert(QStringLiteral("fill"), f.isValid() ? f.name(QColor::HexArgb) : QStringLiteral("#00000000"));
                    }
                    const QString vertical = props.value(QStringLiteral("style:vertical-align"), graphic.value(QStringLiteral("draw:textarea-vertical-align")));
                    if (vertical == QLatin1String("middle")) cell.style.insert(QStringLiteral("verticalAlign"), 1);
                    else if (vertical == QLatin1String("bottom")) cell.style.insert(QStringLiteral("verticalAlign"), 2);
                    // One width and colour per cell: the widest of its edges.
                    qreal widest = 0; QColor edge; bool any = false;
                    for (const char *side : {"fo:border", "fo:border-left", "fo:border-right", "fo:border-top", "fo:border-bottom"}) {
                        const QString v = props.value(QLatin1String(side));
                        if (v.isEmpty()) continue;
                        any = true;
                        if (v == QLatin1String("none")) continue;
                        const auto parts = v.simplified().split(QLatin1Char(' '));
                        const qreal width = centimetres(parts.value(0));
                        if (!std::isnan(width) && units(width) > widest) { widest = units(width); edge = colourOf(parts.value(2)); }
                    }
                    if (any) {
                        cell.style.insert(QStringLiteral("borderWidth"), widest > 0 ? qMax(0.5, widest) : 0.0);
                        if (edge.isValid()) cell.style.insert(QStringLiteral("borderColor"), edge.name(QColor::HexArgb));
                    }
                }
            }
        }
        o.table.headerRows = templated && !explicitFills && truthy(tbl->a("table:use-first-row-styles")) ? 1 : 0;
        o.table.headerColumns = templated && !explicitFills && truthy(tbl->a("table:use-first-column-styles")) ? 1 : 0;
        o.table.banded = templated && !explicitFills && truthy(tbl->a("table:use-banding-rows-styles"));
        if (headerRows > 0 && !templated) o.table.headerRows = 0;
        ctx.objects->append(o);
        remember(el, o.id);
        return o.id;
    }

    // A chart from its own little document: the kind, the data table it
    // carries, titles and series colours.
    QString chart(const El *el, const El *object, Ctx &ctx, const QStringList &groups, const Placement &place) {
        El parsed;
        const El *root = nullptr;
        if (const El *inline_ = object->child("office:document")) root = inline_;
        else {
            QString href = object->a("xlink:href");
            while (href.startsWith(QLatin1String("./"))) href = href.mid(2);
            const QByteArray bytes = part(href + QStringLiteral("/content.xml"));
            QString error;
            if (bytes.isEmpty() || !parseXml(bytes, &parsed, &error)) return {};
            root = &parsed;
        }
        const El *body = root->child("office:body");
        const El *officeChart = body ? body->child("office:chart") : nullptr;
        const El *c = officeChart ? officeChart->child("chart:chart") : nullptr;
        if (!c) return {};
        QHash<QString, const El *> styles;
        if (const El *automatic = root->child("office:automatic-styles"))
            for (const auto *s : automatic->all("style:style")) styles.insert(s->a("style:name"), s);
        const auto chartProps = [&](const QString &name, const char *element) {
            const El *s = styles.value(name);
            const El *p = s ? s->child(element) : nullptr;
            return p ? p->at : Props();
        };
        const El *plot = c->child("chart:plot-area");
        const Props plotProps = plot ? chartProps(plot->a("chart:style-name"), "style:chart-properties") : Props();
        const bool stacked = truthy(plotProps.value(QStringLiteral("chart:stacked"))) || truthy(plotProps.value(QStringLiteral("chart:percentage")));
        const bool horizontal = truthy(plotProps.value(QStringLiteral("chart:vertical")));
        const QString cls = c->a("chart:class").section(QLatin1Char(':'), -1);
        int kind = -1;
        if (cls == QLatin1String("bar")) kind = horizontal ? (stacked ? 3 : 1) : (stacked ? 2 : 0);
        else if (cls == QLatin1String("line")) kind = 4;
        else if (cls == QLatin1String("area")) kind = stacked ? 6 : 5;
        else if (cls == QLatin1String("circle")) kind = 7;
        else if (cls == QLatin1String("ring")) kind = 8;
        else if (cls == QLatin1String("scatter") || cls == QLatin1String("bubble")) kind = 9;
        else if (cls == QLatin1String("radar") || cls == QLatin1String("filled-radar") || cls == QLatin1String("stock")) {
            kind = 4;
            warnings.add(QStringLiteral("A %1 chart was drawn as a line chart").arg(cls), ctx.slide);
        } else return {};

        // The data: the chart's own table, header row first.
        const El *local = c->child("table:table");
        QVector<QStringList> grid;
        if (local) {
            QVector<const El *> rows;
            QVector<const El *> found;
            local->collect("table:table-row", found);
            for (const auto *row : found) {
                QStringList cells;
                for (const auto &cell : row->kids) {
                    if (cell.tag != QLatin1String("table:table-cell") && cell.tag != QLatin1String("table:covered-table-cell")) continue;
                    const int repeat = qBound(1, cell.a("table:number-columns-repeated", QStringLiteral("1")).toInt(), Table::maxColumns);
                    QString value = cell.has("office:value") ? cell.a("office:value") : cell.words().trimmed();
                    for (int i = 0; i < repeat; ++i) cells.append(value);
                }
                grid.append(cells);
                if (grid.size() > Table::maxRows * 4) break;
            }
        }
        if (grid.size() < 2) { warnings.add(QStringLiteral("A chart whose data was not kept in the deck was left out"), ctx.slide); return {}; }
        int columns = 0;
        for (const auto &r : grid) columns = qMax(columns, r.size());
        if (columns < 2) { warnings.add(QStringLiteral("A chart with no data was left out"), ctx.slide); return {}; }
        if (grid.size() > Table::maxRows || columns > Table::maxColumns)
            warnings.add(QStringLiteral("A chart was cut to %1 points and %2 series").arg(Table::maxRows - 1).arg(Table::maxColumns - 1), ctx.slide);
        const int rows = qMin(Table::maxRows, int(grid.size()));
        columns = qMin(Table::maxColumns, columns);

        SceneObject o;
        o.id = Edit::newId(QStringLiteral("chart"));
        o.type = ObjectType::Chart;
        o.chart.kind = kind;
        o.rect = place.rect; o.rotation = place.rotation; o.groups = groups;
        describe(o, el);
        o.fontSize = points(14);
        o.fillToken = QStringLiteral("background"); o.textColorToken = QStringLiteral("foreground"); o.fontToken = QStringLiteral("body");
        o.table = Table::create(rows, columns);
        o.table.headerColumns = 1;
        for (int r = 0; r < rows; ++r)
            for (int col = 0; col < columns; ++col) o.table.cells[r * columns + col].text = grid[r].value(col);
        if (o.table.cells[0].text.isEmpty()) o.table.cells[0].text = kind == 9 ? QStringLiteral("X") : QStringLiteral("Category");
        for (int col = 1; col < columns; ++col)
            if (o.table.cells[col].text.isEmpty()) o.table.cells[col].text = QStringLiteral("Series %1").arg(col);

        // Series colours, in the order the series are drawn.
        if (plot) {
            int col = 1;
            for (const auto *series : plot->all("chart:series")) {
                if (col >= columns) break;
                const El *s = styles.value(series->a("chart:style-name"));
                const El *g = s ? s->child("style:graphic-properties") : nullptr;
                QColor colour;
                if (g) colour = colourOf(kind == 4 ? g->a("svg:stroke-color") : g->a("draw:fill-color"));
                if (!colour.isValid() && g) colour = colourOf(g->a("draw:fill-color", g->a("svg:stroke-color")));
                if (colour.isValid()) o.chart.seriesColors.insert(o.table.cells[col].id, colour);
                const Props sp = s && s->child("style:chart-properties") ? s->child("style:chart-properties")->at : Props();
                if (sp.value(QStringLiteral("chart:data-label-number"), QStringLiteral("none")) != QLatin1String("none")) o.chart.labels = true;
                ++col;
            }
            if (plotProps.value(QStringLiteral("chart:data-label-number"), QStringLiteral("none")) != QLatin1String("none")) o.chart.labels = true;
            o.chart.grid = false;
            for (const auto *axis : plot->all("chart:axis")) {
                const QString dimension = axis->a("chart:dimension");
                const El *t = axis->child("chart:title");
                if (t) (dimension == QLatin1String("x") ? o.chart.xTitle : o.chart.yTitle) = t->words().trimmed();
                if (dimension == QLatin1String("y") && axis->child("chart:grid")) o.chart.grid = true;
            }
        }
        const El *title = c->child("chart:title");
        o.chart.title = title ? title->words().trimmed() : QString();
        o.chart.legend = c->child("chart:legend") != nullptr;
        ctx.objects->append(o);
        remember(el, o.id);
        return o.id;
    }

    void frame(const El *el, Ctx &ctx, const QStringList &groups, const QString &link) {
        const QString cls = el->a("presentation:class");
        if (truthy(el->a("presentation:placeholder"))) return;   // an empty placeholder: only its prompt
        if (cls == QLatin1String("page-number") || cls == QLatin1String("footer") || cls == QLatin1String("date-time") || cls == QLatin1String("header")) return;
        const Placement place = placementOf(el);
        if (!place.valid) return;
        if (const El *box = el->child("draw:text-box")) {
            addShape(el, ctx, groups, place, 0, 0, {}, place.rect, box, true, link);
            return;
        }
        const auto images = el->all("draw:image");
        if (const El *object = el->child("draw:object")) {
            if (!chart(el, object, ctx, groups, place).isEmpty()) return;
            if (!images.isEmpty() && !picture(el, images, ctx, groups, place, link).isEmpty()) {
                warnings.add(QStringLiteral("An embedded object was shown as its picture"), ctx.slide);
                return;
            }
            warnings.add(QStringLiteral("An embedded object was left out"), ctx.slide);
            return;
        }
        if (const El *plugin = el->child("draw:plugin")) {
            if (!media(el, plugin, ctx, groups, place).isEmpty()) return;
            if (!images.isEmpty()) picture(el, images, ctx, groups, place, link);
            return;
        }
        if (const El *tbl = el->child("table:table")) { table(el, tbl, ctx, groups, place); return; }
        if (el->child("draw:object-ole")) {
            if (images.isEmpty() || picture(el, images, ctx, groups, place, link).isEmpty())
                warnings.add(QStringLiteral("An embedded object was left out"), ctx.slide);
            else warnings.add(QStringLiteral("An embedded object was shown as its picture"), ctx.slide);
            return;
        }
        if (!images.isEmpty()) { picture(el, images, ctx, groups, place, link); return; }
        if (el->child("draw:applet") || el->child("draw:floating-frame"))
            warnings.add(QStringLiteral("An applet or web frame was left out"), ctx.slide);
    }

    void object(const El &k, Ctx &ctx, const QStringList &groups, const QString &link = QString()) {
        const QString &t = k.tag;
        if (t == QLatin1String("draw:custom-shape")) customShape(&k, ctx, groups, link);
        else if (t == QLatin1String("draw:frame")) frame(&k, ctx, groups, link);
        else if (t == QLatin1String("draw:rect")) rectangle(&k, ctx, groups, link);
        else if (t == QLatin1String("draw:ellipse") || t == QLatin1String("draw:circle")) ellipse(&k, ctx, groups, link);
        else if (t == QLatin1String("draw:line")) line(&k, ctx, groups, link);
        else if (t == QLatin1String("draw:polygon") || t == QLatin1String("draw:polyline") || t == QLatin1String("draw:path")) polyShape(&k, ctx, groups, link);
        else if (t == QLatin1String("draw:connector")) connector(&k, ctx, groups);
        else if (t == QLatin1String("draw:caption")) rectangle(&k, ctx, groups, link);
        else if (t == QLatin1String("draw:g")) {
            QStringList own = groups;
            own.append(Edit::newId(QStringLiteral("group")));
            const int before = ctx.objects->size();
            for (const auto &kid : k.kids) object(kid, ctx, own, link);
            if (ctx.objects->size() > before) remember(&k, ctx.objects->at(before).id);
        } else if (t == QLatin1String("draw:a")) {
            for (const auto &kid : k.kids) object(kid, ctx, groups, k.a("xlink:href"));
        } else if (t == QLatin1String("dr3d:scene")) warnings.add(QStringLiteral("A 3D shape was left out"), ctx.slide);
        else if (t == QLatin1String("draw:measure")) warnings.add(QStringLiteral("A measuring line was left out"), ctx.slide);
        else if (t == QLatin1String("draw:regular-polygon")) {
            const Placement place = placementOf(&k);
            if (!place.valid) return;
            const int corners = qBound(3, k.a("draw:corners", QStringLiteral("5")).toInt(), 100);
            const bool star = truthy(k.a("draw:concave"));
            const qreal sharp = percentOf(k.a("draw:sharpness", QStringLiteral("50%")), 0.5);
            Geometry::Piece piece;
            const QPointF c = place.rect.center();
            const qreal rx = place.rect.width() / 2, ry = place.rect.height() / 2;
            const int points = star ? corners * 2 : corners;
            for (int i = 0; i < points; ++i) {
                const qreal a = -M_PI / 2 + 2 * M_PI * i / points;
                const qreal f = star && (i % 2) ? 1 - sharp : 1;
                const QPointF p(c.x() + rx * f * std::cos(a), c.y() + ry * f * std::sin(a));
                if (i == 0) piece.path.moveTo(p); else piece.path.lineTo(p);
            }
            piece.path.closeSubpath();
            addShape(&k, ctx, groups, place, 0, 0, {piece}, place.rect, &k, false, link);
        }
    }

    void objects(const El &container, Ctx &ctx) {
        for (const auto &k : container.kids) {
            if (ctx.onMaster && k.has("presentation:class")) continue;
            object(k, ctx, {});
        }
    }

    // ---- backgrounds, masters and pages

    // A background that is not one colour, as a locked box over the whole slide.
    SceneObject backgroundBox(const Fill &fill) {
        SceneObject o;
        o.id = Edit::newId(QStringLiteral("background"));
        o.type = ObjectType::Rect;
        o.rect = QRectF(QPointF(0, 0), doc.size);
        o.strokeWidth = 0;
        o.locked = true;
        o.fillToken.clear();
        applyFill(o, fill);
        return o;
    }

    struct MasterInfo {
        QString id, layoutId, name;
        int index = -1;
        bool backgroundObject = false;   // its first object is its background
    };
    QHash<QString, MasterInfo> masters;   // by style:name

    void readMaster(const El *mp) {
        MasterInfo info;
        info.name = mp->a("style:name");
        Master master;
        master.id = Edit::newId(QStringLiteral("master"));
        master.name = mp->a("style:display-name", info.name);
        if (master.name.isEmpty()) master.name = QStringLiteral("Master %1").arg(doc.masters.size() + 1);
        master.backgroundToken.clear();
        master.background = Qt::white;
        Props page = st.props(Styles::Masters, QStringLiteral("drawing-page"), mp->a("draw:style-name"), "style:drawing-page-properties", false);
        // Without a fill of its own, the master's "-background" style has it (LibreOffice's older way).
        if (page.value(QStringLiteral("draw:fill"), QStringLiteral("none")) == QLatin1String("none"))
            page = st.props(Styles::Masters, QStringLiteral("presentation"), info.name + QStringLiteral("-background"), "style:graphic-properties", false);
        const Fill fill = fillOf(page, 0);
        if (fill.style == 0) master.background = fill.color.isValid() ? fill.color : QColor(Qt::white);
        else if (fill.style != 5) { master.objects.append(backgroundBox(fill)); info.backgroundObject = true; }
        Ctx ctx;
        ctx.scope = Styles::Masters;
        ctx.objects = &master.objects;
        ctx.onMaster = true;
        ctx.masterName = info.name;
        objectByShapeId.clear();
        pendingConnectors.clear();
        objects(*mp, ctx);
        resolveConnectors(master.objects);
        for (auto &o : master.objects) o.locked = true;

        // One layout per master, with its title and body where the master puts them.
        SlideLayout layout;
        layout.id = Edit::newId(QStringLiteral("layout"));
        layout.masterId = master.id;
        layout.name = master.name;
        for (const auto &k : mp->kids) {
            const QString cls = k.a("presentation:class");
            if (cls != QLatin1String("title") && cls != QLatin1String("outline")) continue;
            const QString id = cls == QLatin1String("title") ? QStringLiteral("title") : QStringLiteral("body");
            bool taken = false;
            for (const auto &p : layout.placeholders) if (p.id == id) taken = true;
            if (taken) continue;
            const Placement place = placementOf(&k);
            if (!place.valid) continue;
            const TextBase base = textBase(&k, ctx);
            TextOut text;
            text.empty = false;
            text.box = runLook(base.text);
            text.para = paraLook(base.para, text.box.size);
            text.text = cls == QLatin1String("title") ? QStringLiteral("Title") : QStringLiteral("Body");
            if (cls == QLatin1String("outline")) { int start = 1; text.listStyle = qMax(0, markerOf(base.listStyle, 0, &start)); text.listStart = start; }
            const ObjectStyle os = objectStyle(&k, ctx);
            SceneObject o = textObject(text, os.graphic, os.text, place.inner(padded(place.rect, os.graphic, true)), true, 0);
            o.id = id;
            o.fitKept.clear();
            layout.placeholders.append(o);
        }
        info.id = master.id;
        info.layoutId = layout.id;
        info.index = doc.masters.size();
        doc.masters.append(master);
        doc.layouts.append(layout);
        masters.insert(info.name, info);
    }

    void resolveConnectors(QVector<SceneObject> &list) {
        for (const auto &pending : pendingConnectors) {
            for (auto &o : list) {
                if (o.id != pending.objectId) continue;
                const QString from = objectByShapeId.value(pending.startShape), to = objectByShapeId.value(pending.endShape);
                if (!from.isEmpty() && from != o.id) o.connectorFrom = from;
                if (!to.isEmpty() && to != o.id) o.connectorTo = to;
            }
        }
        pendingConnectors.clear();
    }

    void transition(const Props &p, Slide &slide, const El *page, int number) {
        if (p.value(QStringLiteral("presentation:transition-type")) == QLatin1String("automatic")) {
            const qreal seconds = isoSeconds(p.value(QStringLiteral("presentation:duration"), QStringLiteral("PT0S")));
            if (seconds >= 0) slide.advanceAfter = seconds;
        }
        const QString type = p.value(QStringLiteral("smil:type"));
        const QString subtype = p.value(QStringLiteral("smil:subtype"));
        qreal seconds = -1;
        const QString speed = p.value(QStringLiteral("presentation:transition-speed"));
        if (speed == QLatin1String("slow")) seconds = 2;
        else if (speed == QLatin1String("medium")) seconds = 1;
        else if (speed == QLatin1String("fast")) seconds = 0.5;
        QVector<const El *> filters;
        page->collect("anim:transitionFilter", filters);
        if (!filters.isEmpty()) {
            const qreal dur = smilSeconds(filters.first()->a("smil:dur"));
            if (dur > 0) seconds = dur;
        }
        if (const qreal d = smilSeconds(p.value(QStringLiteral("loext:transition-duration"))); d > 0) seconds = d;
        if (type.isEmpty()) { slide.transition = 0; return; }
        const auto direction = [&]() {
            return subtype == QLatin1String("fromLeft") ? 1 : subtype == QLatin1String("fromBottom") ? 2 : subtype == QLatin1String("fromTop") ? 3 : 0;
        };
        const bool reverse = p.value(QStringLiteral("smil:direction")) == QLatin1String("reverse");
        if (type == QLatin1String("fade")) {
            if (subtype == QLatin1String("fadeOverColor") || subtype == QLatin1String("fadeToColor") || subtype == QLatin1String("fadeFromColor")) {
                const QColor c = colourOf(p.value(QStringLiteral("smil:fadeColor"), QStringLiteral("#000000")));
                slide.transition = !c.isValid() || c.lightness() < 40 ? 6 : 1;
                if (slide.transition == 1) warnings.add(QStringLiteral("A fade through a colour other than black was shown as a plain fade"), number);
            } else slide.transition = 1;
        } else if (type == QLatin1String("pushWipe")) { slide.transition = 2; slide.transitionDirection = direction(); }
        else if (type == QLatin1String("slideWipe")) { slide.transition = reverse ? 5 : 4; slide.transitionDirection = direction(); }
        else if (type == QLatin1String("zoom")) slide.transition = subtype == QLatin1String("rotateIn") ? 8 : 7;
        else {
            slide.transition = 1;
            warnings.add(QStringLiteral("A %1 transition was shown as a fade").arg(type), number);
        }
        if (seconds > 0) slide.transitionSeconds = qBound(0.05, seconds, 10.0);
    }

    // The words of a frame, a paragraph a line.
    static QString plainText(const El &el) {
        QStringList lines;
        QVector<const El *> paragraphs;
        std::function<void(const El &)> walk = [&](const El &e) {
            for (const auto &k : e.kids) {
                if (k.tag == QLatin1String("text:p") || k.tag == QLatin1String("text:h")) {
                    QString line;
                    std::function<void(const El &)> words = [&](const El &w) {
                        for (const auto &x : w.kids) {
                            if (x.isText()) line += x.text;
                            else if (x.tag == QLatin1String("text:s")) line += QString(qBound(1, x.a("text:c", QStringLiteral("1")).toInt(), 1000), QLatin1Char(' '));
                            else if (x.tag == QLatin1String("text:tab")) line += QLatin1Char('\t');
                            else if (x.tag == QLatin1String("text:line-break")) line += QLatin1Char('\n');
                            else if (!x.tag.contains(QLatin1String("annotation"))) words(x);
                        }
                    };
                    words(k);
                    lines.append(line);
                } else if (!k.isText()) walk(k);
            }
        };
        walk(el);
        return lines.join(QLatin1Char('\n')).trimmed();
    }

    QHash<QString, QString> footers, dates;   // presentation:footer-decl / date-time-decl by name

    void convertPage(const El *page, int number) {
        Slide slide;
        slide.id = slideIdByName.value(page->a("draw:name"), Edit::newId(QStringLiteral("slide")));
        QString masterName = page->a("draw:master-page-name");
        if (!masters.contains(masterName)) masterName = masters.isEmpty() ? QString() : doc.masters.isEmpty() ? QString() : masters.begin().key();
        for (auto it = masters.cbegin(); it != masters.cend(); ++it)
            if (it.key() == masterName) slide.layoutId = it->layoutId;
        const MasterInfo info = masters.value(masterName);
        Master *master = info.index >= 0 ? &doc.masters[info.index] : nullptr;

        const Props p = st.props(Styles::Content, QStringLiteral("drawing-page"), page->a("draw:style-name"), "style:drawing-page-properties", false);
        slide.skipped = p.value(QStringLiteral("presentation:visibility")) == QLatin1String("hidden");
        slide.showMasterObjects = p.value(QStringLiteral("presentation:background-objects-visible"), QStringLiteral("true")) != QLatin1String("false");
        transition(p, slide, page, number);

        // The page's own background.
        Ctx ctx;
        ctx.scope = Styles::Content;
        ctx.slide = number;
        ctx.objects = &slide.objects;
        ctx.masterName = masterName;
        const QString fillKind = p.value(QStringLiteral("draw:fill"));
        bool ownBackground = false;
        if (!fillKind.isEmpty() && fillKind != QLatin1String("none")) {
            const Fill fill = fillOf(p, number);
            if (fill.style == 0) {
                slide.background = fill.color.isValid() ? fill.color : QColor(Qt::white);
                slide.backgroundOverride = true;
            } else if (fill.style != 5) {
                slide.background = master ? master->background : QColor(Qt::white);
                slide.backgroundOverride = true;
                slide.objects.append(backgroundBox(fill));
                ownBackground = true;
            }
        }
        // The master's shapes draw over the slide's background in OmaShow, so a
        // master whose background is a shape, under a slide with its own, has
        // its other shapes brought onto the slide above that background.
        if (master && slide.showMasterObjects && (ownBackground || (slide.backgroundOverride && info.backgroundObject))) {
            for (int i = info.backgroundObject ? 1 : 0; i < master->objects.size(); ++i) {
                SceneObject copy = master->objects[i];
                copy.id = Edit::newId(QStringLiteral("shape"));
                copy.locked = true;
                slide.objects.append(copy);
            }
            slide.showMasterObjects = false;
        }

        objectByShapeId.clear();
        pendingConnectors.clear();
        objects(*page, ctx);
        resolveConnectors(slide.objects);

        // Page number, date and footer, as the page asks for them.
        const bool number_ = truthy(p.value(QStringLiteral("presentation:display-page-number")));
        const bool footer = truthy(p.value(QStringLiteral("presentation:display-footer")));
        const bool date = truthy(p.value(QStringLiteral("presentation:display-date-time")));
        slide.showMasterFields = number_ || footer || date;
        if (master && slide.showMasterFields) {
            auto &f = master->fields;
            if (number_) f.showNumber = true;
            if (footer) {
                f.showFooter = true;
                const QString text = footers.value(page->a("presentation:use-footer-name"));
                if (f.footer.isEmpty() && !text.isEmpty()) f.footer = text;
            }
            if (date) {
                f.showDate = true;
                const QString text = dates.value(page->a("presentation:use-date-time-name"));
                if (f.date.isEmpty() && !text.isEmpty()) f.date = text;
            }
        }

        // Words for the speaker.
        if (const El *notes = page->child("presentation:notes"))
            for (const auto *f : notes->all("draw:frame"))
                if (f->a("presentation:class") == QLatin1String("notes"))
                    if (const El *box = f->child("draw:text-box")) slide.notes = plainText(*box);

        // Comments.
        for (const auto &k : page->kids) {
            if (k.tag != QLatin1String("officeooo:annotation") && k.tag != QLatin1String("office:annotation")) continue;
            Comment c;
            c.id = Edit::newId(QStringLiteral("comment"));
            c.slideId = slide.id;
            const El *who = k.child("dc:creator");
            const El *when = k.child("dc:date");
            c.author = who ? who->words().trimmed() : QString();
            if (c.author.isEmpty()) c.author = QStringLiteral("Unknown");
            c.created = when ? when->words().trimmed() : QString();
            c.text = plainText(k);
            c.resolved = truthy(k.a("loext:resolved"));
            doc.comments.append(c);
        }
        doc.slides.append(slide);
    }

    bool run(const QByteArray &raw, QString *error) {
        const bool package = raw.startsWith("PK");
        std::unique_ptr<Zip::Reader> archive;
        if (package) {
            archive = std::make_unique<Zip::Reader>(raw);
            if (!archive->isValid()) { *error = QStringLiteral("this is not an OpenDocument presentation: %1.").arg(archive->error()); return false; }
            zip = archive.get();
            if (zip->contains(QStringLiteral("META-INF/manifest.xml")) && zip->read(QStringLiteral("META-INF/manifest.xml")).contains("encryption-data")) {
                *error = QStringLiteral("this presentation is protected by a password, which OmaShow cannot open.");
                return false;
            }
            const QByteArray mimetype = zip->read(QStringLiteral("mimetype")).trimmed();
            if (!mimetype.isEmpty() && !mimetype.contains("presentation")) {
                *error = QStringLiteral("this OpenDocument file is not a presentation (%1).").arg(QString::fromLatin1(mimetype));
                return false;
            }
            QString why;
            if (!zip->contains(QStringLiteral("content.xml")) || !parseXml(zip->read(QStringLiteral("content.xml")), &content, &why)) {
                *error = QStringLiteral("this is not an OpenDocument presentation: its content could not be read%1.").arg(why.isEmpty() ? QString() : QStringLiteral(" (") + why + QLatin1Char(')'));
                return false;
            }
            if (zip->contains(QStringLiteral("styles.xml"))) parseXml(zip->read(QStringLiteral("styles.xml")), &styles, &why);
            docContent = &content;
            docStyles = styles.tag.isEmpty() ? nullptr : &styles;
        } else {
            QString why;
            if (!parseXml(raw, &flat, &why) || flat.tag != QLatin1String("office:document")) {
                *error = QStringLiteral("this is not an OpenDocument presentation%1.").arg(why.isEmpty() ? QString() : QStringLiteral(": ") + why);
                return false;
            }
            docContent = &flat;
            docStyles = &flat;
        }

        // Styles: common ones, then each part's own.
        if (docStyles) {
            st.addStyles(docStyles->child("office:styles"), st.common, st.listsCommon, true);
            st.addStyles(docStyles->child("office:automatic-styles"), st.autoMasters, st.listsMasters, false);
            st.addFonts(docStyles->child("office:font-face-decls"));
        }
        st.addStyles(docContent->child("office:automatic-styles"), st.autoContent, st.listsContent, false);
        st.addFonts(docContent->child("office:font-face-decls"));
        if (!package) { st.autoMasters = st.autoContent; st.listsMasters = st.listsContent; }

        const El *body = docContent->child("office:body");
        const El *presentation = body ? body->child("office:presentation") : nullptr;
        if (!presentation) {
            *error = body && body->child("office:drawing") ? QStringLiteral("this is an OpenDocument drawing, not a presentation.")
                                                            : QStringLiteral("this is not an OpenDocument presentation: it has no presentation inside.");
            return false;
        }
        const El *masterStyles = docStyles ? docStyles->child("office:master-styles") : nullptr;
        QVector<const El *> masterPages = masterStyles ? masterStyles->all("style:master-page") : QVector<const El *>();

        // Size, from the first master's page.
        qreal widthCm = 28, heightCm = 15.75;
        for (const auto *mp : masterPages) {
            const El *layout = st.pageLayouts.value(mp->a("style:page-layout-name"));
            const El *props = layout ? layout->child("style:page-layout-properties") : nullptr;
            if (!props) continue;
            const qreal w = centimetres(props->a("fo:page-width")), h = centimetres(props->a("fo:page-height"));
            if (!std::isnan(w) && !std::isnan(h) && w > 0 && h > 0) { widthCm = w; heightCm = h; }
            break;
        }
        const qreal ratio = qBound(0.125, heightCm / widthCm, 8.0);
        doc.size = QSizeF(1920, qRound(1920 * ratio));
        k = 1920 / widthCm;

        // Slides, named before anything can link to them.
        const auto pages = presentation->all("draw:page");
        for (const auto *page : pages) {
            const QString name = page->a("draw:name");
            if (!name.isEmpty() && !slideIdByName.contains(name)) slideIdByName.insert(name, Edit::newId(QStringLiteral("slide")));
        }
        for (const auto *decl : presentation->all("presentation:footer-decl")) footers.insert(decl->a("presentation:name"), decl->words().trimmed());
        for (const auto *decl : presentation->all("presentation:date-time-decl"))
            if (decl->a("presentation:source", QStringLiteral("fixed")) == QLatin1String("fixed")) dates.insert(decl->a("presentation:name"), decl->words().trimmed());

        for (const auto *mp : masterPages) readMaster(mp);
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
            MasterInfo info{plain.id, layout.id, QString(), 0, false};
            masters.insert(QString(), info);
        }

        doc.transition = 0;
        doc.transitionDuration = 0.5;
        int number = 0;
        for (const auto *page : pages) convertPage(page, ++number);
        if (doc.slides.isEmpty()) {
            // A template with no slides yet: one empty slide on its first master.
            Slide slide;
            slide.id = Edit::newId(QStringLiteral("slide"));
            slide.layoutId = doc.layouts.first().id;
            slide.background = doc.masters.first().background;
            doc.slides.append(slide);
        }
        // Custom shows.
        if (const El *settings = presentation->child("presentation:settings")) {
            for (const auto *show : settings->all("presentation:show")) {
                CustomShow s;
                s.id = Edit::newId(QStringLiteral("show"));
                s.name = show->a("presentation:name");
                for (const auto &name : show->a("presentation:pages").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                    const QString id = slideIdByName.value(name.trimmed());
                    if (!id.isEmpty()) s.slideIds.append(id);
                }
                if (!s.slideIds.isEmpty()) doc.shows.append(s);
            }
        }

        // The deck's palette and type: from the first master.
        if (!doc.masters.isEmpty()) {
            doc.theme.name = doc.masters.first().name.isEmpty() ? QStringLiteral("Imported") : doc.masters.first().name;
            doc.theme.colors[QStringLiteral("background")] = doc.masters.first().background;
            if (!doc.layouts.isEmpty())
                for (const auto &p : doc.layouts.first().placeholders) {
                    if (p.id == QLatin1String("title")) { doc.theme.colors[QStringLiteral("foreground")] = p.textColor; doc.theme.fonts[QStringLiteral("heading")] = p.fontFamily; }
                    if (p.id == QLatin1String("body")) doc.theme.fonts[QStringLiteral("body")] = p.fontFamily;
                }
        }
        // What OmaShow leaves out.
        QVector<const El *> animations;
        for (const auto *page : pages) {
            QVector<const El *> found;
            page->collect("anim:par", found);
            for (const auto *par : found)
                if (par->a("presentation:node-type") == QLatin1String("on-click") || par->a("presentation:node-type") == QLatin1String("with-previous") ||
                    par->a("presentation:node-type") == QLatin1String("after-previous")) { animations.append(par); break; }
        }
        if (!animations.isEmpty()) warnings.add(QStringLiteral("Builds (animations) were left out for now"), 0);
        return true;
    }
};

} // namespace

namespace Odp {

Result read(const QByteArray &raw) {
    Result result;
    Reader reader;
    reader.scratchDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QString error;
    if (!reader.run(raw, &error)) { result.error = error; return result; }
    result.document = reader.doc;
    const auto keep = [](QVector<SceneObject> &objects) {
        for (auto &o : objects) if (o.fitKept == QLatin1String("*")) o.fitKept = TextLayout::keptFitKey(o);
    };
    for (auto &slide : result.document.slides) keep(slide.objects);
    for (auto &master : result.document.masters) keep(master.objects);
    for (auto &layout : result.document.layouts) keep(layout.placeholders);
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

} // namespace Odp
