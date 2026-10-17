#include "render/textlayout.h"
#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QCache>
#include <QSet>
#include <QMutex>
#include "core/fontswap.h"
#include <QCryptographicHash>
#include <climits>
#include <QtMath>
#include <QImage>
#include "core/textruns.h"
#include "render/mathlayout.h"
#include "core/shape.h"
#include <QDataStream>
#include <cmath>
#include <memory>
namespace {
QFont fontFor(const SceneObject &o, qreal size) {
    QFont font(o.fontFamily);
    font.setPixelSize(qMax(1, int(size)));
    font.setWeight(QFont::Weight(o.fontWeight));
    font.setItalic(o.italic);
    font.setUnderline(o.underline);
    font.setStrikeOut(o.strike);
    font.setLetterSpacing(QFont::AbsoluteSpacing, o.letterSpacing);
    // Capitals as a transform, not a different string: run offsets are against
    // what was typed, and a few letters change length when uppercased.
    font.setCapitalization(o.uppercase ? QFont::AllUppercase : QFont::MixedCase);
    return font;
}
Qt::Alignment alignment(int value) {
    return value == 1   ? Qt::AlignHCenter
           : value == 2 ? Qt::AlignRight
           : value == 3 ? Qt::AlignJustify
                        : Qt::AlignLeft;
}
// 0 none, 1 bullets, 2 numbers, then the rest of the markers people ask for.
// Bullets nest the way they do on paper: disc, then circle, then square.
QTextListFormat::Style listStyleFor(int style, int level) {
    switch (style) {
    case 1: return level <= 1   ? QTextListFormat::ListDisc
                   : level == 2 ? QTextListFormat::ListCircle
                                : QTextListFormat::ListSquare;
    case 2: return QTextListFormat::ListDecimal;
    case 3: return QTextListFormat::ListCircle;
    case 4: return QTextListFormat::ListSquare;
    case 5: return QTextListFormat::ListLowerAlpha;
    case 6: return QTextListFormat::ListUpperAlpha;
    case 7: return QTextListFormat::ListLowerRoman;
    case 8: return QTextListFormat::ListUpperRoman;
    }
    return QTextListFormat::ListDisc;
}
bool numbered(int style) { return style == 2 || style >= 5; }
bool maths(const SceneObject &o) { return o.textKind == 1; }
bool onPath(const SceneObject &o) { return o.textKind == 2 && !o.pathData.isEmpty(); }

// The words, laid along a line that bends. Each letter sits on the path at its
// own angle, so the shape of the line is what the reader follows.
struct PathRun {
    QPointF where;
    qreal angle = 0;
    QString letter;
};
struct PathText {
    QVector<PathRun> letters;
    qreal wanted = 0;    // how much room the words asked for
    qreal length = 0;    // how much the path has
    qreal height = 0;
    qreal lift = 0;      // across the line, not down the page
};
// A closed line has no beginning, so give it one people expect: the top, read
// left to right. An open line is read the way it was drawn.
qreal startOfClosedPath(QPainterPath &path, bool *closed) {
    *closed = false;
    if (path.elementCount() < 2) return 0;
    const auto first = path.pointAtPercent(0), last = path.pointAtPercent(1);
    if (QLineF(first, last).length() > qMax(1.0, path.boundingRect().width() * .01)) return 0;
    *closed = true;
    const int samples = 360;
    const auto topmost = [&](const QPainterPath &candidate) {
        int best = 0;
        for (int i = 1; i < samples; ++i)
            if (candidate.pointAtPercent(qreal(i) / samples).y() <
                candidate.pointAtPercent(qreal(best) / samples).y())
                best = i;
        return best;
    };
    int top = topmost(path);
    // Going forward from the top, the words should travel to the right.
    const auto after = path.pointAtPercent(qreal((top + samples / 36) % samples) / samples);
    if (after.x() < path.pointAtPercent(qreal(top) / samples).x()) {
        path = path.toReversed();
        top = topmost(path);
    }
    return path.length() * qreal(top) / samples;
}
PathText alongPath(const SceneObject &o, qreal size) {
    PathText laid;
    QPainterPath path = Shape::path(o);
    laid.length = path.length();
    if (laid.length <= 0) return laid;
    bool closed = false;
    const qreal begin = startOfClosedPath(path, &closed);
    const QFont font = fontFor(o, size);
    const QFontMetricsF metrics(font);
    laid.height = metrics.height();
    const QString words = (o.uppercase ? o.text.toUpper() : o.text).simplified();
    for (const QChar &letter : words) laid.wanted += metrics.horizontalAdvance(letter);
    // Where the words start is the box's own alignment, read along the line. A
    // closed line is aligned about its top rather than about an end it has not
    // got, so centred words sit at the top the way they are asked to.
    const qreal spare = laid.length - laid.wanted;
    qreal at = closed ? (o.textAlign == 1   ? -laid.wanted / 2
                         : o.textAlign == 2 ? -laid.wanted
                                            : 0)
                      : (o.textAlign == 1   ? qMax(0.0, spare / 2)
                         : o.textAlign == 2 ? qMax(0.0, spare)
                                            : 0);
    // And which side of the line they sit on is its vertical alignment.
    laid.lift = o.verticalAlign == 0 ? -metrics.descent()
              : o.verticalAlign == 2 ? metrics.ascent() : 0;
    for (const QChar &letter : words) {
        const qreal advance = metrics.horizontalAdvance(letter);
        const qreal centre = at + advance / 2;
        at += advance;
        if (!closed && centre > laid.length) break;
        const qreal along = std::fmod(std::fmod(centre + begin, laid.length) + laid.length,
                                      laid.length);
        const qreal percent = path.percentAtLength(along);
        laid.letters.append({path.pointAtPercent(percent), path.angleAtPercent(percent),
                             QString(letter)});
    }
    return laid;
}
// An equation drawn from the words that made it, at the box's own type size.
MathLayout::Rendered equation(const SceneObject &o, qreal size) {
    return MathLayout::build(o.text, fontFor(o, size), size, o.textAlign);
}
bool legacy(const SceneObject &o) {
    return !maths(o) && !onPath(o) && o.origFont.isEmpty() && o.textGlowRadius <= 0 && !o.shadowEnabled && o.runs.isEmpty() && o.textAlign == 0 && o.verticalAlign == 1 && o.lineHeight == 100 &&
           o.paragraphSpacing == 0 && o.textIndent == 0 && o.listStyle == 0 && o.textFit == 0 &&
           o.tabStop == 0 && o.columns <= 1 && o.direction == 0;
}

// How wide one column is, and how far apart the columns sit.
qreal columnGap(const SceneObject &o, qreal size) {
    return o.columnGap > 0 ? o.columnGap : size * 0.8;
}
qreal columnWidth(const SceneObject &o, qreal size) {
    const int columns = qBound(1, o.columns, 6);
    if (columns <= 1) return qMax(1.0, o.rect.width());
    const qreal gaps = columnGap(o, size) * (columns - 1);
    return qMax(1.0, (o.rect.width() - gaps) / columns);
}
// Room past the box's right edge for the last letter's ink: PowerPoint fills a
// line by the letters' advance widths, but Qt also counts a swash that hangs
// past the last advance (script fonts), and breaks lines PowerPoint keeps
// whole. Lines are laid out that much wider and drawn shifted back by the
// alignment's share of it, so nothing moves. Not for justified text, which
// would stretch into it, or columns.
qreal overhang(const SceneObject &o, qreal size) {
    if (o.textAlign == 3 || o.columns > 1) return 0;
    // The furthest any of this text's own letters hangs past its advance (a
    // font's overall figure is often some rare mark hanging a whole em).
    const QFontMetricsF metrics(fontFor(o, size));
    qreal most = 0;
    QSet<QChar> seen;
    for (const QChar c : o.text)
        if (!c.isSpace() && !seen.contains(c)) { seen.insert(c); most = qMax(most, -metrics.rightBearing(c)); }
    return most;
}
qreal overhangShift(const SceneObject &o, qreal size) {
    const qreal extra = overhang(o, size);
    return o.textAlign == 1 ? -extra / 2 : o.textAlign == 2 ? -extra : 0;
}

std::shared_ptr<QTextDocument> layout(const SceneObject &o, qreal size) {
    // QTextDocument belongs to its creating thread. Cache per thread, with a
    // memory budget, and exclude position/opacity so animation reuses layout.
    static thread_local QCache<QByteArray, std::shared_ptr<QTextDocument>> cache(16*1024);
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream << o.text << o.fontFamily << size << o.rect.width() << o.rect.height()
           << o.tabStop << o.columns << o.columnGap << o.direction << o.fontWeight
           << o.italic << o.underline << o.strike << o.letterSpacing << o.uppercase
           << o.textAlign << o.lineHeight << o.origFont << o.fontScale << o.paragraphSpacing << o.textIndent
           << o.listStyle << o.listStart << o.textColor;
    for (const auto &run : o.runs)
        stream << run.start << run.length << run.weight << run.italic << run.underline
               << run.strike << run.baseline << run.fontSize << run.fontFamily << run.color;
    if (auto *cached = cache.object(key)) return *cached;
    auto doc = std::make_shared<QTextDocument>();
    doc->setUndoRedoEnabled(false);
    doc->setDocumentMargin(0);
    doc->setDefaultFont(fontFor(o, size));
    doc->setIndentWidth(size * 1.2);
    doc->setTextWidth(columnWidth(o, size) + overhang(o, size));
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    // Tabs line up on a stop rather than nesting, unless the box is a list,
    // where a leading tab is what makes a sub-item.
    if (o.listStyle == 0)
        option.setTabStopDistance(o.tabStop > 0 ? o.tabStop : size * 4);
    if (o.direction == 1) option.setTextDirection(Qt::LeftToRight);
    if (o.direction == 2) option.setTextDirection(Qt::RightToLeft);
    doc->setDefaultTextOption(option);
    // Columns are pages: the text flows down one and into the next.
    if (o.columns > 1)
        doc->setPageSize(QSizeF(columnWidth(o, size), qMax(1.0, o.rect.height())));
    QTextCursor cursor(doc.get());
    const auto paragraphs = o.text.split('\n');
    const auto vertical = FontSwap::verticalFix(o, size);
    QMap<int, QTextList *> lists;
    // Where each paragraph's text starts, in the string and in the document, so
    // runs can be placed after the whole thing is built.
    QVector<int> authoredStart, documentStart, strippedTabs;
    int authored = 0;
    for (int i = 0; i < paragraphs.size(); ++i) {
        if (i)
            cursor.insertBlock();
        QString text = paragraphs.at(i);
        int level = 1;
        int stripped = 0;
        if (o.listStyle)
            while (text.startsWith('\t')) {
                ++level;
                ++stripped;
                text.remove(0, 1);
            }
        level = qMin(8, level);
        QTextBlockFormat block;
        block.setAlignment(alignment(o.textAlign));
        if (o.direction != 0)
            block.setLayoutDirection(o.direction == 2 ? Qt::RightToLeft : Qt::LeftToRight);
        block.setLineHeight(qBound(10.0, o.lineHeight * vertical.line, 500.0), QTextBlockFormat::ProportionalHeight);
        block.setBottomMargin(i + 1 < paragraphs.size() ? o.paragraphSpacing : 0);
        block.setLeftMargin(o.textIndent);
        cursor.setBlockFormat(block);
        QTextCharFormat chars;
        chars.setFont(fontFor(o, size));
        chars.setForeground(o.textColor);
        cursor.setCharFormat(chars);
        authoredStart.append(authored);
        documentStart.append(cursor.position());
        strippedTabs.append(stripped);
        authored += paragraphs.at(i).size() + 1;   // the newline that split it
        cursor.insertText(text);
        if (o.listStyle && !text.isEmpty()) {
            if (!lists.contains(level)) {
                QTextListFormat list;
                list.setStyle(listStyleFor(o.listStyle, level));
                list.setIndent(level);
                if (numbered(o.listStyle)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
                    list.setStart(level == 1 ? o.listStart : 1);
#else
                    Q_UNUSED(o.listStart); // Qt < 6.6 always starts numbered lists at 1
#endif
                }
                lists[level] = cursor.createList(list);
            } else
                lists[level]->add(cursor.block());
            for (auto it = lists.begin(); it != lists.end();)
                if (it.key() > level)
                    it = lists.erase(it);
                else
                    ++it;
        }
    }
    // Stretches that look different: mapped back onto the built document.
    for (const auto &run : o.runs) {
        for (int i = 0; i < paragraphs.size(); ++i) {
            const int bodyStart = authoredStart.at(i) + strippedTabs.at(i);
            const int bodyEnd = authoredStart.at(i) + paragraphs.at(i).size();
            const int from = qMax(run.start, bodyStart);
            const int to = qMin(run.start + run.length, bodyEnd);
            if (from >= to) continue;
            QTextCursor scope(doc.get());
            scope.setPosition(documentStart.at(i) + (from - bodyStart));
            scope.setPosition(documentStart.at(i) + (to - bodyStart), QTextCursor::KeepAnchor);
            QTextCharFormat format;
            QFont font = fontFor(o, run.fontSize > 0 ? run.fontSize : size);
            if (!run.fontFamily.isEmpty()) font.setFamilies({run.fontFamily});
            if (run.weight != 0) font.setWeight(QFont::Weight(run.weight));
            if (run.italic != 0) font.setItalic(run.italic == 1);
            if (run.underline != 0) font.setUnderline(run.underline == 1);
            format.setFont(font);
            if (run.strike != 0) format.setFontStrikeOut(run.strike == 1);
            if (run.color.isValid()) format.setForeground(run.color);
            if (run.baseline == 1) format.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
            if (run.baseline == 2) format.setVerticalAlignment(QTextCharFormat::AlignSubScript);
            scope.mergeCharFormat(format);
            // A list marker is drawn in its paragraph's colour: let it follow
            // a colour that starts the paragraph, so a hidden line hides its bullet.
            if (run.color.isValid() && from == bodyStart) {
                QTextCharFormat marker;
                marker.setForeground(run.color);
                scope.mergeBlockCharFormat(marker);
            }
        }
    }
    cache.insert(key, new std::shared_ptr<QTextDocument>(doc), qMax(1, int((o.text.size()*10+4096)/1024)));
    return doc;
}
QSizeF contentSize(const SceneObject &o, qreal size) {
    if (onPath(o)) {
        const auto laid = alongPath(o, size);
        // Along a path, running out of room is running off the end of the line.
        return QSizeF(laid.wanted > laid.length ? o.rect.width() + 1 : o.rect.width(),
                      laid.height);
    }
    if (maths(o)) {
        const auto rendered = equation(o, size);
        // What could not be read is drawn as the words it was typed as, so the
        // box is measured that way too.
        if (!rendered.ok) {
            auto words = o;
            words.textKind = 0;
            return contentSize(words, size);
        }
        return rendered.size;
    }
    if (o.columns > 1) {
        // What matters is how many columns the text needs, expressed as height.
        auto doc = layout(o, size);
        const int pages = qMax(1, doc->pageCount());
        const qreal height = qMax(1.0, o.rect.height());
        const int columns = qBound(1, o.columns, 6);
        return QSizeF(o.rect.width(), pages <= columns ? height * pages / columns
                                                       : height * pages / columns);
    }
    if (legacy(o)) {
        const QFontMetricsF metrics(fontFor(o, size));
        return metrics
            .boundingRect(QRectF(0, 0, qMax(1.0, o.rect.width()), 1000000), Qt::TextWordWrap,
                          o.uppercase ? o.text.toUpper() : o.text)
            .size();
    }
    const QSizeF laid = layout(o, size)->documentLayout()->documentSize();
    return QSizeF(qMax<qreal>(0, laid.width() - overhang(o, size)), laid.height());
}
} // namespace
QString TextLayout::keptFitKey(const SceneObject &o) {
    QByteArray key = o.text.toUtf8();
    key += '|' + QByteArray::number(qRound(o.rect.width())) + 'x' + QByteArray::number(qRound(o.rect.height()));
    key += '|' + QByteArray::number(qRound(o.fontSize / (o.fontScale > 0 ? o.fontScale : 1) * 10));
    for (const auto &run : o.runs)
        key += '|' + QByteArray::number(qRound(run.fontSize / (run.fontScale > 0 ? run.fontScale : 1) * 10));
    return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16));
}

