#pragma once

// Reading OpenDocument presentations (`.odp`, `.otp`, `.fodp`): what
// LibreOffice Impress, OnlyOffice, WPS and Calligra write as their own.
//
// The counterpart of `odpwriter.*` and the sibling of `pptx.*`: master pages
// become masters (with a layout each for their title and body), pages become
// slides, and every drawing object becomes native objects: custom shapes are
// drawn from their own enhanced geometry (formulas and all), so a shape
// LibreOffice knows by name arrives looking exactly as it does there. Text
// keeps its runs, lists, links and alignment; pictures their crop and
// adjustments; tables, films, notes, comments, custom shows, hidden slides and
// transitions come across. What has no native equivalent is named in the
// warnings, as on PowerPoint import.

#include <QByteArray>
#include <QStringList>

#include "core/scene.h"

namespace Odp {

struct Result {
    bool ok = false;
    QString error;
    Document document;
    QStringList warnings;
};

// A package (.odp, .otp) or a flat XML document (.fodp); told apart by content.
Result read(const QByteArray &raw);
Result load(const QString &path);

} // namespace Odp
