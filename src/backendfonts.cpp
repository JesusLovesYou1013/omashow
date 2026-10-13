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

bool Backend::extraFontsPack() const {
    QSettings settings(fontSettingsPath(), QSettings::IniFormat);
    return settings.value("extraFontsPack", false).toBool();
}
// On: the pack is wanted (the window offers the download). Off: nothing is
// deleted; a pack download still running is stopped (a deck's fonts go on).
void Backend::setExtraFontsPack(bool on) {
    if (on == extraFontsPack()) return;
    QSettings settings(fontSettingsPath(), QSettings::IniFormat);
    settings.setValue("extraFontsPack", on);
    settings.sync();
    if (!on) cancelPack();
    emit extraFontsPackChanged();
}
QVariantList Backend::fontPack() const { return FontSwap::pack(); }
int Backend::packTotal() const { return FontSwap::pack().size(); }
int Backend::packReady() const {
    int n = 0;
    for (const auto &r : FontSwap::pack()) n += r.toMap().value(QStringLiteral("installed")).toBool();
    return n;
}
double Backend::packDone() const { return m_fontDownloader ? double(m_fontDownloader->bytesDone(m_packFamilies)) : 0; }
double Backend::packBytes() const {
    if (!m_packFamilies.isEmpty()) return double(FontDownloader::bytesFor(m_packFamilies));
    QStringList todo;   // idle: what a download would fetch now
    for (const auto &r : FontSwap::pack())
        if (!r.toMap().value(QStringLiteral("installed")).toBool()) todo << r.toMap().value(QStringLiteral("family")).toString();
    return double(FontDownloader::bytesFor(todo));
}

bool Backend::fontDownloadRunning() const { return !m_deckLeft.isEmpty(); }
double Backend::fontDownloadDone() const { return m_fontDownloader ? double(m_fontDownloader->bytesDone(m_deckFamilies)) : 0; }
double Backend::fontDownloadTotal() const { return double(FontDownloader::bytesFor(m_deckFamilies)); }

void Backend::ensureDownloader() {
    if (m_fontDownloader) return;
    m_fontDownloader = new FontDownloader(this);
    connect(m_fontDownloader, &FontDownloader::progress, this, &Backend::fontDownloadChanged);
    connect(m_fontDownloader, &FontDownloader::familyReady, this, &Backend::familyArrived);
    connect(m_fontDownloader, &FontDownloader::familyFailed, this, &Backend::familyLost);
    connect(m_fontDownloader, &FontDownloader::finished, this, &Backend::runFinished);
}

// A deck's fonts: ahead of anything queued (a running pack included).
bool Backend::downloadFonts(const QStringList &families) {
    if (families.isEmpty() || !m_deckLeft.isEmpty()) return false;
    ensureDownloader();
    m_deckFamilies = families;
    m_deckLeft = QSet<QString>(families.cbegin(), families.cend());
    m_deckInstalled.clear(); m_deckFailed.clear(); m_deckCancelled = false;
    for (const auto &f : families) m_deckNeeded.insert(f);
    m_fontDownloader->add(families, true);
    setStatus(tr("Downloading free fonts: %1…").arg(families.join(QStringLiteral(", "))));
    emit fontDownloadChanged();
    return true;
}
void Backend::cancelFontDownload() {
    if (!m_fontDownloader || m_deckLeft.isEmpty()) return;
    m_deckCancelled = true;
    // Only the deck's own families that the pack does not also want.
    QStringList mine;
    for (const auto &f : std::as_const(m_deckLeft)) if (!m_packLeft.contains(f)) mine.append(f);
    if (mine.size() == m_deckLeft.size() && m_packLeft.isEmpty()) m_fontDownloader->cancel(); else m_fontDownloader->drop(mine);
}

bool Backend::downloadPack() {
    ensureDownloader();
    QStringList families;
    for (const auto &r : FontSwap::pack()) {
        const auto row = r.toMap();
        if (!row.value(QStringLiteral("installed")).toBool()) families.append(row.value(QStringLiteral("family")).toString());
    }
    m_packFamilies = families;
    m_packFailed.clear();
    m_packLeft = QSet<QString>(families.cbegin(), families.cend());
    if (families.isEmpty()) { emit fontDownloadChanged(); emit packFinished(packReady(), packTotal(), {}, false); return true; }
    m_fontDownloader->add(families, false);
    emit fontDownloadChanged();
    return true;
}
void Backend::cancelPack() {
    if (!m_fontDownloader || m_packLeft.isEmpty()) return;
    QStringList drop;
    for (const auto &f : std::as_const(m_packLeft)) if (!m_deckLeft.contains(f)) drop.append(f);
    m_fontDownloader->drop(drop);
}

