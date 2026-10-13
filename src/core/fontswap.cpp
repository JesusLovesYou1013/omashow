#include "core/fontswap.h"

#include <QFile>
#include <QFontDatabase>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QVariantMap>
#include <cmath>

namespace {

struct Entry {
    QString name;                  // as the table spells it
    int weight = 0;
    QVector<FontSwap::Option> options;
};

const QHash<QString, Entry> &table() {
    static const QHash<QString, Entry> loaded = [] {
        QHash<QString, Entry> out;
        QFile file(QStringLiteral(":/fonts/office-font-map.json"));
        if (!file.open(QIODevice::ReadOnly)) return out;
        const auto fonts = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("fonts")).toObject();
        for (auto it = fonts.begin(); it != fonts.end(); ++it) {
            const auto json = it.value().toObject();
            Entry e;
            e.name = it.key();
            e.weight = json.value(QStringLiteral("wt")).toInt();
            for (const auto &value : json.value(QStringLiteral("o")).toArray()) {
                const auto o = value.toObject();
                FontSwap::Option option;
                option.family = o.value(QStringLiteral("f")).toString();
                option.metric = o.value(QStringLiteral("m")).toInt() == 1;
                option.size = o.value(QStringLiteral("s")).toDouble(1);
                option.width = o.value(QStringLiteral("w")).toDouble(1);
                option.line = o.value(QStringLiteral("l")).toDouble(1);
                option.package = o.value(QStringLiteral("pkg")).toString();
                option.googleFonts = o.value(QStringLiteral("gf")).toString();
                option.github = o.value(QStringLiteral("gh")).toString();
                if (option.family.isEmpty() || !(option.size > 0.2 && option.size < 5)) continue;
                e.options.append(option);
            }
            if (!e.options.isEmpty()) out.insert(it.key().toLower(), e);
        }
        return out;
    }();
    return loaded;
}

bool installed(const QString &family) { return !family.isEmpty() && QFontDatabase::hasFamily(family); }

// Sizes written back must be the ones read: a size untouched since the swap
// is restored from what was stored, not divided back with rounding.
bool same(qreal a, qreal b) { return std::abs(a - b) <= 1e-6 * qMax<qreal>(1.0, std::abs(a)); }

qreal unscale(qreal now, qreal before, qreal scale) {
    if (before > 0 && same(now, before * scale)) return before;
    return now / scale;
}

// Whether a family is still the free match that stood in for an Office
// family, rather than one chosen since.
bool stillSwapped(const QString &office, const QString &family) {
    for (const auto &option : FontSwap::options(office))
        if (option.family.compare(family, Qt::CaseInsensitive) == 0) return true;
    return false;
}

template <typename F>
void eachObject(Document &document, F visit) {
    for (auto &slide : document.slides) for (auto &o : slide.objects) visit(o);
    for (auto &master : document.masters) for (auto &o : master.objects) visit(o);
    for (auto &layout : document.layouts) for (auto &o : layout.placeholders) visit(o);
    for (auto &style : document.textStyles) visit(style.look);
    for (auto &style : document.objectStyles) visit(style.appearance);
}

// office → free → uses
using Tally = QMap<QString, QMap<QString, int>>;

QVariantList tallyList(const Tally &tally) {
    QVariantList rows;
    for (auto it = tally.cbegin(); it != tally.cend(); ++it)
        for (auto f = it->cbegin(); f != it->cend(); ++f) {
            bool metric = false; qreal size = 1;
            for (const auto &o : FontSwap::options(it.key()))
                if (o.family == f.key()) { metric = o.metric; size = o.size; break; }
            rows.append(QVariantMap{{QStringLiteral("office"), it.key()}, {QStringLiteral("free"), f.key()},
                                    {QStringLiteral("metric"), metric}, {QStringLiteral("size"), metric ? 1.0 : size},
                                    {QStringLiteral("uses"), f.value()}});
        }
    return rows;
}

