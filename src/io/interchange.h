#pragma once

// Opening decks that were not written by OmaShow.
//
// A `.pptx` (PowerPoint, Google Slides, LibreOffice) or a `.key` (Keynote)
// opens like any other deck, but what arrives is a new, unsaved OmaShow deck:
// it is written back only when the person saves to that name themselves, and
// what it could not bring across is said, not hidden. The readers live in `pptx.*`, `ppt.*`, `odp.*` and `keynote.*`; this is
// the one place that decides which of them a path needs.

#include <QPair>
#include <QStringList>
#include <memory>

#include "core/scene.h"
#include "core/workers.h"

namespace Interchange {

// PowerPointBinary: the 97–2003 format (.ppt, .pps, .pot) and WPS's (.dps, .dpt).
enum Kind { Native, PowerPoint, PowerPointBinary, Keynote, OpenDocument, Unknown };

Kind kindOf(const QString &path);
QString kindName(Kind kind);
inline bool isForeign(Kind kind) { return kind == PowerPoint || kind == PowerPointBinary || kind == Keynote || kind == OpenDocument; }
// The kinds a deck can be saved as: the name typed in the save dialog decides.
inline bool canSave(Kind kind) { return kind == Native || kind == PowerPoint || kind == PowerPointBinary || kind == OpenDocument; }
// A presentation format known by its suffix that OmaShow can't write (.ppt,
// .dps…): its readable name. Empty for a suffix that is no presentation
// format at all, which is then just part of the name.
QString unwritableFormat(const QString &path);

struct Result {
    bool ok = false;
    QString error;
    Document document;
    Kind kind = Unknown;
    // What was left out or approximated, one line each, deduplicated.
    QStringList warnings;
};

// Native decks come back exactly as Bundle::load would give them. Foreign
// decks come back converted; `kind` says which reader answered.
Result load(const QString &path);
Result fromBytes(const QByteArray &raw, Kind kind);

// The deck as the bytes of the format `path`'s suffix names (an OmaShow deck
// for any name that is no other writable format). Empty when it could not be
// written; `warnings` gets what was approximated or left out.
QByteArray encode(const Document &document, const QString &path, QStringList *warnings = nullptr,
                  const std::shared_ptr<Workers::Job> &job = {});

struct Written {
    bool ok = false;
    QString error;
    QStringList lines;   // what was approximated or left out
};
// encode() and the file written in one, replacing it only once complete;
// `keepPrevious` keeps what was there as "<name>.bak", as saving does.
Written write(const Document &document, const QString &path, const std::shared_ptr<Workers::Job> &job = {},
              bool keepPrevious = false);

// For file choosers: every pattern OmaShow can open, native first.
QStringList openPatterns();
// The save dialog's format list: OmaShow, .ppt, .pptx, then All Files.
QList<QPair<QString, QStringList>> saveFilters();

// The name a converted deck should be saved under: the source name with the
// native suffix.
QString suggestedName(const QString &path);
// The name the save dialog offers for a deck opened from `path`: the same name
// when OmaShow can write its format, else suggestedName().
QString saveName(const QString &path);

} // namespace Interchange
