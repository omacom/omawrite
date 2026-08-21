# Omawrite

A dead-simple Markdown writing app built with Qt Quick and C++ that automatically follows system dark/light mode.

<img width="2948" height="3227" alt="screenshot-2026-06-23_15-24-08" src="https://github.com/user-attachments/assets/4e930c0d-edda-4046-b444-a59eff523329" />
<img width="2948" height="3227" alt="screenshot-2026-06-23_15-23-23" src="https://github.com/user-attachments/assets/8ced7c26-961b-4ded-b263-84403001a951" />


## Install

Install via the Omarchy Package Repository via the `omawrite` package. It's installed by default in new installations of Omarchy (from Quattro forward).

## Shortcuts

- `Ctrl+S` saves, though writing is saved for you anyway. Naming a document
  yourself uses the XDG desktop portal file picker.
- `Ctrl+Shift+S` saves as.
- `Ctrl+O` opens a Markdown file through the portal picker.
- `Ctrl+P` opens the system print dialog.
- `Ctrl+E` shows the sidebar listing the Markdown in the current document's
  folder, and puts the keyboard in it. Pressing it again takes the sidebar away
  and hands the keyboard back to the text — it never reaches into the panel
  while you are writing. It starts closed.
- `Ctrl+N` opens a new Omawrite window.
- `Ctrl+Z`, `Ctrl+Shift+Z`, and `Ctrl+Y` handle undo and redo.
- `Super+F` toggles fullscreen. Qt maps this key as `Meta+F`.
- `Ctrl+F` searches the document. Use `Enter` or `Ctrl+G` for the next match and `Shift+Enter` for the previous match.
- `Ctrl+H` opens find and replace.
- `Ctrl+B`, `Ctrl+I`, and `Ctrl+K` insert bold, italic, and link Markdown.
- `Ctrl+?` shows the keyboard shortcut reference.

## Sidebar

The sidebar lists the folder the open document lives in — Markdown files and
the folders beside them, nothing else. Opening a document puts the cursor after
everything already written in it, so writing carries on with the sidebar still
open, and the document you are leaving is saved on the way out rather than
asked about.

With the keyboard, once `Ctrl+E` has put focus there:

- `Up`/`Down` or `j`/`k` move through the folder.
- `Enter`, `Right`, or `l` opens a document and hands the keyboard back to the
  text, or walks into a folder and stays put.
- `Backspace`, `Left`, or `h` goes up a level.
- `a` starts a new Markdown file and `A` a new folder; type the name and press
  `Enter`, or `Esc` to abandon it. A new document opens straight away, and an
  existing name is reported rather than overwritten.
- `Esc` returns to writing, leaving the sidebar open.

The folder is re-read whenever anything in it changes, and the selection keeps
its place through that rather than snapping back to the top. Opening the panel
starts from the document being written.

Drag its right edge to widen it; the width is remembered, and stops short of
squeezing the writing column below its usual measure.

## Saving

Writing is saved a moment after you stop typing, and again whenever you leave
the document — switching files, closing the window, or moving to another app.
There is no prompt to answer and nothing to remember to press; `Ctrl+Z` is the
way back rather than a discard button.

A document you never named takes its name from its first line when you leave
it, landing in the folder the sidebar is showing; if the same name is taken
already it becomes `... 2`. One with nothing written in it is not kept.

Unsaved drafts are recovered after an abnormal exit. Omawrite also watches open files
and warns before an external change can replace local work.

Text follows the desktop text size — `omarchy display text size`, or GNOME's
`text-scaling-factor` — and re-flows without a restart. The default of 12px leaves
Omawrite at the size it is designed around; larger and smaller sizes scale from there.

## Requirements

- Qt 6: `qt6-base`, `qt6-declarative`, `qt6-quickcontrols2`
- `xdg-desktop-portal` and a portal backend

The iA Writer Mono font is bundled under the SIL Open Font License 1.1; see
`fonts/OFL.txt`. The font is copyright Information Architects Inc. and based on
IBM Plex, copyright IBM Corp.
