// Free matches for Office fonts: what was swapped on PowerPoint import, what
// is worth installing, and installing it once the person has said yes.
#include "backend.h"
#include "core/fontswap.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrlQuery>
#include <functional>
#include <memory>

namespace {

QString fontSettingsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/view.ini";
}

// The person's own font folder: fontconfig reads it, nothing system-wide.
QString userFontDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/fonts/omashow";
}

QString installMode() {
    const auto mode = qEnvironmentVariable("OMASHOW_FONT_INSTALL").trimmed().toLower();
    if (mode == QLatin1String("simulate") || mode == QLatin1String("user")) return mode;
    return QStringLiteral("system");
}

// Names from the table and from GitHub's listing become file names and URL
// parts; refuse anything that could leave the folder or the repository.
bool safePart(const QString &part) {
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9._\\[\\],@+-]+(/[A-Za-z0-9._\\[\\],@+-]+)*$"));
    return ok.match(part).hasMatch() && !part.contains(QLatin1String(".."));
}

struct Step {
    QString program;
    QStringList args;
    std::function<void(int code, const QByteArray &out)> done;
};

// Commands one after another; each may add more in front of the rest.
struct Chain : std::enable_shared_from_this<Chain> {
    QPointer<QObject> owner;
    QList<Step> steps;
    std::function<void()> finished;
    void next() {
        if (!owner) return;
        if (steps.isEmpty()) { if (finished) finished(); return; }
        const Step step = steps.takeFirst();
        auto *process = new QProcess(owner);
        auto self = shared_from_this();
        auto settled = std::make_shared<bool>(false);
        QObject::connect(process, &QProcess::finished, process, [self, process, step, settled](int code, QProcess::ExitStatus status) {
            if (*settled) return;
            *settled = true;
            const auto out = process->readAllStandardOutput();
            process->deleteLater();
            if (step.done) step.done(status == QProcess::NormalExit ? code : -1, out);
            self->next();
        });
        QObject::connect(process, &QProcess::errorOccurred, process, [self, process, step, settled](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || *settled) return;
            *settled = true;
            process->deleteLater();
            if (step.done) step.done(-1, {});
            self->next();
        });
        process->start(step.program, step.args);
    }
};

QString compact(QString family) { return family.remove(QLatin1Char(' ')); }

} // namespace

QVariantList Backend::fontSwaps() const { return FontSwap::swapped(m_document); }
QVariantList Backend::fontsToInstall() const { return FontSwap::needed(m_document); }
QVariantList Backend::fontPack() const { return FontSwap::pack(); }
QString Backend::fontInstallMode() const { return installMode(); }

bool Backend::extraFontsPack() const {
    QSettings settings(fontSettingsPath(), QSettings::IniFormat);
    return settings.value("extraFontsPack", false).toBool();
}

void Backend::setExtraFontsPack(bool on) {
    if (on == extraFontsPack()) return;
    QSettings settings(fontSettingsPath(), QSettings::IniFormat);
    settings.setValue("extraFontsPack", on);
    settings.sync();
    emit extraFontsPackChanged();
}

