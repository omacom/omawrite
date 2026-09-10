# Today’s document, window, save, and recovery model

Type: research
Status: resolved

## Question

What is today’s model for a window, a single document, Save, autosave, crash snapshot / `restoreRecovery`, and `newWindow` — citing `src/` and tests — and which of those facts a tabs + session design must treat as constraints or replace?

## Answer

One process is one window, one `Backend`, and one `QTextDocument`. Save and named-file autosave write that document; untitled and guarded paths get a per-window crash JSON restored in `attachDocument`. `newWindow` and Finder open spawn another process. Tabs must keep the autosave/never-read/failed-close guardrails and replace the 1:1:1 identity, crash slots, and process-spawn New Window with a session.

Findings: [`../research/today-document-window-save-recovery.md`](../research/today-document-window-save-recovery.md) (committed on `research/today-document-window-save-recovery`).

## Comments

The replace list from this research is what [`ca901ef`](https://github.com/frivas/omawrite/commit/ca901efaf40e088877a84200316dc8d1f710026c) (PR [omacom/omawrite#59](https://github.com/omacom/omawrite/pull/59)) actually changed: many tabs per `Backend`, `session.json` instead of `recovery-N.json`, Cmd+N as new tab. Named autosave, never-read, and failed-close guardrails stayed.

[`3ba8f6e`](https://github.com/frivas/omawrite/commit/3ba8f6ee5389cd31bb71ec345f8b666cfbbad69b) finishes the last replace: New Window and a drop off the strip spawn another `Backend` in this process. Launch still restores `storedWindowCount()` windows from `session.json`. Tests: `newWindowAsksForAnotherWritingWindow`, `restoresTwoWindowsFromTheSessionFile`, `quittingKeepsEveryWindowInTheSession`.