class Swapper {
public:
    FontSwap::Report report;
    Tally tally;

    const FontSwap::Option *pick(const QString &family) {
        if (family.isEmpty()) return nullptr;
        const auto key = family.toLower();
        const auto cached = cache.constFind(key);
        if (cached != cache.cend()) return *cached;
        const FontSwap::Option *chosen = nullptr;
        if (!installed(family)) {
            const auto entry = table().constFind(key);
            if (entry != table().cend())
                for (const auto &option : entry->options)
                    if (installed(option.family)) { chosen = &option; break; }
        }
        cache.insert(key, chosen);
        return chosen;
    }
    static bool scales(const FontSwap::Option *o) { return o && !o->metric && !qFuzzyCompare(o->size, 1.0); }

    // A family swapped earlier for a lesser match (the best was not installed
    // then): the best installed match now, or null when it already is that.
    const FontSwap::Option *better(const QString &office, const QString &current) {
        const auto entry = table().constFind(office.toLower());
        if (entry == table().cend()) return nullptr;
        int now = -1;
        for (int i = 0; i < entry->options.size(); ++i)
            if (entry->options[i].family.compare(current, Qt::CaseInsensitive) == 0) now = i;
        if (now <= 0) return nullptr;   // the best already, or chosen by hand since
        for (int i = 0; i < now; ++i)
            if (installed(entry->options[i].family)) return &entry->options[i];
        return nullptr;
    }
    void count(const QString &office, const QString &free) { ++tally[office][free]; ++report.changed; }

    void object(SceneObject &o) {
        // The box's size as the deck had it, before any factor of ours.
        const qreal boxBefore = o.origFont.isEmpty() || qFuzzyCompare(o.fontScale, 1.0) ? o.fontSize : o.fontSize / o.fontScale;
        qreal boxScale = 1;
        upgrade(o, boxBefore);
        if (o.origFont.isEmpty()) if (const auto *opt = pick(o.fontFamily)) {
            const QString office = o.fontFamily;
            o.origFont = office;
            o.origFontSize = o.fontSize;
            const int named = FontSwap::namedWeight(office);
            if (named && o.fontWeight == 400) { o.origWeight = o.fontWeight; o.fontWeight = named; }
            o.fontFamily = opt->family;
            if (scales(opt)) { o.fontScale = opt->size; o.fontSize = o.fontSize * opt->size; boxScale = opt->size; }
            count(office, opt->family);
        }
        for (auto &run : o.runs) {
            if (!run.fontFamily.isEmpty()) {
                if (!run.origFont.isEmpty()) continue;
                const auto *opt = pick(run.fontFamily);
                if (!opt) {
                    // A family of its own drawn at the box's size: the box's
                    // factor is not this family's, so it keeps the deck's size.
                    if (boxScale != 1 && run.fontSize <= 0) run.fontSize = boxBefore;
                    continue;
                }
                const QString office = run.fontFamily;
                run.origFont = office;
                const int named = FontSwap::namedWeight(office);
                const int weightNow = run.weight ? run.weight : o.origWeight ? o.origWeight : o.fontWeight;
                if (named && weightNow == 400) { run.origWeight = run.weight ? run.weight : 1; run.weight = named; }
                run.fontFamily = opt->family;
                if (scales(opt)) {
                    run.fontScale = opt->size;
                    run.origFontSize = run.fontSize;   // 0: it took the box's size
                    run.fontSize = (run.fontSize > 0 ? run.fontSize : boxBefore) * opt->size;
                } else if (boxScale != 1 && run.fontSize <= 0) {
                    run.fontSize = boxBefore;
                }
                count(office, opt->family);
            } else if (boxScale != 1 && run.fontSize > 0 && qFuzzyCompare(run.fontScale, 1.0)) {
                run.origFontSize = run.fontSize;
                run.fontScale = boxScale;
                run.fontSize *= boxScale;
            }
        }
        for (auto &cell : o.table.cells) {
            auto &style = cell.style;
            const auto family = style.value(QStringLiteral("fontFamily")).toString();
            if (family.isEmpty() || style.contains(QStringLiteral("origFont"))) continue;
            const auto *opt = pick(family);
            if (!opt) continue;
            style.insert(QStringLiteral("origFont"), family);
            style.insert(QStringLiteral("fontFamily"), opt->family);
            const int named = FontSwap::namedWeight(family);
            const int weightNow = style.value(QStringLiteral("fontWeight"), 400).toInt();
            if (named && weightNow == 400) {
                style.insert(QStringLiteral("origWeight"), 400);
                style.insert(QStringLiteral("fontWeight"), named);
            }
            if (scales(opt)) {
                style.insert(QStringLiteral("fontScale"), opt->size);
                const bool own = style.contains(QStringLiteral("fontSize"));
                const qreal before = own ? style.value(QStringLiteral("fontSize")).toDouble() : boxBefore;
                if (own) style.insert(QStringLiteral("origFontSize"), before);
                style.insert(QStringLiteral("fontSize"), before * opt->size);
            }
            count(family, opt->family);
        }
    }

private:
    QHash<QString, const FontSwap::Option *> cache;

