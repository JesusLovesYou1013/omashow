#include "core/fontsandbox.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSet>

QString FontSandbox::dir() {
    const auto custom = qEnvironmentVariable("OMASHOW_FONTS_DIR").trimmed();
    if (!custom.isEmpty()) return QDir::cleanPath(custom);
    // XDG base dir spec: $XDG_DATA_HOME, or ~/.local/share when unset or not absolute.
    auto data = qEnvironmentVariable("XDG_DATA_HOME");
    if (data.isEmpty() || !QDir::isAbsolutePath(data)) data = QDir::homePath() + QStringLiteral("/.local/share");
    return QDir::cleanPath(data + QStringLiteral("/omashow-fonts"));
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
        if (QFontDatabase::addApplicationFont(path) >= 0) ++count;
    }
    return count;
}
