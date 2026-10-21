#pragma once

// Writing a deck out as OpenDocument (`.odp`, `.otp`, `.fodp`): what
// LibreOffice Impress, OnlyOffice, WPS and Calligra open as their own.
//
// The counterpart of `pptxwriter.*`: the resolved deck — every slide as it
// would be drawn, with its design applied — becomes master pages, pages,
// shapes, text, pictures, films, tables, notes, comments, custom shows and
// transitions. What has no equivalent there (builds, sections, morph, picture
// fills, charts as live objects) is written in the nearest form and named in
// the report. A `.otp` is the same package marked as a template; a `.fodp` is
// one plain XML file with its pictures inside it.

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <memory>

#include "core/scene.h"
#include "core/workers.h"

namespace OdpWriter {

enum Flavor { Presentation, Template, Flat };

// From the name's suffix: .otp a template, .fodp flat, anything else a presentation.
Flavor flavorOf(const QString &path);

struct Report {
    bool ok = false;
    QString error;
    QStringList lines;   // what was approximated or left out; shown in the export log
    qint64 bytes = 0;
};

QByteArray bytes(const Document &document, Flavor flavor = Presentation, QStringList *warnings = nullptr,
                 const std::shared_ptr<Workers::Job> &job = {});
Report write(const Document &document, const QString &path,
             const std::shared_ptr<Workers::Job> &job = {});

} // namespace OdpWriter