    // Moves what an earlier import swapped to a lesser match on to the best
    // one now installed, where the size is still the one the swap set.
    void upgrade(SceneObject &o, qreal boxBefore) {
        if (!o.origFont.isEmpty()) if (const auto *opt = better(o.origFont, o.fontFamily)) {
            const bool untouched = qFuzzyCompare(o.fontScale, 1.0) ? same(o.fontSize, o.origFontSize) : same(o.fontSize, o.origFontSize * o.fontScale);
            if (untouched) {
                const qreal scale = scales(opt) ? opt->size : 1.0;
                o.fontFamily = opt->family;
                o.fontScale = scale;
                o.fontSize = o.origFontSize * scale;
                // Stretches that took the box's family were sized with its factor.
                for (auto &run : o.runs) {
                    if (!run.fontFamily.isEmpty() || qFuzzyCompare(run.fontScale, 1.0) || run.origFontSize <= 0) continue;
                    run.fontSize = run.origFontSize * scale;
                    run.fontScale = scale;
                    if (qFuzzyCompare(scale, 1.0)) run.origFontSize = 0;
                }
                if (scale != 1)
                    for (auto &run : o.runs)
                        if (run.fontFamily.isEmpty() && run.fontSize > 0 && qFuzzyCompare(run.fontScale, 1.0) && run.origFontSize <= 0) {
                            run.origFontSize = run.fontSize; run.fontScale = scale; run.fontSize *= scale;
                        }
                count(o.origFont, opt->family);
            }
        }
        for (auto &run : o.runs) {
            if (run.origFont.isEmpty()) continue;
            const auto *opt = better(run.origFont, run.fontFamily);
            if (!opt) continue;
            const qreal before = run.origFontSize > 0 ? run.origFontSize : boxBefore;
            const qreal expected = qFuzzyCompare(run.fontScale, 1.0) ? before : before * run.fontScale;
            if (qFuzzyCompare(run.fontScale, 1.0) ? run.fontSize > 0 && !same(run.fontSize, expected) && run.origFontSize > 0
                                                  : !same(run.fontSize, expected)) continue;
            const qreal scale = scales(opt) ? opt->size : 1.0;
            run.fontFamily = opt->family;
            run.fontScale = scale;
            if (scale != 1) run.fontSize = before * scale;
            else run.fontSize = run.origFontSize > 0 ? run.origFontSize : !qFuzzyCompare(o.fontScale, 1.0) ? boxBefore : 0;
            count(run.origFont, opt->family);
        }
        for (auto &cell : o.table.cells) {
            auto &style = cell.style;
            const auto office = style.value(QStringLiteral("origFont")).toString();
            if (office.isEmpty()) continue;
            const auto *opt = better(office, style.value(QStringLiteral("fontFamily")).toString());
            if (!opt) continue;
            const qreal oldScale = style.value(QStringLiteral("fontScale"), 1.0).toDouble();
            const bool own = style.contains(QStringLiteral("origFontSize")) || (qFuzzyCompare(oldScale, 1.0) && style.contains(QStringLiteral("fontSize")));
            const qreal before = style.contains(QStringLiteral("origFontSize")) ? style.value(QStringLiteral("origFontSize")).toDouble()
                               : own ? style.value(QStringLiteral("fontSize")).toDouble() : boxBefore;
            if (style.contains(QStringLiteral("fontSize")) && !same(style.value(QStringLiteral("fontSize")).toDouble(), before * oldScale)) continue;
            style.insert(QStringLiteral("fontFamily"), opt->family);
            if (scales(opt)) {
                style.insert(QStringLiteral("fontScale"), opt->size);
                if (own) style.insert(QStringLiteral("origFontSize"), before);
                style.insert(QStringLiteral("fontSize"), before * opt->size);
            } else {
                style.remove(QStringLiteral("fontScale"));
                style.remove(QStringLiteral("origFontSize"));
                if (own) style.insert(QStringLiteral("fontSize"), before); else style.remove(QStringLiteral("fontSize"));
            }
            count(office, opt->family);
        }
    }
};

} // namespace

