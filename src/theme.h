#pragma once

#include <QHash>
#include <QFileSystemWatcher>
#include <QObject>
#include <QPair>
#include <QString>

class QDBusVariant;

// Everything the app needs to look like it belongs on this desktop: the current
// Omarchy palette, the desktop's dark/light preference, and its text scale.
// All of it is optional at runtime — on a machine without Omarchy themes or a
// portal, the built-in fallbacks keep the app looking sane.
class OmarchyTheme : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool darkMode READ darkMode NOTIFY darkModeChanged)
    Q_PROPERTY(qreal textScale READ textScale NOTIFY textScaleChanged)
    Q_PROPERTY(QString background READ background NOTIFY colorsChanged)
    Q_PROPERTY(QString foreground READ foreground NOTIFY colorsChanged)
    Q_PROPERTY(QString accent READ accent NOTIFY colorsChanged)
    // Whether that accent actually came from an Omarchy theme. On plain Arch,
    // in a container or in CI there is no colors.toml, and the app still has to
    // look finished — so the palette falls back to the Oma accent rather than
    // to whatever the default happens to be.
    Q_PROPERTY(bool accentFollowed READ accentFollowed NOTIFY colorsChanged)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY colorsChanged)
    Q_PROPERTY(QString selection READ selection NOTIFY colorsChanged)
    // How solid the editing window's frame (bars, panels, the surround of the slide) is drawn, for the
    // focused and the unfocused window: 1 = solid, the default. A desktop transparency setting can lower
    // it (OmaSkins publishes one), and only the frame follows: slides and thumbnails stay solid, as they
    // must for judging the work. Without such a setting nothing changes.
    Q_PROPERTY(qreal frameOpacity READ frameOpacity NOTIFY frameOpacityChanged)
    Q_PROPERTY(qreal frameOpacityInactive READ frameOpacityInactive NOTIFY frameOpacityChanged)

public:
    explicit OmarchyTheme(QObject *parent = nullptr);

    bool darkMode() const { return m_darkMode; }
    qreal textScale() const { return m_textScale; }
    QString background() const { return m_background; }
    QString foreground() const { return m_foreground; }
    QString accent() const { return m_accent; }
    bool accentFollowed() const { return m_accentFollowed; }
    QString selection() const { return m_selection; }
    QString accentForeground() const { return foregroundFor(m_accent); }
    qreal frameOpacity() const { return m_frameOpacity; }
    qreal frameOpacityInactive() const { return m_frameOpacityInactive; }
    // "focused unfocused ..." as OmaSkins writes it; (1, 1) for anything else.
    static QPair<qreal, qreal> frameOpacityFromText(const QString &text);

    // key = value pairs from an omarchy colors.toml; empty when there is none.
    static QHash<QString, QString> colorsFromFile(const QString &path);
    // "black" or "white", whichever stays legible on the given colour.
    static QString foregroundFor(const QString &color);
    // Whether a colour reads as dark, for deciding a theme's mode.
    static bool isDark(const QString &color, bool *known);

public slots:
    void reload();
    void reloadFrameOpacity();

private slots:
    void handlePortalSettingChanged(const QString &nameSpace, const QString &key,
                                    const QDBusVariant &value);

private:
    static QString colorsPath();
    static QString frameOpacityPath();
    void watchTheme();
    bool detectDarkMode() const;
    qreal detectTextScale() const;

    bool m_darkMode = true;
    qreal m_textScale = 1.0;
    QString m_background;
    QString m_foreground;
    QString m_accent;
    bool m_accentFollowed = false;
    QString m_selection;
    QFileSystemWatcher m_watcher;
    qreal m_frameOpacity = 1.0;
    qreal m_frameOpacityInactive = 1.0;
    QFileSystemWatcher m_frameWatcher;

signals:
    void darkModeChanged();
    void textScaleChanged();
    void colorsChanged();
    void frameOpacityChanged();
};
