// Free matches for Office fonts: what was swapped, what is worth fetching,
// and fetching it into OmaShow's own font folder once the person says yes
// (FONT-CHECK-2: no packages, no password, nothing system-wide).
#include "backend.h"
#include "core/fontdownloader.h"
#include "core/fontsandbox.h"
#include "core/fontswap.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

namespace {

QString fontSettingsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/view.ini";
}

QString fontCheckKey(const QString &source) {
    return QStringLiteral("fontCheckHidden/") + QString::fromLatin1(QCryptographicHash::hash(QFileInfo(source).absoluteFilePath().toUtf8(), QCryptographicHash::Sha1).toHex());
}

} // namespace

QVariantList Backend::fontSwaps() const { return FontSwap::swapped(m_document); }
QVariantList Backend::fontsToInstall() const { return FontSwap::needed(m_document); }
QString Backend::fontSandboxDir() const { return FontSandbox::dir(); }

bool Backend::fontCheckHidden(const QString &source) const {
    if (source.isEmpty()) return false;
    QSettings settings(fontSettingsPath(), QSettings::IniFormat);
    return settings.value(fontCheckKey(source), false).toBool();
}
void Backend::setFontCheckHidden(const QString &source, bool hidden) {
    if (source.isEmpty()) return;
    QSettings settings(fontSettingsPath(), QSettings::IniFormat);
    if (hidden) settings.setValue(fontCheckKey(source), true); else settings.remove(fontCheckKey(source));
    settings.sync();
}
bool Backend::fontAutoDownload() const {
    QSettings settings(fontSettingsPath(), QSettings::IniFormat);
    return settings.value("fontAutoDownload", false).toBool();
}
void Backend::setFontAutoDownload(bool on) {
    if (on == fontAutoDownload()) return;
    QSettings settings(fontSettingsPath(), QSettings::IniFormat);
    settings.setValue("fontAutoDownload", on);
    settings.sync();
    emit fontAutoDownloadChanged();
}
bool Backend::openFontSandbox() {
    const auto dir = FontSandbox::dir();
    // Made only when asked for.
    if (!QDir().mkpath(dir)) return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

bool Backend::fontDownloadRunning() const { return m_fontDownloader && m_fontDownloader->running(); }
double Backend::fontDownloadDone() const { return m_fontDownloader ? double(m_fontDownloader->bytesDone()) : 0; }
double Backend::fontDownloadTotal() const { return m_fontDownloader ? double(m_fontDownloader->bytesTotal()) : 0; }

bool Backend::downloadFonts(const QStringList &families) {
    if (families.isEmpty() || fontDownloadRunning()) return false;
    if (!m_fontDownloader) {
        m_fontDownloader = new FontDownloader(this);
        connect(m_fontDownloader, &FontDownloader::progress, this, &Backend::fontDownloadChanged);
        connect(m_fontDownloader, &FontDownloader::finished, this, &Backend::fontDownloadDoneWith);
    }
    if (!m_fontDownloader->start(families)) return false;
    setStatus(tr("Downloading free fonts: %1…").arg(families.join(QStringLiteral(", "))));
    emit fontDownloadChanged();
    return true;
}

void Backend::cancelFontDownload() { if (m_fontDownloader) m_fontDownloader->cancel(); }

void Backend::fontDownloadDoneWith(const QStringList &installed, const QVariantMap &failed, bool offline, bool cancelled) {
    // Used by the window when it is safe (not mid-show, editors committed).
    if (!installed.isEmpty()) m_fontsPending = true;
    emit fontDownloadChanged();
    QString message;
    if (cancelled) message = tr("Font download cancelled");
    else {
        if (!installed.isEmpty()) message = tr("Installed free fonts: %1").arg(installed.join(QStringLiteral(", ")));
        if (offline) message += (message.isEmpty() ? QString() : QStringLiteral(". ")) + tr("Couldn't reach the font server; similar fonts are used");
        else if (!failed.isEmpty()) message += (message.isEmpty() ? QString() : QStringLiteral(". ")) + tr("Not installed: %1").arg(failed.keys().join(QStringLiteral(", ")));
    }
    setStatus(message);
    emit fontDownloadFinished(installed, failed, offline, cancelled);
}

// Fonts that just arrived are drawn at once, in place. This is not an edit:
// no undo step, no "unsaved changes", and every undo snapshot gets them too,
// so Undo never brings the fallbacks back.
void Backend::applyArrivedFonts() {
    if (!m_fontsPending) return;
    m_fontsPending = false;
    emit fontDownloadChanged();
    if (qEnvironmentVariableIsSet("OMASHOW_NO_FONT_SWAP")) { emit importReportChanged(); return; }
    const auto report = FontSwap::apply(m_document);
    if (report.changed) {
        // Snapshots are only touched when the deck itself had something to upgrade.
        m_history.rewrite([](Document &d) { FontSwap::apply(d); });
        applyActiveShow();
        ++m_revision;
        emit documentChanged();
        emit deckChanged();
        emit selectionChanged();
    }
    emit importReportChanged();
}