static bool fitIsKept(const SceneObject &o) {
    return o.textFit == 1 && !o.fitKept.isEmpty() && o.fitKept == TextLayout::keptFitKey(o);
}

TextLayout::Metrics TextLayout::measure(const SceneObject &o) {
    Metrics m;
    if (o.type != ObjectType::Text)
        return m;
    m.effectiveSize = o.fontSize;
    auto dimensions = contentSize(o, o.fontSize);
    m.naturalHeight = dimensions.height();
    m.renderedHeight = m.naturalHeight;
    // Words only ever run out of height, because they wrap. An equation does not
    // wrap, so shrinking it has to watch its width too.
    const auto fits = [&o](qreal size) {
        const auto room = contentSize(o, size);
        return room.height() - FontSwap::verticalFix(o, size).shift <= o.rect.height() &&
               (!maths(o) || room.width() <= o.rect.width());
    };
    if (o.textFit == 1 && !fitIsKept(o) && !fits(o.fontSize)) {
        int lo = 4, hi = qMax(4, int(o.fontSize));
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (fits(mid))
                lo = mid;
            else
                hi = mid - 1;
        }
        m.effectiveSize = lo;
        dimensions = contentSize(o, lo);
        m.renderedHeight = dimensions.height();
    }
    // A free stand-in drawn moved up to sit where the Office font would (see
    // FontSwap::verticalFix) ends that much higher too.
    if (const auto fix = FontSwap::verticalFix(o, m.effectiveSize); fix.shift != 0)
        m.renderedHeight = qMax<qreal>(0, m.renderedHeight - fix.shift);
    // A kept PowerPoint size is PowerPoint's own fit: not a warning.
    m.overflow = !fitIsKept(o) && (m.renderedHeight > o.rect.height() + .5 ||
                                   dimensions.width() > o.rect.width() + .5);
    return m;
}
namespace {
// Distance from every pixel to the nearest one with a mark, in pixels (exact
// Euclidean, Felzenszwalb & Huttenlocher's two passes).
QVector<float> distanceTo(const QVector<bool> &marked, int w, int h) {
    const float far = 1e20f;
    QVector<float> d(w * h);
    for (int i = 0; i < w * h; ++i) d[i] = marked[i] ? 0 : far;
    const int n = qMax(w, h);
    QVector<float> f(n), out(n), z(n + 1);
    QVector<int> v(n);
    auto pass = [&](int count) {   // squared distance along one line, f → out
        int k = 0;
        v[0] = 0; z[0] = -far; z[1] = far;
        for (int q = 1; q < count; ++q) {
            auto meet = [&](int r) { return ((f[q] + q * q) - (f[r] + r * r)) / (2.0f * (q - r)); };
            float s = meet(v[k]);
            while (s <= z[k]) s = meet(v[--k]);   // z[0] is below every meeting point
            ++k; v[k] = q; z[k] = s; z[k + 1] = far;
        }
        k = 0;
        for (int q = 0; q < count; ++q) {
            while (z[k + 1] < q) ++k;
            out[q] = (q - v[k]) * (q - v[k]) + f[v[k]];
        }
    };
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) f[y] = d[y * w + x];
        pass(h);
        for (int y = 0; y < h; ++y) d[y * w + x] = out[y];
    }
    for (int y = 0; y < h; ++y) {
        std::copy(d.cbegin() + y * w, d.cbegin() + (y + 1) * w, f.begin());
        pass(w);
        for (int x = 0; x < w; ++x) d[y * w + x] = std::sqrt(out[x]);
    }
    return d;
}

