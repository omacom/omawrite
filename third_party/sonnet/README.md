# Sonnet Core 6.13.0

Write uses KDE Sonnet Core with one static platform backend: Apple's
NSSpellChecker on macOS, Windows ISpellChecker on Windows, or system Hunspell on
Linux. Sonnet's Widgets/QML components and other backends are disabled. No
spelling dictionaries are bundled or downloaded by Write.

Upstream: https://invent.kde.org/frameworks/sonnet

Pinned source: https://download.kde.org/stable/frameworks/6.13/sonnet-6.13.0.tar.xz

SHA-256: `d47465a9d623257cfb0192c3a4dc24733490aa8952310422e402d828ad461eee`

`LICENSES/` contains the four upstream license texts, copied unmodified. Sonnet
is not covered by Write's MIT license. See individual upstream source files
for their copyright and SPDX declarations (including LGPL-2.1-or-later and the
Windows backend's LGPL-2.0-or-later).

Local modification: `dependencies/sonnet-static-plugins.cmake` changes the three
selected `qt_add_plugin` declarations to explicitly request STATIC plugins. No
upstream spelling algorithm or dictionary data is modified. The patch, pinned
sources and build instructions are available in this repository.

The current private build statically links Sonnet. Anyone distributing binaries
must independently satisfy applicable LGPL obligations, including corresponding
library source/modifications, notices and the required ability to relink with a
modified library. Copying these notices alone does not establish compliance.
System Hunspell libraries/dictionaries and native OS services have their own
terms. The existing binary-distribution block for syntax-highlighting data also
remains in force; this feature does not clear that audit.
