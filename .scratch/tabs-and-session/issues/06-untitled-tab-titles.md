# Untitled tab titles

Type: grilling
Status: resolved

## Question

What is the title of a document with no path — a fixed “Untitled”, “Untitled 2”, first-line / heading, or something else — and how does that title relate to the window title?

## Answer

A document with no path is titled **Untitled**, then **Untitled 2**, **Untitled 3**, … in that window (and across the session so two windows do not both show a bare “Untitled”). The tab shows that title, not `Untitled.md`. The window title is the active tab’s title, with `*` when that document has unsaved work, then ` - Omawrite`. Titles do not follow the first line or heading — they would jump while typing.

## Comments

Implemented in [`ca901ef`](https://github.com/frivas/omawrite/commit/ca901efaf40e088877a84200316dc8d1f710026c), PR [omacom/omawrite#59](https://github.com/omacom/omawrite/pull/59).

`tabTitle` is Untitled / Untitled 2 / … across live windows. `fileName` stays `Untitled.md` for Save As. The window title binds to `tabTitle`. Test: `numbersUntitledTabsAcrossAWindow`.
