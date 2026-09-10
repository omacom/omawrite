# Tab strip

Type: prototype
Status: resolved

## Question

How should the tab strip look and behave in a window that already has Omawrite’s editor, preview, and footer — enough to react to: titles, the active tab, close, unsaved-work marking, and many tabs?

## Comments

Throwaway prototype (not production QML): [prototype/tab-strip.html](../prototype/tab-strip.html)

- `?variant=A` — Top chrome tabs
- `?variant=B` — Leading document rail
- `?variant=C` — Title + overflow menu

Arrow keys and the bottom bar cycle variants. Closing the last tab in the prototype shows the “window would close” empty state.

## Answer

**Variant A — top chrome tabs.** A strip above the editor (below the window chrome): title per tab, accent on the active tab, a dirty dot for unsaved work, close on the tab. Many tabs scroll the strip horizontally rather than switching to a sidebar (B) or a title-only overflow (C). Asset: [prototype/tab-strip.html](../prototype/tab-strip.html)?variant=A.

## Comments

Shipped in QML in [`ca901ef`](https://github.com/frivas/omawrite/commit/ca901efaf40e088877a84200316dc8d1f710026c), PR [omacom/omawrite#59](https://github.com/omacom/omawrite/pull/59).

`Main.qml` has a horizontal `tabStrip` above the editor: title, accent underline on the active tab, dirty dot, close control. Overflow is a scrolling `ListView`. The HTML prototype stays as the visual record. Test: `tabStripIsOnTheWritingWindow`.