void Backend::familyArrived(const QString &family) {
    // The open deck uses it: applied (by the window, when safe) per family.
    if (m_deckNeeded.contains(family)) m_fontsPending = true;
    if (m_deckLeft.remove(family)) m_deckInstalled.append(family);
    m_packLeft.remove(family);
    if (!m_packFamilies.isEmpty() && m_packLeft.isEmpty() == false)
        setStatus(tr("Extra fonts pack: %1 of %2 fonts ready").arg(packReady()).arg(packTotal()));
    emit fontDownloadChanged();
    runFinished({}, {}, false, false);   // settles whichever group just completed
}
void Backend::familyLost(const QString &family, const QString &why, bool network) {
    if (m_deckLeft.remove(family)) m_deckFailed.insert(family, QVariantMap{{"why", why}, {"network", network}});
    if (m_packLeft.remove(family)) m_packFailed.insert(family, QVariantMap{{"why", why}, {"network", network}});
    emit fontDownloadChanged();
    runFinished({}, {}, false, false);
}

// Groups end when their last family is in (or failed); the run's own end
// only matters for families that never reported.
void Backend::runFinished(const QStringList &, const QVariantMap &, bool, bool) {
    const auto summarise = [](const QVariantMap &failed, QVariantMap &plain, bool &offline) {
        offline = false;
        bool allNetwork = true;
        for (auto it = failed.cbegin(); it != failed.cend(); ++it) {
            if (it.value().toMap().value("why").toString() == tr("cancelled")) continue;   // not a failure
            offline = true;
            plain.insert(it.key(), it.value().toMap().value("why"));
            allNetwork = allNetwork && it.value().toMap().value("network").toBool();
        }
        offline = offline && allNetwork;
    };
    if (m_deckLeft.isEmpty() && !m_deckFamilies.isEmpty()) {
        QVariantMap failed; bool offline = false;
        summarise(m_deckFailed, failed, offline);
        offline = offline && m_deckInstalled.isEmpty();
        const auto installed = m_deckInstalled;
        const bool cancelled = m_deckCancelled;
        m_deckFamilies.clear(); m_deckInstalled.clear(); m_deckFailed.clear();
        QString message;
        if (cancelled) message = tr("Font download cancelled");
        else {
            if (!installed.isEmpty()) message = tr("Installed free fonts: %1").arg(installed.join(QStringLiteral(", ")));
            if (offline) message += (message.isEmpty() ? QString() : QStringLiteral(". ")) + tr("Couldn't reach the font server; similar fonts are used");
            else if (!failed.isEmpty()) message += (message.isEmpty() ? QString() : QStringLiteral(". ")) + tr("Not installed: %1").arg(failed.keys().join(QStringLiteral(", ")));
        }
        setStatus(message);
        emit fontDownloadChanged();
        emit fontDownloadFinished(installed, failed, offline, cancelled);
    }
    if (m_packLeft.isEmpty() && !m_packFamilies.isEmpty()) {
        QVariantMap failed; bool offline = false;
        summarise(m_packFailed, failed, offline);
        m_packFamilies.clear(); m_packFailed.clear();
        const int ready = packReady(), total = packTotal();
        setStatus(ready == total ? tr("Extra fonts pack: all %1 fonts ready").arg(total)
                  : offline ? tr("Extra fonts pack: %1 of %2 fonts ready; couldn't reach the font server").arg(ready).arg(total)
                  : failed.isEmpty() ? tr("Extra fonts pack paused: %1 of %2 fonts ready").arg(ready).arg(total)
                  : tr("Extra fonts pack: %1 of %2 fonts ready, %3 not installed").arg(ready).arg(total).arg(failed.size()));
        emit fontDownloadChanged();
        emit packFinished(ready, total, failed, offline);
    }
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