// Blurs a plane of strengths in place: a Gaussian of this standard
// deviation (pixels), one direction then the other.
void gaussian(QVector<float> &plane, int w, int h, qreal sigma) {
    if (sigma < 0.3) return;
    const int r = qCeil(sigma * 3);
    QVector<float> k(2 * r + 1);
    float sum = 0;
    for (int i = -r; i <= r; ++i) sum += (k[i + r] = float(std::exp(-i * i / (2 * sigma * sigma))));
    for (auto &v : k) v /= sum;
    QVector<float> line(qMax(w, h));
    auto pass = [&](int count, int stride, float *start) {
        for (int i = 0; i < count; ++i) line[i] = start[i * stride];
        for (int i = 0; i < count; ++i) {
            float v = 0;
            for (int j = qMax(-r, -i); j <= qMin(r, count - 1 - i); ++j) v += line[i + j] * k[j + r];
            start[i * stride] = v;
        }
    };
    for (int y = 0; y < h; ++y) pass(w, 1, plane.data() + y * w);
    for (int x = 0; x < w; ++x) pass(h, w, plane.data() + x);
}

// What a box's words cast behind them: PowerPoint's text glow and outer
// shadow, made at the size they are drawn and kept, so a slide that doesn't
// change isn't worked out again.
//
// Both follow PowerPoint's own rules, measured from its exports of test decks
// of text sizes 12-96 pt (history/tools/glow); PowerPoint shrinks its text
// effects on smaller words, so each depends on the text size as well as on its
// setting (all in points):
//   glow    a band of the glow colour around the letters' outline, at full
//           strength near them, half strength `reach` out, fading over a
//           third of that:       reach = 0.0433 x glow^0.857 x size^0.653
//   shadow  cast by the letters AND their glow (a glowing word's shadow is
//           the whole glowing shape), moved   0.0561 x distance^0.929 x size^0.717
//           in its direction and blurred by   0.0133 x blur^1.082 x size^0.709
//           (a Gaussian's standard deviation).
// Each is exactly as strong as its colour's own transparency.
void paintEffects(QPainter &painter, const SceneObject &o, QTextDocument &doc,
                  const QAbstractTextDocumentLayout::PaintContext &context, const QPointF &origin, const QSizeF &area,
                  qreal fontSize, qreal slideHeight) {
    // Shared by every thread that draws slides (screen, thumbnails, export).
    static QMutex lock;
    static QCache<QString, QImage> cache(48 * 1024 * 1024);   // bytes
    const QTransform &t = painter.worldTransform();
    qreal scale = std::sqrt(t.m11() * t.m11() + t.m12() * t.m12());
    const qreal point = qMax<qreal>(1e-3, slideHeight) / 540;   // slide units per point
    const qreal size = qMax<qreal>(1, fontSize) / point;          // the text size, in points
    const bool glows = o.textGlowRadius > 0 && o.textGlowColor.alpha() > 0;
    const bool shadows = o.shadowEnabled && o.shadowColor.alpha() > 0;
    const qreal reach = glows ? point * 0.0433 * std::pow(o.textGlowRadius / point, 0.857) * std::pow(size, 0.653) : 0;
    const qreal soft = reach * 0.343;
    const qreal set = std::hypot(o.shadowX, o.shadowY);
    const qreal moved = shadows && set > 0 ? point * 0.0561 * std::pow(set / point, 0.929) * std::pow(size, 0.717) : 0;
    const QPointF offset = set > 0 ? QPointF(o.shadowX, o.shadowY) * (moved / set) : QPointF();
    const qreal sigma = shadows && o.shadowBlur > 0 ? point * 0.0133 * std::pow(o.shadowBlur / point, 1.082) * std::pow(size, 0.709) : 0;
    const qreal glowEdge = glows ? reach + 4 * soft : 0;
    const qreal margin = glowEdge + (shadows ? moved + 3 * sigma : 0) + 1;
    QSizeF full(area.width() + 2 * margin, area.height() + 2 * margin);
    // Never larger than the screen needs, never a huge picture for a huge zoom.
    scale = qBound(0.05, scale, 4096.0 / qMax(1.0, qMax(full.width(), full.height())));
    const QSize px(qMax(1, qCeil(full.width() * scale)), qMax(1, qCeil(full.height() * scale)));
    const QString key = QStringList{QString::number(quintptr(&doc)), QString::number(qHash(o.text)),
        QString::number(px.width()), QString::number(px.height()), QString::number(area.width()), QString::number(area.height()),
        QString::number(glows ? reach : 0), o.textGlowColor.name(QColor::HexArgb),
        QString::number(shadows), o.shadowColor.name(QColor::HexArgb), QString::number(offset.x()), QString::number(offset.y()),
        QString::number(sigma)}.join(QLatin1Char('|'));
    QImage effects;
    {
        QMutexLocker locked(&lock);
        if (const QImage *kept = cache.object(key)) effects = *kept;   // shared, not copied
    }
    if (effects.isNull()) {
        QImage image(px, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(scale, scale);
        p.translate(margin, margin);
        doc.documentLayout()->draw(&p, context);
        p.end();
        const int w = image.width(), h = image.height();
        QVector<float> letters(w * h), glow(w * h, 0.f);
        for (int y = 0; y < h; ++y) {
            const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < w; ++x) letters[y * w + x] = qAlpha(row[x]) / 255.f;
        }
        if (glows) {
            QVector<bool> inside(w * h);
            for (int i = 0; i < w * h; ++i) inside[i] = letters[i] >= 0.5f;
            const QVector<float> d = distanceTo(inside, w, h);
            const qreal at = reach * scale, fade = qMax<qreal>(0.5, soft * scale), strength = o.textGlowColor.alphaF();
            // Half strength at `reach` from the outline, a smooth edge either side.
            for (int i = 0; i < w * h; ++i) glow[i] = float(strength * 0.5 * std::erfc((d[i] - at) / (fade * M_SQRT2)));
        }
        QVector<float> shadow;
        if (shadows) {
            // The glowing word's own shape, moved (in whole and part pixels) and blurred.
            const qreal dx = offset.x() * scale, dy = offset.y() * scale;
            const int ix = qFloor(dx), iy = qFloor(dy);
            const float fx = float(dx - ix), fy = float(dy - iy);
            auto cast = [&](int x, int y) -> float {
                if (x < 0 || y < 0 || x >= w || y >= h) return 0;
                const float a = letters[y * w + x], g = glow[y * w + x];
                return a + g * (1 - a);
            };
            shadow.resize(w * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int sx = x - ix, sy = y - iy;
                    shadow[y * w + x] = (1 - fy) * ((1 - fx) * cast(sx, sy) + fx * cast(sx - 1, sy))
                                      + fy * ((1 - fx) * cast(sx, sy - 1) + fx * cast(sx - 1, sy - 1));
                }
            gaussian(shadow, w, h, sigma * scale);
            const float strength = float(o.shadowColor.alphaF());
            for (auto &v : shadow) v *= strength;
        }
        const QColor g = o.textGlowColor, c = o.shadowColor;
        for (int y = 0; y < h; ++y) {
            auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < w; ++x) {
                // The glow over the shadow (the words go over both, after).
                const float ga = qBound(0.f, glow[y * w + x], 1.f), sa = shadows ? qBound(0.f, shadow[y * w + x], 1.f) * (1 - ga) : 0.f;
                const float a = ga + sa;
                row[x] = qRgba(qRound(g.red() * ga + c.red() * sa), qRound(g.green() * ga + c.green() * sa),
                               qRound(g.blue() * ga + c.blue() * sa), qRound(a * 255));
            }
        }
        effects = image;
        QMutexLocker locked(&lock);
        cache.insert(key, new QImage(image), int(qMin<qint64>(INT_MAX, qint64(image.sizeInBytes()))));
    }
    painter.drawImage(QRectF(origin - QPointF(margin, margin), full), effects);
}
} // namespace

