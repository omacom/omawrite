# Closing the last tab

Type: grilling
Status: resolved

## Question

When the user closes the last tab in a window (Cmd/Ctrl+W or the tab’s close control), does that window close, or does it keep one empty untitled tab?

## Answer

The window **closes**. Omawrite does not invent an empty Untitled tab. Unsaved work on that last tab still gets save / discard / cancel; discard or successful save then closes the window. If it was the last window, that is quit (hot exit of whatever other windows already went away — here, none).

## Comments

Implemented in [`ca901ef`](https://github.com/frivas/omawrite/commit/ca901efaf40e088877a84200316dc8d1f710026c), PR [omacom/omawrite#59](https://github.com/omacom/omawrite/pull/59).

`closeTab` on the last tab clears the strip, writes the session without this window, and emits `closeWindowRequested`. QML closes the window. Cmd/Ctrl+W and the tab × use `requestCloseTab`, which prompts when the tab is dirty. Test: `closingTheLastTabAsksTheWindowToClose`.

[`3ba8f6e`](https://github.com/frivas/omawrite/commit/3ba8f6ee5389cd31bb71ec345f8b666cfbbad69b) covers the QML window itself: `closingTheLastTabClosesTheWritingWindow`, and `closingADirtyTabAsksBeforeDroppingIt` for the unsaved dialog then discard.
