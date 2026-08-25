# Omawrite

A dead-simple Markdown writing app built with Qt Quick and C++ that automatically follows system dark/light mode.

<img width="2948" height="3227" alt="screenshot-2026-06-23_15-24-08" src="https://github.com/user-attachments/assets/4e930c0d-edda-4046-b444-a59eff523329" />
<img width="2948" height="3227" alt="screenshot-2026-06-23_15-23-23" src="https://github.com/user-attachments/assets/8ced7c26-961b-4ded-b263-84403001a951" />


## Install

Install via the Omarchy Package Repository via the `omawrite` package. It's installed by default in new installations of Omarchy (from Quattro forward).

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
- `Ctrl+B`, `Ctrl+I`, and `Ctrl+K` insert bold, italic, and link Markdown.
- `Ctrl+Alt+V` turns vim mode on and off.
- `Ctrl+?` shows the keyboard shortcut reference.

## Vim mode

The diamond in the bottom-left corner, or `Ctrl+Alt+V`, toggles vim key
bindings, and the choice is remembered between sessions. The current mode sits
in the bottom-left corner, and normal mode draws the caret as a block. Insert
mode is the plain editor: smart returns, list continuation and Markdown paste
all behave as they do with vim mode off.

- Modes: `i` `I` `a` `A` `o` `O` to insert, `v` and `V` to select, `Esc` to
  return to normal.
- Motions: `h` `j` `k` `l`, `w` `W` `b` `B` `e` `E` `ge` `gE`, `0` `^` `$`,
  `gg` `G`, `{` `}` by paragraph, `(` `)` by sentence, and `f` `F` `t` `T`
  with `;` and `,`.
- `j` and `k` follow the wrapped line, so a paragraph moves the way it reads
  on screen. `gj` and `gk` reach the logical line the Markdown has, which is
  the mirror of vim, where `g` is the display-line prefix. Operators keep the
  logical line either way: `dj` takes two whole paragraphs.
- Operators: `d` `c` `y` with any motion, doubled for whole lines (`dd`, `cc`,
  `yy`), plus `D` `C` `Y` `S` `s` `x` `X` `r` `J` `~` and `p` / `P`.
- Text objects: `iw` `aw`, `ip` `ap`, `i"` `a"`, `i'` `a'`, and the bracket
  pairs `i(` `i[` `i{` with their `a` forms. They take a span without a
  motion, under an operator or from visual mode.
- Counts work throughout: `3j`, `d2w`, `2dd`.
- `u` and `Ctrl+R` undo and redo a whole command at a time, and `.` repeats the
  last change, including the text typed during it.
- `/` opens the app's find bar, with `n` and `N` for next and previous match.
  `Ctrl+D` and `Ctrl+U` scroll by a page.

### Registers

Yanks and deletes go to vim's unnamed register, which stays inside the editor:
an `x` never costs you what you copied from a browser. `"` names a register
for the command after it.

- `"ayy` and `"ap` keep a line aside in `a`, one of `"a` to `"z`. A named yank
  fills the unnamed register too, so a bare `p` still pastes what you last
  took.
- `"+y` and `"+p` are the system clipboard, for carrying text out of the
  window and back. `"*` is the primary selection, where a middle click pastes
  from, falling back to the clipboard on desktops without one.
- A clipboard carries no linewise flag, so a trailing newline stands in for
  one: `"+yy` here pastes as a whole line into another vim, and text copied
  from elsewhere ending in a newline arrives as a line.

Every `Ctrl` shortcut above keeps working in either mode.

### The `:` command line

`:` opens a command line along the bottom edge. `Enter` runs it; `Esc`, or
backspacing past the start, abandons it.

- `:w`, `:w <path>`, `:wq`, `:x`, `:q`, `:q!` write and quit. Writing an
  unsaved document opens the portal picker, and `:q` on a modified one asks
  before closing, the same as clicking the window's close button.
- `:e <path>` opens a file, `:e!` reloads the open one from disk. Relative
  paths hang off the open document's directory, and `~` is your home.
- `:42`, `:$` jump to a line.
- `:s/pattern/replacement/`, with `%`, `3`, `2,5` or `'<,'>` in front of it to
  choose the lines, and `g` (every match on the line) or `i` (ignore case)
  after it. `&` stands for the whole match and `\1` for a capture group, and
  any punctuation can stand in for `/` as the separator. Patterns are
  JavaScript regular expressions, not vim's.
- `:d` deletes the lines in the range, into the same register `p` pastes from.
- `:noh` clears the search highlight.

`u` undoes a whole `:s` or `:d` in one step.

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
