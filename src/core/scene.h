#pragma once

// Authored content and shared design definitions. The design resolver applies
// inheritance before the deterministic animation evaluator produces a frame.

#include <QColor>
#include <utility>
#include <QImage>
#include <QPicture>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QVector>
#include <QVariantList>
#include <memory>

#include "anim/build.h"
#include "core/tabledata.h"
#include "core/chartdata.h"
#include "core/datasource.h"

enum class ObjectType { Rect, Text, Image, Media, Table, Chart };

// One colour stop of a gradient (PowerPoint's a:gs): where it sits
// (0-1 along the gradient), its colour with its own transparency (alpha), and
// PowerPoint's Brightness (-1 to 1: darker or lighter, as lumMod/lumOff).
struct GradientStop {
    qreal position = 0;
    QColor color;
    qreal brightness = 0;
    bool operator==(const GradientStop &o) const { return qFuzzyCompare(position + 1, o.position + 1) && color == o.color && qFuzzyCompare(brightness + 1, o.brightness + 1); }
};

// A stretch of text that does not look like the rest of its box. Only what
// differs is stored: an empty family, a zero size or weight and an invalid
// colour all mean "as the box says", so changing the box still carries.
struct TextRun {
    int start = 0, length = 0;
    int weight = 0;              // 0 inherits
    int italic = 0, underline = 0, strike = 0;   // 0 inherits, 1 on, 2 off
    int baseline = 0;            // 0 normal, 1 raised, 2 lowered
    qreal fontSize = 0;          // 0 inherits
    QString fontFamily;
    QColor color;                // invalid inherits
    QString language;            // empty inherits: what this stretch is written in
    // Font swapping on PowerPoint import (core/fontswap.h): the Office family
    // this stretch named before a free match stood in for it, the size it had
    // (0: it inherited the box's), the factor that size was multiplied by
    // (1: none), and the weight before the Office family's named weight was
    // applied (0: unchanged, 1: it inherited the box's). Written back as they
    // were on PowerPoint save.
    QString origFont;
    qreal origFontSize = 0, fontScale = 1;
    int origWeight = 0;
    bool operator==(const TextRun &other) const {
        return start == other.start && length == other.length && weight == other.weight &&
               italic == other.italic && underline == other.underline &&
               strike == other.strike && baseline == other.baseline &&
               qFuzzyCompare(fontSize + 1, other.fontSize + 1) &&
               fontFamily == other.fontFamily && color == other.color &&
               language == other.language && origFont == other.origFont &&
               qFuzzyCompare(origFontSize + 1, other.origFontSize + 1) &&
               qFuzzyCompare(fontScale, other.fontScale) && origWeight == other.origWeight;
    }
};

// One thing on a slide. This doubles as the *resolved state* of that thing at a
// given time: the evaluator returns a copy with the animated fields changed, so
// there is exactly one shape flowing from document to renderer.
struct SceneObject {
    QString id;                       // stable across slides — this is what Morph matches on
    ObjectType type = ObjectType::Rect;
    QRectF rect;                      // in document coordinates
    qreal rotation = 0.0;             // degrees
    qreal opacity = 1.0;
    // Drawn this much bigger or smaller about the middle of rect. Only ever
    // set by an animation in flight and never saved: scaling the picture
    // rather than the font keeps text from growing in whole-pixel steps and
    // from re-wrapping half-way through.
    qreal paintScale = 1.0;
    QColor fill = QColor(255, 255, 255);
    qreal cornerRadius = 0.0;

