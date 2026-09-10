# Today’s document, window, save, and recovery model

Primary sources: `src/` and `tests/`. Graphify query expanded to vocab tokens `backend save autosave snapshot recovery restore untitled window document` (`graphify query`, plus `graphify explain` on `Backend` and `restoreRecovery`).

This is the model as of this research. Tabs and session are not implemented.

## One Backend, one QTextDocument, one window, one process

`main()` constructs a single `Backend` on the `QApplication`, exposes it as the QML context property `backend`, loads one `Main.qml` window, and parents that window onto the backend (`src/main.cpp` L76–L117).

`Backend` holds one `QPointer<QTextDocument> m_document` (`src/backend.h` L329). `attachDocument` takes the source editor’s `QQuickTextDocument`, stores `textDocument()`, installs the highlighter, then calls `restoreRecovery()` (`src/backend.cpp` L629–L655). The source `TextEdit` does that in `Component.onCompleted` (`src/Main.qml` L1477–L1479). The preview `TextEdit` never calls `attachDocument` (`src/Main.qml` L1484–L1487).

The window title is that backend’s one file: `(modified ? "* " : "") + fileName + " - Omawrite"` (`src/Main.qml` L19). Untitled display name is `Untitled.md` when `fileUrl` is empty (`src/backend.cpp` L222–L224).

There is no in-process second window. `newWindow()` is a second OS process (below). Graphify: `Backend` defines `save`, `saveForClose`, `restoreRecovery`, `newWindow`; `QTextDocument` is referenced from `src/backend.h` L329.

**Tabs/session:** this 1:1:1 identity is what a tabbed window must *replace*. Several windows remaining legal is a product rule, but today a window *is* a process *is* a backend *is* a document.

## Save, saveAsDialog, saveForClose, closeAfterSave

### `save`

- Empty/`invalid` `fileUrl` → `saveAsDialog()` (`src/backend.cpp` L722–L726). Test: untitled footer Save requests the dialog (`savesAndOpensFromFooterButtons`, `tests/tst_omawrite.cpp` L809–L811).
- Named but `m_pathNeverRead` and the path now exists → do not write; clear the close latch; emit `externalFileAppeared` (`src/backend.cpp` L735–L739). Tests: `asksBeforeAFirstSaveReplacesAFileThatAppeared` (L434), `doesNotAutosaveOntoANeverReadPath` (L1408), `asksOnlyOnceWhenWhatAppearedCannotBeRead` (L493).
- Otherwise `saveTo(m_fileUrl)` (`src/backend.cpp` L742).

Cmd/Ctrl+S and File → Save call `backend.save()` (`src/Main.qml` L192–L196, L471–L474). Footer Save does the same (`src/Main.qml` L1547).

### `saveAsDialog` / `saveAs`

`saveAsDialog` emits `saveDialogRequested(suggestedSaveUrl())` (`src/backend.cpp` L755–L757). QML opens a Save File dialog; accept → `backend.saveAs(selectedFile)`; reject → `fileDialogCanceled()` plus drop pending close/open (`src/Main.qml` L626–L636).

`saveAs` is `saveTo(url)` (`src/backend.cpp` L759–L761). `saveTo` writes UTF-8 via `QSaveFile`, updates known contents, `setModified(false)`, `clearRecovery()`, `saveSucceeded` (`src/backend.cpp` L1437–L1484). Non-local or failed open/commit: drop `m_closeAfterSave`, `saveFailed` (L1438–L1470). Last save directory is remembered in `QSettings` (`src/backend.cpp` L1479–L1480); test `remembersLastSaveDirectory` (`tests/tst_omawrite.cpp` L2912).

A path that is not on disk yet can still be this document’s name: `open` with `mayStartNewFile` loads blank text, sets `m_pathNeverRead`, does not create the file (`src/backend.cpp` L674–L695). First `save()` then writes there with no picker (`startsANewFileFromAPathThatIsNotThereYet`, `tests/tst_omawrite.cpp` L363, L387–L408).

### Close: two latches, QML uses one

`saveForClose`: if not modified, emit `closeAfterSave` immediately; else set `m_closeAfterSave` and call `save()` (`src/backend.cpp` L745–L753). Successful `saveTo` then emits `closeAfterSave` (`src/backend.cpp` L1473–L1487). `fileDialogCanceled` and failed/refused saves clear `m_closeAfterSave` (`src/backend.cpp` L763–L764, L737, L1439, L1448, L1467).

**QML never calls `saveForClose`.** Window close with `modified` rejects the close, sets `pendingAction = "close"`, and opens `UnsavedChangesDialog` (`src/Main.qml` L60–L67). Save on that dialog is `backend.save()` (`src/Main.qml` L655–L657). `onSaveSucceeded` runs `completePendingAction()`, which for `"close"` sets `closeConfirmed` and `close()` (`src/Main.qml` L118–L123, L400–L403). `onCloseAfterSave` exists as a second closer (`src/Main.qml` L395–L397) but is idle unless something invokes `saveForClose`.

