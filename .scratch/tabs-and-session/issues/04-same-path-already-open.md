# Same path already open

Type: grilling
Status: resolved
Blocked by: 02

## Question

If a path is already open as a tab (this window or another), does Open / OS-open focus that tab, or may the same file appear in two tabs?

## Answer

**Focus the existing tab.** Raise its window if it is in another window. Do not open a second tab on the same path, so two buffers cannot diverge. Untitled documents have no path, so they never match this rule.

## Comments

Implemented in [`ca901ef`](https://github.com/frivas/omawrite/commit/ca901efaf40e088877a84200316dc8d1f710026c), PR [omacom/omawrite#59](https://github.com/omacom/omawrite/pull/59).

`open` looks up the path in this window, then in other live `Backend`s, and activates that tab (`requestActivate` on the parent window). Test: `focusesAnAlreadyOpenPathInsteadOfDuplicatingIt`.
