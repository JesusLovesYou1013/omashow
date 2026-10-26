// OmaShow — native presentations for Omarchy.

#include <tuple>
#include "core/fonts.h"
#include "core/fontsandbox.h"
#include <QElapsedTimer>
// TEST-ONLY BEGIN: offscreen --ui-shot steps; history/tools/make-upstream.py leaves this out
#include <QFontInfo>
// TEST-ONLY END

#include <QGuiApplication>
#include <QFont>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QUrl>

#include "backend.h"
#include "anim/presentation.h"
#include "io/bundle.h"
#include "io/interchange.h"
#include "io/odpwriter.h"
#include "io/pptxwriter.h"
#include "io/pdf.h"
#include "showguard.h"
#include "presenter.h"
#include "slideview.h"
#include "tableaccessibility.h"
#include "thumbnailprovider.h"
#include "theme.h"

#include <QCommandLineOption>
#include "cli/cli.h"
#include "render/effectpreview.h"
#include <QCommandLineParser>
#include <QDebug>
#include <QQmlEngine>
#include <QTextStream>
#include <QDir>
#include <QFile>
// TEST-ONLY BEGIN: offscreen --ui-shot steps; history/tools/make-upstream.py leaves this out
#include <QJsonDocument>
#include <QQuickItem>
#include <QWheelEvent>
#include <functional>
// TEST-ONLY END
#include <QEventLoop>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSurfaceFormat>
#include <QTimer>

namespace {

// The shot harness. Every visual claim in Gate 0 is checked through this rather
// than by looking at a running window, and it is the seam the video encoder and
// the wider CLI grow out of later: one time in, one deterministic frame out.
int renderShots(const Backend &backend, const QString &outDir,
                const QString &timesSpec, int width, int fps, bool includeSkipped) {
    // An even frame sequence, named so ffmpeg can read it directly. This is the
    // video export path in embryo: the encoder's only job later is to consume
    // what the evaluator already produces at exact frame intervals.
    if (fps > 0) {
        QTextStream out(stdout);
        const qreal duration = Presentation::duration(backend.document(),includeSkipped);
        const int frames = int(duration * fps) + 1;
        for (int i = 0; i < frames; ++i) {
            const QString path = QStringLiteral("%1/frame-%2.png")
                                     .arg(outDir)
                                     .arg(i, 5, 10, QLatin1Char('0'));
            if (!backend.renderFrame(qreal(i) / fps, path, width, includeSkipped)) {
                qCritical() << "Could not write" << path;
                return 1;
            }
        }
        out << frames << " frames at " << fps << " fps" << Qt::endl;
        return 0;
    }

    QStringList times = timesSpec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (times.isEmpty()) {
        // A spread that lands inside every build and across the transition.
        const qreal duration = Presentation::duration(backend.document(),includeSkipped);
        for (int i = 0; i <= 12; ++i)
            times << QString::number(duration * i / 12.0, 'f', 3);
    }

    QTextStream out(stdout);
    int index = 0;
    for (const QString &spec : times) {
        bool ok = false;
        const qreal t = spec.trimmed().toDouble(&ok);
        if (!ok) {
            qCritical() << "Not a time:" << spec;
            return 2;
        }
        const QString path = QStringLiteral("%1/frame-%2-t%3.png")
                                 .arg(outDir)
                                 .arg(index, 3, 10, QLatin1Char('0'))
                                 .arg(t, 0, 'f', 3);
        if (!backend.renderFrame(t, path, width, includeSkipped)) {
            qCritical() << "Could not write" << path;
            return 1;
        }
        out << path << Qt::endl;
        ++index;
    }
    return 0;
}

} // namespace