QVector<FontSwap::Option> FontSwap::options(const QString &officeFamily) {
    const auto it = table().constFind(officeFamily.toLower());
    return it == table().cend() ? QVector<Option>() : it->options;
}

int FontSwap::namedWeight(const QString &officeFamily) {
    const auto it = table().constFind(officeFamily.toLower());
    return it == table().cend() ? 0 : it->weight;
}

bool FontSwap::isOffice(const QString &family) { return table().contains(family.toLower()); }

QString FontSwap::officeFor(const QString &freeFamily) {
    // Only free families drawn with the Office family's own advance widths
    // (metric-compatible, measured), so the saved deck lays out the same in
    // PowerPoint with no size change.
    static const QHash<QString, QString> pairs = {
        {QStringLiteral("carlito"), QStringLiteral("Calibri")},
        {QStringLiteral("liberation sans"), QStringLiteral("Arial")},
        {QStringLiteral("arimo"), QStringLiteral("Arial")},
        {QStringLiteral("nimbus sans"), QStringLiteral("Arial")},
        {QStringLiteral("liberation sans narrow"), QStringLiteral("Arial Narrow")},
        {QStringLiteral("nimbus sans narrow"), QStringLiteral("Arial Narrow")},
        {QStringLiteral("liberation serif"), QStringLiteral("Times New Roman")},
        {QStringLiteral("tinos"), QStringLiteral("Times New Roman")},
        {QStringLiteral("nimbus roman"), QStringLiteral("Times New Roman")},
        {QStringLiteral("liberation mono"), QStringLiteral("Courier New")},
        {QStringLiteral("cousine"), QStringLiteral("Courier New")},
        {QStringLiteral("nimbus mono ps"), QStringLiteral("Courier New")},
        {QStringLiteral("gelasio"), QStringLiteral("Georgia")},
        {QStringLiteral("caladea"), QStringLiteral("Cambria")},
        {QStringLiteral("intos"), QStringLiteral("Aptos")},
        {QStringLiteral("intos display"), QStringLiteral("Aptos Display")},
        {QStringLiteral("intos narrow"), QStringLiteral("Aptos Narrow")},
        {QStringLiteral("intos serif"), QStringLiteral("Aptos Serif")},
        {QStringLiteral("urw gothic"), QStringLiteral("Century Gothic")},
        {QStringLiteral("p052"), QStringLiteral("Palatino Linotype")},
        {QStringLiteral("c059"), QStringLiteral("Century Schoolbook")},
        {QStringLiteral("urw bookman"), QStringLiteral("Bookman Old Style")},
        {QStringLiteral("selawik"), QStringLiteral("Segoe UI")},
    };
    return pairs.value(freeFamily.toLower());
}

