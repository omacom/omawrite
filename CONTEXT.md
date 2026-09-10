# Omawrite

A Markdown writing app. One window can hold several documents; each document is a tab.

## Language

**Window**:
A top-level Omawrite frame. It can hold several tabs and can exist alongside other windows.
_Avoid_: Screen, view, workspace

**Tab**:
One document’s place in a window’s tab strip. A window has one active tab.
_Avoid_: Pane, page, buffer

**Document**:
The Markdown being edited — optionally named by a file path, or never saved to a path.
_Avoid_: File (unless the path on disk is meant), buffer, note

**Session**:
The windows, tabs, and document contents that come back after a quit or a crash. Clean named documents belong to the session as paths; unsaved work belongs as cached text.
_Avoid_: Workspace, project, snapshot (that word is the old crash-only recovery)

**Unsaved work**:
Document text that is not the last saved file — never given a path, or named and dirty.
_Avoid_: Cache (the store, not the work itself)
