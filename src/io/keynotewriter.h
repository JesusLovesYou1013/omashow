#pragma once

// Writing a deck out as a Keynote presentation (`.key`).
//
// The counterpart of `keynote.*`: the package Keynote 6 and later keep, a zip
// of "Index/*.iwa" object files (protocol buffer archives in Snappy chunks),
// the pictures under "Data/", the metadata Keynote checks on open and the
// previews Finder shows. Each layout becomes a master slide with its objects,
// title, body and slide-number placeholders; each slide its shapes, text,
// pictures and notes, with every look written as a Keynote style so it stays
// editable there. What Keynote has no form for, or what is not carried yet
// (tables and charts as live objects, films, builds, comments), is written in
// the nearest form and named in the report.

#include <QByteArray>
#include <QStringList>
#include <memory>

#include "core/scene.h"
#include "core/workers.h"

namespace KeynoteWriter {

QByteArray bytes(const Document &document, QStringList *warnings = nullptr,
                 const std::shared_ptr<Workers::Job> &job = {});

} // namespace KeynoteWriter