FontSwap::Report FontSwap::apply(Document &document) {
    Swapper swapper;
    eachObject(document, [&](SceneObject &o) { swapper.object(o); });
    for (auto it = document.theme.fonts.begin(); it != document.theme.fonts.end(); ++it) {
        if (document.theme.fontOrigins.contains(it.key())) {
            const auto office = document.theme.fontOrigins.value(it.key());
            if (const auto *opt = swapper.better(office, it.value())) { swapper.count(office, opt->family); it.value() = opt->family; }
            continue;
        }
        if (const auto *opt = swapper.pick(it.value())) {
            document.theme.fontOrigins.insert(it.key(), it.value());
            swapper.count(it.value(), opt->family);
            it.value() = opt->family;
        }
    }
    swapper.report.swaps = tallyList(swapper.tally);
    return swapper.report;
}

void FontSwap::revert(Document &document) {
    eachObject(document, [](SceneObject &o) {
        bool boxSwapped = false;
        if (!o.origFont.isEmpty() && stillSwapped(o.origFont, o.fontFamily)) {
            boxSwapped = true;
            if (!qFuzzyCompare(o.fontScale, 1.0)) o.fontSize = unscale(o.fontSize, o.origFontSize, o.fontScale);
            if (o.origWeight && o.fontWeight == namedWeight(o.origFont)) o.fontWeight = o.origWeight;
            o.fontFamily = o.origFont;
        } else if (const auto office = officeFor(o.fontFamily); !office.isEmpty()) {
            o.fontFamily = office;
        }
        for (auto &run : o.runs) {
            if (!run.fontFamily.isEmpty()) {
                if (!run.origFont.isEmpty() && stillSwapped(run.origFont, run.fontFamily)) {
                    if (!qFuzzyCompare(run.fontScale, 1.0)) {
                        if (run.origFontSize > 0) run.fontSize = unscale(run.fontSize, run.origFontSize, run.fontScale);
                        else if (same(run.fontSize, o.fontSize * run.fontScale)) run.fontSize = 0;   // took the box's
                        else run.fontSize /= run.fontScale;
                    }
                    if (run.origWeight && run.weight == namedWeight(run.origFont)) run.weight = run.origWeight == 1 ? 0 : run.origWeight;
                    run.fontFamily = run.origFont;
                } else if (const auto office = officeFor(run.fontFamily); !office.isEmpty()) {
                    run.fontFamily = office;
                }
            } else if (boxSwapped && !qFuzzyCompare(run.fontScale, 1.0) && run.fontSize > 0) {
                run.fontSize = unscale(run.fontSize, run.origFontSize, run.fontScale);
            }
            run.origFont.clear(); run.origFontSize = 0; run.fontScale = 1; run.origWeight = 0;
        }
        for (auto &cell : o.table.cells) {
            auto &style = cell.style;
            const auto family = style.value(QStringLiteral("fontFamily")).toString();
            const auto office = style.value(QStringLiteral("origFont")).toString();
            if (!office.isEmpty() && stillSwapped(office, family)) {
                if (style.contains(QStringLiteral("fontScale"))) {
                    const qreal scale = style.value(QStringLiteral("fontScale")).toDouble();
                    const qreal now = style.value(QStringLiteral("fontSize")).toDouble();
                    if (style.contains(QStringLiteral("origFontSize")))
                        style.insert(QStringLiteral("fontSize"), unscale(now, style.value(QStringLiteral("origFontSize")).toDouble(), scale));
                    else if (same(now, o.fontSize * scale)) style.remove(QStringLiteral("fontSize"));
                    else style.insert(QStringLiteral("fontSize"), now / scale);
                }
                if (style.contains(QStringLiteral("origWeight")) && style.value(QStringLiteral("fontWeight")).toInt() == namedWeight(office))
                    style.insert(QStringLiteral("fontWeight"), style.value(QStringLiteral("origWeight")));
                style.insert(QStringLiteral("fontFamily"), office);
            } else if (const auto back = officeFor(family); !back.isEmpty()) {
                style.insert(QStringLiteral("fontFamily"), back);
            }
            for (const auto *key : {"origFont", "origFontSize", "fontScale", "origWeight"}) style.remove(QLatin1String(key));
        }
        o.origFont.clear(); o.origFontSize = 0; o.fontScale = 1; o.origWeight = 0;
    });
    auto &theme = document.theme;
    for (auto it = theme.fonts.begin(); it != theme.fonts.end(); ++it) {
        const auto office = theme.fontOrigins.value(it.key());
        if (!office.isEmpty() && stillSwapped(office, it.value())) it.value() = office;
        else if (const auto back = officeFor(it.value()); !back.isEmpty()) it.value() = back;
    }
    theme.fontOrigins.clear();
}

