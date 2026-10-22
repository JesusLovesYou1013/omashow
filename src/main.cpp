// OmaShow — native presentations for Omarchy.

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
        check(QStringLiteral("shape fill"), QStringLiteral("Fill colour"), QStringLiteral("#30a060"), QStringLiteral("hex"), [&] { return selection("fill"); });
        backend.setSelectedProperty(QStringLiteral("fillStyle"), 1);
        settle(200);
        check(QStringLiteral("shape second fill colour"), QStringLiteral("Second fill colour"), QStringLiteral("#f0c020"), QStringLiteral("hex"), [&] { return selection("fillSecondary"); });
        check(QStringLiteral("shape outline"), QStringLiteral("Stroke colour"), QStringLiteral("#602080"), QStringLiteral("hex"), [&] { return selection("strokeColor"); });
        backend.setSelectedProperty(QStringLiteral("shadowEnabled"), true);
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
    const bool headless = argc > 1 && Cli::isVerb(QString::fromLocal8Bit(argv[1]));
    if (headless && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

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