bool Backend::installFonts(const QVariantList &rows) {
    if (m_fontInstallRunning || rows.isEmpty()) return false;
    const QString mode = installMode();
    QStringList packages, families, packageOnly;
    QList<QVariantMap> downloads;
    for (const auto &value : rows) {
        const auto row = value.toMap();
        const auto family = row.value("family").toString();
        if (family.isEmpty() || QFontDatabase::hasFamily(family)) continue;
        const auto package = row.value("package").toString();
        const bool canDownload = !row.value("googleFonts").toString().isEmpty() || !row.value("github").toString().isEmpty();
        if (mode == QLatin1String("system") && !package.isEmpty() && safePart(package)) packages.append(package);
        else if (canDownload) downloads.append(row);
        else { if (!package.isEmpty()) packageOnly.append(QStringLiteral("%1 (Arch package %2)").arg(family, package)); continue; }
        families.append(family);
    }
    packages.removeDuplicates();
    if (families.isEmpty()) {
        if (!packageOnly.isEmpty()) {
            const auto message = tr("Only as system packages: %1").arg(packageOnly.join(QStringLiteral(", ")));
            setStatus(message);
            emit fontInstallFinished(false, message);
        }
        return false;
    }

    if (mode == QLatin1String("simulate")) {
        // Testing: say what would happen, change nothing.
        QJsonArray plan;
        for (const auto &package : packages)
            plan.append(QJsonObject{{"how", "pacman"}, {"command", "pkexec pacman -S --needed --noconfirm " + package}});
        for (const auto &row : downloads)
            plan.append(QJsonObject{{"how", "download"}, {"family", row.value("family").toString()},
                                    {"from", !row.value("googleFonts").toString().isEmpty() ? "google/fonts " + row.value("googleFonts").toString() : "github " + row.value("github").toString()},
                                    {"into", userFontDir()}});
        const auto dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        QFile file(dir + "/font-install-plan.json");
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(plan).toJson());
        auto message = tr("Simulated: would install %1").arg(families.join(QStringLiteral(", ")));
        if (!packageOnly.isEmpty()) message += QStringLiteral(". ") + tr("Only as system packages: %1").arg(packageOnly.join(QStringLiteral(", ")));
        setStatus(message);
        emit fontInstallFinished(true, message);
        return true;
    }

    m_fontInstallRunning = true;
    emit fontInstallChanged();
    auto chain = std::make_shared<Chain>();
    chain->owner = this;
    auto installed = std::make_shared<QStringList>(), failed = std::make_shared<QStringList>();
    // Not something this mode installs: said so at the end, not dropped.
    for (const auto &name : packageOnly) failed->append(name);
    const QString dir = userFontDir();

    if (!packages.isEmpty()) {
        QStringList args{QStringLiteral("pacman"), QStringLiteral("-S"), QStringLiteral("--needed"), QStringLiteral("--noconfirm")};
        args += packages;
        chain->steps.append({QStringLiteral("pkexec"), args, [chain, packages, installed, failed](int code, const QByteArray &) {
            if (code != 0) { *failed += packages; return; }
            // The running app learns of the new files at once.
            QStringList list{QStringLiteral("-Qlq")};
            list += packages;
            chain->steps.prepend({QStringLiteral("pacman"), list, [packages, installed](int, const QByteArray &out) {
                for (const auto &line : QString::fromUtf8(out).split(QLatin1Char('\n')))
                    if (line.endsWith(QLatin1String(".ttf"), Qt::CaseInsensitive) || line.endsWith(QLatin1String(".otf"), Qt::CaseInsensitive))
                        QFontDatabase::addApplicationFont(line.trimmed());
                *installed += packages;
            }});
        }});
    }
    for (const auto &row : downloads) {
        const auto family = row.value("family").toString();
        const auto gf = row.value("googleFonts").toString(), gh = row.value("github").toString();
        QString api, owner, repo, path;
        if (!gf.isEmpty() && safePart(gf)) { owner = "google"; repo = "fonts"; path = gf; }
        else if (!gh.isEmpty() && safePart(gh) && gh.count(QLatin1Char('/')) >= 1) {
            const auto parts = gh.split(QLatin1Char('/'));
            owner = parts.value(0); repo = parts.value(1); path = parts.mid(2).join(QLatin1Char('/'));
        } else { failed->append(family); continue; }
        api = QStringLiteral("https://api.github.com/repos/%1/%2/contents/%3").arg(owner, repo, path);
        const bool googleFonts = owner == QLatin1String("google");
        const QString target = dir + "/" + compact(family);
        chain->steps.append({QStringLiteral("curl"), {QStringLiteral("-fsSL"), QStringLiteral("--max-time"), QStringLiteral("60"), api},
            [chain, family, owner, repo, path, googleFonts, target, installed, failed](int code, const QByteArray &out) {
            if (code != 0) { failed->append(family); return; }
            auto added = std::make_shared<int>(0);
            QList<Step> fetches;
            for (const auto &item : QJsonDocument::fromJson(out).array()) {
                const auto o = item.toObject();
                const auto name = o.value("name").toString();
                if (o.value("type").toString() != QLatin1String("file") || !safePart(name) || name.contains(QLatin1Char('/'))) continue;
                if (!name.endsWith(QLatin1String(".ttf"), Qt::CaseInsensitive) && !name.endsWith(QLatin1String(".otf"), Qt::CaseInsensitive)) continue;
                // A repository of several families: only this family's files.
                if (!googleFonts) {
                    const auto stem = compact(family);
                    if (!name.startsWith(stem + "-") && !name.startsWith(stem + "[") && !name.startsWith(stem + ".")) continue;
                }
                QString url;
                if (googleFonts) url = o.value("download_url").toString();
                else {
                    // Git LFS files are served from media.githubusercontent.com.
                    const auto ref = QUrlQuery(QUrl(o.value("url").toString())).queryItemValue("ref");
                    url = QStringLiteral("https://media.githubusercontent.com/media/%1/%2/%3/%4/%5").arg(owner, repo, ref.isEmpty() ? "HEAD" : ref, path, name);
                }
                if (!url.startsWith(QLatin1String("https://"))) continue;
                const QString file = target + "/" + name;
                fetches.append({QStringLiteral("curl"), {QStringLiteral("-fsSL"), QStringLiteral("--max-time"), QStringLiteral("120"), QStringLiteral("--create-dirs"), QStringLiteral("-o"), file, url},
                    [file, added](int code, const QByteArray &) {
                        if (code != 0) { QFile::remove(file); return; }
                        if (QFontDatabase::addApplicationFont(file) < 0) QFile::remove(file);   // not a font after all
                        else ++*added;
                    }});
            }
            if (fetches.isEmpty()) { failed->append(family); return; }
            fetches.append({QStringLiteral("true"), {}, [family, added, installed, failed](int, const QByteArray &) {
                if (*added > 0) installed->append(family); else failed->append(family);
            }});
            chain->steps = fetches + chain->steps;
        }});
    }
    QPointer<Backend> self(this);
    chain->finished = [self, installed, failed, dir] {
        if (!self) return;
        if (QFileInfo::exists(dir)) QProcess::startDetached(QStringLiteral("fc-cache"), {QStringLiteral("-f"), dir});
        self->fontInstallDone(failed->isEmpty(), *installed, *failed);
    };
    setStatus(tr("Installing %1…").arg(families.join(QStringLiteral(", "))));
    chain->next();
    return true;
}

void Backend::fontInstallDone(bool ok, const QStringList &installed, const QStringList &failed) {
    m_fontInstallRunning = false;
    emit fontInstallChanged();
    // Boxes still naming an Office font can use what just arrived.
    m_history.begin(m_document, tr("Use installed free fonts"));
    const auto report = FontSwap::apply(m_document);
    if (report.changed) { m_history.commit(); touch(); } else m_history.abandon();
    emit importReportChanged();
    QString message = installed.isEmpty() ? QString() : tr("Installed %1").arg(installed.join(QStringLiteral(", ")));
    if (!failed.isEmpty()) message += (message.isEmpty() ? QString() : QStringLiteral(". ")) + tr("Could not install %1").arg(failed.join(QStringLiteral(", ")));
    setStatus(message);
    emit fontInstallFinished(ok, message);
}
