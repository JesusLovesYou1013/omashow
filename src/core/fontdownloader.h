#pragma once

// Downloads free fonts into OmaShow's own font folder (FONT-CHECK-2/3). Only
// files listed in :/fonts/font-sources.json are fetched: exact paths in
// google/fonts at one pinned commit, each with its size and sha256, checked
// before anything parses the file. A family is built in <sandbox>.staging/
// <Family>/ and moved into the sandbox with one directory rename once every
// file is verified, so a family folder in the sandbox always means complete
// (and nothing half-made is ever in the folder other apps may share).
//
// One queue for everything (a deck's fonts and the Extra fonts pack); a deck
// can move its families to the front. Four files at a time; a file that sends
// nothing for 20 s, more bytes than listed, or fails its hash fails its
// family. Three network failures in a row (HTTP 429 and 5xx count) end the
// run as offline; a write that fails ends it as "disk full". Free space is
// checked before families are queued. No overall deadline.
// OMASHOW_FONT_BASE_URL replaces the base URL (tests; hashes still apply);
// OMASHOW_FONT_TEST_FREE_BYTES fakes the free space (tests).

#include <QHash>
#include <QList>
#include <QObject>
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

    static bool downloadable(const QString &family);
    static qint64 bytesFor(const QStringList &families);
    // Its folder is in the sandbox (complete by construction).
    static bool complete(const QString &family);

    bool running() const { return m_running; }
    // Queue these families (front: ahead of everything queued, also moving
    // ones already queued). Starts the run when idle.
    bool add(const QStringList &families, bool front);
    void drop(const QStringList &families);   // these only, as cancelled
    void cancel();                            // everything
    qint64 bytesDone(const QStringList &families) const;

signals:
    void progress();
    void familyReady(const QString &family);
    void familyFailed(const QString &family, const QString &why, bool network);
    void finished(const QStringList &installed, const QVariantMap &failed, bool offline, bool cancelled);

private:
    struct File { QString family, path, sha256, target; qint64 size = 0; bool font = true; };
    struct Active { File file; QFile *out = nullptr; QTimer *stall = nullptr; qint64 received = 0; };
    struct Family { int left = 0; qint64 total = 0, done = 0; };
    void pump();
    void done(QNetworkReply *reply);
    void settle(const QString &family);
    void failFamily(const QString &family, const QString &why, bool network);
    void stopAll(const QString &why, bool network);
    void finish(bool cancelled);
    static bool loadFamily(const QString &family);

    QNetworkAccessManager *m_net = nullptr;
    QList<File> m_queue;
    QHash<QNetworkReply *, Active> m_active;
    QHash<QString, Family> m_fam;   // families still coming
    QStringList m_installed;
    QVariantMap m_failed;
    int m_networkFails = 0;
    bool m_running = false, m_cancelled = false, m_allNetwork = true;
};
