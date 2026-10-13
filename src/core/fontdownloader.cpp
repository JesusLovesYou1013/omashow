#include "core/fontdownloader.h"
#include "core/fontsandbox.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

namespace {

constexpr int kParallel = 4;
constexpr int kStallMs = 20000;
constexpr int kDeadlineMs = 5 * 60 * 1000;
constexpr qint64 kMaxFile = 8 * 1024 * 1024;

struct Source { QString path, sha256; qint64 size = 0; bool font = true; };

const QJsonObject &table() {
    static const QJsonObject t = [] {
        QFile f(QStringLiteral(":/fonts/font-sources.json"));
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
    }();
    return t;
}

// A path inside the pinned repository: plain segments only.
bool safePath(const QString &p) {
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9._\\[\\],@+-]+(/[A-Za-z0-9._\\[\\],@+-]+)*$"));
    return ok.match(p).hasMatch() && !p.contains(QLatin1String(".."));
}

QList<Source> sourcesOf(const QString &family) {
    QList<Source> out;
    const auto families = table().value(QStringLiteral("families")).toObject();
    QJsonObject entry;
    for (auto it = families.begin(); it != families.end(); ++it)
        if (it.key().compare(family, Qt::CaseInsensitive) == 0) { entry = it.value().toObject(); break; }
    auto add = [&](const QJsonObject &o, bool font) {
        Source s{o.value("p").toString(), o.value("sha256").toString().toLower(), qint64(o.value("size").toDouble()), font};
        if (safePath(s.path) && s.sha256.size() == 64 && s.size > 0 && s.size <= kMaxFile) out.append(s);
    };
    for (const auto &v : entry.value(QStringLiteral("files")).toArray()) add(v.toObject(), true);
    if (!out.isEmpty() && entry.contains(QStringLiteral("license"))) add(entry.value(QStringLiteral("license")).toObject(), false);
    return out;
}

QString folderName(QString family) { return family.remove(QLatin1Char(' ')); }

QString sha256Of(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
}

QString baseUrl() {
    auto base = qEnvironmentVariable("OMASHOW_FONT_BASE_URL").trimmed();
    if (base.isEmpty()) base = table().value(QStringLiteral("base")).toString();
    if (!base.endsWith(QLatin1Char('/'))) base += QLatin1Char('/');
    return base;
}

} // namespace

FontDownloader::FontDownloader(QObject *parent) : QObject(parent) {}
FontDownloader::~FontDownloader() { cancel(); }

bool FontDownloader::downloadable(const QString &family) { return !sourcesOf(family).isEmpty(); }

qint64 FontDownloader::bytesFor(const QStringList &families) {
    qint64 n = 0;
    for (const auto &f : families) for (const auto &s : sourcesOf(f)) n += s.size;
    return n;
}

qint64 FontDownloader::bytesDone() const {
    qint64 n = m_finishedBytes;
    for (const auto &a : m_active) n += a.received;
    return n;
}

bool FontDownloader::load(const QString &path) {
    const int id = QFontDatabase::addApplicationFont(path);
    return id >= 0 && !QFontDatabase::applicationFontFamilies(id).isEmpty();
}

bool FontDownloader::start(const QStringList &families) {
    if (running()) return false;
    m_queue.clear(); m_failed.clear(); m_families.clear();
    m_allNetwork = true; m_total = 0; m_finishedBytes = 0;
    m_running = true; m_cancelled = false;
    const QString root = FontSandbox::dir();
    for (const auto &family : families) {
        const auto sources = sourcesOf(family);
        if (sources.isEmpty()) { fail(family, tr("no download is listed for it"), false); continue; }
        m_families.append(family);
        const QString dir = root + QLatin1Char('/') + folderName(family);
        if (!QDir().mkpath(dir) || !QFileInfo(dir).isWritable()) { fail(family, tr("cannot write to %1").arg(root), false); continue; }
        // Leftovers of an earlier, interrupted run.
        for (const auto &part : QDir(dir).entryList({QStringLiteral("*.part")}, QDir::Files)) QFile::remove(dir + QLatin1Char('/') + part);
        for (const auto &s : sources) {
            File file{family, s.path, s.sha256, dir + QLatin1Char('/') + QFileInfo(s.path).fileName(), s.size, s.font};
            // Already here with the right bytes (a retry, or installed before).
            if (QFileInfo::exists(file.target) && sha256Of(file.target) == file.sha256) {
                if (file.font) load(file.target);
                continue;
            }
            m_queue.append(file);
            m_total += s.size;
        }
    }
    if (m_queue.isEmpty()) {
        QTimer::singleShot(0, this, [this] { finish(false); });
        return true;
    }
    if (!m_net) m_net = new QNetworkAccessManager(this);
    if (!m_deadline) {
        m_deadline = new QTimer(this);
        m_deadline->setSingleShot(true);
        connect(m_deadline, &QTimer::timeout, this, [this] {
            for (const auto &f : std::as_const(m_queue)) fail(f.family, tr("timed out"), true);
            m_queue.clear();
            for (auto *reply : m_active.keys()) reply->abort();
        });
    }
    m_deadline->start(kDeadlineMs);
    pump();
    return true;
}