    int linkKind = 0;
    QString linkTarget;
    bool connector = false;
    QString connectorFrom, connectorTo;
    QPointF connectorStart, connectorEnd;
    int connectorFromSide = 0, connectorToSide = 0, connectorRoute = 1;
    bool connectorArrowStart = false, connectorArrowEnd = true;
    int shapeKind = 0; // Shape::names(), or 99 for editable paths
    QVariantList pathData;
    bool pathWinding = false;
    int fillStyle = 0; // solid, linear, radial, pattern, picture, none
    QColor fillSecondary = QColor(255,255,255);
    qreal fillAngle = 0;
    int patternStyle = 9; // Qt patterns from Dense1 through DiagCross
    QColor strokeColor = QColor(0,0,0);
    qreal strokeWidth = 0;
    int strokeStyle = 0, strokeJoin = 1, strokeCap = 1;
    // The rest of PowerPoint's Format Shape > Fill and Line (see
    // render/fillpaint.h). Left at these values an object draws exactly as it
    // always did: two-colour gradients from fill/fillSecondary, the 13 Qt
    // patterns, the five Qt dashes.
    //   gradient   any number of stops (empty: fill to fillSecondary); for a
    //              path gradient (fillStyle 2) its shape, 0 circle, 1 rectangle,
    //              2 the shape's own outline, growing from the focus box
    //              (a:fillToRect: insets from the left, top, right and bottom,
    //              0-1; all 0.5 is the middle); a:lin scaled; rotate with shape.
    //              fill and fillSecondary follow the first and last stop, so
    //              older builds show a two-colour version.
    QVector<GradientStop> gradientStops;
    int gradientPath = 0;
    qreal gradientFocusL = .5, gradientFocusT = .5, gradientFocusR = .5, gradientFocusB = .5;
    bool gradientScaled = false, gradientRotate = true;
    //   pattern    one of PowerPoint's preset patterns by its a:pattFill name
    //              (empty: patternStyle's Qt pattern); fill is its foreground,
    //              fillSecondary its background. patternStyle follows the
    //              nearest Qt pattern for older builds.
    QString patternPreset;
    //   picture    (fillStyle 4, the picture in image*) tiled instead of placed
    //              by imageMode: offset (slide units), scale (%), the corner or
    //              side the tiles start from (0-8 as shadowAlign) and mirroring
    //              (0 none, 1 x, 2 y, 3 both); stretched (imageMode 2) with
    //              insets (a:fillRect l/t/r/b, fractions; negative bleeds out);
    //              its own transparency (0-1) and rotate with shape.
    bool pictureTile = false;
    qreal pictureTileX = 0, pictureTileY = 0, pictureTileScaleX = 100, pictureTileScaleY = 100;
    int pictureTileAlign = 0, pictureTileFlip = 0;
    qreal pictureInsetL = 0, pictureInsetT = 0, pictureInsetR = 0, pictureInsetB = 0;
    qreal pictureTransparency = 0;
    bool pictureRotate = true;
    //   line       a dash by PowerPoint's preset name (empty: strokeStyle's Qt
    //              dash), or "custom" with strokeDashCustom (dash, space, ... in
    //              line widths); compound 0 single, 1 double, 2 thick-thin,
    //              3 thin-thick, 4 triple; a gradient line (strokeGradient)
    //              from strokeStops along strokeAngle, or a path gradient
    //              (strokeGradientPath 1 circle, 2 rectangle, 3 shape; 0 linear).
    //              strokeStyle and strokeColor follow for older builds.
    QString strokeDash;
    QVariantList strokeDashCustom;
    int strokeCompound = 0;
    bool strokeGradient = false;
    QVector<GradientStop> strokeStops;
    qreal strokeAngle = 0;
    int strokeGradientPath = 0;
    // A text box with a fill and outline of its own (the fill* and
    // stroke* fields above, and its glow and boxShadow* effects drawn about
    // that box), as one PowerPoint p:sp holding both. The box is the text's
    // rectangle grown by these insets (PowerPoint's lIns/tIns/rIns/bIns: OmaShow
    // lays words out in rect itself). Off, a text box draws no box: older decks
    // stored a white solid fill on every text object that was never drawn.
    bool boxDrawn = false;
    qreal boxInsetL = 0, boxInsetT = 0, boxInsetR = 0, boxInsetB = 0;
    bool shadowEnabled = false;
    QColor shadowColor = QColor(0,0,0,100);
    qreal shadowX = 8, shadowY = 8;
    // How soft the shadow's edge is (PowerPoint's blur), in slide units; 0 a
    // hard edge. On text the shadow follows PowerPoint's own sizing (see
    // TextLayout's paintEffects).
    qreal shadowBlur = 0;
    // The rest of PowerPoint's shadow (a:outerShdw / a:innerShdw).
    // Inside the shape instead of behind it (inner); its size and slant as
    // PowerPoint's sx/sy (percent) and kx/ky (degrees), taken about the point
    // of the shape named by shadowAlign (0-8: tl t tr l ctr r bl b br, as
    // a:outerShdw's algn). Shapes and pictures draw all of it; text draws the
    // outer shadow by PowerPoint's own measured rule and keeps the rest only
    // to write it back.
    bool shadowInner = false;
    qreal shadowScaleX = 100, shadowScaleY = 100;
    qreal shadowSkewX = 0, shadowSkewY = 0;
    int shadowAlign = 0;
    // A glow around the shape itself (spPr a:glow): its outline
    // and fill, or just the outline when it has no fill. How far it reaches in
    // slide units, and its colour with its own transparency. 0: none. Text
    // boxes glow their words instead (textGlowRadius), as PowerPoint does for
    // a box with no fill and no outline.
    qreal glowRadius = 0;
    QColor glowColor = QColor(68, 114, 196, 102);
    // A text box's own shadow (p:spPr a:effectLst), apart from
    // its words' (shadow*, a:rPr). Only text objects use it; their box glow is
    // glowRadius/glowColor above. With no fill and no outline PowerPoint draws
    // box effects on the words, so they are drawn there, each kind only where
    // the words have none of their own (TextLayout::paint). The same fields as
    // shadow*, swapped in and out by swapBoxShadow().
    bool boxShadowEnabled = false;
    QColor boxShadowColor = QColor(0,0,0,100);
    qreal boxShadowX = 8, boxShadowY = 8, boxShadowBlur = 0;
    bool boxShadowInner = false;
    qreal boxShadowScaleX = 100, boxShadowScaleY = 100;
    qreal boxShadowSkewX = 0, boxShadowSkewY = 0;
    int boxShadowAlign = 0;
    void swapBoxShadow() {
        std::swap(shadowEnabled, boxShadowEnabled); std::swap(shadowColor, boxShadowColor);
        std::swap(shadowX, boxShadowX); std::swap(shadowY, boxShadowY); std::swap(shadowBlur, boxShadowBlur);
        std::swap(shadowInner, boxShadowInner); std::swap(shadowScaleX, boxShadowScaleX); std::swap(shadowScaleY, boxShadowScaleY);
        std::swap(shadowSkewX, boxShadowSkewX); std::swap(shadowSkewY, boxShadowSkewY); std::swap(shadowAlign, boxShadowAlign);
    }

