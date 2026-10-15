#include "backend.h"
#include "core/starter.h"
#include "io/recents.h"
#include "io/recovery.h"
#include <QDesktopServices>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace {
QString startSettingsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/start.ini";
}
} // namespace

// The slide size last chosen on the start page, so the next launch offers it again. A file someone
// edited by hand can hold anything, so every value is brought back into range here.
QVariantMap Backend::startState() const {
    QSettings settings(startSettingsPath(), QSettings::IniFormat);
    return {{"slideSize", qBound(0, settings.value("slideSize", 0).toInt(), 4)},
            {"customWidth", qBound(240, settings.value("customWidth", 1920).toInt(), 10000)},
            {"customHeight", qBound(240, settings.value("customHeight", 1080).toInt(), 10000)}};
}

void Backend::setStartState(const QString &key, const QVariant &value) {
    static const QStringList known{"slideSize", "customWidth", "customHeight"};
    if (!known.contains(key)) return;
    QSettings settings(startSettingsPath(), QSettings::IniFormat);
    if (settings.value(key) == value) return;
    settings.setValue(key, value);
    settings.sync();
}
void Backend::activateDocument() {
    m_hasDocument = true;
    m_startVisible = false;
    emit startChanged();
}
void Backend::showStart() {
    pause();
    m_startVisible = true;
    emit startChanged();
    emit recentsChanged();
}
void Backend::resumeDeck() {
    if (m_hasDocument) {
        m_startVisible = false;
        emit startChanged();
    }
}
bool Backend::createDeck(int theme, qreal width, qreal height, int layout) {
    const auto document = Starter::create(theme, QSizeF(width, height), layout);
    if (document.slides.isEmpty()) {
        emit failed(tr("Choose dimensions between 240 and 10,000, and a valid template."));
        return false;
    }
    m_autosave.stop();
    Recovery::discard();
    setDocument(document);
    setFileUrl(QUrl());
    setStatus(tr("New deck"));
    return true;
}
QVariantList Backend::recentFiles() const { return Recents::read(Recents::settingsPath()); }
void Backend::rememberRecent(const QString &path) {
    if (!QStandardPaths::isTestModeEnabled())
        Recents::remember(Recents::settingsPath(), path);
    emit recentsChanged();
}
void Backend::pinRecent(const QString &path, bool pinned) {
    Recents::pin(Recents::settingsPath(), path, pinned);
    emit recentsChanged();
}
void Backend::removeRecent(const QString &path) {
    Recents::remove(Recents::settingsPath(), path);
    emit recentsChanged();
}
void Backend::revealRecent(const QString &path) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}