Discard: `discardRecovery()` then complete the pending action (`src/Main.qml` L650–L652). Cancel: clear `pendingAction` (`src/Main.qml` L659).

A save that does not happen must drop the pending close so a later Save does not close the window (issue #23). Tests: `dropsThePendingCloseWhenASaveFails` (`tests/tst_omawrite.cpp` L1169), `dropsThePendingCloseWhenTheSaveIsRefused` (L537). QML `onSaveFailed` and `onExternalFileAppeared` clear `pendingAction` (`src/Main.qml` L406–L411, L421–L426). Backend also clears `m_closeAfterSave` on `externalFileAppeared` (`src/backend.cpp` L737).

Clean close: `onClosing` returns immediately if `!backend.modified` (`src/Main.qml` L61–L62). No session write on quit. `Backend::~Backend` is default (`src/backend.cpp` L216); the recovery lock is released, leftover JSON only if nobody called `clearRecovery`.

**Tabs/session:** keep the “failed/refused save must not carry a later close” invariant. Replace window-level `pendingAction` / `saveForClose` / `closeAfterSave` with a per-tab close. Product hot-exit says *quit* must not prompt; today’s prompt is on *window* close and is the only unsaved-work gate.

## Autosave vs untitled snapshot

One debounce timer (`m_recoveryTimer`, interval = `autosaveDelayMs`, default 750 ms, clamped 200–60000) fires `persistDocument` (`src/backend.cpp` L176–L178, L1490–L1528; settings L171–L175). Any edit that leaves the buffer dirty calls `scheduleRecovery()` (`src/backend.cpp` L1323–L1325). Matching the known baseline clears modified and `clearRecovery()` (L1315–L1317). Empty untitled is clean; emptying a named file is dirty (`revertsEmptyDocumentToCleanState`, `tests/tst_omawrite.cpp` L2940–L3015).

`persistDocument`: if not modified, return. If `canAutosaveToFile()`, `saveTo(m_fileUrl)` (named-file autosave). Else `writeRecovery()` (snapshot) (`src/backend.cpp` L1514–L1528).

`canAutosaveToFile` is false when autosave is off, URL is not a local file, an external change is unanswered, or `m_pathNeverRead` (`src/backend.cpp` L1494–L1511; header comment L182–L183).

Tests:

| Test | Fact |
|------|------|
| `autosavesANamedFileOnceTypingStops` (`tests/tst_omawrite.cpp` L1304) | Named file, autosave on: debounce writes the document path, `modified` becomes false. |
| `snapshotsUntitledDocumentsInsteadOfSavingThem` (L1337) | Untitled: `!canAutosaveToFile()`; snapshot exists; still modified; JSON `text` set, `fileUrl` empty; no invented user file. |
| `doesNotAutosaveOverAnUnansweredExternalChange` (L1362) | Watcher sets unanswered; debounce must not overwrite disk; `keepExternalVersion` re-allows autosave. |
| `doesNotAutosaveOntoANeverReadPath` (L1408) | Named-but-never-read: debounce does not write; explicit `save()` asks. |
| `turnsAutosaveOffAndRemembersIt` (L1447) | Autosave off still writes the crash snapshot; does not write the user file; setting persists in `QSettings`. |
| `boundsTheAutosaveDelayAndRemembersIt` (L1487) | Delay bounds and persistence. |

`writeRecovery` writes compact JSON `{fileUrl, pathNeverRead, text}` to `recoveryPath()` (`src/backend.cpp` L1542–L1556). Tests: `writesTheNeverReadPathIntoTheSnapshot` (L681).

**Tabs/session:** treat named-file autosave and its guardrails as *constraints* (standing preference: named autosave stays; untitled never autosaves onto a user path). Treat “the debounce’s other branch is a per-window crash JSON” as *replace* with session/cache storage.

## restoreRecovery, recoveryPath, clearRecovery

On construct, the backend claims a slot under `QStandardPaths::AppDataLocation`: `recovery-{0..99}.json` plus a `QLockFile` on `.lock`. Pass 0 prefers slots that already have a snapshot (orphaned crash); pass 1 takes an empty slot. Comment: a crash in window 2 is still recovered if window 1 exited normally (`src/backend.cpp` L123–L141). Public `recoveryPath()` is that claimed JSON (`src/backend.h` L186–L188, `src/backend.cpp` L1530–L1531). Cap: 100 concurrent backends.

`restoreRecovery` runs from `attachDocument` (`src/backend.cpp` L655). If the JSON has `text`, it loads it, restores `fileUrl`, sets known contents from disk if readable, restores `pathNeverRead` from the snapshot (or true if the named file cannot be read), `setModified(true)`, status `"Recovered unsaved changes"` (`src/backend.cpp` L1559–L1586). Test: `remembersANeverReadPathAcrossRecovery` (L730) — after restore, `save()` still emits `externalFileAppeared` if a file arrived while down (L778–L781).

`clearRecovery` stops the timer and deletes the JSON (`src/backend.cpp` L1588–L1590). Called after successful open of an existing or new-named file (L687, L712), successful `saveTo` (L1483), discard (`discardRecovery` L767–L768), and when the buffer matches the known baseline (L1317).

This is crash recovery, not session restore. A normal quit after save or discard leaves no snapshot. A crash leaves the JSON because `clearRecovery` never ran. Geometry is a single `window/x|y|width|height|maximized` in `QSettings`, last window to destroy wins (`src/backend.cpp` L1376–L1394, `src/Main.qml` L1767–L1776) — not a multi-window session.

**Tabs/session:** replace slot-per-process crash JSON and single geometry blob with a full session (windows, tabs, dirty named, untitled cache). Keep `pathNeverRead` / “file appeared” across restore as a *constraint* if never-saved named tabs exist.

## newWindow and OmawriteApplication file-open

`newWindow()` calls `launchNewInstance()` with no path; on failure, status `"Could not open a new window."` (`src/backend.cpp` L1252–L1254). `launchNewInstance` on macOS `open -n -a <bundle>` (plus `--args` file if given); otherwise `startDetached` of this executable (`src/backend.cpp` L82–L84 in `src/backend.h`, implementation L1229–L1249). File → New Window and StandardKey.New both call it (`src/Main.qml` L316–L320, L460–L463). Shortcuts dialog copy: “New Window” (`src/Main.qml` L690). Bundle path helper is tested by `findsTheAppBundleAroundAMacExecutable` (`tests/tst_omawrite.cpp` L1222).

`OmawriteApplication` swallows `QFileOpenEvent`, queues URLs until a handler is set (`src/main.cpp` L21–L47). After the window loads: if argv has a path and the backend is not modified, `backend.open` that file (`src/main.cpp` L119–L121). Finder opens: if the URL is already this `fileUrl`, ignore; if the window is modified *or already showing a file*, `launchNewInstance(path)`; else `backend.open(url)` (`src/main.cpp` L123–L134). Comment: “Keep one document per window, matching the existing New Window behavior.”

Open of another file in the *same* window goes through QML `requestOpen`: if modified, unsaved dialog then `backend.open` (`src/Main.qml` L70–L77). `open` replaces this backend’s document (`src/backend.cpp` L662–L719).

**Tabs/session:** replace process-spawn `newWindow` and “one document per window” file-open. OS file-open and argv still need a home (tab vs window vs new window) — that is ticket 02, not invented here. Standing preference: Cmd/Ctrl+N opens a *tab*; a new window is a separate action.

## Constraints vs replace

### Treat as constraints (preserve unless a later ticket explicitly overrides)

- Cmd/Ctrl+S saves the **active** document; untitled / empty URL opens the picker (`save` → `saveAsDialog`).
- Named-file autosave when `canAutosaveToFile()`; never autosave onto a never-read path; never autosave over an unanswered external change.
- Untitled (and any document the guardrails hold back) is **not** written to a user-chosen path by the debounce; work still leaves memory (`writeRecovery` / equivalent cache).
- Turning autosave off still snapshots; it is not a choice to lose work on a crash (`turnsAutosaveOffAndRemembersIt`).
- First write to a never-read path must ask if a file appeared; ask once (`asksBeforeAFirstSaveReplacesAFileThatAppeared`, `asksOnlyOnceWhenWhatAppearedCannotBeRead`).
- Reload must not blank the only copy if the file vanished (`keepsTheDocumentWhenReloadRacesADeletion`).
- A save that fails or is refused must not leave a close/open intent that a later save carries out.
- Local files only for open/save (`openPath` / `saveTo`).
- Several windows remain legal (today: several processes; later: several windows with tabs).

### Must replace for tabs + session

- One `Backend` + one `QTextDocument` + one `ApplicationWindow` + one process.
- Recovery as `recovery-N.json` locked per backend (max 100), restored only in `attachDocument`, cleared on save/open/discard — crash snapshot, not hot-exit session.
- `newWindow` / Finder open spawning another process to keep one document per window.
- Cmd/Ctrl+N = New Window (`StandardKey.New` → `backend.newWindow()`).
- Unsaved prompt on **window** close as the only place work is thrown away; quit has no session write.
- `saveForClose` / `closeAfterSave` as a window-level latch (and QML `pendingAction` as the live equivalent).
- Single `window/*` geometry in `QSettings`.

`saveForClose` is implemented on `Backend` but unused by `Main.qml`; the live close-after-save path is QML `pendingAction` + `save()`. A tabs design should not revive the unused latch as-is; it should replace both with per-tab close.