void FontDownloader::pump() {
    while (m_active.size() < kParallel && !m_queue.isEmpty()) {
        const File file = m_queue.takeFirst();
        if (m_failed.contains(file.family)) continue;   // one bad file fails the family
        auto *out = new QFile(file.target + QStringLiteral(".part"), this);
        if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            delete out;
            fail(file.family, tr("cannot write to %1").arg(FontSandbox::dir()), false);
            continue;
        }
        QNetworkRequest request(QUrl(baseUrl() + file.path));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setMaximumRedirectsAllowed(3);
        auto *reply = m_net->get(request);
        auto *stall = new QTimer(reply);
        stall->setSingleShot(true);
        stall->start(kStallMs);
        connect(stall, &QTimer::timeout, reply, [reply] { reply->setProperty("stalled", true); reply->abort(); });
        m_active.insert(reply, Active{file, reply, out, stall, 0});
        connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
            auto it = m_active.find(reply);
            if (it == m_active.end()) return;
            const auto bytes = reply->readAll();
            it->received += bytes.size();
            it->stall->start(kStallMs);
            if (it->received > it->file.size) { reply->setProperty("oversize", true); reply->abort(); return; }
            it->out->write(bytes);
            emit progress();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply] { done(reply); });
    }
    if (m_active.isEmpty() && m_queue.isEmpty()) finish(false);
}

void FontDownloader::done(QNetworkReply *reply) {
    auto it = m_active.find(reply);
    if (it == m_active.end()) { reply->deleteLater(); return; }
    Active a = *it;
    m_active.erase(it);
    a.out->write(reply->readAll());
    a.out->close();
    const QString part = a.out->fileName();
    a.out->deleteLater();
    reply->deleteLater();
    m_finishedBytes += a.file.size;
    // A licence text that does not arrive costs nothing: the fonts still work.
    const auto drop = [&](const QString &why, bool network) { QFile::remove(part); if (a.file.font) fail(a.file.family, why, network); };
    if (reply->property("cancelled").toBool()) QFile::remove(part);
    else if (reply->property("stalled").toBool()) drop(tr("the download stalled"), true);
    else if (reply->property("oversize").toBool()) drop(tr("the server sent a different file"), false);
    else if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        drop(status ? tr("the server answered %1").arg(status) : reply->errorString(), status == 0);
    } else if (sha256Of(part) != a.file.sha256) drop(tr("the file did not match its checksum"), false);
    else {
        QFile::remove(a.file.target);
        if (!QFile::rename(part, a.file.target)) drop(tr("cannot write to %1").arg(FontSandbox::dir()), false);
        else if (a.file.font && !load(a.file.target)) { QFile::remove(a.file.target); fail(a.file.family, tr("not a usable font"), false); }
    }
    emit progress();
    pump();
}

void FontDownloader::fail(const QString &family, const QString &why, bool network) {
    if (!network) m_allNetwork = false;
    if (!m_failed.contains(family)) m_failed.insert(family, why);
}

void FontDownloader::cancel() {
    if (!running()) return;
    m_cancelled = true;
    m_queue.clear();
    const auto replies = m_active.keys();
    for (auto *reply : replies) { reply->setProperty("cancelled", true); reply->abort(); }
    // abort() finishes synchronously; anything left is closed here.
    for (auto it = m_active.begin(); it != m_active.end(); ++it) { it->out->close(); QFile::remove(it->out->fileName()); }
    m_active.clear();
    finish(true);
}

void FontDownloader::finish(bool cancelled) {
    if (!m_running) return;
    m_running = false;
    cancelled = cancelled || m_cancelled;
    if (m_deadline) m_deadline->stop();
    QStringList installed;
    for (const auto &f : std::as_const(m_families)) if (!m_failed.contains(f)) installed.append(f);
    emit finished(installed, m_failed, !m_failed.isEmpty() && m_allNetwork, cancelled);
}