// Photograph the interface, one PNG per workspace, for design review off the
// user's screen. Run offscreen: QT_QPA_PLATFORM=offscreen omashow --ui-shot dir
int captureInterface(QQmlApplicationEngine &engine, Backend &backend, Presenter &presenter,
                     const QString &outDir, const QSize &size, bool haveDeck) {
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    if (!window) return 1;
    QDir().mkpath(outDir);
    window->resize(size);
    const auto settle = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    const auto save = [&](QQuickWindow *w, const QString &name) {
        settle(300);
        w->grabWindow().save(outDir + QLatin1Char('/') + name + QStringLiteral(".png"));
    };
    if (!haveDeck) {
        save(window, QStringLiteral("start"));
        // TEST-ONLY BEGIN: offscreen --ui-shot steps; history/tools/make-upstream.py leaves this out
        // A narrow start page: Shift + wheel moves it sideways, and the pointer
        // over it shows the sideways bar (OMASHOW_UI_SHOT_START_SCROLL=1).
        if (qEnvironmentVariableIsSet("OMASHOW_UI_SHOT_START_SCROLL")) {
            const std::function<QQuickItem *(QQuickItem *, const QString &)> find =
                [&](QQuickItem *item, const QString &name) -> QQuickItem * {
                if (item->objectName() == name) return item;
                for (QQuickItem *child : item->childItems())
                    if (QQuickItem *found = find(child, name)) return found;
                return nullptr;
            };
            QQuickItem *scroll = find(window->contentItem(), QStringLiteral("templateScroll"));
            QQuickItem *bar = find(window->contentItem(), QStringLiteral("startSidewaysBar"));
            QObject *flick = scroll ? scroll->property("contentItem").value<QObject *>() : nullptr;
            qInfo().noquote() << "ui-shot start: overflows" << (scroll ? scroll->property("overflowsX").toBool() : false)
                              << "contentX" << (flick ? flick->property("contentX").toDouble() : -1)
                              << "bar visible" << (bar ? bar->isVisible() : false);
            if (scroll && flick) {
                const QPointF at = scroll->mapToScene(QPointF(scroll->width() / 2, scroll->height() / 2));
                QMouseEvent move(QEvent::MouseMove, at, at, window->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &move);
                settle(200);
                qInfo().noquote() << "ui-shot start: pointer over it, bar visible" << (bar ? bar->isVisible() : false);
                for (int i = 0; i < 3; ++i) {
                    QWheelEvent wheel(at, window->mapToGlobal(at), QPoint(), QPoint(0, -120), Qt::NoButton,
                                      Qt::ShiftModifier, Qt::NoScrollPhase, false);
                    QCoreApplication::sendEvent(window, &wheel);
                    settle(60);
                }
                qInfo().noquote() << "ui-shot start: after Shift+wheel contentX" << flick->property("contentX").toDouble()
                                  << "contentY" << flick->property("contentY").toDouble();
                save(window, QStringLiteral("start-scrolled"));
                QWheelEvent plain(at, window->mapToGlobal(at), QPoint(), QPoint(0, -120), Qt::NoButton,
                                  Qt::NoModifier, Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(window, &plain);
                settle(200);
                qInfo().noquote() << "ui-shot start: after plain wheel contentX" << flick->property("contentX").toDouble()
                                  << "contentY" << flick->property("contentY").toDouble();
            }
        }
        // TEST-ONLY END
        backend.createDeck(0, 1920, 1080, 0);
    }
    // TEST-ONLY BEGIN: offscreen --ui-shot steps; history/tools/make-upstream.py leaves this out
    // SAVE-FORMAT-1 (OMASHOW_UI_SHOT_SAVE_AS=<n>): Save as… n times, each
    // answered by a stand-in portal (history/tools/fakeportal.py) with the name
    // to type; says what was written each time.
    if (const int rounds = qEnvironmentVariableIntValue("OMASHOW_UI_SHOT_SAVE_AS")) {
        for (int i = 0; i < rounds; ++i) {
            QEventLoop wait;
            QObject::connect(&backend, &Backend::saved, &wait, &QEventLoop::quit);
            QObject::connect(&backend, &Backend::failed, &wait, &QEventLoop::quit);
            QObject::connect(&backend, &Backend::saveCanceled, &wait, &QEventLoop::quit);
            QTimer::singleShot(20000, &wait, &QEventLoop::quit);
            backend.saveAsDialog();
            wait.exec();
            settle(200);
            qInfo().noquote() << "ui-shot saveas:" << i << "file" << backend.property("fileUrl").toUrl().toLocalFile()
                              << "| status" << backend.property("status").toString();
        }
        return 0;
    }
    // FONT-CHECK-1/2 (OMASHOW_UI_SHOT_FONTCHECK=1|hide|download): the font
    // check shown on opening a deck, photographed. hide: tick "Don't show
    // again" and close. download: press Download, photograph the progress and
    // the result, and say what the deck uses afterwards.
    if (haveDeck && !qEnvironmentVariableIsEmpty("OMASHOW_UI_SHOT_FONTCHECK")) {
        const auto mode = qEnvironmentVariable("OMASHOW_UI_SHOT_FONTCHECK");
        const auto json = [](const QVariant &v) { return QString::fromUtf8(QJsonDocument::fromVariant(v).toJson(QJsonDocument::Compact)); };
        settle(500);
        QObject *check = window->findChild<QObject *>(QStringLiteral("fontCheckDialog"));
        qInfo().noquote() << "ui-shot fontcheck: sandbox" << backend.fontSandboxDir();
        qInfo().noquote() << "ui-shot fontcheck: needed" << json(backend.fontsToInstall());
        qInfo().noquote() << "ui-shot fontcheck: shown" << (check && check->property("visible").toBool())
                          << "deck held" << backend.fontGate() << "show running" << presenter.property("running").toBool();
        qInfo().noquote() << "ui-shot fontcheck: swaps" << json(backend.fontSwaps());
        save(window, QStringLiteral("fontcheck"));
        // FONT-CHECK-3: "Use fallback fonts" on the loading state, that long in.
        if (const int ms = qEnvironmentVariableIntValue("OMASHOW_UI_SHOT_GATE_SKIP_MS")) {
            settle(ms);
            QObject *skip = window->findChild<QObject *>(QStringLiteral("fontGateFallback"));
            qInfo().noquote() << "ui-shot fontcheck: gate skip at" << ms << "ms; held" << backend.fontGate() << "button" << bool(skip);
            if (skip) QMetaObject::invokeMethod(skip, "clicked");
            settle(1500);
            qInfo().noquote() << "ui-shot fontcheck: after skip held" << backend.fontGate() << "show running" << presenter.property("running").toBool();
            save(window, QStringLiteral("gate-skipped"));
        }
        if (qEnvironmentVariableIsSet("OMASHOW_UI_SHOT_SAVE")) backend.saveTo(outDir + QStringLiteral("/before.omashow"));
        const QString source = check ? check->property("source").toString() : QString();
        if (check && mode == QLatin1String("hide")) {
            if (auto *box = check->findChild<QObject *>(QStringLiteral("fontCheckHide"))) box->setProperty("checked", true);
            QMetaObject::invokeMethod(check, "close");
            settle(300);
            qInfo().noquote() << "ui-shot fontcheck: hidden for" << source << backend.fontCheckHidden(source);
        }
        // FONT-CHECK-3 conditions (OMASHOW_UI_SHOT_FONT_COND, with MODE=download):
        // show: a rehearsal runs while the fonts arrive (deck untouched until it ends);
        // editor: a text box is being typed in (typing kept, then redrawn);
        // undo: an edit is made first; undo after arrival undoes the edit only.
        const QString cond = qEnvironmentVariable("OMASHOW_UI_SHOT_FONT_COND");
        const std::function<QQuickItem *(QQuickItem *, const QString &)> findIn = [&](QQuickItem *item, const QString &name) -> QQuickItem * {
            if (!item) return nullptr;
            if (item->objectName() == name) return item;
            for (QQuickItem *child : item->childItems()) if (QQuickItem *f = findIn(child, name)) return f;
            return nullptr;
        };
        QQuickItem *condCanvas = findIn(window->contentItem(), QStringLiteral("editCanvas"));
        QQuickItem *condEditor = nullptr;
        QString condTextId;
        // With MODE=pack the opening deck is first shown on fallbacks (its
        // popup closed), so the pack's fonts arrive into a deck on screen.
        if (check && mode == QLatin1String("pack")) { QMetaObject::invokeMethod(check, "close"); settle(300); }
        if (check && (mode == QLatin1String("download") || mode == QLatin1String("pack")) && !cond.isEmpty()) {
            for (const QVariant &object : backend.slideObjects())
                if (object.toMap().value(QStringLiteral("text")).toBool()) { condTextId = object.toMap().value(QStringLiteral("id")).toString(); break; }
            if (cond == QLatin1String("show")) {
                presenter.start(false, true);
                settle(300);
                qInfo().noquote() << "ui-shot fontcheck: cond show running" << presenter.property("running").toBool();
            } else if (cond == QLatin1String("editor") && condCanvas) {
                backend.select(condTextId);
                settle(100);
                QMetaObject::invokeMethod(condCanvas, "beginTextEdit");
                settle(200);
                for (QQuickItem *k : condCanvas->findChildren<QQuickItem *>())
                    if (QByteArray(k->metaObject()->className()).contains("TextEdit") && k->isVisible()) { condEditor = k; break; }
                if (condEditor) {
                    QMetaObject::invokeMethod(condEditor, "deselect");
                    const int at = condEditor->property("length").toInt();
                    QMetaObject::invokeMethod(condEditor, "insert", Q_ARG(int, at), Q_ARG(QString, QStringLiteral(" TYPED-WHILE-DOWNLOADING")));
                }
                qInfo().noquote() << "ui-shot fontcheck: cond editor open" << condCanvas->property("editingText").toBool() << "editor found" << bool(condEditor);
            } else if (cond == QLatin1String("undo")) {
                backend.select(condTextId);
                backend.nudgeSelected(40, 0);
                qInfo().noquote() << "ui-shot fontcheck: cond undo nudged" << condTextId << "canUndo" << backend.canUndo();
            }
            save(window, QStringLiteral("cond-before"));
        }
        const auto objectText = [&] {
            if (condCanvas && condCanvas->property("editingText").toBool()) return QVariantMap();   // don't disturb an open editor
            backend.select(condTextId);
            return backend.property("selection").toMap();
        };
        const auto condAfter = [&] {
                if (cond == QLatin1String("show")) {
                    qInfo().noquote() << "ui-shot fontcheck: cond show still running" << presenter.property("running").toBool() << "pending" << backend.fontsPending();
                    save(window, QStringLiteral("cond-during-show"));
                    QMetaObject::invokeMethod(&presenter, "stop");
                    settle(600);
                    qInfo().noquote() << "ui-shot fontcheck: cond show ended; pending" << backend.fontsPending() << "swaps" << json(backend.fontSwaps());
                } else if (cond == QLatin1String("editor")) {
                    settle(300);
                    const bool stillOpen = condCanvas && condCanvas->property("editingText").toBool();
                    const QString plain = stillOpen && condEditor ? condEditor->property("text").toString() : objectText().value(QStringLiteral("editHtml")).toString();
                    qInfo().noquote() << "ui-shot fontcheck: cond editor open after" << stillOpen
                                      << "typed text kept" << plain.contains(QStringLiteral("TYPED-WHILE-DOWNLOADING"));
                } else if (cond == QLatin1String("undo")) {
                    const qreal x = objectText().value(QStringLiteral("x")).toReal();
                    backend.undo();
                    settle(300);
                    qInfo().noquote() << "ui-shot fontcheck: cond undo x before" << x << "after undo" << objectText().value(QStringLiteral("x")).toReal()
                                      << "swaps after undo" << json(backend.fontSwaps()) << "canUndo" << backend.canUndo();
                }
        };
        // FONT-CHECK-3 pack (OMASHOW_UI_SHOT_FONTCHECK=pack): turn on the Extra
        // fonts pack, photograph its offer, download it. OMASHOW_UI_SHOT_DECK_MS:
        // that long into the pack, ask for the open deck's own fonts (they must
        // jump the queue). OMASHOW_UI_SHOT_CANCEL_MS cancels the pack.
        if (check && mode == QLatin1String("pack")) {
            backend.setExtraFontsPack(true);
            QMetaObject::invokeMethod(check, "showPack");
            settle(400);
            {   // every pack family usable under its map name (aliases included)
                QStringList missing;
                for (const auto &r : backend.fontPack()) {
                    const auto f = r.toMap().value("family").toString();
                    // What a text set in the map's name actually draws with.
                    const auto drawn = QFontInfo(QFont(f)).family();
                    if (!FontSandbox::available(f) || (drawn.compare(f, Qt::CaseInsensitive) && drawn.compare(QFont::substitute(f), Qt::CaseInsensitive))) missing << f + QStringLiteral("->") + drawn;
                    else if (drawn.compare(f, Qt::CaseInsensitive)) qInfo().noquote() << "ui-shot fontcheck: alias" << f << "draws as" << drawn;
                }
                qInfo().noquote() << "ui-shot fontcheck: pack families not usable" << missing.size() << missing.join(QLatin1Char(','));
            }
            qInfo().noquote() << "ui-shot fontcheck: pack" << backend.packReady() << "of" << backend.packTotal() << "ready," << qRound(backend.packBytes() / 1048576) << "MB";
            save(window, QStringLiteral("pack-offer"));
            QElapsedTimer clock; clock.start();
            QEventLoop wait;
            bool shotProgress = false;
            QStringList order;
            QObject::connect(&backend, &Backend::fontDownloadChanged, &wait, [&] {
                if (!shotProgress && backend.packDone() > 4e6) { shotProgress = true; QTimer::singleShot(0, &wait, [&] { save(window, QStringLiteral("pack-progress")); }); }
            });
            QObject::connect(&backend, &Backend::fontDownloadFinished, &wait, [&](const QStringList &installed, const QVariantMap &failed, bool, bool) {
                qInfo().noquote() << "ui-shot fontcheck: deck finished at" << clock.elapsed() << "ms installed" << installed.join(QLatin1Char(',')) << "failed" << json(failed)
                                  << "pack still running" << backend.packRunning() << backend.packReady() << "of" << backend.packTotal();
                QTimer::singleShot(0, &wait, [&] { save(window, QStringLiteral("pack-deck-done")); });
            });
            QObject::connect(&backend, &Backend::packFinished, &wait, [&](int ready, int total, const QVariantMap &failed, bool offline) {
                qInfo().noquote() << "ui-shot fontcheck: pack finished at" << clock.elapsed() << "ms" << ready << "of" << total << "failed" << failed.size() << json(failed).left(300) << "offline" << offline;
                QTimer::singleShot(400, &wait, &QEventLoop::quit);
            });
            if (const int ms = qEnvironmentVariableIntValue("OMASHOW_UI_SHOT_DECK_MS"))
                QTimer::singleShot(ms, &backend, [&] {
                    QStringList fams;
                    for (const auto &r : backend.fontsToInstall()) if (r.toMap().value("downloadable").toBool()) fams << r.toMap().value("family").toString();
                    qInfo().noquote() << "ui-shot fontcheck: deck asks at" << clock.elapsed() << "ms for" << fams.join(QLatin1Char(',')) << "pack" << backend.packReady() << "of" << backend.packTotal();
                    backend.downloadFonts(fams);
                });
            if (const int ms = qEnvironmentVariableIntValue("OMASHOW_UI_SHOT_CANCEL_MS"))
                QTimer::singleShot(ms, &backend, [&] { save(window, QStringLiteral("pack-progress")); qInfo().noquote() << "ui-shot fontcheck: pack cancel at" << clock.elapsed() << "ms"; backend.cancelPack(); });
            if (const int ms = qEnvironmentVariableIntValue("OMASHOW_UI_SHOT_DECK_CANCEL_MS"))
                QTimer::singleShot(ms, &backend, [&] { qInfo().noquote() << "ui-shot fontcheck: deck cancel at" << clock.elapsed() << "ms"; backend.cancelFontDownload(); });
            QTimer::singleShot(1500000, &wait, &QEventLoop::quit);
            QMetaObject::invokeMethod(check->findChild<QObject *>(QStringLiteral("fontCheckDownload")), "clicked");
            wait.exec();
            qInfo().noquote() << "ui-shot fontcheck: dialog state" << check->property("phase").toString() << "visible" << check->property("visible").toBool();
            if (!cond.isEmpty()) { QMetaObject::invokeMethod(check, "close"); settle(300); condAfter(); }
            qInfo().noquote() << "ui-shot fontcheck: swaps after" << json(backend.fontSwaps());
            qInfo().noquote() << "ui-shot fontcheck: status" << backend.property("status").toString() << "| pack status" << backend.packStatus();
            save(window, QStringLiteral("pack-after"));
            return 0;
        }
        if (check && mode == QLatin1String("download") && check->property("visible").toBool()) {
            QEventLoop wait;
            bool shotProgress = false;
            QObject::connect(&backend, &Backend::fontDownloadChanged, &wait, [&] {
                // Not from inside the download's own signal: save() spins an event loop.
                if (!shotProgress && backend.fontDownloadDone() > 0) { shotProgress = true; QTimer::singleShot(0, &wait, [&] { save(window, QStringLiteral("fontcheck-progress")); }); }
            });
            QObject::connect(&backend, &Backend::fontDownloadFinished, &wait,
                             [&](const QStringList &installed, const QVariantMap &failed, bool offline, bool cancelled) {
                qInfo().noquote() << "ui-shot fontcheck: finished installed" << installed.join(QLatin1Char(','))
                                  << "failed" << json(failed) << "offline" << offline << "cancelled" << cancelled;
                QTimer::singleShot(400, &wait, &QEventLoop::quit);
            });
            QTimer::singleShot(qEnvironmentVariableIntValue("OMASHOW_UI_SHOT_CANCEL_MS") > 0 ? qEnvironmentVariableIntValue("OMASHOW_UI_SHOT_CANCEL_MS") : 1 << 30,
                               &backend, [&] { if (!shotProgress) save(window, QStringLiteral("fontcheck-progress")); backend.cancelFontDownload(); });
            QTimer::singleShot(400000, &wait, &QEventLoop::quit);
            QMetaObject::invokeMethod(check->findChild<QObject *>(QStringLiteral("fontCheckDownload")), "clicked");
            wait.exec();
            qInfo().noquote() << "ui-shot fontcheck: dialog state" << check->property("phase").toString() << "visible" << check->property("visible").toBool();
            qInfo().noquote() << "ui-shot fontcheck: swaps after" << json(backend.fontSwaps());
            qInfo().noquote() << "ui-shot fontcheck: needed after" << json(backend.fontsToInstall());
            qInfo().noquote() << "ui-shot fontcheck: modified" << backend.modified() << "canUndo" << backend.canUndo()
                              << "revision" << backend.revision() << "deck held" << backend.fontGate() << "show running" << presenter.property("running").toBool();
            condAfter();
            save(window, QStringLiteral("fontcheck-after"));
            if (qEnvironmentVariableIsSet("OMASHOW_UI_SHOT_SAVE")) backend.saveTo(outDir + QStringLiteral("/after.omashow"));
            if (check->property("visible").toBool()) QMetaObject::invokeMethod(check, "close");
            settle(300);
            save(window, QStringLiteral("fontcheck-deck"));
        }
        return 0;
    }
    // TEST-ONLY END
    const struct { int workspace; const char *name; } views[] = {
        {0, "edit"}, {1, "design"}, {2, "animate"}, {3, "review"}, {4, "present"}, {6, "sorter"}};
    for (const auto &view : views) {
        window->setProperty("workspace", view.workspace);
        save(window, QString::fromLatin1(view.name));
    }
    window->setProperty("workspace", 0);
    window->setProperty("sidebar", 1);
    save(window, QStringLiteral("document"));
    window->setProperty("sidebar", 0);
    QMetaObject::invokeMethod(window, "showExport");
    save(window, QStringLiteral("export"));
    for (QObject *sheet : window->findChildren<QObject *>(QStringLiteral("exportSheet")))
        QMetaObject::invokeMethod(sheet, "close");
    const QVariantList objects = backend.slideObjects();
    if (!objects.isEmpty()) {
        backend.select(objects.first().toMap().value(QStringLiteral("id")).toString());
        save(window, QStringLiteral("edit-selected"));
        backend.setProperty("currentSlide", 0);
    }
    // TEST-ONLY BEGIN: offscreen --ui-shot steps; history/tools/make-upstream.py leaves this out
    // The continuous canvas: mouse-wheel notches down until the next slide
    // snaps in (the frame before is mid-scroll), then back up to the first.
    // Visual children, not QObject children: views inside layouts are found here.
    const std::function<QQuickItem *(QQuickItem *, const QString &)> findItem =
        [&](QQuickItem *item, const QString &name) -> QQuickItem * {
        if (item->objectName() == name) return item;
        for (QQuickItem *child : item->childItems())
            if (QQuickItem *found = findItem(child, name)) return found;
        return nullptr;
    };
    // STYLE3D: open or close Style tab groups and rows (Inspector.qml styleOpen).
    const auto styleOpen = [&](const QStringList &keys, bool open) {
        QQuickItem *inspector = findItem(window->contentItem(), QStringLiteral("inspectorPanel"));
        QObject *state = inspector ? inspector->findChild<QObject *>(QStringLiteral("styleOpen")) : nullptr;
        if (!state) state = window->findChild<QObject *>(QStringLiteral("styleOpen"));
        if (!state) { qInfo().noquote() << "ui-shot: no styleOpen FAIL"; return; }
        for (const auto &key : keys) QMetaObject::invokeMethod(state, "set", Q_ARG(QVariant, key), Q_ARG(QVariant, open));
    };
    const auto styleCloseAll = [&] {
        QQuickItem *inspector = findItem(window->contentItem(), QStringLiteral("inspectorPanel"));
        if (QObject *state = inspector ? inspector->findChild<QObject *>(QStringLiteral("styleOpen")) : nullptr) QMetaObject::invokeMethod(state, "closeAll");
    };
    // Pointer events as a mouse sends them; the window's cursor is read back.
    const auto mouse = [&](QEvent::Type type, const QPointF &at, Qt::MouseButtons held) {
        QMouseEvent event(type, at, at, window->mapToGlobal(at),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
        settle(30);
    };
    int cursorFailures = 0;
    const auto expectCursor = [&](const QString &what, int want) {
        const int got = window->cursor().shape();
        if (got != want) ++cursorFailures;
        qInfo().noquote() << QStringLiteral("ui-shot cursor: %1 = %2 (want %3) %4")
                                 .arg(what).arg(got).arg(want).arg(got == want ? "ok" : "FAIL");
    };
    // EFFECTS-UI-1 (OMASHOW_UI_SHOT_EFFECTS=1): the Style tab's Effects for a
    // text box, a shape and a filled box with words (the "Apply to" choice),
    // folded and unfolded. The deck is the effects test deck: slide 1 text
    // glows, slide 2 shape glows, slide 7 text boxes. Says what each holds.
    if (qEnvironmentVariableIsSet("OMASHOW_UI_SHOT_EFFECTS")) {
        QQuickItem *panel = findItem(window->contentItem(), QStringLiteral("effectsPanel"));
        QQuickItem *tabs = findItem(window->contentItem(), QStringLiteral("inspectorTabs"));
        if (!panel || !tabs) { qInfo().noquote() << "ui-shot effects: no effects panel FAIL"; return 3; }
        const auto fold = [&](bool open) {
            // STYLE3D: the groups and the Glow and Shadow rows of both sets.
            styleOpen({QStringLiteral("group.effects"), QStringLiteral("group.text")}, true);
            for (const char *set : {"shape.", "text."})
                for (const char *key : {"glow", "glowPresets", "shadow", "shadowPresets"}) styleOpen({QString::fromLatin1(set) + QLatin1String(key)}, open);
            settle(200);
        };
        const auto scrollTo = [&](QQuickItem *item) {
            for (QQuickItem *up = item->parentItem(); up; up = up->parentItem())
                if (up->inherits("QQuickFlickable")) {
                    const QPointF in = item->mapToItem(up->property("contentItem").value<QQuickItem *>(), QPointF());
                    const qreal most = qMax(0.0, up->property("contentHeight").toReal() - up->height());
                    up->setProperty("contentY", qBound(0.0, in.y() - 40, most));
                    settle(150);
                    return up;
                }
            return static_cast<QQuickItem *>(nullptr);
        };
        const auto shoot = [&](const QString &name) {
            tabs->setProperty("currentIndex", 1);
            settle(150);
            QQuickItem *flick = scrollTo(panel);
            settle(300);
            const QImage whole = window->grabWindow();
            whole.save(outDir + QStringLiteral("/effects-%1-window.png").arg(name));
            if (flick) {
                const QRectF r = flick->mapRectToScene(QRectF(0, 0, flick->width(), flick->height()));
                const qreal dpr = whole.devicePixelRatio();
                const QRect crop = QRect(int((r.left() - 8) * dpr), 0, int((r.width() + 16) * dpr), whole.height()).intersected(whole.rect());
                whole.copy(crop).save(outDir + QStringLiteral("/effects-%1.png").arg(name));
            }
            const auto text = backend.effectState(QStringLiteral("text")), shape = backend.effectState(QStringLiteral("shape"));
            qInfo().noquote() << "ui-shot effects:" << name << "mode" << panel->property("mode").toString()
                              << "text" << text.value("count").toInt() << "glow" << text.value("glowRadius").toDouble() << "shadow" << text.value("shadowEnabled").toBool()
                              << "inner" << text.value("shadowInner").toBool() << "sy" << text.value("shadowScaleY").toDouble() << "kx" << text.value("shadowSkewX").toDouble()
                              << "| shape" << shape.value("count").toInt() << "glow" << shape.value("glowRadius").toDouble() << "shadow" << shape.value("shadowEnabled").toBool()
                              << "inner" << shape.value("shadowInner").toBool();
        };
        const auto onSlide = [&](int slide) { backend.setProperty("currentSlide", slide); settle(300); return backend.slideObjects(); };
        const auto firstOf = [](const QVariantList &objects, const std::function<bool(const QVariantMap &)> &want, int skip = 0) {
            for (const auto &v : objects) { const auto m = v.toMap(); if (want(m) && skip-- == 0) return m.value("id").toString(); }
            return QString();
        };
        int found = 0;
        // A text box (glowing words).
        auto objects = onSlide(0);
        if (const QString id = firstOf(objects, [](const QVariantMap &m) { return m.value("name").toString() == QLatin1String("Glow"); }); !id.isEmpty()) {
            ++found; backend.select(id); fold(false); shoot(QStringLiteral("text-collapsed")); fold(true); shoot(QStringLiteral("text-expanded"));
        }
        // A shape (glowing).
        objects = onSlide(1);
        if (const QString id = firstOf(objects, [](const QVariantMap &m) { return m.value("name").toString() == QLatin1String("Shape"); }); !id.isEmpty()) {
            ++found; backend.select(id); fold(false); shoot(QStringLiteral("shape-collapsed")); fold(true); shoot(QStringLiteral("shape-expanded"));
        }
        // A shape with an inner shadow, unfolded.
        objects = onSlide(4);
        if (const QString id = firstOf(objects, [](const QVariantMap &m) { return m.value("name").toString() == QLatin1String("Shape"); }); !id.isEmpty()) {
            ++found; backend.select(id); fold(true); shoot(QStringLiteral("shape-inner-expanded"));
        }
        // TEXT-SHADOW-2: words with an inner, then a perspective, shadow; the
        // shadow unfolded (glow folded) so the Inner and Perspective presets show.
        const auto shadowOnly = [&] {
            fold(false);
            styleOpen({QStringLiteral("text.shadow"), QStringLiteral("text.shadowPresets")}, true);
            settle(200);
        };
        for (const auto &[slide, name] : {std::pair{7, "text-inner-expanded"}, std::pair{8, "text-perspective-expanded"}}) {
            objects = onSlide(slide);
            const QString id = firstOf(objects, [](const QVariantMap &m) {
                const QString n = m.value("name").toString();
                return n == QLatin1String("Shadow") || n == QLatin1String("Text");
            });
            if (id.isEmpty()) continue;
            ++found; backend.select(id); shadowOnly(); shoot(QString::fromLatin1(name));
        }
        // A filled box with words: both, so "Apply to" shows; then the box chosen.
        objects = onSlide(6);
        {
            QString box, words;
            const QString filled = firstOf(objects, [](const QVariantMap &m) { return m.value("name").toString() == QLatin1String("Shape"); }, 1);
            bool after = false;
            for (const auto &v : objects) {
                const auto m = v.toMap();
                if (m.value("id").toString() == filled) { after = true; continue; }
                if (after && m.value("name").toString() == QLatin1String("Text box")) { words = m.value("id").toString(); break; }
            }
            box = filled;
            if (!box.isEmpty() && !words.isEmpty()) {
                ++found;
                backend.selectIds(QStringList{box, words});
                fold(false); shoot(QStringLiteral("both-collapsed"));
                fold(true); shoot(QStringLiteral("both-text-expanded"));
                panel->setProperty("chosen", QStringLiteral("shape"));
                settle(200);
                shoot(QStringLiteral("both-box-expanded"));
                // BOTH-EFFECTS-1: Both, the two sets one under the other.
                panel->setProperty("chosen", QStringLiteral("both"));
                fold(false); shoot(QStringLiteral("both-mode-collapsed"));
                fold(true); shoot(QStringLiteral("both-mode-expanded"));
                panel->setProperty("chosen", QString());
            }
            // The plain text box (no fill, no line): its box glow is now the words'.
            const QString plain = firstOf(objects, [](const QVariantMap &m) { return m.value("name").toString() == QLatin1String("Text box"); }, 1);
            if (!plain.isEmpty()) {
                backend.select(plain); fold(true); shoot(QStringLiteral("plain-textbox-expanded"));
                // BOTH-EFFECTS-1: its Box (the hint: drawn on the words), then Both.
                panel->setProperty("chosen", QStringLiteral("shape"));
                fold(false); styleOpen({QStringLiteral("shape.glow")}, true); settle(200); shoot(QStringLiteral("plain-textbox-box"));
                panel->setProperty("chosen", QStringLiteral("both"));
                fold(false); styleOpen({QStringLiteral("shape.glow"), QStringLiteral("shape.shadow"), QStringLiteral("text.glow"), QStringLiteral("text.shadow")}, true); settle(200);
                shoot(QStringLiteral("plain-textbox-both"));
                panel->setProperty("chosen", QString());
            }
        }
        qInfo().noquote() << "ui-shot effects: cases" << found << (found == 6 ? "ok" : "FAIL");
        return found == 6 ? 0 : 3;
    }
    // FILL-UI-1 (OMASHOW_UI_SHOT_FILL=1): the Style tab's Fill and Line for
    // shapes and text boxes, folded and unfolded, on the fill test deck
    // (scratch FILL-UI-test.pptx). Says what each holds.
    if (qEnvironmentVariableIsSet("OMASHOW_UI_SHOT_FILL")) {
        QQuickItem *panel = findItem(window->contentItem(), QStringLiteral("fillPanel"));
        QQuickItem *tabs = findItem(window->contentItem(), QStringLiteral("inspectorTabs"));
        if (!panel || !tabs) { qInfo().noquote() << "ui-shot fill: no fill panel FAIL"; return 3; }
        // The deck's Office fonts (Aptos) hold it behind the font check: use the fallbacks.
        if (backend.fontGate()) { backend.skipFontGate(); settle(500); }
        if (QObject *check = window->findChild<QObject *>(QStringLiteral("fontCheckDialog")); check && check->property("visible").toBool()) {
            QMetaObject::invokeMethod(check, "close");
            settle(800);
        }
        const auto fold = [&](bool fill, bool line, bool presets = false) {
            styleOpen({QStringLiteral("group.fill")}, true);
            styleOpen({QStringLiteral("shape.fill")}, fill); styleOpen({QStringLiteral("shape.line")}, line); styleOpen({QStringLiteral("shape.fillPresets")}, presets);
            settle(250);
        };
        const auto shoot = [&](const QString &name) {
            tabs->setProperty("currentIndex", 1);
            settle(150);
            QQuickItem *flick = nullptr;
            for (QQuickItem *up = panel->parentItem(); up; up = up->parentItem())
                if (up->inherits("QQuickFlickable")) {
                    const QPointF in = panel->mapToItem(up->property("contentItem").value<QQuickItem *>(), QPointF());
                    const qreal most = qMax(0.0, up->property("contentHeight").toReal() - up->height());
                    up->setProperty("contentY", qBound(0.0, in.y() - 40, most));
                    flick = up;
                    break;
                }
            settle(500);
            const QImage whole = window->grabWindow();
            whole.save(outDir + QStringLiteral("/fill-%1-window.png").arg(name));
            if (flick) {
                const QRectF r = flick->mapRectToScene(QRectF(0, 0, flick->width(), flick->height()));
                const qreal dpr = whole.devicePixelRatio();
                const QRect crop = QRect(int((r.left() - 8) * dpr), 0, int((r.width() + 16) * dpr), whole.height()).intersected(whole.rect());
                whole.copy(crop).save(outDir + QStringLiteral("/fill-%1.png").arg(name));
            }
            const auto st = backend.fillState();
            qInfo().noquote() << "ui-shot fill:" << name << "target" << st.value("target").toString() << "count" << st.value("count").toInt()
                              << "fillStyle" << st.value("fillStyle").toInt() << "stops" << st.value("gradientStops").toList().size()
                              << "pattern" << st.value("patternPreset").toString() << "tile" << st.value("pictureTile").toBool()
                              << "line" << st.value("strokeWidth").toDouble() << "dash" << st.value("strokeDash").toString()
                              << "compound" << st.value("strokeCompound").toInt() << "visible" << panel->isVisible();
        };
        const auto onSlide = [&](int slide) { backend.setProperty("currentSlide", slide); settle(300); return backend.slideObjects(); };
        // The deck's objects in order (title, then each sample and its label).
        struct Case { int slide, index; const char *shot; bool fill, line, presets; };
        const Case cases[] = {
            {1, 5, "shape-gradient", true, false, false},         // 5 stops 45°
            {1, 5, "shape-collapsed", false, false, false},
            {1, 5, "shape-presets", true, false, true},
            {2, 19, "shape-path-gradient", true, false, false},   // shape path, off centre
            {3, 23, "shape-picture-tile", true, false, false},    // tile 40% mirror xy
            {3, 3, "shape-picture-stretch", true, false, false},  // stretch with offsets
            {4, 97, "shape-pattern", true, false, false},         // weave
            {6, 13, "shape-line", false, true, false},            // triple 4 pt dash
            {8, 3, "shape-gradient-line", false, true, false},    // gradient line, 5 stops
            {1, 25, "textbox-gradient", true, false, false},      // text box, gradient fill
            {9, 17, "textbox-fill-line", true, true, false},      // text box, fill, line and insets
            {9, 15, "textbox-line", false, true, false},          // text box, dashed outline
            {0, 0, "textbox-plain", true, true, false},           // the title: no fill, no line
        };
        int found = 0;
        for (const auto &c : cases) {
            const auto objects = onSlide(c.slide);
            if (c.index >= objects.size()) { qInfo().noquote() << "ui-shot fill: missing" << c.shot << "slide" << backend.property("currentSlide").toInt() << "has" << objects.size() << "FAIL"; continue; }
            ++found;
            backend.select(objects.at(c.index).toMap().value("id").toString());
            fold(c.fill, c.line, c.presets);
            shoot(QString::fromLatin1(c.shot));
        }
        const int want = int(sizeof cases / sizeof cases[0]);
        qInfo().noquote() << "ui-shot fill: cases" << found << (found == want ? "ok" : "FAIL");
        return found == want ? 0 : 3;
    }
    // STYLE3D (OMASHOW_UI_SHOT_STYLE3D=1): the Style tab's three groups for a
    // shape and a text box on the STYLE3D test deck (scratch STYLE3D-UI-test):
    // all closed, then each group open with its rows, then the Effects
    // galleries. Then the new settings through the backend, each undone.
    if (qEnvironmentVariableIsSet("OMASHOW_UI_SHOT_STYLE3D")) {
        QQuickItem *tabs = findItem(window->contentItem(), QStringLiteral("inspectorTabs"));
        QQuickItem *inspector = findItem(window->contentItem(), QStringLiteral("inspectorPanel"));
        if (!tabs || !inspector) { qInfo().noquote() << "ui-shot style3d: no inspector FAIL"; return 3; }
        if (backend.fontGate()) { backend.skipFontGate(); settle(500); }
        if (QObject *check = window->findChild<QObject *>(QStringLiteral("fontCheckDialog")); check && check->property("visible").toBool()) {
            QMetaObject::invokeMethod(check, "close");
            settle(800);
        }
        const QSize was = window->size();
        window->resize(was.width(), 3200);   // tall: a whole open group in one picture
        settle(400);
        const auto shoot = [&](const QString &name) {
            tabs->setProperty("currentIndex", 1);
            settle(150);
            QQuickItem *flick = nullptr;
            for (QQuickItem *up = findItem(inspector, QStringLiteral("styleGroupFill")); up; up = up->parentItem())
                if (up->inherits("QQuickFlickable")) { flick = up; break; }
            if (flick) {
                QQuickItem *first = nullptr;
                for (const char *g : {"styleGroupFill", "effectsPanel", "styleGroupText"})
                    if (QQuickItem *it = findItem(inspector, QString::fromLatin1(g)); it && it->isVisible()) { first = it; break; }
                if (first) {
                    const QPointF in = first->mapToItem(flick->property("contentItem").value<QQuickItem *>(), QPointF());
                    const qreal most = qMax(0.0, flick->property("contentHeight").toReal() - flick->height());
                    flick->setProperty("contentY", qBound(0.0, in.y() - 40, most));
                }
            }
            settle(900);
            const QImage whole = window->grabWindow();
            if (flick) {
                const QRectF r = flick->mapRectToScene(QRectF(0, 0, flick->width(), flick->height()));
                const qreal dpr = whole.devicePixelRatio();
                const QRect crop = QRect(int((r.left() - 8) * dpr), int(r.top() * dpr), int((r.width() + 16) * dpr), int(r.height() * dpr)).intersected(whole.rect());
                whole.copy(crop).save(outDir + QStringLiteral("/style3d-%1.png").arg(name));
            } else {
                whole.save(outDir + QStringLiteral("/style3d-%1.png").arg(name));
            }
            qInfo().noquote() << "ui-shot style3d: shot" << name;
        };
        const auto onSlide = [&](int slide) { backend.setProperty("currentSlide", slide); settle(300); return backend.slideObjects(); };
        // slideObjects names a text box by its words: the shape is the first
        // without words, the text box the one reading "Text Fill".
        const auto byName = [](const QVariantList &objects, const QString &words, const QString &type) {
            for (const auto &v : objects) {
                const auto m = v.toMap();
                if (type == QLatin1String("text") ? m.value("name").toString().startsWith(words) : !m.value("text").toBool()) return m.value("id").toString();
            }
            return QString();
        };
        const QStringList effectRows{"shadow", "reflection", "glow", "soft", "format", "rotation"};
        const QStringList galleryRows{"reflectionPresets", "softPresets", "formatPresets", "rotationPresets"};
        const auto rows = [](const QString &prefix, const QStringList &names) {
            QStringList out;
            for (const auto &n : names) out << prefix + QLatin1Char('.') + n;
            return out;
        };
        int shots = 0, failures = 0;
        const auto expect = [&](const QString &what, bool ok) {
            qInfo().noquote() << "ui-shot style3d: check" << what << (ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        const int slide = qMax(0, qEnvironmentVariableIntValue("OMASHOW_UI_SHOT_STYLE3D_SLIDE"));
        const auto objects = onSlide(slide);
        for (const auto &[kind, name, type] : {std::tuple{"shape", "", "rect"}, std::tuple{"textbox", "Text Fill", "text"}}) {
            const QString id = byName(objects, QString::fromLatin1(name), QString::fromLatin1(type));
            if (id.isEmpty()) { qInfo().noquote() << "ui-shot style3d: no" << kind << "FAIL"; ++failures; continue; }
            backend.select(id);
            settle(300);
            const QString k = QString::fromLatin1(kind);
            styleCloseAll(); shoot(k + QStringLiteral("-collapsed")); ++shots;
            styleOpen({QStringLiteral("group.fill"), QStringLiteral("shape.fill"), QStringLiteral("shape.line")}, true);
            shoot(k + QStringLiteral("-fill-line")); ++shots;
            styleCloseAll();
            styleOpen(QStringList{QStringLiteral("group.effects")} + rows(QStringLiteral("shape"), effectRows), true);
            shoot(k + QStringLiteral("-effects")); ++shots;
            styleCloseAll();
            styleOpen(QStringList{QStringLiteral("group.effects")} + rows(QStringLiteral("shape"), effectRows) + rows(QStringLiteral("shape"), galleryRows), true);
            shoot(k + QStringLiteral("-effects-galleries")); ++shots;
            if (k == QLatin1String("textbox")) {
                styleCloseAll();
                styleOpen(QStringList{QStringLiteral("group.text"), QStringLiteral("text.fill"), QStringLiteral("text.line")} + rows(QStringLiteral("text"), effectRows), true);
                shoot(k + QStringLiteral("-text-fill-outline")); ++shots;
            }
            styleCloseAll();
            const auto fx = backend.effectState(QStringLiteral("shape")), tfx = backend.effectState(QStringLiteral("text"));
            const auto tf = backend.textFillState();
            qInfo().noquote() << "ui-shot style3d:" << kind << "box: soft" << fx.value("softEdge").toDouble()
                              << "reflection" << fx.value("reflection").toMap().size() << "format" << fx.value("format3d").toMap().keys()
                              << "scene" << fx.value("scene3d").toMap().value("camera").toString()
                              << "| text: count" << tfx.value("count").toInt() << "reflection" << tfx.value("reflection").toMap().size()
                              << "format" << tfx.value("format3d").toMap().keys() << "fill" << tf.value("fillStyle").toInt() << "outline" << tf.value("strokeWidth").toDouble();
            // Each new setting through the backend: set, read back, undone.
            const auto roundTrip = [&](const QString &what, const std::function<void()> &set, const std::function<bool()> &holds) {
                const QString before = QString::fromUtf8(QJsonDocument::fromVariant(backend.slideObjects()).toJson(QJsonDocument::Compact));
                set(); settle(100);
                const bool took = holds();
                backend.undo(); settle(100);
                const QString after = QString::fromUtf8(QJsonDocument::fromVariant(backend.slideObjects()).toJson(QJsonDocument::Compact));
                expect(k + QLatin1Char(' ') + what, took && before == after);
            };
            const QString target = k == QLatin1String("shape") ? QStringLiteral("shape") : QStringLiteral("text");
            const QVariantMap refl = backend.style3dPresets().value("reflections").toList().value(4).toMap().value("value").toMap();
            roundTrip(QStringLiteral("reflection preset"), [&] { backend.setSelectedEffect(target, {{"reflection", refl}}); },
                      [&] { return backend.effectState(target).value("reflection").toMap().value("endPos").toDouble() == refl.value("endPos").toDouble(); });
            roundTrip(QStringLiteral("bevel"), [&] { backend.setSelectedEffect(target, {{"format3d", QVariantMap{{"top", QVariantMap{{"preset", "angle"}, {"w", 6}, {"h", 6}}}, {"depth", 10}, {"material", "metal"}}},
                                                                                {"scene3d", QVariantMap{{"camera", "orthographicFront"}, {"rig", "balanced"}, {"rigDir", "t"}}}}); },
                      [&] { return backend.effectState(target).value("format3d").toMap().value("material").toString() == QLatin1String("metal"); });
            roundTrip(QStringLiteral("3-D rotation"), [&] { backend.setSelectedEffect(target, {{"scene3d", QVariantMap{{"camera", "perspectiveFront"}, {"lat", 20}, {"lon", 30}, {"rev", 0}, {"rig", "threePt"}, {"rigDir", "t"}}}}); },
                      [&] { return backend.effectState(target).value("scene3d").toMap().value("lon").toDouble() == 30; });
            if (target == QLatin1String("shape"))
                roundTrip(QStringLiteral("soft edge"), [&] { backend.setSelectedEffect(target, {{"softEdge", 5}}); },
                          [&] { return backend.effectState(target).value("softEdge").toDouble() == 5; });
            else {
                roundTrip(QStringLiteral("text gradient fill"), [&] { backend.setSelectedTextFill({{"fillStyle", 1}, {"fillAngle", 90},
                                                                                                  {"gradientStops", QVariantList{QVariantMap{{"p", 0}, {"c", "#ffff0000"}}, QVariantMap{{"p", 1}, {"c", "#ff0000ff"}}}}}); },
                          [&] { return backend.textFillState().value("fillStyle").toInt() == 1; });
                roundTrip(QStringLiteral("text outline"), [&] { backend.setSelectedTextFill({{"strokeWidth", 2}, {"strokeColor", "#ff00aa00"}, {"strokeDash", "dash"}}); },
                          [&] { return backend.textFillState().value("strokeWidth").toDouble() == 2; });
                roundTrip(QStringLiteral("text pattern fill"), [&] { backend.setSelectedTextFill({{"fillStyle", 3}, {"patternPreset", "wave"}}); },
                          [&] { return backend.textFillState().value("patternPreset").toString() == QLatin1String("wave"); });
            }
        }
        window->resize(was);
        qInfo().noquote() << "ui-shot style3d: shots" << shots << "failures" << failures << (failures == 0 && shots == 9 ? "ok" : "FAIL");
        return failures == 0 && shots == 9 ? 0 : 3;
    }
    // ROT-SHADOW-1 (OMASHOW_UI_SHOT_ROT=1): the 3-D Rotation section's link to
    // the Shadow and Reflection rows, for a shape and a text box: the
    // explanation and Add buttons, each button pressed (the row opens and comes
    // into view), the Edit buttons, the rows' "Follows 3-D rotation" hints, and
    // the same with following turned off. Everything is undone after.
    if (qEnvironmentVariableIsSet("OMASHOW_UI_SHOT_ROT")) {
        QQuickItem *tabs = findItem(window->contentItem(), QStringLiteral("inspectorTabs"));
        QQuickItem *inspector = findItem(window->contentItem(), QStringLiteral("inspectorPanel"));
        if (!tabs || !inspector) { qInfo().noquote() << "ui-shot rot: no inspector FAIL"; return 3; }
        if (backend.fontGate()) { backend.skipFontGate(); settle(500); }
        if (QObject *check = window->findChild<QObject *>(QStringLiteral("fontCheckDialog")); check && check->property("visible").toBool()) {
            QMetaObject::invokeMethod(check, "close");
            settle(800);
        }
        tabs->setProperty("currentIndex", 1);
        settle(300);
        QQuickItem *flick = nullptr;
        for (QQuickItem *up = findItem(inspector, QStringLiteral("effectsPanel")); up; up = up->parentItem())
            if (up->inherits("QQuickFlickable")) { flick = up; break; }
        if (!flick) { qInfo().noquote() << "ui-shot rot: no scroll FAIL"; return 3; }
        const auto scrollTo = [&](QQuickItem *item, qreal above) {
            if (!item) return;
            const QPointF in = item->mapToItem(flick->property("contentItem").value<QQuickItem *>(), QPointF());
            const qreal most = qMax(0.0, flick->property("contentHeight").toReal() - flick->height());
            flick->setProperty("contentY", qBound(0.0, in.y() - above, most));
            settle(300);
        };
        int shots = 0, failures = 0;
        const auto shoot = [&](const QString &name) {
            settle(700);
            const QImage whole = window->grabWindow();
            const QRectF r = flick->mapRectToScene(QRectF(0, 0, flick->width(), flick->height()));
            const qreal dpr = whole.devicePixelRatio();
            const QRect crop = QRect(int((r.left() - 8) * dpr), int(r.top() * dpr), int((r.width() + 16) * dpr), int(r.height() * dpr)).intersected(whole.rect());
            whole.copy(crop).save(outDir + QStringLiteral("/rot-%1.png").arg(name));
            ++shots;
            qInfo().noquote() << "ui-shot rot: shot" << name;
        };
        const auto expect = [&](const QString &what, bool ok) {
            qInfo().noquote() << "ui-shot rot: check" << what << (ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        // Is the item on screen inside the scroll's view?
        const auto inView = [&](QQuickItem *item) {
            if (!item || !item->isVisible()) return false;
            const QRectF view = flick->mapRectToScene(QRectF(0, 0, flick->width(), flick->height()));
            const QRectF at = item->mapRectToScene(QRectF(0, 0, item->width(), qMin<qreal>(item->height(), 40)));
            return view.contains(at.topLeft()) && view.contains(at.bottomLeft());
        };
        const auto press = [&](QQuickItem *button) { if (button) QMetaObject::invokeMethod(button, "clicked"); settle(500); };
        backend.setProperty("currentSlide", qMax(0, qEnvironmentVariableIntValue("OMASHOW_UI_SHOT_ROT_SLIDE")));
        settle(300);
        const auto objects = backend.slideObjects();
        const auto pick = [&](bool text) {
            for (const auto &v : objects) {
                const auto m = v.toMap();
                if (text ? m.value("name").toString().startsWith(QLatin1String("Text Fill")) : !m.value("text").toBool()) return m.value("id").toString();
            }
            return QString();
        };
        for (const bool text : {false, true}) {
            const QString k = text ? QStringLiteral("textbox") : QStringLiteral("shape");
            const QString target = text ? QStringLiteral("text") : QStringLiteral("shape");
            const QString group = text ? QStringLiteral("styleGroupText") : QStringLiteral("effectsPanel");
            const QString id = pick(text);
            if (id.isEmpty()) { expect(k + QStringLiteral(" found"), false); continue; }
            backend.select(id);
            settle(300);
            tabs->setProperty("currentIndex", 1);   // the Style tab (a text box opens on another)
            settle(300);
            const QString before = QString::fromUtf8(QJsonDocument::fromVariant(backend.slideObjects()).toJson(QJsonDocument::Compact));
            // Turned, with no shadow or reflection yet.
            backend.setSelectedEffect(target, {{"scene3d", QVariantMap{{"camera", "perspectiveContrastingRightFacing"}, {"rig", "threePt"}, {"rigDir", "t"}}},
                                               {"shadowEnabled", false}, {"reflection", QVariantMap()}});
            settle(300);
            QQuickItem *set = findItem(inspector, group);
            const auto item = [&](const char *name) { return set ? findItem(set, QString::fromLatin1(name)) : nullptr; };
            const auto textOf = [&](const char *name) { QQuickItem *i = item(name); return i ? i->property("text").toString() : QString(); };
            styleCloseAll();
            styleOpen({text ? QStringLiteral("group.text") : QStringLiteral("group.effects"), target + QStringLiteral(".rotation")}, true);
            settle(300);
            scrollTo(item("effects3dRotationExpander"), 8);
            QQuickItem *shadowButton = item("effects3dShadowButton"), *reflButton = item("effects3dReflectionButton");
            expect(k + QStringLiteral(" Add buttons"), shadowButton && reflButton && shadowButton->property("text").toString() == QLatin1String("Add shadow") &&
                                                      reflButton->property("text").toString() == QLatin1String("Add reflection"));
            shoot(k + QStringLiteral("-1-rotation-add"));
            press(shadowButton);
            settle(300);
            auto fx = backend.effectState(target);
            expect(k + QStringLiteral(" Add shadow writes the Shadow settings (Perspective: Below)"),
                   fx.value("shadowEnabled").toBool() && qAbs(fx.value("shadowScaleY").toDouble() + 19) < 0.01 && fx.value("shadowAlign").toInt() == 7);
            expect(k + QStringLiteral(" shadow row opened and in view"), inView(item("effectsShadowSection")));
            expect(k + QStringLiteral(" shadow follow hint"), item("effectsShadowFollowNote") && item("effectsShadowFollowNote")->isVisible() &&
                                                             textOf("effectsShadowExpander").contains(QLatin1String("Follows 3-D rotation")));
            shoot(k + QStringLiteral("-2-shadow-revealed"));
            scrollTo(item("effects3dRotationExpander"), 8);
            expect(k + QStringLiteral(" Edit shadow"), textOf("effects3dShadowButton") == QLatin1String("Edit shadow"));
            press(reflButton);
            settle(300);
            fx = backend.effectState(target);
            expect(k + QStringLiteral(" Add reflection writes the Reflection settings (Half, touching)"),
                   qAbs(fx.value("reflection").toMap().value("endPos").toDouble() - 55) < 0.01);
            expect(k + QStringLiteral(" reflection row opened and in view"), inView(item("effectsReflectionSection")));
            shoot(k + QStringLiteral("-3-reflection-revealed"));
            scrollTo(item("effects3dRotationExpander"), 8);
            expect(k + QStringLiteral(" Edit reflection"), textOf("effects3dReflectionButton") == QLatin1String("Edit reflection"));
            shoot(k + QStringLiteral("-4-rotation-edit"));
            // The rows closed: their summaries carry the hint.
            styleCloseAll();
            styleOpen({text ? QStringLiteral("group.text") : QStringLiteral("group.effects")}, true);
            settle(300);
            scrollTo(item("effectsShadowExpander"), 60);
            expect(k + QStringLiteral(" reflection summary hint"), textOf("effectsReflectionExpander").contains(QLatin1String("Follows 3-D rotation")));
            shoot(k + QStringLiteral("-5-summaries"));
            // Following off: OmaShow's own setting, read back.
            QQuickItem *follow = item("effects3dFollow");
            styleOpen({target + QStringLiteral(".rotation")}, true);
            settle(300);
            if (follow) { follow->setProperty("checked", false); QMetaObject::invokeMethod(follow, "toggled"); }
            settle(400);
            expect(k + QStringLiteral(" follow off"), !backend.effectState(target).value("effects3dFollow").toBool() &&
                                                     textOf("effectsShadowExpander").contains(QLatin1String("Flat")));
            scrollTo(item("effectsShadowExpander"), 60);
            shoot(k + QStringLiteral("-6-follow-off"));
            for (int i = 0; i < 4; ++i) { backend.undo(); settle(100); }
            const QString after = QString::fromUtf8(QJsonDocument::fromVariant(backend.slideObjects()).toJson(QJsonDocument::Compact));
            expect(k + QStringLiteral(" all undone"), before == after);
        }
        styleCloseAll();
        const bool ok = failures == 0 && shots == 12;
        qInfo().noquote() << "ui-shot rot: shots" << shots << "failures" << failures << (ok ? "ok" : "FAIL");
        return ok ? 0 : 3;
    }
    // COLOR-PICK-2 (OMASHOW_UI_SHOT_COLORS=1): every colour setting, by hand. Each swatch is
    // clicked, the chooser photographed, a colour typed in its hex box (or a grid swatch
    // clicked) and "Use colour" clicked; what the deck now holds is read back.
    if (qEnvironmentVariableIsSet("OMASHOW_UI_SHOT_COLORS")) {
        int failures = 0, cases = 0;
        const auto centreOf = [](QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)); };
        const auto click = [&](QQuickItem *item) {
            const QPointF at = centreOf(item);
            mouse(QEvent::MouseMove, at, Qt::NoButton);
            mouse(QEvent::MouseButtonPress, at, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, at, Qt::NoButton);
        };
        const auto sameRgb = [](const QString &a, const QString &b) {
            const QColor x(a), y(b);
            return x.isValid() && y.isValid() && x.rgb() == y.rgb();
        };
        // The visible swatch whose field is named so; the inspectors' tabs and scroll are moved to it.
        const auto swatchFor = [&](const QString &name) -> QQuickItem * {
            const std::function<void(QQuickItem *, QList<QQuickItem *> &)> all = [&](QQuickItem *item, QList<QQuickItem *> &out) {
                if (item->objectName() == QLatin1String("colorSwatch")) out.append(item);
                for (QQuickItem *child : item->childItems()) all(child, out);
            };
            QList<QPair<QQuickItem *, int>> places{{nullptr, 0}};
            for (const char *bar : {"inspectorTabs", "designInspectorTabs"})
                if (QQuickItem *tabs = findItem(window->contentItem(), QString::fromLatin1(bar)); tabs && tabs->isVisible())
                    for (int tab = 0; tab < 4; ++tab) places.append({tabs, tab});
            for (const auto &[tabs, tab] : places) {
                if (tabs) { tabs->setProperty("currentIndex", tab); settle(150); }
                QList<QQuickItem *> found;
                all(window->contentItem(), found);
                for (QQuickItem *swatch : found) {
                    QQuickItem *field = swatch->parentItem();
                    if (!swatch->isVisible() || !field || field->property("accessibleName").toString() != name) continue;
                    bool shown = true;
                    for (QQuickItem *up = field; up; up = up->parentItem()) if (!up->isVisible()) shown = false;
                    if (!shown) continue;
                    // Scrolled into view, if it sits in a scrolling panel.
                    for (QQuickItem *up = field->parentItem(); up; up = up->parentItem())
                        if (up->inherits("QQuickFlickable")) {
                            const QPointF in = field->mapToItem(up->property("contentItem").value<QQuickItem *>(), QPointF());
                            up->setProperty("contentY", qMax(0.0, in.y() - 60));
                            settle(100);
                            break;
                        }
                    return swatch;
                }
            }
            return nullptr;
        };
        const auto pickerOf = [&](QQuickItem *swatch) -> QObject * {
            for (QObject *o : swatch->parentItem()->findChildren<QObject *>(QStringLiteral("colorPicker")))
                if (o->property("opened").toBool()) return o;
            return nullptr;
        };
        const auto inPicker = [&](QObject *picker, const QString &name) {
            auto *content = picker->property("contentItem").value<QQuickItem *>();
            for (QQuickItem *up = content; up && up->parentItem(); up = up->parentItem()) content = up;
            return content ? findItem(content, name) : nullptr;
        };
        // how: "hex" types the colour, "grid" clicks the first grid swatch, "cancel" cancels.
        const auto check = [&](const QString &label, const QString &field, const QString &want, const QString &how,
                               const std::function<QString()> &read, const std::function<bool()> &also = {}) {
            ++cases;
            QQuickItem *swatch = swatchFor(field);
            if (!swatch) { ++failures; qInfo().noquote() << "ui-shot colors:" << label << "- no visible swatch FAIL"; return; }
            const QString before = read();
            save(window, QStringLiteral("color-before-%1").arg(cases, 2, 10, QLatin1Char('0')));
            click(swatch);
            settle(250);
            QObject *picker = pickerOf(swatch);
            if (!picker) {
                ++failures;
                const QPointF at = centreOf(swatch);
                QQuickItem *hit = window->contentItem()->childAt(at.x(), at.y());
                int pickers = 0;
                for (QObject *o : window->findChildren<QObject *>(QStringLiteral("colorPicker"))) { ++pickers; Q_UNUSED(o); }
                save(window, QStringLiteral("color-fail-%1").arg(cases, 2, 10, QLatin1Char('0')));
                qInfo().noquote() << "ui-shot colors:" << label << "- click opened no chooser FAIL at" << at
                                  << "window" << window->size() << "top item" << (hit ? hit->objectName() : QString()) << "pickers" << pickers;
                return;
            }
            save(window, QStringLiteral("color-%1").arg(cases, 2, 10, QLatin1Char('0')));
            if (cases == 1)
                if (QQuickItem *pickerTabs = inPicker(picker, QStringLiteral("colorPickerTabs"))) {
                    pickerTabs->setProperty("currentIndex", 1);
                    save(window, QStringLiteral("color-wheel"));
                    pickerTabs->setProperty("currentIndex", 0);
                    settle(100);
                }
            QString expected = want;
            if (how == QLatin1String("grid")) {
                QQuickItem *grid = inPicker(picker, QStringLiteral("colorSwatches"));
                QQuickItem *first = nullptr;
                const std::function<void(QQuickItem *)> seek = [&](QQuickItem *item) {
                    if (first) return;
                    if (item != grid && item->property("swatch").isValid() && item->width() > 4
                        && item->property("swatch").value<QColor>().hsvSaturationF() > 0.3) { first = item; return; }
                    for (QQuickItem *c : item->childItems()) seek(c);
                };
                if (grid) seek(grid);
                if (first) { expected = first->property("swatch").value<QColor>().name(); click(first); }
            } else if (how == QLatin1String("hex")) {
                if (QQuickItem *hex = inPicker(picker, QStringLiteral("colorPickerHex"))) {
                    hex->setProperty("text", want);
                    QMetaObject::invokeMethod(hex, "textEdited");
                }
            }
            settle(100);
            QQuickItem *button = inPicker(picker, how == QLatin1String("cancel") ? QStringLiteral("colorPickerCancel") : QStringLiteral("colorPickerUse"));
            if (button) click(button);
            settle(300);
            const QString after = read();
            const bool closed = !pickerOf(swatch);
            bool ok = closed && (how == QLatin1String("cancel") ? after == before : sameRgb(after, expected));
            const bool extra = !also || also();
            if (!ok || !extra) ++failures;
            qInfo().noquote() << QStringLiteral("ui-shot colors: %1: %2 -> %3 (want %4)%5 %6")
                                     .arg(label, before, after, how == QLatin1String("cancel") ? before : expected,
                                          extra ? QString() : QStringLiteral(" [side check failed]"),
                                          ok && extra ? QStringLiteral("ok") : QStringLiteral("FAIL"));
        };
        const auto selection = [&](const char *key) { return backend.property("selection").toMap().value(QLatin1String(key)).toString(); };
        QQuickItem *editCanvas = findItem(window->contentItem(), QStringLiteral("editCanvas"));
        backend.setProperty("currentSlide", 0);

        // Slide background, nothing selected.
        backend.select(QString());
        settle(200);
        check(QStringLiteral("slide background"), QStringLiteral("Slide background colour"), QStringLiteral("#203040"), QStringLiteral("hex"),
              [&] { return backend.property("slideDesign").toMap().value("background").toString(); });

        // A text box: the whole box, then selected words, then cancel.
        backend.addText();
        settle(300);
        const QString textId = backend.selectedId();
        check(QStringLiteral("text box colour"), QStringLiteral("Text colour"), QStringLiteral("#c03030"), QStringLiteral("hex"),
              [&] { return selection("textColor"); });
        check(QStringLiteral("text box colour, cancelled"), QStringLiteral("Text colour"), QString(), QStringLiteral("cancel"),
              [&] { return selection("textColor"); });
        if (editCanvas) {
            QMetaObject::invokeMethod(editCanvas, "beginTextEdit");
            settle(200);
            if (QQuickItem *editor = findItem(window->contentItem(), QStringLiteral("inlineTextEditor")))
                QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 4));
            settle(200);
            const QString boxBefore = selection("textColor");
            check(QStringLiteral("selected words' colour"), QStringLiteral("Text colour"), QStringLiteral("#2060c0"), QStringLiteral("grid"),
                  // The selected letters' own colour, as the deck holds it.
                  [&] { return backend.textSelection().value("color").toString(); },
                  [&] { return sameRgb(selection("textColor"), boxBefore); });
            // The caret: the colour waits for the next letter typed.
            QQuickItem *editor = findItem(window->contentItem(), QStringLiteral("inlineTextEditor"));
            const int end = editor ? editor->property("length").toInt() : 0;
            if (editor) QMetaObject::invokeMethod(editor, "select", Q_ARG(int, end), Q_ARG(int, end));
            settle(200);
            // The colour the deck holds for the letter after the caret's place.
            const auto lastRun = [&] {
                backend.setTextSelection(end, end + 1);
                const QString colour = backend.textSelection().value("color").toString();
                backend.setTextSelection(0, 0);
                return colour;
            };
            check(QStringLiteral("caret colour"), QStringLiteral("Text colour"), QStringLiteral("#20a0a0"), QStringLiteral("hex"),
                  [&] { return backend.textSelection().value("color").toString(); },
                  [&] {
                      // Typed after the choice, the letter carries it into the deck.
                      if (!editor || !editor->hasActiveFocus()) return false;
                      QKeyEvent press(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
                      QKeyEvent release(QEvent::KeyRelease, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
                      QCoreApplication::sendEvent(window, &press);
                      QCoreApplication::sendEvent(window, &release);
                      settle(200);
                      QMetaObject::invokeMethod(editCanvas, "commitTextEdit");
                      settle(200);
                      return sameRgb(lastRun(), QStringLiteral("#20a0a0")) && !sameRgb(selection("textColor"), QStringLiteral("#20a0a0"));
                  });
            QMetaObject::invokeMethod(editCanvas, "commitTextEdit");
            settle(200);
        }

        // A shape: fill, second colour (gradient), outline, shadow.
        backend.addRect();
        settle(300);
        // FILL-UI-1: a shape's colours sit in the Style tab's Fill and Line, folded away.
        styleOpen({QStringLiteral("group.fill"), QStringLiteral("shape.fill"), QStringLiteral("shape.line")}, true);
        settle(200);
        check(QStringLiteral("shape fill"), QStringLiteral("Fill colour"), QStringLiteral("#30a060"), QStringLiteral("hex"), [&] { return selection("fill"); });
        backend.setSelectedProperty(QStringLiteral("fillStyle"), 3);   // FILL-UI-1: a pattern's background
        settle(200);
        check(QStringLiteral("shape second fill colour"), QStringLiteral("Second fill colour"), QStringLiteral("#f0c020"), QStringLiteral("hex"), [&] { return selection("fillSecondary"); });
        backend.setSelectedProperty(QStringLiteral("strokeWidth"), 2);   // FILL-UI-1: the line's colour shows once it has one
        settle(200);
        check(QStringLiteral("shape outline"), QStringLiteral("Stroke colour"), QStringLiteral("#602080"), QStringLiteral("hex"), [&] { return selection("strokeColor"); });
        backend.setSelectedProperty(QStringLiteral("shadowEnabled"), true);
        // EFFECTS-UI-1: the shadow's colour sits in the Style tab's Effects, folded away.
        styleOpen({QStringLiteral("group.effects"), QStringLiteral("shape.shadow")}, true);
        settle(200);
        check(QStringLiteral("shape shadow"), QStringLiteral("Shadow colour"), QStringLiteral("#404040"), QStringLiteral("hex"), [&] { return selection("shadowColor"); });
        const QString shapeA = backend.selectedId();

        // A connector between two shapes.
        backend.addRect();
        settle(200);
        backend.setSelectedProperty(QStringLiteral("x"), 1200);
        const QString shapeB = backend.selectedId();
        backend.selectIds(QStringList{shapeA, shapeB});
        settle(200);
        if (backend.connectSelected()) {
            settle(300);
            check(QStringLiteral("connector"), QStringLiteral("Connector colour"), QStringLiteral("#e06010"), QStringLiteral("hex"), [&] { return selection("strokeColor"); });
        } else { ++failures; ++cases; qInfo() << "ui-shot colors: connector - could not connect FAIL"; }

        // A picture's tint.
        {
            QImage dot(64, 64, QImage::Format_RGB32);
            dot.fill(QColor(200, 200, 200));
            const QString png = outDir + QStringLiteral("/tint-source.png");
            dot.save(png);
            if (backend.insertImage(QUrl::fromLocalFile(png))) {
                settle(400);
                check(QStringLiteral("picture tint"), QStringLiteral("Tint colour"), QStringLiteral("#806040"), QStringLiteral("hex"), [&] { return selection("imageTint"); });
            } else { ++failures; ++cases; qInfo() << "ui-shot colors: picture - not inserted FAIL"; }
        }

        // A chart's first series.
        if (backend.addChart(0)) {
            settle(400);
            const auto series = [&] {
                const QVariantList list = backend.property("selection").toMap().value("chartSeries").toList();
                return list.isEmpty() ? QString() : list.first().toMap().value("color").toString();
            };
            check(QStringLiteral("chart series"), QStringLiteral("Series colour"), QStringLiteral("#a02060"), QStringLiteral("hex"), series);
        }

        // A table cell: text, fill, border (the table editor).
        if (backend.addTable(3, 3)) {
            settle(300);
            QObject *editor = nullptr;
            for (QObject *o : window->findChildren<QObject *>(QStringLiteral("tableEditor"))) editor = o;
            if (editor) QMetaObject::invokeMethod(editor, "show");
            settle(400);
            const auto cell = [&](const char *key) {
                return std::function<QString()>([&backend, key] {
                    return backend.tableModel()->property("selection").toMap().value(QLatin1String(key)).toString();
                });
            };
            check(QStringLiteral("table cell text"), QStringLiteral("Cell text colour"), QStringLiteral("#103070"), QStringLiteral("hex"), cell("textColor"));
            check(QStringLiteral("table cell fill"), QStringLiteral("Cell fill colour"), QStringLiteral("#e0f0ff"), QStringLiteral("hex"), cell("fill"));
            check(QStringLiteral("table cell border"), QStringLiteral("Cell border colour"), QStringLiteral("#505050"), QStringLiteral("hex"), cell("borderColor"));
            if (editor) QMetaObject::invokeMethod(editor, "close");
            settle(200);
        }

        // Masters and layouts: master background, placeholder colour, theme colours.
        backend.select(QString());
        window->setProperty("workspace", 1);
        settle(500);
        const auto design = [&] { return backend.property("design").toMap(); };
        check(QStringLiteral("master background"), QStringLiteral("Master background colour"), QStringLiteral("#182028"), QStringLiteral("hex"),
              [&] { const QVariantList m = design().value("masters").toList(); return m.isEmpty() ? QString() : m.first().toMap().value("background").toString(); });
        for (const QString &name : {QStringLiteral("Placeholder text colour"), QStringLiteral("Placeholder fill colour")})
            if (swatchFor(name))
                check(QStringLiteral("layout placeholder"), name, QStringLiteral("#d0a000"), QStringLiteral("hex"), [&, name] {
                    QObject *ws = nullptr;
                    for (QObject *o : window->findChildren<QObject *>(QStringLiteral("designWorkspace"))) ws = o;
                    const QVariantMap p = ws ? ws->property("placeholder").toMap() : QVariantMap();
                    return p.value(name.contains(QLatin1String("text")) ? "textColor" : "fill").toString();
                });
        for (const QString &token : {QStringLiteral("background"), QStringLiteral("foreground"), QStringLiteral("muted"), QStringLiteral("accent")})
            check(QStringLiteral("theme colour %1").arg(token), QStringLiteral("Theme colour %1").arg(token),
                  token == QLatin1String("accent") ? QString() : QStringLiteral("#%1").arg(QString::number(0x305070 + token.size() * 0x111111, 16)),
                  token == QLatin1String("accent") ? QStringLiteral("grid") : QStringLiteral("hex"),
                  [&, token] { return design().value("colors").toMap().value(token).toString(); });
        window->setProperty("workspace", 0);
        settle(200);
        qInfo().noquote() << QStringLiteral("ui-shot colors: %1 of %2 cases failed").arg(failures).arg(cases);
        return failures ? 3 : 0;
    }
    // FX-STATE (OMASHOW_UI_SHOT_FXSTATE=1, OMASHOW_UI_SHOT_FXSTATE_SLIDE=n,
    // OMASHOW_UI_SHOT_FXSTATE_IDS=id,id…): what an effect control shows against what
    // is drawn, on turned objects. For each object, turned in 3-D from the box's
    // or the words' Effects (as by hand), a glow (a Glow variation), a shadow
    // ("Add shadow") and a reflection ("Add reflection") are added from each
    // Effects set in turn. The canvas is grabbed before and after; the set's own
    // row, its closed row's line and its group's line must say the effect is on
    // exactly when something was drawn, and every other set must not claim it.
    if (qEnvironmentVariableIsSet("OMASHOW_UI_SHOT_FXSTATE")) {
        QQuickItem *tabs = findItem(window->contentItem(), QStringLiteral("inspectorTabs"));
        QQuickItem *inspector = findItem(window->contentItem(), QStringLiteral("inspectorPanel"));
        QQuickItem *canvas = findItem(window->contentItem(), QStringLiteral("editSlideView"));
        if (!tabs || !inspector || !canvas) { qInfo().noquote() << "ui-shot fxstate: parts missing FAIL"; return 3; }
        if (backend.fontGate()) { backend.skipFontGate(); settle(500); }
        if (QObject *check = window->findChild<QObject *>(QStringLiteral("fontCheckDialog")); check && check->property("visible").toBool()) {
            QMetaObject::invokeMethod(check, "close");
            settle(800);
        }
        backend.setProperty("currentSlide", qMax(0, qEnvironmentVariableIntValue("OMASHOW_UI_SHOT_FXSTATE_SLIDE")));
        settle(500);
        const auto grab = [&]() {
            settle(600);
            const QImage whole = window->grabWindow();
            const QRectF r = canvas->mapRectToScene(QRectF(0, 0, canvas->width(), canvas->height()));
            const qreal dpr = whole.devicePixelRatio();
            return whole.copy(QRect(int(r.left() * dpr), int(r.top() * dpr), int(r.width() * dpr), int(r.height() * dpr)).intersected(whole.rect()))
                .convertToFormat(QImage::Format_RGB32);
        };
        const auto changed = [](const QImage &a, const QImage &b) {   // pixels moved more than 12 grey levels
            if (a.size() != b.size() || a.isNull()) return -1;
            int n = 0;
            for (int y = 0; y < a.height(); ++y) {
                const QRgb *p = reinterpret_cast<const QRgb *>(a.constScanLine(y)), *q = reinterpret_cast<const QRgb *>(b.constScanLine(y));
                for (int x = 0; x < a.width(); ++x) n += qAbs(qGray(p[x]) - qGray(q[x])) > 4;
            }
            return n;
        };
        const QVariantMap scene{{"camera", QStringLiteral("perspectiveContrastingRightFacing")}, {"rig", "threePt"}, {"rigDir", "t"}};
        const QVariantMap none{{"scene3d", QVariantMap()}, {"glowRadius", 0}, {"shadowEnabled", false}, {"reflection", QVariantMap()}, {"effects3dFollow", true}};
        struct Effect { QString name, on, row, button; };
        const QList<Effect> effects{{"glow", "glowOn", "effectsGlowExpander", QString()},
                                    {"shadow", "shadowOn", "effectsShadowExpander", "effects3dShadowButton"},
                                    {"reflection", "reflectionOn", "effectsReflectionExpander", "effects3dReflectionButton"}};
        const QStringList sets{QStringLiteral("shape"), QStringLiteral("text")};
        const auto setItem = [&](const QString &target) {
            return findItem(inspector, target == QLatin1String("text") ? QStringLiteral("effectsSetText") : QStringLiteral("effectsSetBox"));
        };
        const auto group = [&](const QString &target) {
            QQuickItem *g = findItem(inspector, target == QLatin1String("text") ? QStringLiteral("styleGroupText") : QStringLiteral("effectsPanel"));
            return g && g->isVisible() ? g->property("summary").toString() : QString();
        };
        int n = 0, failures = 0;
        for (const QString &id : qEnvironmentVariable("OMASHOW_UI_SHOT_FXSTATE_IDS").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            for (const QString &turnVia : sets)
                for (const QString &via : sets)
                    for (const Effect &e : effects) {
                        backend.select(id);
                        settle(200);
                        tabs->setProperty("currentIndex", 1);
                        QQuickItem *set = setItem(via);
                        if (!set || !set->property("available").toBool()) continue;   // no such set for this object
                        for (const QString &t : sets) backend.setSelectedEffect(t, t == QLatin1String("text") ? QVariantMap{{"glowRadius", 0}, {"shadowEnabled", false}, {"reflection", QVariantMap()}, {"scene3d", QVariantMap()}} : none);
                        backend.setSelectedEffect(turnVia, {{"scene3d", scene}});
                        const QImage before = grab();
                        if (e.button.isEmpty()) {
                            const qreal u = backend.property("ptPerUnit").toDouble() > 0 ? 1 / backend.property("ptPerUnit").toDouble() : 2;
                            const QVariant glow = QVariantMap{{"glowRadius", 11 * u}, {"glowColor", QStringLiteral("#66e97132")}};
                            QMetaObject::invokeMethod(set, "set", Q_ARG(QVariant, glow));
                        } else if (QQuickItem *b = findItem(set, e.button)) {
                            QMetaObject::invokeMethod(b, "clicked");
                        }
                        const QImage after = grab();
                        const int drawn = changed(before, after);
                        QQuickItem *row = findItem(set, e.row);
                        const bool on = set->property(e.on.toUtf8()).toBool();
                        const QString rowLine = row ? row->property("summary").toString() : QStringLiteral("?");
                        const QString setLine = set->property("summary").toString(), groupLine = group(via);
                        const QString word = e.name == QLatin1String("glow") ? QStringLiteral("Glow") : e.name == QLatin1String("shadow") ? QStringLiteral("Shadow") : QStringLiteral("Reflection");
                        // Every other set: claims it only if it is that set's own.
                        QStringList others;
                        for (const QString &t : sets)
                            if (t != via) if (QQuickItem *o = setItem(t); o && o->property("available").toBool() && o->property(e.on.toUtf8()).toBool()) others << t;
                        const bool shown = on && rowLine != QLatin1String("None") && setLine.contains(word);
                        // A box with no fill or outline draws none of its own (PowerPoint's
                        // way, measured): the set says so in its note instead.
                        const bool explained = via == QLatin1String("shape") && set->property("onWords").toBool() && drawn <= 150;
                        // Given to the words, a text box's own Effects row must not say just
                        // None: the words' effect draws on the box (the user's case: the
                        // glow test deck's words glowing and turning, the box's Glow None).
                        QString boxRow;
                        if (via == QLatin1String("text")) if (QQuickItem *box = setItem(QStringLiteral("shape")); box && box->property("available").toBool())
                            if (QQuickItem *r = findItem(box, e.row)) boxRow = r->property("summary").toString();
                        const bool ok = ((drawn > 150) == shown || explained) && others.isEmpty() && (drawn <= 150 || boxRow != QLatin1String("None"));
                        if (!ok) ++failures;
                        if (qEnvironmentVariableIsSet("OMASHOW_UI_SHOT_FXSTATE_SAVE"))
                            after.save(outDir + QStringLiteral("/fxstate-%1-%2-%3-%4.png").arg(n).arg(turnVia, via, e.name));
                        qInfo().noquote() << "ui-shot fxstate:" << id << "turned via" << turnVia << e.name << "via" << via << "drawn px" << drawn
                                          << "| set on" << on << "row" << rowLine << "| set line" << setLine << "| group line" << groupLine
                                          << "| others claim" << others.join(QLatin1Char('+')) << "| box row" << boxRow << (ok ? "ok" : "FAIL");
                        for (const QString &t : sets) backend.setSelectedEffect(t, t == QLatin1String("text") ? QVariantMap{{"glowRadius", 0}, {"shadowEnabled", false}, {"reflection", QVariantMap()}, {"scene3d", QVariantMap()}} : none);
                    }
            // The words' glow given to all of the text while editing (it goes on
            // the stretch), then the box selected again: the Text effects must show
            // the glow that is drawn, and turning it off there must take it away.
            if (QQuickItem *words = setItem(QStringLiteral("text")); words) {
                backend.select(id);
                settle(200);
                if (words->property("available").toBool()) {
                    backend.setSelectedEffect(QStringLiteral("text"), {{"glowRadius", 0}, {"scene3d", scene}});
                    const QImage before = grab();
                    const int length = backend.selection().value(QStringLiteral("text")).toString().size();
                    backend.setTextSelection(0, length);
                    const qreal u = backend.property("ptPerUnit").toDouble() > 0 ? 1 / backend.property("ptPerUnit").toDouble() : 2;
                    const QVariant glow = QVariantMap{{"glowRadius", 11 * u}, {"glowColor", QStringLiteral("#66e97132")}};
                    QMetaObject::invokeMethod(words, "set", Q_ARG(QVariant, glow));
                    backend.setTextSelection(0, 0);
                    const QImage on = grab();
                    QQuickItem *row = findItem(words, QStringLiteral("effectsGlowExpander"));
                    const bool shown = words->property("glowOn").toBool() && row && row->property("summary").toString() != QLatin1String("None");
                    const QVariant off = QVariantMap{{"glowRadius", 0}};
                    QMetaObject::invokeMethod(words, "set", Q_ARG(QVariant, off));
                    const QImage gone = grab();
                    const int drawn = changed(before, on), left = changed(before, gone);
                    const bool ok = length > 0 && drawn > 150 && shown && left <= 150 && !words->property("glowOn").toBool();
                    if (!ok) ++failures;
                    qInfo().noquote() << "ui-shot fxstate:" << id << "words' glow on the whole text: drawn px" << drawn << "shown" << shown
                                      << "row" << (row ? row->property("summary").toString() : QStringLiteral("?")) << "| after No glow px" << left << (ok ? "ok" : "FAIL");
                    backend.setSelectedEffect(QStringLiteral("text"), {{"glowRadius", 0}, {"scene3d", QVariantMap()}});
                }
            }
            ++n;
        }
        qInfo().noquote() << "ui-shot fxstate: objects" << n << "failures" << failures;
        return failures ? 4 : 0;
    }
    // FX-FOLLOW (OMASHOW_UI_SHOT_FXFOLLOW=1, OMASHOW_UI_SHOT_FXFOLLOW_SLIDE=n,
    // OMASHOW_UI_SHOT_FXFOLLOW_IDS=id,id…): for each object, as by hand: turned
    // in 3-D, the 3-D Rotation section's "Add shadow" pressed, then "Shadow and
    // reflection follow 3-D rotation" turned off. The slide canvas is grabbed at
    // each step; the pixels the shadow changed are reported. Drawn flat, it must
    // be drawn following the turn too, about as much (FX-FOLLOW, A1).
    if (qEnvironmentVariableIsSet("OMASHOW_UI_SHOT_FXFOLLOW")) {
        QQuickItem *tabs = findItem(window->contentItem(), QStringLiteral("inspectorTabs"));
        QQuickItem *inspector = findItem(window->contentItem(), QStringLiteral("inspectorPanel"));
        QQuickItem *canvas = findItem(window->contentItem(), QStringLiteral("editSlideView"));
        if (!tabs || !inspector || !canvas) { qInfo().noquote() << "ui-shot fxfollow: parts missing FAIL"; return 3; }
        if (backend.fontGate()) { backend.skipFontGate(); settle(500); }
        if (QObject *check = window->findChild<QObject *>(QStringLiteral("fontCheckDialog")); check && check->property("visible").toBool()) {
            QMetaObject::invokeMethod(check, "close");
            settle(800);
        }
        backend.setProperty("currentSlide", qMax(0, qEnvironmentVariableIntValue("OMASHOW_UI_SHOT_FXFOLLOW_SLIDE")));
        settle(500);
        QObject *opener = findItem(inspector, QStringLiteral("styleOpen"));
        if (!opener) opener = inspector->findChild<QObject *>(QStringLiteral("styleOpen"));
        const auto open = [&](const QStringList &keys) { if (opener) for (const auto &k : keys) QMetaObject::invokeMethod(opener, "set", Q_ARG(QVariant, k), Q_ARG(QVariant, true)); settle(300); };
        const auto grab = [&]() {
            settle(700);
            const QImage whole = window->grabWindow();
            const QRectF r = canvas->mapRectToScene(QRectF(0, 0, canvas->width(), canvas->height()));
            const qreal dpr = whole.devicePixelRatio();
            return whole.copy(QRect(int(r.left() * dpr), int(r.top() * dpr), int(r.width() * dpr), int(r.height() * dpr)).intersected(whole.rect()))
                .convertToFormat(QImage::Format_RGB32);
        };
        const auto change = [](const QImage &a, const QImage &b) {   // pixels moved more than 4 grey levels
            if (a.size() != b.size() || a.isNull()) return -1;
            int n = 0;
            for (int y = 0; y < a.height(); ++y) {
                const QRgb *p = reinterpret_cast<const QRgb *>(a.constScanLine(y)), *q = reinterpret_cast<const QRgb *>(b.constScanLine(y));
                for (int x = 0; x < a.width(); ++x) n += qAbs(qGray(p[x]) - qGray(q[x])) > 4;
            }
            return n;
        };
        const QString camera = qEnvironmentVariable("OMASHOW_UI_SHOT_FXFOLLOW_CAMERA", QStringLiteral("perspectiveContrastingRightFacing"));
        int n = 0, failures = 0;
        for (const QString &id : qEnvironmentVariable("OMASHOW_UI_SHOT_FXFOLLOW_IDS").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            backend.select(id);
            settle(300);
            tabs->setProperty("currentIndex", 1);
            settle(300);
            const bool words = backend.effectState(QStringLiteral("text")).value("onWords").isValid() && !backend.effectState(QStringLiteral("shape")).value("scene3d").isValid();
            Q_UNUSED(words);
            // Which panel has the turn: a text box's words or the shape.
            const QString target = qEnvironmentVariable("OMASHOW_UI_SHOT_FXFOLLOW_TARGET", QStringLiteral("shape"));
            const QString group = target == QLatin1String("text") ? QStringLiteral("styleGroupText") : QStringLiteral("effectsPanel");
            backend.setSelectedEffect(target, {{"scene3d", QVariantMap{{"camera", camera}, {"rig", "threePt"}, {"rigDir", "t"}}},
                                               {"shadowEnabled", false}, {"effects3dFollow", true}});
            open({target == QLatin1String("text") ? QStringLiteral("group.text") : QStringLiteral("group.effects"), target + QStringLiteral(".rotation")});
            QQuickItem *set = findItem(inspector, group);
            QQuickItem *button = set ? findItem(set, QStringLiteral("effects3dShadowButton")) : nullptr;
            QQuickItem *follow = set ? findItem(set, QStringLiteral("effects3dFollow")) : nullptr;
            const QImage turned = grab();
            turned.save(outDir + QStringLiteral("/fxfollow-%1-a-turned.png").arg(n));
            if (button) { QMetaObject::invokeMethod(button, "clicked"); }
            const QImage on = grab();
            on.save(outDir + QStringLiteral("/fxfollow-%1-b-shadow-follow.png").arg(n));
            const auto fx = backend.effectState(target);
            if (follow) { follow->setProperty("checked", false); QMetaObject::invokeMethod(follow, "toggled"); }
            const QImage off = grab();
            off.save(outDir + QStringLiteral("/fxfollow-%1-c-shadow-flat.png").arg(n));
            // The same, flat, without the shadow: what the shadow adds flat (its
            // glow, say, goes flat too, and is not the shadow's).
            backend.setSelectedEffect(target, {{"shadowEnabled", false}});
            const QImage flatNone = grab();
            backend.undo();
            settle(100);
            const int dOn = change(turned, on), dOff = change(flatNone, off);
            // Not this object's set (a shape has no Text effects): nothing to check.
            if (!fx.value("count").toInt()) { qInfo().noquote() << "ui-shot fxfollow:" << n << id << "target" << target << "not applicable"; for (int i = 0; i < 3; ++i) { backend.undo(); settle(100); } ++n; continue; }
            // Following the turn may change where the shadow falls, never whether
            // it shows: drawn flat, it is drawn turned too, about as much.
            const bool ok = button && follow && fx.value("shadowEnabled").toBool() && (dOff <= 150 ? true : dOn > 150 && dOn * 10 >= dOff * 3);
            if (!ok) ++failures;
            qInfo().noquote() << "ui-shot fxfollow:" << n << id << "target" << target << "button" << bool(button) << "follow box" << bool(follow)
                              << "shadow set" << fx.value("shadowEnabled").toBool() << "change follow-on" << dOn << "follow-off" << dOff << (ok ? "ok" : "FAIL");
            for (int i = 0; i < 3; ++i) { backend.undo(); settle(100); }
            ++n;
        }
        qInfo().noquote() << "ui-shot fxfollow: objects" << n << "failures" << failures;
        return failures ? 4 : 0;
    }
    // SIDEBAR-CHECK (OMASHOW_UI_SHOT_SIDEBAR=first|second|third, one throwaway HOME
    // for all three launches): the sidebar's hide/show arrow and grip driven with
    // real mouse events, then the width lock and the width kept for the next launch.
    //   first:  opens shown at the standard width; arrow hides, arrow shows; a drag
    //           widens it; locked; a drag no longer changes it; left hidden at exit.
    //   second: shown again at launch, at the locked width; unlocked; a drag; exit.
    //   third:  unlocked, so back at the standard width.
    if (qEnvironmentVariableIsSet("OMASHOW_UI_SHOT_SIDEBAR")) {
        const QString phase = qEnvironmentVariable("OMASHOW_UI_SHOT_SIDEBAR");
        QObject *win = window;
        if (backend.fontGate()) { backend.skipFontGate(); settle(500); }
        if (QObject *check = window->findChild<QObject *>(QStringLiteral("fontCheckDialog")); check && check->property("visible").toBool()) {
            QMetaObject::invokeMethod(check, "close");
            settle(800);
        }
        settle(800);
        QQuickItem *panel = findItem(window->contentItem(), QStringLiteral("inspectorPanel"));
        QQuickItem *grip = findItem(window->contentItem(), QStringLiteral("inspectorGrip"));
        QQuickItem *arrow = grip ? findItem(grip, QStringLiteral("sidebarToggleButton")) : nullptr;
        if (!panel || !grip || !arrow) { qInfo().noquote() << "ui-shot sidebar: parts missing FAIL" << bool(panel) << bool(grip) << bool(arrow); return 3; }
        int failures = 0;
        const auto expect = [&](const QString &what, bool ok) {
            qInfo().noquote() << "ui-shot sidebar:" << phase << "check" << what << (ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        const auto state = [&](const QString &when) {
            QStringList kept;
            const auto ps = backend.panelState();
            for (auto it = ps.cbegin(); it != ps.cend(); ++it) kept << it.key() + QLatin1Char('=') + it.value().toString();
            qInfo().noquote() << "ui-shot sidebar:" << phase << when << "collapsed" << win->property("inspectorCollapsed").toBool()
                              << "fixed" << win->property("inspectorFixed").toBool() << "width" << win->property("inspectorWidth").toReal()
                              << "panel shown" << panel->isVisible() << "panel width" << panel->width() << "saved" << kept.join(QLatin1Char(' '));
        };
        const auto shoot = [&](const QString &name) { settle(500); window->grabWindow().save(outDir + QStringLiteral("/sidebar-%1.png").arg(name)); qInfo().noquote() << "ui-shot sidebar: shot" << name; };
        ulong stamp = 1000;
        const auto mouse = [&](QEvent::Type type, QPointF at, Qt::MouseButtons held) {
            QMouseEvent ev(type, at, window->mapToGlobal(at.toPoint()), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held, Qt::NoModifier);
            ev.setTimestamp(stamp += 16);
            QCoreApplication::sendEvent(window, &ev);
        };
        const auto click = [&](QQuickItem *item) {
            const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
            mouse(QEvent::MouseButtonPress, at, Qt::LeftButton);
            settle(60);
            mouse(QEvent::MouseButtonRelease, at, Qt::NoButton);
            settle(600);
        };
        // Grab the grip away from its arrow and pull it toward the canvas (left) by `by`.
        const auto drag = [&](qreal by) {
            const QPointF from = grip->mapToScene(QPointF(grip->width() / 2, qMin(grip->height() - 20, arrow->y() + arrow->height() + 120)));
            mouse(QEvent::MouseButtonPress, from, Qt::LeftButton);
            settle(40);
            for (int step = 1; step <= 10; ++step) { mouse(QEvent::MouseMove, from - QPointF(by * step / 10, 0), Qt::LeftButton); settle(30); }
            mouse(QEvent::MouseButtonRelease, from - QPointF(by, 0), Qt::NoButton);
            settle(600);
        };
        const qreal standard = 380;
        const qreal near = 3;
        state(QStringLiteral("launch"));
        if (phase == QLatin1String("first")) {
            expect(QStringLiteral("shown at launch"), panel->isVisible() && !win->property("inspectorCollapsed").toBool());
            expect(QStringLiteral("standard width 380"), qAbs(win->property("inspectorWidth").toReal() - standard) < near);
            shoot(QStringLiteral("1-launch"));
            click(arrow);
            state(QStringLiteral("after hide arrow"));
            expect(QStringLiteral("hide arrow hides it"), !panel->isVisible() && win->property("inspectorCollapsed").toBool());
            shoot(QStringLiteral("2-hidden"));
            click(arrow);
            state(QStringLiteral("after show arrow"));
            expect(QStringLiteral("show arrow brings it back at the same width"), panel->isVisible() && qAbs(panel->width() - standard) < near);
            shoot(QStringLiteral("3-shown"));
            drag(90);
            state(QStringLiteral("after drag 90"));
            const qreal dragged = win->property("inspectorWidth").toReal();
            expect(QStringLiteral("drag widens it"), dragged > standard + 60);
            win->setProperty("inspectorFixed", true);   // what View > Lock sidebar width runs
            settle(400);
            state(QStringLiteral("after lock"));
            drag(60);
            state(QStringLiteral("after drag while locked"));
            expect(QStringLiteral("locked width does not move"), qAbs(win->property("inspectorWidth").toReal() - dragged) < near);
            shoot(QStringLiteral("4-locked"));
            click(arrow);
            state(QStringLiteral("left hidden at exit"));
        } else if (phase == QLatin1String("second")) {
            const qreal kept = backend.panelState().value(QStringLiteral("inspectorWidth")).toReal();
            expect(QStringLiteral("shown at launch though left hidden"), panel->isVisible() && !win->property("inspectorCollapsed").toBool());
            expect(QStringLiteral("locked, at the kept width"), win->property("inspectorFixed").toBool() && kept > standard + 60
                                                               && qAbs(win->property("inspectorWidth").toReal() - kept) < near && qAbs(panel->width() - kept) < near);
            shoot(QStringLiteral("5-relaunch-locked"));
            win->setProperty("inspectorFixed", false);
            settle(400);
            drag(40);
            state(QStringLiteral("after unlock and drag 40"));
        } else {
            expect(QStringLiteral("unlocked: back at the standard width"), !win->property("inspectorFixed").toBool()
                                                                          && qAbs(win->property("inspectorWidth").toReal() - standard) < near && panel->isVisible());
            shoot(QStringLiteral("6-relaunch-unlocked"));
        }
        qInfo().noquote() << "ui-shot sidebar:" << phase << "failures" << failures;
        return failures ? 4 : 0;
    }
    QQuickItem *canvas = findItem(window->contentItem(), QStringLiteral("editSlideView"));
    if (canvas && backend.slideCount() > 1) {
        backend.select(QString());
        settle(300);
        const QPointF at = canvas->mapToScene(QPointF(canvas->width() / 2, canvas->height() / 2));
        const auto scrollUntilSlideChanges = [&](int notch, const QString &before, const QString &after) {
            const int from = backend.currentSlide();
            QImage last = window->grabWindow();
            int sent = 0;
            while (backend.currentSlide() == from && sent < 200) {
                QWheelEvent wheel(at, window->mapToGlobal(at), QPoint(), QPoint(0, notch), Qt::NoButton,
                                  Qt::NoModifier, Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(window, &wheel);
                ++sent;
                settle(40);
                if (backend.currentSlide() == from) {
                    last = window->grabWindow();
                    // Over the neighbour slide coming in at the edge (OS-011).
                    mouse(QEvent::MouseMove, canvas->mapToScene(QPointF(canvas->width() / 2,
                          notch < 0 ? canvas->height() - 6 : 6)), Qt::NoButton);
                }
            }
            expectCursor(QStringLiteral("over neighbour slide before snap"), Qt::ArrowCursor);
            last.save(outDir + QLatin1Char('/') + before + QStringLiteral(".png"));
            save(window, after);
            qInfo().noquote() << QStringLiteral("ui-shot scroll: slide %1 -> %2 after %3 notches")
                                     .arg(from + 1).arg(backend.currentSlide() + 1).arg(sent);
        };
        scrollUntilSlideChanges(-120, QStringLiteral("edit-scroll-mid"), QStringLiteral("edit-scroll-snapped"));
        settle(200);
        scrollUntilSlideChanges(120, QStringLiteral("edit-scroll-back-mid"), QStringLiteral("edit-scroll-back"));
        backend.setProperty("currentSlide", 0);
    }
    // Edit canvas cursors (OS-011): hover, drags, handle sizes at three zooms.
    if (canvas && !backend.slideObjects().isEmpty()) {
        backend.setProperty("currentSlide", 0);
        backend.select(backend.slideObjects().first().toMap().value(QStringLiteral("id")).toString());
        settle(300);
        const auto docPoint = [&](qreal x, qreal y) {
            const QPointF origin = canvas->property("contentOrigin").toPointF();
            return canvas->mapToScene(origin + QPointF(x, y) * canvas->property("contentScale").toReal());
        };
        const auto centre = [](QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)); };
        const QVariantMap sel = backend.property("selection").toMap();
        const QPointF body = docPoint(sel.value("x").toReal() + sel.value("w").toReal() / 2,
                                      sel.value("y").toReal() + sel.value("h").toReal() / 2);
        // Below the centred slide, clear of the handles (they reach 12 px out).
        const QPointF empty = canvas->mapToScene(QPointF(canvas->width() / 2 + 60, canvas->height() - 4));
        mouse(QEvent::MouseMove, empty, Qt::NoButton);
        expectCursor(QStringLiteral("empty canvas"), Qt::ArrowCursor);
        mouse(QEvent::MouseMove, body, Qt::NoButton);
        expectCursor(QStringLiteral("object body"), Qt::OpenHandCursor);
        mouse(QEvent::MouseButtonPress, body, Qt::LeftButton);
        mouse(QEvent::MouseMove, body + QPointF(30, 20), Qt::LeftButton);
        expectCursor(QStringLiteral("moving object"), Qt::ClosedHandCursor);
        mouse(QEvent::MouseMove, empty, Qt::LeftButton);
        expectCursor(QStringLiteral("moving object, pointer over empty canvas"), Qt::ClosedHandCursor);
        mouse(QEvent::MouseMove, body, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, body, Qt::NoButton);
        expectCursor(QStringLiteral("after move release (over object)"), Qt::OpenHandCursor);
        const int wantHandle[] = {Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                  Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor};
        for (int i = 0; i < 8; ++i)
            if (QQuickItem *handle = findItem(window->contentItem(), QStringLiteral("resizeHandle%1").arg(i))) {
                mouse(QEvent::MouseMove, centre(handle), Qt::NoButton);
                expectCursor(QStringLiteral("resize handle %1").arg(i), wantHandle[i]);
            }
        for (const qreal zoom : {0.25, 1.0, 4.0}) {
            QMetaObject::invokeMethod(canvas, "zoomAt", Q_ARG(qreal, zoom), Q_ARG(qreal, canvas->width() / 2),
                                      Q_ARG(qreal, canvas->height() / 2));
            settle(100);
            qreal shown = 1e9, hit = 1e9;
            for (int i = 0; i < 8; ++i)
                if (QQuickItem *handle = findItem(window->contentItem(), QStringLiteral("resizeHandle%1").arg(i))) {
                    const QRectF r = handle->mapRectToScene(handle->boundingRect());
                    shown = qMin(shown, qMin(r.width(), r.height()));
                    if (QQuickItem *area = handle->childItems().value(0)) {
                        const QRectF a = area->mapRectToScene(area->boundingRect());
                        hit = qMin(hit, qMin(a.width(), a.height()));
                    }
                }
            if (shown < 10 || hit < 10) ++cursorFailures;
            qInfo().noquote() << QStringLiteral("ui-shot cursor: handles at %1%: drawn %2 px, hit %3 px %4")
                                     .arg(zoom * 100).arg(shown).arg(hit).arg(shown >= 10 && hit >= 10 ? "ok" : "FAIL");
        }
        QMetaObject::invokeMethod(canvas, "fit");
        settle(200);
        if (QQuickItem *handle = findItem(window->contentItem(), QStringLiteral("resizeHandle3"))) {
            const QPointF at = centre(handle);
            mouse(QEvent::MouseButtonPress, at, Qt::LeftButton);
            mouse(QEvent::MouseMove, at - QPointF(40, 0), Qt::LeftButton);
            mouse(QEvent::MouseMove, body, Qt::LeftButton);
            expectCursor(QStringLiteral("resizing, pointer over object body"), Qt::SizeHorCursor);
            mouse(QEvent::MouseMove, empty, Qt::LeftButton);
            expectCursor(QStringLiteral("resizing, pointer over empty canvas"), Qt::SizeHorCursor);
            mouse(QEvent::MouseMove, at, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, at, Qt::NoButton);
            expectCursor(QStringLiteral("after resize release (over handle)"), Qt::SizeHorCursor);
            mouse(QEvent::MouseMove, empty, Qt::NoButton);
            expectCursor(QStringLiteral("after resize, empty canvas"), Qt::ArrowCursor);
        }
        if (QQuickItem *rotate = findItem(window->contentItem(), QStringLiteral("rotationHandle"))) {
            const QPointF at = centre(rotate);
            mouse(QEvent::MouseMove, at, Qt::NoButton);
            expectCursor(QStringLiteral("rotation handle"), Qt::CrossCursor);
            mouse(QEvent::MouseButtonPress, at, Qt::LeftButton);
            mouse(QEvent::MouseMove, body, Qt::LeftButton);
            expectCursor(QStringLiteral("rotating, pointer over object body"), Qt::CrossCursor);
            mouse(QEvent::MouseMove, at, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, at, Qt::NoButton);
            mouse(QEvent::MouseMove, empty, Qt::NoButton);
            expectCursor(QStringLiteral("after rotate, empty canvas"), Qt::ArrowCursor);
        }
        QQuickItem *editCanvas = findItem(window->contentItem(), QStringLiteral("editCanvas"));
        for (const QVariant &object : backend.slideObjects()) {
            backend.select(object.toMap().value(QStringLiteral("id")).toString());
            const QVariantMap text = backend.property("selection").toMap();
            if (!editCanvas || text.value("type").toString() != QLatin1String("text")) continue;
            settle(100);
            QMetaObject::invokeMethod(editCanvas, "beginTextEdit");
            settle(200);
            mouse(QEvent::MouseMove, docPoint(text.value("x").toReal() + text.value("w").toReal() / 2,
                                              text.value("y").toReal() + text.value("h").toReal() / 2), Qt::NoButton);
            expectCursor(QStringLiteral("editing text"), Qt::IBeamCursor);
            QMetaObject::invokeMethod(editCanvas, "commitTextEdit");
            break;
        }
        backend.select(QString());
        mouse(QEvent::MouseMove, empty, Qt::NoButton);
        qInfo().noquote() << QStringLiteral("ui-shot cursor: %1 failures").arg(cursorFailures);
    }
    // Crop mode (OS-013): the cut-off part of a picture, straight and rotated. Frames before
    // and after trimming the right and bottom quarters; the log names sample points.
    if (QQuickItem *editCanvas = findItem(window->contentItem(), QStringLiteral("editCanvas")); canvas && editCanvas) {
        backend.setProperty("currentSlide", 0);
        QString picture;
        for (const QVariant &object : backend.slideObjects()) {
            backend.select(object.toMap().value(QStringLiteral("id")).toString());
            if (backend.property("selection").toMap().value("type").toString() == QLatin1String("image")) {
                picture = backend.selectedId();
                break;
            }
        }
        const auto centreOf = [](QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)); };
        const auto cropShot = [&](const QString &name, qreal rotation) {
            backend.select(picture);
            if (!qFuzzyIsNull(rotation)) backend.setSelectedProperty(QStringLiteral("rotation"), rotation);
            settle(200);
            QMetaObject::invokeMethod(editCanvas, "startCrop");
            settle(300);
            QQuickItem *right = findItem(window->contentItem(), QStringLiteral("cropHandle3"));
            QQuickItem *left = findItem(window->contentItem(), QStringLiteral("cropHandle7"));
            QQuickItem *bottom = findItem(window->contentItem(), QStringLiteral("cropHandle5"));
            QQuickItem *top = findItem(window->contentItem(), QStringLiteral("cropHandle1"));
            if (!right || !left || !bottom || !top) { qInfo() << "ui-shot crop: no crop handles"; return; }
            save(window, name + QStringLiteral("-start"));
            const QPointF r0 = centreOf(right), l0 = centreOf(left);
            const auto drag = [&](const QPointF &from, const QPointF &to) {
                mouse(QEvent::MouseButtonPress, from, Qt::LeftButton);
                for (int step = 1; step <= 4; ++step) mouse(QEvent::MouseMove, from + (to - from) * step / 4.0, Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, to, Qt::NoButton);
            };
            drag(r0, r0 + (l0 - r0) * 0.25);
            settle(50);
            // Trimming the right moved the bottom handle with the bottom edge's middle.
            const QPointF b1 = centreOf(bottom), t1 = centreOf(top);
            drag(b1, b1 + (t1 - b1) * 0.25);
            mouse(QEvent::MouseMove, canvas->mapToScene(QPointF(4, 4)), Qt::NoButton);
            save(window, name);
            const QPointF kept = l0 + (r0 - l0) * 0.35;
            const QPointF cutRight = l0 + (r0 - l0) * 0.9, cutBottom = t1 + (b1 - t1) * 0.9;
            qInfo().noquote() << QStringLiteral("ui-shot crop: %1 kept %2,%3 cut-right %4,%5 cut-bottom %6,%7")
                                     .arg(name).arg(kept.x()).arg(kept.y()).arg(cutRight.x()).arg(cutRight.y())
                                     .arg(cutBottom.x()).arg(cutBottom.y());
            QMetaObject::invokeMethod(editCanvas, "finishCrop");
            settle(100);
            backend.undo(); backend.undo();
            if (!qFuzzyIsNull(rotation)) backend.undo();
            settle(200);
        };
        if (!picture.isEmpty()) {
            cropShot(QStringLiteral("edit-crop"), 0);
            cropShot(QStringLiteral("edit-crop-rotated"), 20);
        }
        backend.select(QString());
    }
    // TEST-ONLY END
    // The presenter console, rehearsing so no display has to exist.
    presenter.start(false, true);
    // Let the first slide's builds finish, so the console shows a settled slide.
    for (int waited = 0; backend.playing() && waited < 15000; waited += 100) settle(100);
    for (QObject *root : engine.rootObjects())
        for (auto *console : root->findChildren<QQuickWindow *>(QStringLiteral("presenterConsole"))) {
            console->resize(size);
            save(console, QStringLiteral("presenter-console"));
        }
    presenter.stop();
    return 0;
}

int main(int argc, char *argv[]) {
    // A verb means there is nobody watching: no window, no graphics device, and
    // no reason to need a display. Everything else keeps the behaviour it had.
    const QString first = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    const bool headless = Cli::isVerb(first);
    // Nor does asking for help. The platform is replaced, not defaulted: an
    // Omarchy session exports QT_QPA_PLATFORM=wayland;xcb to every process,
    // which is no use over SSH, on a console or in an agent's sandbox, and Qt
    // aborts rather than fail politely when it cannot reach the display. A
    // platform that already draws nowhere (offscreen, with or without a
    // config file, or minimal) is kept as given.
    static const QStringList asksForHelp{QStringLiteral("--help"), QStringLiteral("-h"),
                                         QStringLiteral("--help-all"), QStringLiteral("--version"),
                                         QStringLiteral("-v")};
    if (headless || asksForHelp.contains(first)) {
        const auto platform = qEnvironmentVariable("QT_QPA_PLATFORM");
        if (!platform.startsWith(QLatin1String("offscreen")) && !platform.startsWith(QLatin1String("minimal")))
            qputenv("QT_QPA_PLATFORM", "offscreen");
        // The platform theme Omarchy names (gtk3) opens the display for itself
        // and ends the process when it cannot; nothing headless needs a theme.
        qunsetenv("QT_QPA_PLATFORMTHEME");
    }

    // FBO-backed QPainter is supported on Qt's OpenGL renderer. Honour explicit
    // platform/backend choices, including the software screenshot harness.
    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND") &&
        qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND") && qgetenv("OMASHOW_RENDERER") != "raster") {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
        auto format = QSurfaceFormat::defaultFormat();
        format.setSamples(4);
        QSurfaceFormat::setDefaultFormat(format);
    }
    // The editing window's frame can be drawn see-through (Theme.frameWindow), so windows get an alpha
    // channel. With the default, solid frame this changes nothing on screen.
    if (!headless)
        QQuickWindow::setDefaultAlphaBuffer(true);
    QGuiApplication app(argc, argv);
    Fonts::loadBundled();   // Inter travels with OmaShow; draw it the same everywhere
    // FONT-CHECK-1: extra fonts from OmaShow's own folder, this process only;
    // fontconfig never sees them, so the rest of Omarchy is unaffected.
    {
        QElapsedTimer t; t.start();
        const int n = FontSandbox::load();
        if (qEnvironmentVariableIsSet("OMASHOW_FONT_TIMING")) qInfo().noquote() << "font sandbox:" << n << "files in" << t.elapsed() << "ms";
    }
    Workers::Session workerSession;

    // The identity trio. setDesktopFileName is what becomes the Wayland app_id,
    // which is what Hyprland rules, the taskbar and the icon lookup key off.
    app.setApplicationName(QStringLiteral("omashow"));
    app.setDesktopFileName(QStringLiteral("omashow"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("omashow")));

    // OmaShow's own Controls style (src/style/OmaShowStyle, in qrc under the
    // default qt/qml import path): the Oma suite look, falling back to Basic
    // for anything it does not restyle.
    QQuickStyle::setStyle(QStringLiteral("OmaShowStyle"));

    if (headless) {
        // The same Backend the interface drives, with nothing drawing it.
        Backend backend;
        QStringList words;
        for (int i = 1; i < argc; ++i) words.append(QString::fromLocal8Bit(argv[i]));
        return Cli::run(backend, words);
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("OmaShow — presentations for Omarchy."));
    parser.addHelpOption();
    QCommandLineOption shotOption(QStringLiteral("shot"),
                                  QStringLiteral("Render frames to <dir> and exit."),
                                  QStringLiteral("dir"));
    QCommandLineOption timesOption(QStringLiteral("times"),
                                   QStringLiteral("Comma-separated times in seconds."),
                                   QStringLiteral("list"));
    QCommandLineOption widthOption(QStringLiteral("width"),
                                   QStringLiteral("Frame width in pixels (default 1920)."),
                                   QStringLiteral("px"), QStringLiteral("1920"));
    // Open parked at a given second. Handy for looking at one moment of a build
    // without scrubbing to it by hand, and it is what the headless GUI check
    // uses to photograph the live view mid-transition.
    QCommandLineOption atOption(QStringLiteral("at"),
                                QStringLiteral("Open at <seconds> instead of the start."),
                                QStringLiteral("seconds"));
    parser.addOption(shotOption);
    parser.addOption(timesOption);
    parser.addOption(widthOption);
    // The start of the brief's `omashow new`: write a deck and exit. It is also
    // how the container gets checked against an unzip that is not ours.
    QCommandLineOption writeOption(QStringLiteral("write"),
                                   QStringLiteral("Write the current deck to <file> and exit."),
                                   QStringLiteral("file"));
    parser.addOption(writeOption);
    QCommandLineOption pdfOption(QStringLiteral("pdf"),
                                 QStringLiteral("Export the deck to <file> and exit."),
                                 QStringLiteral("file"));
    QCommandLineOption stagesOption(QStringLiteral("stages"),
                                    QStringLiteral("One PDF page per build stage."));
    parser.addOption(pdfOption);
    parser.addOption(stagesOption);
    QCommandLineOption skippedOption(QStringLiteral("include-skipped"),QStringLiteral("Include skipped slides in PNG and PDF exports."));
    parser.addOption(skippedOption);
    QCommandLineOption fpsOption(QStringLiteral("fps"),
                                 QStringLiteral("Render an even sequence at <n> frames a second."),
                                 QStringLiteral("n"), QStringLiteral("0"));
    parser.addOption(atOption);
    parser.addOption(fpsOption);
    QCommandLineOption uiShotOption(QStringLiteral("ui-shot"),
                                    QStringLiteral("Photograph each workspace to <dir> and exit."),
                                    QStringLiteral("dir"));
    QCommandLineOption uiSizeOption(QStringLiteral("ui-size"),
                                    QStringLiteral("Window size for --ui-shot (default 1920x1080)."),
                                    QStringLiteral("WxH"), QStringLiteral("1920x1080"));
    parser.addOption(uiShotOption);
    // Open straight into the show (autostart). The show starts only once the
    // deck is displayed, i.e. after its font check (FONT-CHECK-3).
    QCommandLineOption showOption(QStringLiteral("show"), QStringLiteral("Start the slide show once the deck is open."));
    parser.addOption(showOption);
    parser.addOption(uiSizeOption);
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("Deck to open."));
    parser.process(app);

    OmarchyTheme theme;
    Backend backend;
    ShowGuard showGuard;
    Presenter presenter(&backend, &showGuard);

    // Headless commands must load their positional deck before exporting.
    const auto cliFiles = parser.positionalArguments();
    if (!cliFiles.isEmpty() && (parser.isSet(writeOption) || parser.isSet(pdfOption) || parser.isSet(shotOption))) {
        const auto loaded = Interchange::load(cliFiles.first());
        if (!loaded.ok) { qCritical().noquote() << loaded.error; return 1; }
        backend.setDocument(loaded.document);
    }

    if (parser.isSet(writeOption)) {
        const QString path = parser.value(writeOption);
        // The name's suffix is the format, as in the save dialog.
        const auto written = Interchange::write(backend.document(), path);
        if (!written.ok) {
            qCritical().noquote() << written.error;
            return 1;
        }
        QTextStream(stdout) << path << Qt::endl;
        return 0;
    }

    if (parser.isSet(pdfOption)) {
        Pdf::Options options;
        options.pagePerBuildStage = parser.isSet(stagesOption);
        options.includeSkipped = parser.isSet(skippedOption);
        QString error;
        if (!Pdf::write(backend.document(), parser.value(pdfOption), options, &error)) {
            qCritical().noquote() << error;
            return 1;
        }
        QTextStream(stdout) << parser.value(pdfOption) << Qt::endl;
        return 0;
    }

    if (parser.isSet(shotOption)) {
        return renderShots(backend, parser.value(shotOption), parser.value(timesOption),
                           parser.value(widthOption).toInt(), parser.value(fpsOption).toInt(), parser.isSet(skippedOption));
    }

    // A capture run rehearses a show, and a rehearsal must never reach the
    // real desktop: no do-not-disturb, no idle inhibitor.
    if (parser.isSet(uiShotOption)) {
        showGuard.setIdleInhibitor(QString());
        showGuard.setRunner([](const QString &, const QStringList &) { return QString(); });
    }

    if (parser.isSet(atOption)) {
        backend.setDocument(backend.document());
        backend.setTime(parser.value(atOption).toDouble());
    }

        qmlRegisterType<TableAccessibility>("Omashow",1,0,"TableAccessibility");
    qmlRegisterType<SlideView>("Omashow", 1, 0, "SlideView");
    // The desktop palette, and the design system built on top of it. Theme.qml
    // is the single source of every colour and dimension in the interface; see
    // the note at the top of it.
    qmlRegisterSingletonInstance("Omashow", 1, 0, "OmarchyTheme", &theme);
    qmlRegisterSingletonType(QUrl(QStringLiteral("qrc:/Theme.qml")),
                             "Omashow", 1, 0, "Theme");
    // Lucide line icons, tinted without a shader (see src/ui/Icon.qml).
    qmlRegisterSingletonType(QUrl(QStringLiteral("qrc:/ui/icons/Icons.qml")),
                             "Omashow", 1, 0, "Icons");
    qmlRegisterType(QUrl(QStringLiteral("qrc:/ui/Icon.qml")), "Omashow", 1, 0, "Icon");
    // How many sheets are in front of the deck, so the shell's shortcuts can
    // stand down while one has the keyboard (see src/Sheet.qml).
    qmlRegisterSingletonType(QUrl(QStringLiteral("qrc:/ui/Sheets.qml")),
                             "Omashow", 1, 0, "Sheets");

    // Carry the desktop's text size into the default font so every control
    // grows with `omarchy display text size`, without a restart.
    const qreal basePointSize = app.font().pointSizeF();
    const auto scaleFont = [&app, basePointSize](qreal scale) {
        QFont font = app.font();
        font.setPointSizeF(basePointSize * scale);
        app.setFont(font);
    };
    scaleFont(theme.textScale());
    QObject::connect(&theme, &OmarchyTheme::textScaleChanged, &app, [&theme, scaleFont] {
        scaleFont(theme.textScale());
    });

    // While the frame is drawn see-through, the compositor must not fade the whole window on top of it
    // (the slides would fade too). OmaSkins keeps its fade off any app that leaves this marker, named
    // after the process, in the runtime directory. No OmaSkins transparency → opaque frame, so we
    // write nothing here (no marker, no Hyprland tag request, no log).
    const auto markOwnTransparency = [&theme] {
        if (!OmarchyTheme::omaskinsTransparencyPresent())
            return;
        if (theme.frameOpacity() >= 0.999 && theme.frameOpacityInactive() >= 0.999)
            return;
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
        if (runtime.isEmpty())
            return;
        const QString dir = runtime + QStringLiteral("/omaskins-qt");
        if (!QDir().mkpath(dir))
            return;
        QFile mark(dir + QLatin1Char('/') + QString::number(QCoreApplication::applicationPid()));
        if (!mark.open(QIODevice::WriteOnly))
            return;
    };
    markOwnTransparency();
    QObject::connect(&theme, &OmarchyTheme::frameOpacityChanged, &app, markOwnTransparency);

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("slides"), new SlideThumbnailProvider(&backend));
    engine.addImageProvider(QStringLiteral("effects"), new EffectPreviewProvider);
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            qWarning().noquote() << warning.toString();
    });
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("showGuard"), &showGuard);
    engine.rootContext()->setContextProperty(QStringLiteral("presenter"), &presenter);
    // Belt and braces: whatever route the app exits by, the lock comes back.
    QObject::connect(&app, &QGuiApplication::aboutToQuit, &showGuard, &ShowGuard::release);

    engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "Could not load the omashow interface.";
        return -1;
    }

    // "Open with", and `omashow somefile` from a terminal.
    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty()) {
        if (parser.isSet(atOption))
            QObject::connect(&backend, &Backend::opened, &app, [&] { backend.setTime(parser.value(atOption).toDouble()); }, Qt::SingleShotConnection);
        if (parser.isSet(showOption))
            QObject::connect(&backend, &Backend::opened, &app, [&] { QMetaObject::invokeMethod(&presenter, [&] { presenter.start(false, false); }, Qt::QueuedConnection); }, Qt::SingleShotConnection);
        // TEST-ONLY BEGIN: offscreen --ui-shot steps; history/tools/make-upstream.py leaves this out
        if (parser.isSet(uiShotOption)) {
            // FONT-CHECK-3 test: what the deck looks like the moment it is first displayed.
            QObject::connect(&backend, &Backend::opened, &app, [&] {
                qInfo().noquote() << "ui-shot fontcheck: first display swaps" << QString::fromUtf8(QJsonDocument::fromVariant(backend.fontSwaps()).toJson(QJsonDocument::Compact))
                                  << "revision" << backend.revision() << "show running" << presenter.property("running").toBool();
            }, Qt::SingleShotConnection);
        }
        // TEST-ONLY END
        if (parser.isSet(uiShotOption)) backend.open(QUrl::fromLocalFile(positional.first()));
        else backend.openAsync(QUrl::fromLocalFile(positional.first()));
    } else if (parser.isSet(atOption)) backend.setTime(parser.value(atOption).toDouble());
    if (parser.isSet(uiShotOption)) {
        const QStringList wh = parser.value(uiSizeOption).split(QLatin1Char('x'));
        const QSize size(wh.value(0).toInt(), wh.value(1).toInt());
        return captureInterface(engine, backend, presenter, parser.value(uiShotOption),
                                size.isValid() ? size : QSize(1920, 1080), !positional.isEmpty());
    }

    return app.exec();
}
