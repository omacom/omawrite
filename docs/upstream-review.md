# Native Markdown enhancements — review proposal

This branch brings together the merged, tested editor enhancements for
[Write/Omawrite](https://github.com/omacom/omawrite). It is a complete integration
proposal for feedback, not a request to accept every feature unchanged or to
review the entire change as an indivisible patch.

The starting points are upstream `8ee488b` and the integrated development tree
`701a16b` (through the reference/footnote work). Upstream is an ancestor of that
tree; existing commit history and contributor attribution are retained. Unmerged
experimental branches are not included.

## Feedback requested

1. Which capabilities fit Write's intentionally focused scope, and which should
   remain optional or outside the application?
2. Is the native source/projection approach appropriate for Edit/Read, tables,
   and footnotes? Are any interaction or shortcut choices surprising?
3. Are KSyntaxHighlighting and Sonnet acceptable dependencies? Which platforms
   and minimum Qt version should be supported?
4. How should this be split into reviewable contributions, and what additional
   Linux/Omarchy testing or licensing work should precede them?

Nothing has been submitted upstream automatically. The overview and
[screenshots](screenshots/README.md) are intended to make a first discussion
possible before arranging smaller patches.

## What's included

| Area | Behavior | Implementation / regression coverage |
| --- | --- | --- |
| Native Markdown editing | Caret/selection-aware markers, headings, rules, lists, quotes, inline formatting; source-preserving copying and undo | `src/livepreview.*`, `src/listpreview.h`, `src/markdownhighlighter.*`; `tests/livepreview/`, `tests/headings/`, `tests/tst_editor.cpp` |
| Edit/Read transitions | Retained documents, reading/caret position, incremental projection, native scenegraph refresh after imports | `src/livepreview.cpp`; mode-switch, source/undo and native pixel regressions |
| Tables | Virtualized editable grids, alignment, source view, rectangular fill, overflow, expanded grids, shared Read tables and draft protection | `src/TableGrid.qml`, `src/markdowntablemodel.*`, `src/livepreviewtables.cpp`; table and native scrollbar regressions |
| Code blocks | Syntax coloring, continuous panels, language labels, copy commands and horizontal scrolling | `src/codehighlighter.*`, `src/readcodehighlighting.*`, `src/CodeBlockControls.qml`; `tests/codehighlighting/` |
| References / footnotes | Full, collapsed and shortcut references; superscripts, safe hover text, internal note jumps, insertion shortcuts and single-step undo | `src/markdownreferences.h`, `src/EditorMutations.js`; reference/hover/insertion tests and live-preview ordinal tests |
| Images / attachments | Local rendering, clipboard-image staging, collision-safe names, save-as handling, copy/repair commands and source preservation | `src/attachmenthelper.h`, `src/AttachmentMenu.qml`; `tests/attachments/` |
| Optional Mermaid | Asynchronous local CLI rendering, cached results and source fallback | `src/mermaidrenderer.*`, `src/previewblocks.*`; `tests/mermaid/`, `tests/previewblocks/` |
| Spelling | Native desktop providers, prose-aware exclusion, suggestions and session actions | `src/spellingcontroller.*`, `src/spellingprose.*`; `tests/spellchecking/` |
| Windows and recovery | Per-document windows, file-open routing, single-instance IPC, draft ownership, external replacement watching and autosave protection | `src/windowmanager.*`, `src/instancebroker.*`, `src/fileopenrequests.*`, `src/backend.cpp`; `tests/instancebroker/` and editor/recovery tests |
| Print / PDF | Immutable print snapshots, local images and wide-table appendices with surfaced errors | `src/printdocument.*`; `tests/printing/` |
| Build / portability | CMake plus qmake development targets, Windows dependency setup, architecture checks, installer automation, Qt 6.8/6.12 native/offscreen CI | `CMakeLists.txt`, `dependencies/`, `build.bat`, `bin/`, `packaging/`, `.github/workflows/test.yml` |

The [user guide](writing-and-shortcuts.md) describes limits and exact shortcuts.
Existing [benchmark records](benchmarks/) and [performance reports](performance/)
are retained as historical observations, not current cross-platform promises.

## Upstream-facing cleanup

- User-facing names, examples and links use Write/Omawrite and its upstream URL.
- The application keeps upstream's `omawrite` / `Omacom` / `omacom.io` identity,
  data locations and IPC namespace. No other application's settings or recovery
  files are imported or overwritten. Fork-specific profile migration is omitted;
  do not expect an experimental build's separate recovery store to migrate.
- Windows installer metadata points upstream. Its proposed identity, default
  `Programs\Omawrite` directory and shortcut paths are separate from downstream
  installers, and it no longer deletes downstream shortcuts or executables.
  Final installer identity/location remains a maintainer decision; do not
  override its destination to an existing development distribution.
- Copyright attribution and dependency notices remain intact. Removing product
  branding does not remove authorship or alter dependency licensing.
- Gallery images are real Qt-rendered captures from checked-in synthetic fixture
  documents, not mockups or screenshots of personal documents.

## Validation and limits

The integrated source baseline passed [CI run 44091075871](https://github.com/ponchoalv/PonchoWriter/actions/runs/44091075871):
Linux build/tests, Windows Qt 6.8.3 and 6.12.0 native and portable suites, and
Windows packaging checks. That run predates this branch's upstream-identity and
gallery preparation; it is not validation of later changes.

Local preparation checks on macOS / Qt 6.12 (qmake):

- Extended editor suite: **176 passed, 0 failed, 7 skipped** (including the
  intentionally opt-in gallery capture).
- Live-preview suite: **78 passed, 0 failed, 1 skipped**.
- Instance-broker suite: **40 passed, 0 failed**.
- Opt-in native gallery capture: **3 passed, 0 failed**, seven screenshots
  generated with unchanged source documents.

The branding/identity and installer preparation changes have not yet received a
fresh Windows CI run. PowerShell installer smoke checks cannot run on this local
machine because PowerShell is not installed.

See [testing instructions](testing.md) and the [gallery capture recipe](screenshots/README.md#reproducing-the-gallery).
The opt-in capture test does not run during normal CI and checks source
preservation while generating the gallery.

Known review/release considerations:

- The additional dependencies change Linux packaging and the minimum toolchain.
  The historical Arch packaging recipe is disabled pending an updated recipe and
  license audit; this is not a drop-in update to the published Omarchy package.
- KSyntaxHighlighting resources have mixed licenses; static Sonnet linking adds
  LGPL source/relinking obligations. The audit is not complete. Distribution
  guards and notices are retained; unsigned installer scripts are not release
  clearance.
- Mermaid invokes an external Node/browser toolchain. Input filtering is not an
  OS sandbox; this is not a general-purpose untrusted-diagram renderer.
- Native Edit projections use source/view mappings and bounded inline objects.
  Some work remains document-sized; virtualization is not a blanket performance
  guarantee.
- The queued `QQuickTextEdit::q_invalidate` hook is private Qt API. Invocation is
  checked and warns if unavailable, but future Qt releases require native pixel
  regression coverage or a supported replacement.
- Native printers, installed spelling dictionaries, desktop/portal integration,
  and real installer upgrades still require platform-specific manual testing.

## Suggested review sequence

A possible order is build/dependency policy and test harness; source-preserving
projection foundations; tables and code UI; images/Mermaid/spelling; window,
recovery and print integration; then references/footnotes and documentation.
These areas share infrastructure: this is a discussion outline, not a claim
that existing commits can be cherry-picked independently without adaptation.
