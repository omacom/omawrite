# Writing, visual blocks, attachments and shortcuts

This is the user guide for this feature-rich [Write/Omawrite fork](https://github.com/ponchoalv/PonchoWriter). See [copyright and dependency notices](../COPYRIGHT.md). Markdown remains the file format; this is not a rich-text document format or an Obsidian plugin.

## Files and windows

Launching the app without a file opens a fresh untitled window, including from the macOS Dock/Finder. Recovered drafts remain separate. Simply switching back to the app does not create a window. Opening a file already owned by this app activates its existing window without reloading. A different file opens separately; the file picker no longer replaces or discards the originating draft. Cmd/Ctrl+N always opens a fresh untitled window. Save As cannot overwrite a file owned by another window. Recovery drafts retain independent slots and do not get consumed by New Window.

On macOS, prefer `open -a "/path/to/Write.app" "/path/to/note.md"` rather than `-n` or `--args`. Windows/Linux executable launches forward file requests through the same local Qt broker. Quit older, un-routed copies before using this build. If forwarding fails, retry after closing old copies; do not delete recovery data or compatible lock files to force a second owner.

An empty Edit document shows the decorative **# Start writing** hint even with the caret focused. Typing or IME preedit hides it; clearing/cancelling restores it. Read mode hides it. It is never inserted into Markdown or undo history.

## Printing and PDF

**Cmd+P / Ctrl+P** opens **Print layout**, then the system print/PDF dialog. The default **Move wide tables to appendix** keeps long narrow tables inline with repeating headers, moves too-wide tables into landscape appendices and repeats column 1 across extra-wide column groups. Original staged/local images print without saving first. This uses a separate canonical Markdown snapshot, not the viewport or virtualized grid. Source/undo are unchanged; missing images require explicit placeholder confirmation. Mermaid prints as source; page ranges apply to the complete job, including appendices. Native drivers/physical printers and Windows/Linux remain unverified; QtPDF output was checked independently.

## macOS key names

Qt normally maps a source-code `Ctrl` modifier to **Command** on macOS and `Meta` to the physical **Control** key. The F1 reference now formats labels for the running platform. It also calls Alt **Option** on a Mac.

**Find & Replace uses Cmd+Option+F on macOS**, not Cmd+H, which macOS reserves for Hide App. Fullscreen uses the conventional **Cmd+Ctrl+F** on macOS. Linux keeps Ctrl+H and Super+F. F11 remains an alternate fullscreen binding; your keyboard/system may intercept function keys or require Fn.

| Action | macOS | Linux / source convention |
| --- | --- | --- |
| Open | Cmd+O | Ctrl+O |
| Save | Cmd+S | Ctrl+S |
| Save As | Cmd+Shift+S | Ctrl+Shift+S |
| New window | Cmd+N | Ctrl+N |
| Print/PDF snapshot (layout options, then system dialog) | Cmd+P | Ctrl+P |
| Undo | Cmd+Z | Ctrl+Z |
| Redo | Cmd+Shift+Z or Cmd+Y | Ctrl+Shift+Z or Ctrl+Y |
| Fullscreen | Cmd+Ctrl+F or F11 | Super+F or F11 |
| Find | Cmd+F | Ctrl+F |
| Find & Replace | Cmd+Option+F | Ctrl+H |
| Next search match | Cmd+G, or Return in Find | Ctrl+G, or Enter in Find |
| Previous search match | Shift+Return in Find | Shift+Enter in Find |
| Close search | Escape in search panel | Escape in search panel |
| Bold | Cmd+B | Ctrl+B |
| Italic | Cmd+I | Ctrl+I |
| Underline | Cmd+U | Ctrl+U |
| Strikethrough | Cmd+Shift+X | Ctrl+Shift+X |
| Inline code | Cmd+T | Ctrl+T |
| Fenced code block | Cmd+Shift+T | Ctrl+Shift+T |
| Insert Markdown link | Cmd+K | Ctrl+K |
| Insert reference link | Cmd+Shift+K | Ctrl+Shift+K |
| Insert footnote | Cmd+Option+K | Ctrl+Alt+K |
| Copy selection as HTML (whole document if no selection) | Cmd+Shift+C | Ctrl+Shift+C |
| Increase text size | Cmd+= or Cmd++ | Ctrl+= or Ctrl++ |
| Decrease text size | Cmd+- | Ctrl+- |
| Reset text size | Cmd+0 | Ctrl+0 |
| Change text size with wheel/trackpad | Cmd+scroll | Ctrl+scroll |
| Shortcut reference | Cmd+?, Cmd+/, or F1 | Ctrl+?, Ctrl+/, or F1 |
| Toggle Read/Edit | Cmd+Shift+M | Ctrl+Shift+M |
| Unsaved dialog: Save | Option+S | Alt+S |
| Unsaved dialog: Discard | Option+D | Alt+D |
| Unsaved dialog: Cancel | Option+C or Escape | Alt+C or Escape |

Usual cut/copy/paste/select-all keys come from Qt's text editor. There is no user-configurable keymap or command palette. Labels and automated tests do not establish that every combination is available on every keyboard or desktop configuration.

## Opening links

In **Edit**, ordinary clicking keeps editing. **Cmd-click on macOS / Ctrl-click elsewhere** opens the link through your OS; right-click a link and choose **Open Link** for the same action. Blank space does not activate the nearest link, and image menus take priority. **Read** keeps ordinary single-click links. Native macOS pointer/context checks pass; Windows/Linux remain untested.

### Reference links and footnotes

Both modes resolve full `[label][ref]`, collapsed `[ref][]` and shortcut `[ref]` links from `[ref]: https://example.com` definitions. Hover shows the URL; opening follows the same rules as inline links. Labels ignore case and repeated whitespace. Code and unresolved references stay literal.

Footnotes use `Text[^1]` and `[^1]: Note text` (indent continuation lines by four spaces). Hover over the marker to preview its text **without clicking**. Cmd/Ctrl-click in Edit jumps to its definition; a regular click in Read jumps to the numbered note. Both modes number notes by first use. Edit shows a small superscript number while the caret/selection is outside the marker; moving into it with the arrow keys or selecting it reveals the original `[^id]` syntax. Definitions and unresolved markers stay editable Markdown. Saved source is unchanged.

- **Cmd/Ctrl+Shift+K** inserts a reference link and a uniquely named definition. Selected text becomes the label; a clipboard URL becomes the destination. With clipboard prose instead, that text becomes the label and `https://` is selected for editing. Without either, the `link text` placeholder is selected.
- **Cmd+Option+K / Ctrl+Alt+K** inserts a footnote. Selected text is moved into the note; otherwise clipboard text becomes its body, or a selected `Footnote text` placeholder is provided.
- Marker and definition are inserted together: **one Undo** restores the original text. These shortcuts are Edit-only and do not operate inside an active table cell.

## Code formatting and automatic pairs

**Cmd/Ctrl+U** toggles underline using `<u>text</u>` (Markdown has no standard underline delimiter). Edit previews the underline and hides the HTML tags until the caret or selection touches the span; Read renders the same underline. Like bold/italic, this is Edit-only and one undo restores the original source.

**Cmd/Ctrl+T** toggles inline code around the selection; **Cmd/Ctrl+Shift+T** toggles a fenced code block. Empty selections put the caret inside a new pair/block. Fences occupy separate lines, adding newlines around a mid-paragraph selection when needed. Longer backtick delimiters protect literal backticks in the body. Both are Edit-only, retain the body selection and use one canonical undo. Multiple unrelated spans/blocks are wrapped safely rather than having outer delimiters stripped.

Typing `[` / `{` and straight quotes pairs them conservatively. Selections are surrounded; an existing closer is skipped; Backspace inside an empty pair deletes both. Word-interior apostrophes/escapes, paste and IME commit text are not automatically transformed.

### Literal punctuation and backticks

Outside code, a backslash escapes ASCII punctuation: `\*literal\*`, `\_name\_`, `\# not a heading`, and `\\` (one literal backslash). Letters and spaces are **not** backslash escapes. Use code for literal spacing rather than `\ `.

Inside inline code, backslashes are literal. To include backticks, choose a longer matching delimiter run; for example:

~~~markdown
`` C-x ` ``
~~~

This renders as a code span containing **C-x followed by a backtick**. The one padding space at each end is removed. Closing runs must match the opening length exactly; single and double backticks are not interchangeable. Edit hides the outer delimiters and optional padding while inactive, revealing them when the caret or selection touches the span; literal backticks inside remain visible. The original source is preserved, and switching to Read never rewrites the Markdown.

## Language and fonts

The footer's **Font** button opens a searchable font picker. UI translations follow upstream's `~/.config/omarchy/language` file (a language code on its first line, such as `de`); absent or unsupported translations fall back to English. Language changes update every open window without changing document text.

## Existing writing behavior

- Bold, italic, strike and link markers are caret-aware: source becomes visible when editing the relevant span.
- Heading levels 1–6 use the same relative font sizes in Edit and Read, including when zoom changes. Inactive headings hide their `#` markers; moving into the heading reveals them in the original muted gray, at the heading's size. A selection crossing a heading reveals its markers too.
- Return normally inserts two newlines (a paragraph break); Shift+Return inserts one. Blank lines and fenced code use one newline.
- Return continues lists and blockquotes; Return on an empty marker exits the list/quote.
- Tab / Shift+Tab indent eligible list items by up to four columns, not arbitrary code blocks.
- Selecting words and pasting a URL makes a Markdown link. Other text paste stays plain text.
- Find is literal and case-insensitive, with the existing accent-folding rules. Enter in the replacement field replaces the current match. **All** replaces all matches as one undo command.
- Named documents auto-save after a short pause. Untitled documents wait for an explicit save. Recovery drafts are not silently saved over files. External-change warnings pause auto-save until resolved.
- Font selection is in the footer. Font zoom is remembered, from 50% through 300%.

## Edit and Read

Use the upper-right **book/pencil** button or Cmd+Shift+M (Ctrl+Shift+M on Linux).

**Edit** keeps ordinary Markdown text editable. Supported pipe tables appear as native editable grids: click a cell, use Tab/Shift+Tab, or choose row/column actions from **⋯**, including left/center/right alignment of the current column. Drag a rectangular cell selection, type a value and Enter/Tab to fill all selected cells with that value (one undo); Backspace/Delete with grid focus clears it. IME-first input uses the same native editor, and stale source revisions invalidate rectangle coordinates safely. Cells expand as you type and hide their rendered label while being edited. **Source** reveals the original pipe Markdown; **Grid** or moving outside returns to the grid. Standalone local image paragraphs remain native visual blocks: click to reveal their source and move away to render them again. Selections crossing a block reveal source rather than copying an opaque picture. Standalone `---` separators (at the start or after a blank line) also render as full-width rules while inactive; clicking/navigating onto the line or selecting across it reveals the original Markdown. Code/HTML literals and Setext heading underlines are not converted into rules. Headings and existing inline emphasis retain caret-aware markers.

Ordinary explicit blockquotes hide `>` while inactive; click anywhere in that paragraph to reveal its exact source prefix (no replacement fringe). Quote prose is justified in both modes, and Read follows standard Markdown line breaks in quotes, just as in ordinary prose. Selection/search reveal markers too; styling never changes saved Markdown or undo. Code/HTML/list/table/ambiguous quote contexts conservatively retain fallback. This is automatic quote styling, not a global justification command.

Ordinary ordered/unordered lists now keep native hanging indentation in Edit, including wrapped lines and supported nested/continuation paragraphs. Inactive prefixes show bullets or automatic numbering; placing the caret in the prefix (or selecting/searching across it) reveals the exact source marker. The body column stays fixed even while editing that marker. This is view-only paragraph spacing and passive marker decoration: text, source positions and undo are unchanged. Ambiguous/tab-prefixed, quoted, code, HTML and table contexts conservatively retain source rather than pretending to be a complete Markdown parser; task checkboxes are not newly rendered.

Body paragraphs use softer, theme-aware contrast while headings retain the stronger theme foreground. Text stays regular-weight unless Markdown calls for emphasis; links and selections keep their own colors rather than being faded with the whole editor.

**Read** renders supported Markdown without an editing caret. Switching back with the book/pencil button or shortcut preserves the reading viewport and places a new caret near its center, instead of returning to the previous editing caret. A single source newline joins the surrounding prose; a blank line starts a new paragraph. End a prose line with a backslash or two spaces for an explicit line break without a paragraph gap. Paragraphs and headings retain Edit's blank-line breathing room; wrapped lines, code lines and tight list items stay compact. Links use the same theme accent, and bold, italic and code backgrounds match Edit. Links are clickable. This uses Qt's Markdown/HTML subset, not a web browser: arbitrary web CSS/layout is not supported. Remote images are deliberately not fetched. Unsupported or unavailable visual blocks remain usable source; rendering is never a Markdown conversion on save. One canonical source document owns saving, search, formatting and undo.

Edit now retains its native document during ordinary typing, updates affected parser/style regions and patches only changed visual objects. Decorations and native glyph rendering are viewport-aware for long documents. Source undo, save format and IME safeguards remain; global operations and Read import can still take document-sized work. This is not a blanket constant-latency promise.

Mode switches now retain the native Edit document/highlighter and reuse unchanged Read content. First Read after an edit still performs conversion. An independent viewport-safe caret avoids Qt's offscreen-cursor visibility latch without disabling long-document rendering optimization.

Try [preview-example.md](preview-example.md) for tables, images, Mermaid and fallback cases.

## Counts, remembered position and Reveal

The footer shows the existing document word count, plus **N selected** when text is selected in either mode. Selection counting is debounced and asynchronous; obsolete results are discarded. It uses the same rules as the existing total (including code/Markdown text; CJK characters count individually), not a new semantic prose parser.

Existing local files remember their mode and reading position between opens/restarts. Edit stores a source anchor, pixel offset and caret; Read stores a relative scroll position. Layout changes or changed file contents can make restoration approximate. Untitled drafts have no per-file position record. Save As keeps the current view and records the new filename. The local settings cache retains at most 100 files; positions are not written into Markdown or synced between machines.

Right-click in either mode and choose **Reveal in Finder**, **Reveal in File Explorer**, or **Reveal in File Manager**. Save untitled documents first. macOS/Windows request file selection; Linux requests `org.freedesktop.FileManager1.ShowItems` and falls back to opening the parent folder if unsupported. That fallback does not guarantee file selection. Windows/Linux implementations are source-reviewed but not runtime-tested here.

## Clipboard behavior

- **Cmd+C / Ctrl+C in Edit** copies selected Markdown, including hidden formatting markers. There is no redundant Copy Markdown command.
- **Standard Copy in Read** keeps Qt's rendered-selection behavior when selected. With nothing selected, keyboard or context-menu Copy copies the entire original Markdown from the canonical source—not reconstructed Markdown.
- **Cmd+Shift+C / Ctrl+Shift+C** copies only the selection as HTML in either mode; with no selection, it copies the whole rendered document. Edit renders the selected Markdown fragment, so selecting only part of a Markdown delimiter pair may not retain that formatting. Read preserves selected semantic emphasis/links while removing the writer's theme colors and font sizes. The context menu also offers **Copy as HTML**. Both `text/html` and `text/plain` contain the exported HTML markup: rich destinations can paste formatting, while a plain-text/code editor receives actual HTML source. Previously the plain-text fallback contained only rendered words, making HTML export look like ordinary Copy.

Code blocks and raw table source now use **local horizontal scrolling without wrapping** in both modes; prose still wraps. The horizontal bar stays at the visible bottom of a tall block. Code navigation to either line end (Mac Cmd+Left/Right) reveals the corresponding column. Supported tables use the same virtualized grid in both modes; Read disables cell editing, actions and Source. The square **⛶** button opens a larger-width/height view of the same grid, editable only in Edit. Close finishes a valid cell or waits for unresolved drafts/IME; scroll positions return to the inline view. Read links/autolinks work in either view. A prose selection across a Read grid exports that whole table as text/HTML rather than a slot; no-selection Copy still returns canonical Markdown. Cell text itself is not document-prose drag-selectable. Unsupported tables retain native fallback. Unresolved cell drafts block Save/close until corrected or explicitly copied/discarded. Attached native table bars reach either end and back after overdrag, respecting TableView's changing origin/end extents. Width/validation caches reduce repeated commit work without changing canonical undo. Typing immediately before a large table no longer reveals/re-collapses the entire raw table each key; raw Source reuses bounded native width/style work. Cold sizing, snapshots and global operations can still scale with document size; this is not a blanket constant-latency claim.

## Language-fence highlighting

Tagged backtick and tilde fences use native theme-aware syntax colors in **Edit and Read**. Use `python`/`py`, `cpp`/`c++`, `js`, `ts`, `json`, `bash`/`sh`, `yaml`, or another supported language name. Unknown/untagged code stays plain. Multiline tokens continue within a fence and reset at every separate fence, including adjacent same-language fences. Code keeps the writing font, zoom, background and search behavior; Markdown source and undo are unchanged. This adds token coloring, not autocomplete or LSP.

Fenced code now sits on **one continuous full-column panel**, including empty and long unwrapped lines, with generous, zoom-scaled padding on all sides, a subtle half-pixel border and small rounded corners in both modes. Presentation-only spacing keeps the padded backgrounds clear of neighboring prose and other code blocks. Edit includes the fence delimiters; Read omits them. Inline code keeps its small character background. Panels are native decorations behind selectable text, not images or extra document structure; font, source positions, selection, search and undo stay unchanged; long lines now use the panel's horizontal scrollbar.

Hover over a fenced-code panel in either mode to show **Copy** at its top right. It copies only the native code body as plain text through Qt's clipboard: no opening/closing fences or language tag, and no dependence on the current selection. Read mode also shows the first language token to the left of Copy; untagged blocks say `text`. Unknown language tags remain visible. A reserved header keeps these controls off the code; clicking Copy retains editor focus and selection.

Read validates its source/import alignment before baking colors into HTML; complex parser disagreements fall back to plain code rather than coloring the wrong text. See the [example document](code-highlighting-example.md) and [dependency/license status](../third_party/ksyntaxhighlighting/README.md). Highlighting needs no Node/browser/runtime download.

## Paste screenshots without saving them manually

On macOS, **Cmd+Ctrl+Shift+4** copies a selected screenshot region to the clipboard (an OS shortcut, not Write's). Then paste with **Cmd+V** into the document.

The app stages a PNG in persistent application data and inserts ordinary Markdown:

```markdown
![Screenshot](img/screenshot-unique-id.png)
```

The next successful save/auto-save copies it to `img/` beside the document. An untitled note keeps the staged image available to recovery; there is no upload or dependency on a disposable temporary file. A failed staging write inserts no image reference.

- Undo removes the reference, not the image file. Redo and references in other documents remain safe.
- Save As to a different folder copies supported local image dependencies and retained undo aliases before saving Markdown.
- Differing destination collisions, missing required images, unsafe traversal paths, or attachment/document destination collisions fail visibly instead of overwriting data. Byte-identical existing images can be reused.
- New names must be portable relative paths. Existing relative references with spaces, Unicode and percent encoding are supported conservatively. Absolute paths stay absolute rather than being copied.
- There is no global attachment database, Finder-move tracking, automatic vault-wide repair or file garbage collection. Old files are intentionally retained. Multi-file saves are not a transaction: a failed save can leave unused image copies, but should not replace a differing image.
- Externally changed image pixels are not watched automatically; reopen the document if its preview is stale.

### Optional image names and missing-image repair

Right-click a local image and choose **Copy Image** to put its actual pixels on the system clipboard, not its Markdown link. It works in **Edit** for standalone images/source paragraphs and in **Read** for native images, including inline/reference images. Copies retain original decoded dimensions and orientation rather than the smaller on-screen preview. No network fetch is attempted; unreadable/remote images or files beyond the 20 MiB / 24-million-pixel safety limits leave the clipboard unchanged.

In **Edit**, the same menu also provides:

- **Name / folder…** — choose a portable relative destination such as `img/architecture.png`. The app copies the image and updates matching references in the current document as one undo operation; originals are retained for undo and other notes.
- **Locate image…** — select the actual local file for a missing reference. It is staged under a safe relative name and the current document is updated; no vault-wide search/repair is attempted.

The keyboard Menu key or **Shift+F10** opens the same menu with the caret in an image paragraph. This is optional after paste; no naming dialog interrupts normal screenshot paste. Inline images inside text/tables remain outside the Edit naming/repair menu's scope. Read offers Copy Image only: naming/repair controls are hidden and disabled.

## Minimal spelling assistance

In **Edit**, right-click prose (or use **Shift+F10/Menu** at the caret). The
context menu's **Spelling** submenu has **Check spelling** and the installed
language choices. Pick the dictionary for your writing language; missing
languages must be installed through the OS or Linux package manager.

Flagged words offer suggestions, **Ignore for this session** and **Learn word**.
Accepting a suggestion is one undoable Markdown edit. Learn changes that
backend's personal dictionary and may affect other apps using it; it is not
needed to dismiss a one-off word. There is no autocorrect or automatic language
selection. Read has no spelling squiggles.

Sonnet Core uses NSSpellChecker on macOS, ISpellChecker on Windows and installed
Hunspell dictionaries on Linux. Code fences/inline code, Mermaid, URLs and image
paths are excluded. Rendered objects are never underlined. See the
[spelling example](spellchecking-example.md).
Windows/Linux runtime verification remains pending; macOS uses its installed
system service, with no dictionary download.

## Optional Mermaid

Mermaid is not built into Qt. If **`mmdc`** (Mermaid CLI) is already installed and available on the app's **PATH**, the fork can render a constrained set of local diagrams asynchronously. Check from the terminal:

```sh
command -v mmdc
./build/cmake/Write.app/Contents/MacOS/Write docs/preview-example.md
```

Launching from Finder may give a different PATH than your shell. Restart the app after changing renderer availability. Write does **not** install or download the renderer. Mermaid CLI normally needs its own Node/Puppeteer/Chromium setup: no embedded WebEngine does not mean no external browser runtime.

Rendering uses an external CLI on macOS/Linux and Windows. Windows resolves standard npm installs to direct Node invocation and contains the process tree in a kill-on-close Job Object; native Windows validation is pending (see [setup/checklist](install.md#windows)). Missing tools or failed containment keep Mermaid source. Document-provided configuration, HTML labels, click actions, styling directives, encoded escapes and remote-resource syntax are rejected by the constrained renderer. This conservative syntax filter is not an OS/browser sandbox; do not treat an external renderer as a general untrusted-code execution boundary. Missing CLI, invalid/incomplete/unsupported syntax and render failures leave source usable. There is no drag-node editor, whiteboard or arbitrary Mermaid graphical round-trip support.

## Limits and upgrades

Oversized or unsupported tables retain source rather than risking unbounded grid allocation. The grid is limited to 50,000 cells, 1,024 columns, 4 Mi UTF-16 units / 8 MiB UTF-8 per table and 65,536 units per cell. Unresolved cell drafts block Save/close; copy or explicitly discard them rather than assuming they were committed.

Quit older copies before upgrading. Existing drafts, attachments, settings and compatible instance locks may still use legacy application-data locations: retain them, and never delete them as part of build cleanup. Theme/font availability depends on the host desktop.

For dependencies, building and deployment use the single [build guide](install.md).
