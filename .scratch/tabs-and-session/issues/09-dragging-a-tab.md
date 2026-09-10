# Dragging a tab

Type: grilling
Status: resolved

## Question

What happens when a tab is dragged out of its window — into another Omawrite window, or as a new window?

## Answer

The document **moves**. Dropping it on another window **adds it to that window’s tab strip** (alongside tabs already there); it does not replace them. If the drop creates a new window, that window’s strip is just this tab. Leaving a window with no tabs closes the source window. Same-path still holds: if the destination already has that path, focus that tab and do not keep two.

## Comments

The move itself is `Backend::adoptTabFrom` in [`ca901ef`](https://github.com/frivas/omawrite/commit/ca901efaf40e088877a84200316dc8d1f710026c), PR [omacom/omawrite#59](https://github.com/omacom/omawrite/pull/59).

The destination strip gains the tab. An empty source emits `closeWindowRequested`. Same path on the destination focuses that tab instead of duplicating. The C++ seam is tested by `movesATabOntoAnotherWindow`.

[`3ba8f6e`](https://github.com/frivas/omawrite/commit/3ba8f6ee5389cd31bb71ec345f8b666cfbbad69b) wires the strip: release on another window calls `finishTabDrag`, a drop off every window emits `detachTabRequested` and `takeDetachedTab` so the new strip is just that tab, and a drop on the same strip calls `moveTab`. Tests: `droppingATabOntoAnotherWindowMovesIt`, `droppingATabOffEveryWindowDetachesIt`, `reordersTabsInTheSameWindow`.
