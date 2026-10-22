#pragma once

// Writing a deck out as a PowerPoint 97–2003 presentation (`.ppt`, `.pps`,
// `.pot`), which WPS Presentation also keeps as `.dps` and `.dpt`.
//
// The counterpart of `ppt.*`: a compound file (`cfb.*`) holding the
// "PowerPoint Document" stream of records, "Current User", and "Pictures".
// The resolved deck — every slide as it is drawn — becomes masters, slides
// drawn with Office Art shapes, text with its runs, pictures, notes,
// comments, links, custom shows, hidden slides and transitions. What the old
// format has no form for (tables and charts as live objects, films, builds,
// numbered lists) is written in the nearest form and named in the report.

#include <QByteArray>
#include <QStringList>
#include <memory>

#include "core/scene.h"
#include "core/workers.h"

namespace PptWriter {

QByteArray bytes(const Document &document, QStringList *warnings = nullptr,
                 const std::shared_ptr<Workers::Job> &job = {});

} // namespace PptWriter
