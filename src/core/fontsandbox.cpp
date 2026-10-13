#include "core/fontsandbox.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QSet>

QString FontSandbox::dir() {
    const auto custom = qEnvironmentVariable("OMASHOW_FONTS_DIR").trimmed();
    if (!custom.isEmpty()) return QDir::cleanPath(custom);
    // XDG base dir spec: $XDG_DATA_HOME, or ~/.local/share when unset or not absolute.
    auto data = qEnvironmentVariable("XDG_DATA_HOME");
    if (data.isEmpty() || !QDir::isAbsolutePath(data)) data = QDir::homePath() + QStringLiteral("/.local/share");
    return QDir::cleanPath(data + QStringLiteral("/omashow-fonts"));
}

namespace {
// folder key (lower case, no spaces) -> families its files declared
QHash<QString, QStringList> &aliases() { static QHash<QString, QStringList> a; return a; }
QString key(QString name) { return name.remove(QLatin1Char(' ')).toLower(); }
}

QStringList FontSandbox::loadFile(const QString &path, const QString &folder) {
    const int id = QFontDatabase::addApplicationFont(path);
    if (id < 0) return {};
    const auto families = QFontDatabase::applicationFontFamilies(id);
    auto &known = aliases()[key(folder)];
    for (const auto &f : families) if (!known.contains(f)) known.append(f);
    return families;
}

bool FontSandbox::available(const QString &family) {
    if (family.isEmpty()) return false;
    if (QFontDatabase::hasFamily(family)) return true;
    const auto it = aliases().constFind(key(family));
    if (it == aliases().cend() || it->isEmpty()) return false;
    if (QFont::substitute(family) == family) QFont::insertSubstitution(family, it->first());
    return true;
}

int FontSandbox::load() {
    static QSet<QString> loaded;
    const auto root = dir();
    if (!QFileInfo(root).isDir()) return 0;
    int count = 0;
    QDirIterator it(root, {QStringLiteral("*.ttf"), QStringLiteral("*.otf"), QStringLiteral("*.ttc"), QStringLiteral("*.otc"),
                           QStringLiteral("*.TTF"), QStringLiteral("*.OTF"), QStringLiteral("*.TTC"), QStringLiteral("*.OTC")},
                    QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto path = QFileInfo(it.next()).canonicalFilePath();
        if (path.isEmpty() || loaded.contains(path)) continue;
        loaded.insert(path);
        // The family folder is the first level under the sandbox.
        const QString folder = QDir(root).relativeFilePath(path).section(QLatin1Char('/'), 0, 0);
        if (!loadFile(path, folder).isEmpty()) ++count;
    }
    return count;
}
