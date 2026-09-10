# Assemble this folder

Type: grilling
Status: resolved

## Question

When a writer keeps a piece of writing as several Markdown files in one folder, how do they print or send the whole folder without maintaining a master of content-block paths?

## Answer

**File → Assemble this folder.** The named Document's directory is scanned. Files named as content blocks are absorbed into their parents. Leftover siblings concatenate in filename order. The receipt offers Print, Save as PDF, or Save Markdown. Unsaved work in open tabs wins over the last save. Untitled documents have no folder, so the action is disabled. The snapshot is saved in the parent directory (the folder's name as `.md` or `.pdf`) so the next assemble does not collate its own output. Session is untouched: no project file, no extra tabs.

## Comments

Implemented in [`a62cf5d`](https://github.com/frivas/omawrite/commit/a62cf5d488a07642ba3cd881989e090d46fced4d), PR [omacom/omawrite#59](https://github.com/omacom/omawrite/pull/59).

`Backend::assembleFolder` lists immediate `*.md` in the Document's folder, drops files that another file already includes, and concatenates the rest. Live tab text overlays disk. The QML receipt is `AssembleFolderDialog`. Shortcut: Cmd/Ctrl+Shift+P. Tests: `assemblesSiblingMarkdownInFilenameOrder`, `assemblesIncludedChaptersOnceThroughTheMaster`, `assembleUsesUnsavedTabTextThenDiskAfterReload`, `untitledDocumentCannotAssembleAFolder`, `savesAssembledMarkdownAndPdf`, `assembleFolderDialogListsTheReceiptOnTheWritingWindow`.