    // Embedded raster data is implicitly shared between snapshots and duplicates.
    std::shared_ptr<const SceneObject> imageOriginal;
    QImage image;
    QByteArray imageData;
    QString imageId;
    QString imageFormat = QStringLiteral("png");
    QPicture vectorPicture = QPicture();
    QSizeF vectorSize;
    QRectF imageCrop = QRectF(0,0,1,1); // normalized source region
    int imageMode = 0; // fit, fill, stretch
    qreal imageFocalX = .5, imageFocalY = .5;
    int imageMask = 0; // rectangle, ellipse, rounded, hexagon, heart
    qreal imageBrightness = 0, imageContrast = 1, imageSaturation = 1, imageTintAmount = 0;
    QColor imageTint = QColor(0,0,0);

    // Media bytes are shared by history snapshots. Local read permission and
    // playback position are derived session state, never written into a deck.
    std::shared_ptr<const SceneObject> mediaOriginal;
    QByteArray mediaData;
    QString mediaId, mediaPath, mediaName, mediaContainer, mediaCodec;
    qint64 mediaBytes = 0, mediaModified = 0;
    qreal mediaDuration = 0, mediaTrimStart = 0, mediaTrimEnd = 0;
    qreal mediaVolume = 1;
    int mediaLoops = 1;
    bool mediaVideo = false, mediaAudio = false;
    qreal mediaPosition = -1;
    bool mediaActive = false, mediaReadAllowed = false;

    TableData table;
    ChartData chart;
    DataSource dataSource;

    // Text only
    QString text;
    qreal fontSize = 48.0;
    int fontWeight = 400;
    QColor textColor = QColor(255, 255, 255);
    bool uppercase = false;
    qreal letterSpacing = 0.0;
    bool italic = false, underline = false;
    int textAlign = 0;       // left, centre, right, justify
    int verticalAlign = 1;   // top, middle, bottom; middle preserves older decks
    qreal lineHeight = 100;  // percentage
    qreal paragraphSpacing = 0, textIndent = 0;
    int listStyle = 0;       // none, bullets, numbers; leading tabs nest items
    int listStart = 1;   // where a numbered list starts counting
    int textFit = 0;         // clip with overflow warning, shrink to fit
    int textKind = 0;        // 0 the words as typed, 1 an equation, 2 along a path
    QString language;        // what this box is written in; empty follows the deck
    qreal tabStop = 0;       // 0 uses four times the type size
    int columns = 1;         // text flows down one column, then into the next
    qreal columnGap = 0;     // 0 uses one line of space between columns
    int direction = 0;       // 0 follows the words, 1 left to right, 2 right to left
    QString fontFamily = QStringLiteral("Inter");
    // The Office family this box named before a free match stood in for it on
    // PowerPoint import, with its size and weight then and the size factor
    // applied (1: none, the match is metric-compatible). See core/fontswap.h.
    QString origFont;
    qreal origFontSize = 0, fontScale = 1;
    int origWeight = 0;
    // A soft glow around the letters (PowerPoint's text glow): how far it
    // reaches, in slide units, and its colour with its own transparency.
    // 0: none. Drawn behind the text and not cut off at the box.
    qreal textGlowRadius = 0;
    QColor textGlowColor = QColor(255, 255, 255, 191);
    QString fillToken, textColorToken, fontToken;
    // Stretches of this text that differ from the box, in authored order.
    QVector<TextRun> runs;
    // What a screen reader would be told this is. Never rendered.
    QString altTitle, altText;
    QString placeholderId;
    QString textStyleId;      // the named look this box follows, if any
    QStringList overrides;
    QStringList groups; // outermost to innermost
    bool locked = false;
    bool hidden = false;
};

