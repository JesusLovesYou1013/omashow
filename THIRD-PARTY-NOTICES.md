# Third-party notices

OmaShow is licensed under the GNU General Public License, version 3 or (at
your option) any later version — see `LICENSE`. It builds on the following
work, each under its own licence.

## Vendored in this repository

| Component | Where | Licence |
|---|---|---|
| [Lucide](https://lucide.dev) icons (some derived from [Feather](https://feathericons.com)) | `src/ui/icons/Icons.qml`, generated from `tools/icons.txt` | ISC, with Feather's portions under MIT — full text in `src/ui/icons/LICENSE.lucide` |
| PowerPoint's 54 preset pattern fills as 8×8 cells, from [office-open-xml-viewer](https://github.com/yukiyokotani/office-open-xml-viewer) (`packages/core/src/shape/pattern-bitmaps.ts`) | `src/core/fillstyle.cpp` (`kPatterns`) | MIT — see below |
| PowerPoint's 3-D camera presets (rotation, oblique skew), light rigs (light directions and colours) and preset materials as numbers, from LibreOffice's [`oox/source/drawingml/scene3dhelper.cxx`](https://git.libreoffice.org/core/+/refs/heads/master/oox/source/drawingml/scene3dhelper.cxx) (the values themselves come from Microsoft's [MS-OI29500] notes on DrawingML) | `src/core/effects3d.cpp` (`kCameras`, the rig and material tables) | MPL 2.0 — data values only, no code copied; the MPL 2.0 text is at https://mozilla.org/MPL/2.0/ |

## Linked at run time (system packages, not redistributed here)

| Component | Licence |
|---|---|
| [Qt 6](https://www.qt.io) — Core, Gui, Qml, Quick, Quick Controls, Svg, Multimedia, Concurrent, DBus, Network | LGPL-3.0 / GPL-3.0 |
| [FFmpeg](https://ffmpeg.org) — libavformat, libavcodec, libavutil, libswscale | LGPL-2.1-or-later; GPL-3.0 as built with `--enable-gpl --enable-version3` by Arch Linux |
| [zlib](https://zlib.net) | zlib licence |
| [Hunspell](https://hunspell.github.io) | GPL-2.0-or-later / LGPL-2.1-or-later / MPL-1.1 |
| [libetonyek](https://wiki.documentfoundation.org/DLP/Libraries/libetonyek) — reads Keynote decks | MPL-2.0 |
| [librevenge](https://sourceforge.net/p/libwpd/wiki/librevenge/) — the interface libetonyek speaks | MPL-2.0 / LGPL-2.1-or-later |

Because the FFmpeg build OmaShow links against is GPL-3.0, OmaShow as a whole
is distributed under GPL-3.0-or-later terms. No "nonfree" component is used.

## Spelling dictionaries

OmaShow bundles no dictionaries. It reads the Hunspell dictionaries installed on
the system, each under its own licence, and says so when there are none for the
language a deck is written in.

## Fonts

OmaShow bundles one typeface: **Inter 4.1** by The Inter Project Authors
(https://github.com/rsms/inter), the static Regular, Bold, Italic and Bold
Italic TrueType files from the official release `Inter-4.1.zip`
(sha256 9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e),
unmodified, under the SIL Open Font License 1.1 (`src/fonts/inter/LICENSE.txt`,
installed as `/usr/share/licenses/omashow/LICENSE.Inter`). OmaShow draws it
app-wide and embeds it in PowerPoint decks it saves, as the OFL allows when
bundled with software. Other fonts are the system's own. PDF export embeds subsets of the fonts a deck uses; the licence
of a font chosen for a deck is that font's own.

## Office font map

`src/fonts/office/office-font-map.json` (compiled in as `:/fonts/office-font-map.json`)
is **office-font-map** by JesusLovesYou1013
(https://github.com/JesusLovesYou1013/office-font-map), licensed under the
Creative Commons Attribution-ShareAlike 4.0 International licence (CC BY-SA 4.0,
https://creativecommons.org/licenses/by-sa/4.0/), which is compatible with
OmaShow's GPL 3. It lists the best free matches per Office font with their
measured size, width and line factors. It contains no fonts.

## Free fonts downloaded on request

`src/fonts/office/font-sources.json` lists, for the free matches OmaShow can
fetch, the exact files in the google/fonts repository
(https://github.com/google/fonts) at one pinned commit, each with its size and
SHA-256. When a person asks for them (the font check on opening a deck, or
the Extra fonts pack), OmaShow downloads those files, checks each one against
its SHA-256 before using it, and keeps them in its own font folder
(`$XDG_DATA_HOME/omashow-fonts`), which only OmaShow reads. Each family's
licence file is downloaded beside it. The fonts are under their own licences,
almost all the SIL Open Font License 1.1; none is bundled with OmaShow.

## office-open-xml-viewer (pattern cells)

```
MIT License

Copyright (c) 2026 Yuki Yokotani

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
