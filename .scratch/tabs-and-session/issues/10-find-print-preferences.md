# Find, print, and preferences vs the active tab

Type: grilling
Status: resolved

## Question

Do find, print, and other “this document” commands apply only to the active tab? Are preferences per tab or still app-wide?

## Answer

Find, replace, print, Save, and the other document commands apply to the **active tab**. Preferences stay **app-wide** as today (font, autosave, theme, measure) — not a per-tab settings sheet.

## Comments

Implemented in [`ca901ef`](https://github.com/frivas/omawrite/commit/ca901efaf40e088877a84200316dc8d1f710026c), PR [omacom/omawrite#59](https://github.com/omacom/omawrite/pull/59).

Save, find, and print still talk to the one `Backend` document, which is always the active tab. Preferences are unchanged window-level settings. No extra test beyond the existing save/find/print suite, which now runs against the active tab.

Assemble this folder is a folder export, not a second Print of the active tab. See [Assemble this folder](11-assemble-this-folder.md).
