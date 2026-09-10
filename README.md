# Omawrite

A dead-simple Markdown writing app built with Qt Quick and C++ that automatically follows system dark/light mode.

<img width="2948" height="3227" alt="screenshot-2026-06-23_15-24-08" src="https://github.com/user-attachments/assets/4e930c0d-edda-4046-b444-a59eff523329" />
<img width="2948" height="3227" alt="screenshot-2026-06-23_15-23-23" src="https://github.com/user-attachments/assets/8ced7c26-961b-4ded-b263-84403001a951" />


## Install

Install via the Omarchy Package Repository via the `omawrite` package. It's installed by default in new installations of Omarchy (from Quattro forward).

### macOS (Apple Silicon)

Install Qt 6 and build an arm64 application bundle:

```sh
brew install qtbase qtdeclarative qttools
./bin/build
```

To bundle the Qt frameworks and create a local DMG:

```sh
./bin/package-macos-arm64
```

The package is written to `build-macos/omawrite.dmg`. It is ad-hoc signed for local
use; public distribution still requires an Apple Developer ID signature and
notarization.

## Shortcuts

- `Ctrl+S` saves. Unsaved documents use the system file picker (the XDG
  desktop portal on Linux).
- `Ctrl+Shift+S` saves as.
- `Ctrl+O` opens a Markdown file through the system picker.
- `Ctrl+P` opens the system print dialog.
- `Ctrl+N` opens a new Omawrite window.
- `Ctrl+Z`, `Ctrl+Shift+Z`, and `Ctrl+Y` handle undo and redo.
- `Super+F` toggles fullscreen on Linux; macOS uses `Control+Command+F`.
- `Ctrl+F` searches the document. Use `Enter` or `Ctrl+G` for the next match and `Shift+Enter` for the previous match.
- `Ctrl+H` opens find and replace on Linux; macOS uses `Command+Option+F`.
- `Ctrl+B`, `Ctrl+I`, and `Ctrl+K` insert bold, italic, and link Markdown.
- `Ctrl++` and `Ctrl+-` adjust the editor text size. `Ctrl+=` also increases it,
  and `Ctrl+0` resets it to the original size.
- `Alt+Up` and `Alt+Down` move the paragraph at the caret past its neighbour.
- `Ctrl+L` puts each sentence of the paragraph on its own line, and puts them
  back together again.
- `Ctrl+Shift+O` lists the document's headings and jumps to one.
- `Ctrl+?` shows the keyboard shortcut reference. On macOS, Qt maps `Ctrl`
  shortcuts to their native `Command` equivalents.

`Ctrl+,` opens Preferences, which covers everything below. On macOS the same
settings live under the app menu. They are stored with QSettings, so they can
also be set from a script:

| Setting | Default | What it does |
| --- | --- | --- |
| `editor/fontFamily` | `iA Writer Quattro S` | Editor and printed page. Quattro, Duo and Mono all ship with the app; any installed family can be named instead |
| `editor/caretStyle` | `line` | `line` is a 2px accent bar; `block` covers the glyph |
| `editor/caretBlink` | `true` | Blinks on the desktop's own flash time |
| `editor/measureChars` | `65` | Characters per line, which is what sets the editor's margins |
| `print/marginMm` | `15` | Margins on the printed page and on Save as PDF |
| `print/fontPointSize` | `11` | Body size on paper, in points -- the editor's pixel size is a screen measurement |
| `editor/paragraphOnReturn` | `false` | Return breaks the line once; on, it opens a Markdown paragraph |
| `editor/wordTarget` | `0` (off) | Footer counts towards it, and names a first-draft goal a quarter longer |
| `editor/autosave` | `true` | See below |
| `editor/autosaveDelayMs` | `750` | See below |

Autosave is on. A document that has been saved once follows you to disk about
750ms after you stop typing, so `Ctrl+S` becomes something you press out of
habit rather than need. Untitled drafts, and a document whose file changed
underneath you before you said which version wins, get the crash snapshot
instead -- autosave never picks a version for you and never writes over a file
it has not read. Turn it off or retune it with the `editor/autosave` and
`editor/autosaveDelayMs` settings.

A line holding nothing but a file path embeds that file when the document is
rendered -- iA Writer calls these content blocks:

    parts/intro.txt
    snippet.py
    data.csv (Quarterly figures)
    chart.png "Figure 1"

Text arrives as itself, code arrives fenced under its own extension so it is
highlighted, a CSV or TSV becomes a table, and an image becomes an image with
its caption as alt text. Paths resolve inside the folder the document was
opened from and nowhere else, so a document cannot read its way out of it. The
source keeps the path you typed; the embedding happens in the preview and in
what is printed or saved as PDF.

Fenced code is highlighted when the fence names a language: bash, python,
javascript and typescript, C and C++, rust, go, ruby, sql, json, yaml, toml and
qml, under their usual short names. Keywords take the theme's accent so
highlighted code still belongs to the page; a fence with no language stays
plain.

About Omawrite, in the application menu, names the version and links to the
commit the binary was built from. A build from a tree with uncommitted changes
marks the commit with a `+` and links nowhere, so it never points at a page
whose code is not what is running.

Unsaved drafts are recovered after an abnormal exit. Omawrite also watches open files
and warns before an external change can replace local work.

Text follows the desktop text size — `omarchy display text size`, or GNOME's
`text-scaling-factor` — and re-flows without a restart. The default of 12px leaves
Omawrite at the size it is designed around; larger and smaller sizes scale from there.
Editor text size starts at 20px, changes in 2px steps from 10px to 48px, and is
remembered across launches. Desktop text scaling is applied on top of this base size.

## Requirements

- Qt 6: `qt6-base`, `qt6-declarative`, `qt6-quickcontrols2`
- Linux: `xdg-desktop-portal` and a portal backend
- macOS 13 or newer: Xcode Command Line Tools

iA Writer Quattro, Duo and Mono are bundled under the SIL Open Font License
1.1; see `fonts/OFL.txt`. They differ only in how many character widths they
allow -- Mono is fully monospaced, Duo gives W and M more room, Quattro has
four widths and is the default. The fonts are copyright Information Architects
Inc. and based on IBM Plex, copyright IBM Corp.
