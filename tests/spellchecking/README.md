# Spellchecking tests and integration API

These standalone QtTest targets use the project's existing Qt and private Sonnet
installation from `spellchecking.pri`. Parent build/runner wiring belongs to the
main change. No additional packages, downloads, or personal dictionary writes
are needed by these tests.

## Run

From the repository root, use a separate shadow directory for each target:

```sh
root="$PWD"
mkdir -p /tmp/omawrite-spellchecking-tests
cd /tmp/omawrite-spellchecking-tests
qmake6 "$root/tests/spellchecking/spellchecking.pro"
make -j4
./tst_spellchecking
```

Use `qmake` if that is the Qt 6 command on your system, and the matching platform
build tool. Other targets follow the same pattern:

| Project | Executable | Coverage |
| --- | --- | --- |
| `spellchecking.pro` | `tst_spellchecking` | Pure classifier; injected fake dictionary; debounce, cache eviction/reuse, ordered batches, stale generations, language switches, unavailable dictionaries, ignores, learning, suggestions and composition |
| `integration.pro` | `tst_spellingintegration` | Real source/view TextEdits and both highlighters; no projection rebuild on async arrival, object-safe mapping, Read mode, IME events, merged/search formats, affected-block-only clearing, untouched source undo |
| `main.pro` | `tst_spelling_main` | Actual Backend/Main.qml with fake dictionary; one source undo/redo correction, stale action rejection, pointer/keyboard menus, installed-language controls, attachment priority |
| `native.pro` | `tst_native_spellchecking` | Static backend registration; opt-in macOS NSSpellChecker smoke with an installed English dictionary, known word, typo and suggestion completion |

GUI integration targets can run with `QT_QPA_PLATFORM=offscreen`. Fake tests
explicitly select their dictionary and isolate preferences from the user's app
settings. The controller-only suite requires no display server. CMake/CTest also builds `omawrite_native_spellchecking_tests`: its platform-plugin registration check always runs, independently of installed languages, so a discarded static import cannot masquerade as missing dictionaries. The real OS smoke needs a normal macOS GUI-capable session and is skipped by default:

```sh
OMAWRITE_NATIVE_SPELLCHECK=1 ./tst_native_spellchecking
```

It skips if no English OS dictionary is installed (`en`, `en_US`, or a regional
English ID). It never calls native `learn`. Actual suggestions may be empty.
Windows ISpellChecker and Linux Hunspell are **unverified**; fake tests do not
claim to verify either native backend.

## Source integration API

- `SpellingDictionary` is the small injectable native-adapter interface:
  `dictionaries`, `selectLanguage`, `isCorrect`, `suggestions`, and `learn`.
  Every adapter call, including construction/destruction, stays on the owning
  GUI thread. Tests inject it into `SpellingController` or `Backend`.
- `SpellingController::attach(QTextDocument*)` observes authoritative plain source.
  `ranges()` returns sorted UTF-16 source offsets only; `rangesChanged` is a
  paint notification, not permission to rebuild a presentation document.
  Text edits, attach, enabled/language changes, ignore and learn invalidate the
  generation immediately. Classification alone runs on a snapshot worker.
- The controller uses a 250 ms debounce, queued native batches of at most 64
  words / 4 ms (one OS call itself cannot be preempted), an 8,192-word reuse
  cache, and one paint publication per completed snapshot (not growing prefixes).
  A single worker snapshot can finish after invalidation but its result is discarded.
- `wordAt(int)` is cache-only and returns a generation/word/range token; it queues
  at most eight suggestions separately. `menuChanged` permits a validated menu
  refresh. `validate`, `ignore` and `learn` reject stale tokens.
  `Backend::replaceSpelling` revalidates, then uses the existing `replaceRange`
  source edit block, so one correction is one normal source undo operation.
- `LivePreview::spellingWordAt(int)` is the menu entry point: unlike generic
  source-position mapping, it rejects collapsed object segments. Spelling
  projection intersects only text spans. Existing `MarkdownHighlighter` merges
  underlines before search formatting; only changed blocks are rehighlighted.
  Its spelling-only pass suppresses formatting notifications, never changes page
  size, and does not schedule a projection rebuild. Read mode is unmarked.
- `setComposing` pauses native batches, menu lookups and result publication;
  LivePreview forwards real preedit lifecycle events.

## Deliberate limitations

`SpellingProse` is conservative, not a full CommonMark parser. It excludes nested
quote/list fences (including Mermaid), exact backtick runs, destinations,
reference definitions/IDs, autolinks/bare scheme URLs, images, tags/comments and
entities, but keeps ordinary visible link labels. Ordinary indented code is
recognized relative to list/quote containers, without hiding lazy paragraph
continuations; thematic breaks end paragraph state. Arbitrary raw HTML element
bodies are not fully recognized. Bracket matching is linear even
for malformed input. All positions are UTF-16, including surrogate pairs and
combining marks.

Ignore state belongs to one retained controller, is language-scoped, survives
file changes, and is neither shared with another process/controller nor saved
across restarts. Learn uses the OS personal dictionary and reports failure;
native learning is intentionally not exercised by tests. No autocorrection,
cloud service, downloadable dictionaries, or session-ignore API in Sonnet is
used. Settings persist only the enabled flag and selected installed language.
The initial locale matches hyphenated Windows IDs as well as underscore IDs,
with same-language fallback only; no unrelated language is silently selected.

Async classification has no injectable executor: most tests poll eventual
results. The outstanding-snapshot IME regression holds the shared worker slot
to force that scheduling interleaving and checks that a redundant debounce does
not duplicate ranges. Synthetic preedit tests do not verify a real platform IME
or native suggestion ranking.
