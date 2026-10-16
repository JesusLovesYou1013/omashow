#pragma once

// Decks that open everywhere, with features that switch on only where they
// are understood.
//
// A deck is written in two layers inside the same .omashow file:
//
//   the base deck      every JSON file holds only what OmaShow 0.1.0 (upstream
//                      main 74ce940) reads: the field names it knows, with
//                      values it accepts. A feature it doesn't know is simply
//                      absent there: no strikethrough, the stock text fit, no
//                      newer animation effects, no PowerPoint extras.
//   features.json      each feature that was left out, by name, with where it
//                      goes and the exact values. An app switches on the ones
//                      it knows and ignores the rest; OmaShow 0.1.0 never
//                      opens this file at all.
//
// Each entry carries a short fingerprint of the base element it belongs to. An
// app that didn't know the feature may have changed that element; then the
// entry no longer fits and is dropped rather than put back on the wrong thing.
// Entries this build doesn't know are kept and written back unchanged, so a
// feature from a newer build survives a round trip through an older one.
//
// What 0.1.0 reads is frozen here (the base field lists and value rules). A
// field this build writes that isn't on the lists goes to features.json by
// itself (as "key:<part>.<field>"), so a new feature can't make a deck that
// 0.1.0 refuses.

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

#include "io/zip.h"

namespace DeckFeatures {

// Rewrites the JSON entries to the base deck and adds features.json when
// anything was left out (plus `passthrough`: entries this build didn't know
// when the deck was opened, a JSON array).
void split(QVector<Zip::Entry> &entries, const QByteArray &passthrough);

// The JSON files of an opened deck with the features this build knows put
// back: name -> bytes, only for files that changed. `passthrough` receives the
// entries this build didn't know (a JSON array, empty when none). A missing or
// damaged features.json changes nothing: the base deck opens as it is.
QHash<QString, QByteArray> merge(const Zip::Reader &reader, QByteArray *passthrough);

// The feature names this build switches on.
QStringList known();

} // namespace DeckFeatures
