#pragma once

// Downloads the free fonts a deck needs into OmaShow's own font folder
// (FONT-CHECK-2). Only files listed in :/fonts/font-sources.json are fetched:
// exact paths in google/fonts at one pinned commit, each with its size and
// sha256. A file is written as <name>.part, its hash checked before anything
// parses it, then renamed and handed to QFontDatabase::addApplicationFont.
// Nothing outside FontSandbox::dir() is touched; no password, no packages.
//
// Up to four files at a time; a file that sends nothing for 20 s, or more
// bytes than listed, is dropped; the whole run gives up after 5 minutes.
// OMASHOW_FONT_BASE_URL replaces the base URL (tests; hashes still apply).

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QTimer;

class FontDownloader : public QObject {
    Q_OBJECT
public:
    explicit FontDownloader(QObject *parent = nullptr);
    ~FontDownloader() override;

    // Families with files listed, and their total download size in bytes.
    static bool downloadable(const QString &family);
    static qint64 bytesFor(const QStringList &families);

    bool running() const { return m_running; }
    // Starts fetching every listed file of these families that is not already
    // in the folder with the right hash. False when nothing could start.
    bool start(const QStringList &families);
    void cancel();

    qint64 bytesDone() const;
    qint64 bytesTotal() const { return m_total; }

signals:
    void progress();
    // installed: families with every file in place and loaded.
    // failed: family -> reason. offline: every failure was the network.
    void finished(const QStringList &installed, const QVariantMap &failed, bool offline, bool cancelled);

private:
    struct File {
        QString family, path, sha256, target;
        qint64 size = 0;
        bool font = true;
    };
    struct Active {
        File file;
        QNetworkReply *reply = nullptr;
        QFile *out = nullptr;
        QTimer *stall = nullptr;
        qint64 received = 0;
    };
    void pump();
    void done(QNetworkReply *reply);
    void fail(const QString &family, const QString &why, bool network);
    void finish(bool cancelled);
    static bool load(const QString &path);

    QNetworkAccessManager *m_net = nullptr;
    QTimer *m_deadline = nullptr;
    QList<File> m_queue;
    QHash<QNetworkReply *, Active> m_active;
    QStringList m_families;
    QVariantMap m_failed;
    bool m_allNetwork = true, m_running = false, m_cancelled = false;
    qint64 m_total = 0, m_finishedBytes = 0;
};
