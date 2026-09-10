# Tabs and session

## Destination

Omawrite windows hold tabs. Cmd/Ctrl+S saves the active document. A normal quit is hot exit: the session comes back on launch (every window, its tabs, unsaved work). Closing a tab drops it from the session, with a save / discard / cancel prompt if it has unsaved work.

## Notes

- Domain language: [`CONTEXT.md`](../../CONTEXT.md). Consult `/domain-modeling` when a term shifts.
- Skills: `/graphify` for how the current code is shaped; `/grilling` and `/prototype` on HITL tickets.
- Tracker: local markdown under `.scratch/tabs-and-session/`.
- Standing preferences already locked while charting (not restated on tickets):
  - Tabs live **inside** windows; several windows stay legal. Cmd/Ctrl+N opens a **tab** in the current window. A new window is a separate action.
  - Restore is a **full session**. Never-saved documents return from cache; dirty named tabs return dirty; clean named tabs reload from disk.
  - **Hot exit**: quit does not ask to pick a file. Close-tab is where work is thrown away.
  - Named-file **autosave stays as today**. Untitled documents never autosave onto a user-chosen path. Cmd/Ctrl+S saves the **active** tab (picker if it has no path).
- Graphify (code graph, no `tab` vocabulary): one `Backend` and one `QTextDocument` per window; `save` / `saveForClose` / `restoreRecovery` / `newWindow` on that backend; untitled work is snapshotted (`snapshotsUntitledDocumentsInsteadOfSavingThem`); `OmawriteApplication` in `src/main.cpp` owns window-level file-open.

## Decisions so far

- [Today’s document, window, save, and recovery model](issues/01-today-document-window-save-recovery.md) — One window is one Backend/document/process; named autosave and crash snapshots stay as constraints, process-spawn New Window and crash-slot restore must be replaced.
- [Open and OS-opened files](issues/02-open-and-os-opened-files.md) — Cmd/Ctrl+O and OS-open add a tab in the current window; launching by opening a file makes that file the first tab.
- [Closing the last tab](issues/03-closing-the-last-tab.md) — Closing the last tab closes the window; no phantom Untitled tab.
- [Same path already open](issues/04-same-path-already-open.md) — Open focuses the existing tab (and its window); no second tab on the same path.
- [Untitled tab titles](issues/06-untitled-tab-titles.md) — Untitled, Untitled 2, …; window title is the active tab’s title, with * if unsaved.
- [Tab strip](issues/05-tab-strip.md) — Top chrome tabs (prototype variant A): titles, active accent, dirty dot, close on the tab; overflow scrolls.
- [Session store](issues/07-session-store.md) — One AppData `session.json` for all windows/tabs; text only for unsaved work; replaces `recovery-N.json`.
- [Dragging a tab](issues/09-dragging-a-tab.md) — Drop on another window adds the tab to that strip; a new window gets only that tab; empty source window closes.
- [External change with several tabs](issues/08-external-change-with-several-tabs.md) — Dialog and unanswered-change guard belong to the tab whose path changed; switch to that tab; queue if several files change.
- [Find, print, and preferences vs the active tab](issues/10-find-print-preferences.md) — Document commands hit the active tab; preferences stay app-wide.

Implementation: [`ca901ef`](https://github.com/frivas/omawrite/commit/ca901efaf40e088877a84200316dc8d1f710026c), [`3ba8f6e`](https://github.com/frivas/omawrite/commit/3ba8f6ee5389cd31bb71ec345f8b666cfbbad69b), PR [omacom/omawrite#59](https://github.com/omacom/omawrite/pull/59). Each ticket's Comments block names what shipped.

## Status

Shipped on `macos`. The destination above is in the app. Do not open a new wayfinder for tabs and session unless a bug shows up in use.

## Follow-on

- [Assemble this folder](issues/11-assemble-this-folder.md) — File menu collates the named Document's directory into one printable or saveable document. Content-block includes are absorbed once; leftover siblings concatenate in filename order. Unsaved tabs win. Untitled has no folder.

## Not yet specified

- (none — the way to the destination is specified)

## Out of scope

- Tab groups, split panes, or a single-window-only app.
- Syncing the session across machines.
- Changing Markdown rendering, content blocks, or the highlighter.