struct Slide {
    QString id;
    QColor background = QColor(12, 16, 24);
    QVector<SceneObject> objects;
    Timeline timeline;
    QString layoutId;
    QString sectionId;
    QString notes;
    // Object ids in the order they should be read. Anything not listed keeps
    // its place in the stacking order, after those that are.
    QStringList readingOrder;
    // How the show arrives at this slide. -1 follows the deck's own choice.
    int transition = -1;
    int transitionDirection = 0;    // push: 0 left, 1 right, 2 up, 3 down
    qreal transitionSeconds = -1;
    // Seconds to hold after the last build before moving on by itself.
    // -1 waits for the speaker.
    qreal advanceAfter = -1;
    bool backgroundOverride = false;
    bool showMasterObjects = true;
    bool showMasterFields = true;
    bool skipped = false;

    const SceneObject *find(const QString &id) const;
    SceneObject *find(const QString &id);

    // Topmost object containing the point, or empty. Later objects draw over
    // earlier ones, so hit testing walks backwards.
    QString objectAt(const QPointF &point) const;
};

struct DeckTheme {
    QString name = QStringLiteral("Midnight");
    QMap<QString, QColor> colors = {
        {QStringLiteral("background"), QColor(12, 16, 24)},
        {QStringLiteral("foreground"), QColor(231, 237, 247)},
        {QStringLiteral("muted"), QColor(167, 177, 193)},
        {QStringLiteral("accent"), QColor(39, 194, 255)}
    };
    QMap<QString, QString> fonts = {
        {QStringLiteral("heading"), QStringLiteral("Inter")},
        {QStringLiteral("body"), QStringLiteral("Inter")}
    };
    // Theme font slot → the Office family it named before a free match stood
    // in for it on PowerPoint import.
    QMap<QString, QString> fontOrigins;
    // Office family → the installed fallback that stood in for it (no mapped
    // free match was installed), so saving knows it was not chosen by hand.
    QMap<QString, QString> fontFallbacks;
};

struct MasterFields {
    bool showNumber = false, showDate = false, showFooter = false, hideOnFirst = false;
    int firstNumber = 1;
    QString date, footer;
};

struct Master {
    QString id, name;
    QColor background = QColor(12, 16, 24);
    QString backgroundToken = QStringLiteral("background");
    QVector<SceneObject> objects;
    MasterFields fields;
};

struct SlideLayout {
    QString id, name, masterId;
    // Stable ids identify placeholder roles across layouts.
    QVector<SceneObject> placeholders;
};

struct Section { QString id, name; };

// A named order of slides that already exist — never a copy of them.
struct CustomShow { QString id, name; QStringList slideIds; };

// A note on a slide, or on one object, or a reply to either. Replies are one
// level deep: a thread is a comment and the replies under it.
struct Comment {
    QString id, slideId, objectId, parentId;
    QString author, created;   // ISO-8601, exactly as it was written
    QString text;
    bool resolved = false;
};

struct ObjectStyle { QString id, name; SceneObject appearance; };

// A named way for text to look, that boxes can follow. Changing the style
// changes every box that follows it, except where a box says otherwise.
struct TextStyle { QString id, name; SceneObject look; };

struct Document {
    QVector<ObjectStyle> objectStyles;
    QVector<TextStyle> textStyles;
    QSizeF size = QSizeF(1920, 1080);
    QVector<Slide> slides;
    QVector<Section> sections;
    DeckTheme theme;
    QVector<Master> masters;
    QVector<SlideLayout> layouts;
    QVector<Comment> comments;
    QVector<CustomShow> shows;
    // Which slides are being shown right now, in order. Session state: it is
    // never written to a file, and an empty list means the whole deck.
    QStringList activeShow;
    // Review findings the author has looked at and does not want raised again.
    QStringList dismissedIssues;
    // What the deck is written in, as "en_GB"; empty follows the computer. The
    // words it has been taught are its own, not the computer's, so they travel
    // with the file.
    QString language;
    QStringList knownWords;
    // Straight quotes and double hyphens become the real marks as you type.
    bool smartPunctuation = true;

    // What a slide does unless it says otherwise: the kind of transition into
    // it, and how long that takes.
    int transition = 3;   // Presentation::Morph
    qreal transitionDuration = 0.9;

    // Features a newer OmaShow wrote that this build doesn't know (from the
    // deck's features.json, io/deckfeatures.h), kept to be written back as
    // they were. A JSON array; empty when there are none.
    QByteArray unknownFeatures;
};
