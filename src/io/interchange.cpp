#include "io/interchange.h"

#include "core/fontswap.h"

#include "io/bundle.h"
#include "io/keynote.h"
#include "io/pptx.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace Interchange {

namespace {
struct Format {
    const char *suffix;
    const char *name;   // what the file is, as the app that makes it calls it
    bool opens, saves;  // by OmaShow
};

// Presentation formats OmaShow knows by name, so a typed suffix it can't write
// is named rather than quietly saved as something else.
const Format kFormats[] = {
    {"omashow", "OmaShow deck", true, true},
    {"pptx", "PowerPoint presentation", true, true},
    {"ppsx", "PowerPoint show", false, false},
    {"potx", "PowerPoint template", false, false},
    {"pptm", "PowerPoint macro-enabled presentation", false, false},
    {"ppsm", "PowerPoint macro-enabled show", false, false},
    {"potm", "PowerPoint macro-enabled template", false, false},
    {"key", "Keynote presentation", true, false},
    {"ppt", "PowerPoint 97–2003 presentation", false, false},
    {"pps", "PowerPoint 97–2003 show", false, false},
    {"pot", "PowerPoint 97–2003 template", false, false},
    {"odp", "OpenDocument presentation (LibreOffice Impress)", false, true},
    {"otp", "OpenDocument presentation template (LibreOffice Impress)", false, true},
    {"fodp", "Flat OpenDocument presentation (LibreOffice Impress)", false, true},
    {"dps", "WPS Presentation", false, false},
    {"dpt", "WPS Presentation template", false, false},
    {"prdx", "SoftMaker Presentations", false, false},
    {"prd", "SoftMaker Presentations (older)", false, false},
};

const Format *formatOf(const QString &path) {
    const auto suffix = QFileInfo(QDir::cleanPath(path)).suffix().toLower();
    for (const auto &format : kFormats)
        if (suffix == QLatin1String(format.suffix)) return &format;
    return nullptr;
}

} // namespace

Kind kindOf(const QString &path) {
    // A Keynote package is a folder; a shell may hand it over with a slash.
    const auto suffix = QFileInfo(QDir::cleanPath(path)).suffix().toLower();
    if (suffix == QStringLiteral("omashow")) return Native;
    if (suffix == QStringLiteral("pptx")) return PowerPoint;
    if (suffix == QStringLiteral("key")) return Keynote;
    if (suffix == QStringLiteral("odp") || suffix == QStringLiteral("otp") || suffix == QStringLiteral("fodp")) return OpenDocument;
    return Unknown;
}

QString kindName(Kind kind) {
    switch (kind) {
    case Native: return QStringLiteral("OmaShow");
    case PowerPoint: return QStringLiteral("PowerPoint");
    case Keynote: return QStringLiteral("Keynote");
    case OpenDocument: return QStringLiteral("OpenDocument");
    case Unknown: break;
    }
    return QString();
}

Result load(const QString &path) {
    Result result;
    result.kind = kindOf(path);
    // OpenDocument is written, not yet read.
    if (result.kind == Unknown || result.kind == OpenDocument) {
        const auto *format = formatOf(path);
        result.error = format ? QStringLiteral("%1 is a %2 file, which OmaShow can't open yet.")
                                    .arg(QFileInfo(path).fileName(), QString::fromUtf8(format->name))
                              : QStringLiteral("%1 is not a deck OmaShow can open.").arg(QFileInfo(path).fileName());
        return result;
    }
    if (result.kind == Native) {
        const auto read = Bundle::load(path);
        result.ok = read.ok;
        result.error = read.error;
        result.document = read.document;
        return result;
    }
    if (result.kind == Keynote) {
        // Keynote packages can be folders; the reader takes the path itself.
        const auto read = Keynote::load(path);
        result.ok = read.ok;
        result.error = read.error;
        result.document = read.document;
        result.warnings = read.warnings;
        return result;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("%1 could not be read.").arg(QFileInfo(path).fileName());
        return result;
    }
    return fromBytes(file.readAll(), result.kind);
}

Result fromBytes(const QByteArray &raw, Kind kind) {
    Result result;
    result.kind = kind;
    if (kind == Native) {
        const auto read = Bundle::fromBytes(raw);
        result.ok = read.ok;
        result.error = read.error;
        result.document = read.document;
        // FONT-CHECK-2: a saved deck naming Office fonts (or a fallback from
        // another computer) gets this computer's best free match too.
        if (result.ok && !qEnvironmentVariableIsSet("OMASHOW_NO_FONT_SWAP")) FontSwap::apply(result.document);
    } else if (kind == PowerPoint) {
        const auto read = Pptx::read(raw);
        result.ok = read.ok;
        result.error = read.error;
        result.document = read.document;
        result.warnings = read.warnings;
        // Office fonts this computer lacks become their measured free matches;
        // what they were is kept and written back on PowerPoint save.
        if (result.ok && !qEnvironmentVariableIsSet("OMASHOW_NO_FONT_SWAP")) FontSwap::apply(result.document);
    } else if (kind == Keynote) {
        const auto read = Keynote::read(raw);
        result.ok = read.ok;
        result.error = read.error;
        result.document = read.document;
        result.warnings = read.warnings;
    } else {
        result.error = QStringLiteral("not a deck OmaShow can open.");
    }
    return result;
}

QStringList openPatterns() {
    QStringList patterns;
    for (const auto &format : kFormats)
        if (format.opens) patterns.append(QStringLiteral("*.") + QLatin1String(format.suffix));
    return patterns;
}


QString unwritableFormat(const QString &path) {
    const auto *format = formatOf(path);
    return format && !format->saves ? QString::fromUtf8(format->name) : QString();
}

QList<QPair<QString, QStringList>> saveFilters() {
    return {{QStringLiteral("OmaShow (.omashow)"), {QStringLiteral("*.omashow")}},
            {QStringLiteral("PowerPoint (.ppt)"), {QStringLiteral("*.ppt")}},
            {QStringLiteral("PowerPoint (.pptx)"), {QStringLiteral("*.pptx")}},
            {QStringLiteral("All Files"), {QStringLiteral("*")}}};
}

QString saveName(const QString &path) {
    return canSave(kindOf(path)) ? QFileInfo(QDir::cleanPath(path)).fileName() : suggestedName(path);
}

QString suggestedName(const QString &path) {
    const auto base = QFileInfo(QDir::cleanPath(path)).completeBaseName();
    return (base.isEmpty() ? QStringLiteral("Untitled") : base) + QStringLiteral(".omashow");
}

} // namespace Interchange
