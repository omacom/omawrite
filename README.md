# Omawrite

A dead-simple Markdown writing app built with Qt Quick and C++ that automatically follows system dark/light mode.

<img width="2948" height="3227" alt="screenshot-2026-06-23_15-24-08" src="https://github.com/user-attachments/assets/4e930c0d-edda-4046-b444-a59eff523329" />
<img width="2948" height="3227" alt="screenshot-2026-06-23_15-23-23" src="https://github.com/user-attachments/assets/8ced7c26-961b-4ded-b263-84403001a951" />


## Install

Install via the Omarchy Package Repository via the `omawrite` package. It's installed by default in new installations of Omarchy (from Quattro forward).

## Command line

`omawrite` opens an empty document and `omawrite FILE` opens a Markdown file.
`omawrite --help` and `omawrite --version` answer in the terminal without opening a window.

## Shortcuts

- `Ctrl+S` saves. Unsaved documents use the XDG desktop portal file picker.
- `Ctrl+Shift+S` saves as.
- `Ctrl+O` opens a Markdown file through the portal picker.
- `Ctrl+P` opens the system print dialog.
- `Ctrl+N` opens a new Omawrite window.
- `Ctrl+Z`, `Ctrl+Shift+Z`, and `Ctrl+Y` handle undo and redo.
- `Super+F` toggles fullscreen. Qt maps this key as `Meta+F`.
- `Ctrl+F` searches the document. Use `Enter` or `Ctrl+G` for the next match and `Shift+Enter` for the previous match.
- `Ctrl+H` opens find and replace.
- `Ctrl+B`, `Ctrl+I`, and `Ctrl+Shift+X` toggle bold, italic, and strikethrough Markdown. `Ctrl+K` inserts a link.
- `Ctrl+=` and `Ctrl+-` change the text size, `Ctrl+0` returns to the default.
  `Ctrl+scroll` does the same.
- `Ctrl+Shift+C` copies the rendered document as HTML, with plain text as a fallback.
- `F7` checks spelling in the selection, or the whole document if nothing is selected.
- `Ctrl+?` shows the keyboard shortcut reference.
- In the unsaved-changes dialog, `Alt+C`, `Alt+D`, and `Alt+S` choose Cancel,
  Discard, and Save.

Once a document has been saved to a file, edits are auto-saved to that file a
moment after you stop typing, so `Ctrl+S` is rarely needed. Untitled drafts are
never auto-saved — they wait for an explicit save so Omawrite never picks a file
name for you. Unsaved drafts are recovered after an abnormal exit. Omawrite also
watches open files and warns before an external change can replace local work;
auto-save pauses until you reload, keep your version, or save. A recovered draft
is never auto-saved over its file — save it to keep it.

The font button in the footer picks the writing font from any installed text font,
and Omawrite remembers the choice. IBM Plex Mono is the default.

Text follows the desktop text size — `omarchy display text size`, or GNOME's
`text-scaling-factor` — and re-flows without a restart. The default of 12px leaves
Omawrite at the size it is designed around; larger and smaller sizes scale from there.
`Ctrl+=`, `Ctrl+-`, and `Ctrl+scroll` set your own size on top of that, from 67% to
200% in browser-sized steps. It is remembered for the next window you open.

## Spell checking

Press `F7` (or `Fn+F7` on some laptops), or click the spelling button in the
footer, to start a review. Nothing is checked while you write. The review shows
one word at a time in a compact bar above the document. Edit the correction or
choose a suggestion, then press Enter or Replace. Up/Down cycles suggestions.
Replace is undoable; Skip skips one occurrence. The ⋯ menu offers Ignore All
and Add to Dictionary for each selected language. Escape returns to the editor.
F7 focuses an already-open review without restarting it.

Click the language names in the bar to choose one or more installed dictionaries.
The selection and preferred language are remembered. A word is accepted if any selected
language recognizes it; suggestions from the preferred language come first.
Enabling extra languages can hide a typo that is a valid word in another language.
On first use, the system locale is selected if its dictionary is installed;
otherwise choose a language explicitly. An unavailable selected dictionary is
reported instead of silently checking in a different language.

Install Hunspell dictionaries with your distribution's package manager. On
Omarchy/Arch, `hunspell-en_us` supplies English (US). For Danish, the
[LibreOffice dictionary collection](https://github.com/LibreOffice/dictionaries/tree/master/da_DK)
provides `da_DK.aff` and `da_DK.dic`. You can place matching
`<language>.aff` and `<language>.dic` files under
`~/.local/share/omawrite/dictionaries` (or the corresponding XDG data directory).
Reopen the review after installing dictionaries. System `hunspell` and `myspell`
data directories are searched too. Prefer UTF-8 dictionaries; unsupported
encodings produce an explanation in the review.

Checks run locally, on a background worker. Prose and link labels are checked;
fenced/indented code, inline code, link destinations, reference definitions,
URLs, and email addresses are skipped. A selection checks only complete words.
This is dictionary-based spelling, not grammar or contextual proofreading.
Changing the text during a review invalidates its results; start a new check to
review the updated document. Personal words are stored per language in the
application settings and are matched with their saved capitalization.

## Requirements

- Qt 6: `qt6-base`, `qt6-declarative`, `qt6-quickcontrols2`
- `xdg-desktop-portal` and a portal backend
- Hunspell library (`hunspell`) and `pkg-config` for building; install language
  dictionaries separately to use spell checking

The IBM Plex Mono font is bundled under the SIL Open Font License 1.1; see
`fonts/OFL.txt`. The font is copyright IBM Corp.