QVariantList FontSwap::swapped(const Document &document) {
    Tally tally;
    Document copy = document;
    eachObject(copy, [&](SceneObject &o) {
        if (!o.origFont.isEmpty()) ++tally[o.origFont][o.fontFamily];
        for (const auto &run : o.runs) if (!run.origFont.isEmpty()) ++tally[run.origFont][run.fontFamily];
        for (const auto &cell : o.table.cells)
            if (cell.style.contains(QStringLiteral("origFont")))
                ++tally[cell.style.value(QStringLiteral("origFont")).toString()][cell.style.value(QStringLiteral("fontFamily")).toString()];
    });
    for (auto it = document.theme.fontOrigins.cbegin(); it != document.theme.fontOrigins.cend(); ++it)
        ++tally[it.value()][document.theme.fonts.value(it.key())];
    return tallyList(tally);
}

namespace {
QVariantMap installRow(const FontSwap::Option &best, const QStringList &offices) {
    return {{QStringLiteral("family"), best.family}, {QStringLiteral("offices"), offices},
            {QStringLiteral("metric"), best.metric}, {QStringLiteral("package"), best.package},
            {QStringLiteral("googleFonts"), best.googleFonts}, {QStringLiteral("github"), best.github}};
}
QVariantList rowsFor(const QStringList &offices) {
    QMap<QString, QStringList> byFamily;
    QHash<QString, FontSwap::Option> option;
    for (const auto &office : offices) {
        const auto opts = FontSwap::options(office);
        if (opts.isEmpty() || installed(office) || installed(opts.first().family)) continue;
        byFamily[opts.first().family].append(table().value(office.toLower()).name);
        option.insert(opts.first().family, opts.first());
    }
    QVariantList rows;
    for (auto it = byFamily.cbegin(); it != byFamily.cend(); ++it) {
        QStringList names = it.value();
        names.removeDuplicates();
        rows.append(installRow(option.value(it.key()), names));
    }
    return rows;
}
} // namespace

QVariantList FontSwap::needed(const Document &document) {
    QStringList offices;
    Document copy = document;
    const auto note = [&](const QString &family) { if (isOffice(family)) offices.append(family); };
    eachObject(copy, [&](SceneObject &o) {
        note(o.origFont.isEmpty() ? o.fontFamily : o.origFont);
        for (const auto &run : o.runs) note(run.origFont.isEmpty() ? run.fontFamily : run.origFont);
        for (const auto &cell : o.table.cells)
            note(cell.style.value(cell.style.contains(QStringLiteral("origFont")) ? QStringLiteral("origFont") : QStringLiteral("fontFamily")).toString());
    });
    for (auto it = document.theme.fonts.cbegin(); it != document.theme.fonts.cend(); ++it)
        note(document.theme.fontOrigins.value(it.key(), it.value()));
    offices.removeDuplicates();
    return rowsFor(offices);
}

QVariantList FontSwap::pack() {
    QStringList offices;
    for (const auto &entry : table()) offices.append(entry.name);
    offices.sort(Qt::CaseInsensitive);
    return rowsFor(offices);
}