void TextLayout::paint(QPainter &painter, const SceneObject &o, qreal slideHeight) {
    if (onPath(o)) {
        const auto metrics = measure(o);
        const auto laid = alongPath(o, metrics.effectiveSize);
        painter.save();
        painter.setPen(o.textColor);
        painter.setFont(fontFor(o, metrics.effectiveSize));
        for (const auto &letter : laid.letters) {
            painter.save();
            painter.translate(letter.where);
            painter.rotate(-letter.angle);
            painter.drawText(QPointF(-QFontMetricsF(painter.font())
                                          .horizontalAdvance(letter.letter) / 2,
                                      laid.lift),
                             letter.letter);
            painter.restore();
        }
        painter.restore();
        return;
    }
    if (maths(o)) {
        const auto metrics = measure(o);
        const auto rendered = equation(o, metrics.effectiveSize);
        if (!rendered.ok) {
            // The fallback is honest: the source, as ordinary text, and the
            // review says what stopped it.
            auto words = o;
            words.textKind = 0;
            paint(painter, words);
            return;
        }
        const qreal spare = qMax(0.0, o.rect.height() - rendered.size.height());
        const qreal dy = o.verticalAlign == 1 ? spare / 2 : o.verticalAlign == 2 ? spare : 0;
        const qreal room = qMax(0.0, o.rect.width() - rendered.size.width());
        const qreal dx = o.textAlign == 1 ? room / 2 : o.textAlign == 2 ? room : 0;
        painter.save();
        if (o.textFit < 2 && !fitIsKept(o)) painter.setClipRect(o.rect, Qt::IntersectClip);   // spill and grow draw it all
        MathLayout::paint(painter, rendered, o.rect.topLeft() + QPointF(dx, dy),
                          fontFor(o, metrics.effectiveSize), o.textColor);
        painter.restore();
        return;
    }
    if (legacy(o)) {
        painter.setFont(fontFor(o, o.fontSize));
        painter.setPen(o.textColor);
        painter.drawText(o.rect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                         o.uppercase ? o.text.toUpper() : o.text);
        return;
    }
    const auto metrics = measure(o);
    auto doc = layout(o, metrics.effectiveSize);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, o.textColor);
    if (o.columns > 1) {
        // Each column is one page of the same document, side by side.
        const int columns = qBound(1, o.columns, 6);
        const qreal width = columnWidth(o, metrics.effectiveSize);
        const qreal gap = columnGap(o, metrics.effectiveSize);
        const qreal height = qMax(1.0, o.rect.height());
        painter.save();
        if (o.textFit < 2 && !fitIsKept(o)) painter.setClipRect(o.rect, Qt::IntersectClip);   // spill and grow draw it all
        for (int column = 0; column < qMin(columns, qMax(1, doc->pageCount())); ++column) {
            painter.save();
            painter.translate(o.rect.topLeft() + QPointF(column * (width + gap), -FontSwap::verticalFix(o, metrics.effectiveSize).shift));
            painter.setClipRect(QRectF(0, 0, width, height), Qt::IntersectClip);
            painter.translate(0, -column * height);
            doc->documentLayout()->draw(&painter, context);
            painter.restore();
        }
        painter.restore();
        return;
    }
    const qreal spare = qMax(0.0, o.rect.height() - metrics.renderedHeight);
    const qreal dy = o.verticalAlign == 1 ? spare / 2 : o.verticalAlign == 2 ? spare : 0;
    const QPointF origin = o.rect.topLeft() + QPointF(overhangShift(o, metrics.effectiveSize), dy - FontSwap::verticalFix(o, metrics.effectiveSize).shift);
    // Shadow and glow first, under the words and not cut off at the box.
    if ((o.textGlowRadius > 0 && o.textGlowColor.alpha() > 0) || (o.shadowEnabled && o.shadowColor.alpha() > 0))
        // PowerPoint sizes its effects by its own text size: before a swapped
        // Office font's size factor (FontSwap), which only matches the look.
        paintEffects(painter, o, *doc, context, origin, QSizeF(o.rect.width() + overhang(o, metrics.effectiveSize), qMax(metrics.renderedHeight, o.rect.height())),
                     !o.origFont.isEmpty() && o.fontScale > 0 ? metrics.effectiveSize / o.fontScale : metrics.effectiveSize, slideHeight);
    painter.save();
    if (o.textFit < 2 && !fitIsKept(o)) painter.setClipRect(o.rect, Qt::IntersectClip);   // spill and grow draw it all
    painter.translate(origin);
    doc->documentLayout()->draw(&painter, context);
    painter.restore();
}

QString TextLayout::editorHtml(const SceneObject &object) {
    auto content = object;
    content.uppercase = false;
    // An equation, or words on a path, are edited as what was typed.
    content.textKind = 0;
    return layout(content, measure(object).effectiveSize)->toHtml();
}
