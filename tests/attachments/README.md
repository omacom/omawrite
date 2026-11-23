# Attachment backend tests and integration

Run in a separate build directory (Qt 6 with QtTest, Qt Quick, PrintSupport):

```sh
mkdir -p /tmp/omawrite-attachment-tests
cd /tmp/omawrite-attachment-tests
qmake6 /path/to/omawrite/tests/attachments/attachments.pro
make -j4
QT_QPA_PLATFORM=offscreen ./tst_attachments  # macOS: ./tst_attachments.app/Contents/MacOS/tst_attachments
```

Backend API for the editor/preview worker:

- `pasteClipboardImage(proposedRelativePath = "")` returns Markdown, or empty if
  no image or an error. Insert the result using the editor's normal undoable
  insertion. Default: `![Screenshot](img/screenshot-<uuid>.png)`. Proposed paths
  must be unused, portable relative PNG paths (ASCII letters/digits and `_`,
  `-`, `/`, `.`, with no empty, hidden, dot, or traversal components).
- `resolveImageUrl(path)` returns a local file URL, preferring recovery staging
  before first save and the document-relative disk copy afterward. Empty means
  disallowed/missing. Remote URLs and file URLs with hosts are rejected. Preview
  must also independently block network resources rather than fall back to the
  original URL. Watch `attachmentUrlsChanged` and `fileUrlChanged` to re-resolve;
  these methods are not reactive QML properties.
- `renameAttachment(oldPath, newRelativePath)` changes an image's logical path,
  including moving to a subdirectory. `locateAttachment(oldPath, localFileUrl,
  newRelativePath = "")` copies a selected local replacement and retargets it to
  a fresh generated path by default. Both return success/failure, update matching
  Markdown image references in the current document in one undo block, and
  never delete originals. Errors use the existing backend `status` property.
  The source scanner is confirmed against Qt's Markdown parser so code examples
  are not rewritten. Unsupported image syntax is refused rather than partially
  retargeted. These APIs rely on the existing editor text-change notification
  for modified-state/auto-save handling, just like `replaceRange`.

Safety and limitations:

- Unique immutable PNG/assets live under AppDataLocation/attachments; recovery
  JSON stores logical paths and validated storage IDs plus materialized paths.
  Existing recovery debounce still applies to the text/map snapshot.
- Save/auto-save install staged assets before Markdown. Save As copies local
  Markdown/Qt-supported HTML image dependencies and retained undo aliases.
  Existing byte-identical files are reused; differing collisions, missing
  sources, traversal paths, and symlink destination components fail safely.
  Existing relative paths may contain Unicode, spaces, escaped parentheses,
  and percent encoding; unsafe `../` paths are refused on Save As.
- Multiple-file save is not a transaction. Failure can leave unused copied
  images/directories, but does not knowingly replace differing images. The
  existing Markdown QSaveFile direct-write fallback remains unchanged.
- No global attachment ownership/tracking or deletion/garbage collection is
  claimed. Staging and old disk images are retained intentionally for undo and
  other documents. Already-saved images edited externally are used from disk,
  not overwritten from stale staging. There is no filesystem image-change
  watcher/cache invalidation for preview in this backend feature.
- HTML image references can be copied by Save As but are not source-rewritten
  by rename/locate. Inline and reference-style Markdown images are supported;
  unusual syntax may require manual editing. Absolute references remain absolute.
- Filesystem checks are defensive against ordinary collisions/symlinks, not a
  sandbox against hostile concurrent directory replacement. There is no remote
  downloading or rendering in these helpers.
