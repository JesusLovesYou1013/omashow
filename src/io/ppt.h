#pragma once

// Reading PowerPoint 97–2003 presentations (`.ppt`, `.pps`, `.pot`), and WPS
// Presentation's own (`.dps`, `.dpt`), which are the same format.
//
// The sibling of `pptx.*` for the binary format every Office before 2007
// wrote: a compound file (`cfb.*`) whose "PowerPoint Document" stream holds
// records, slides drawn with Office Art shapes and their text in atoms beside
// them. Masters become masters with a layout for their title and body, shapes
// keep their fill, outline, shadow and text, pictures their crop; notes,
// comments, links, hidden slides, custom shows and transitions come across.
// What has no native equivalent is named in the warnings, as on PowerPoint
// import.

#include <QByteArray>
#include <QStringList>

#include "core/scene.h"

namespace Ppt {

struct Result {
    bool ok = false;
    QString error;
    Document document;
    QStringList warnings;
};

Result read(const QByteArray &raw);
Result load(const QString &path);

} // namespace Ppt
