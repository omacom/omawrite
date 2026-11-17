# Tests

Build/dependency/kit requirements are in [install.md](install.md).

```sh
./bin/test                 # CMake/CTest: core + extended editor offscreen suites
./bin/test --rendered
./bin/test --native-spelling
```

For an existing CMake build: `ctest --test-dir build/cmake --output-on-failure`. CTest checks that the platform's static spelling plugin is registered even without installed dictionaries. The real macOS dictionary smoke is opt-in: `OMAWRITE_NATIVE_SPELLCHECK=1 build/cmake/omawrite_native_spellchecking_tests`. Native probes also retain their qmake harnesses.

On Windows: `build.bat --test` runs the main editor and Mermaid suites, not the complete Unix suite set. Native tests need an interactive desktop; focus failures in a headless session are not necessarily editor regressions. Missing system spelling dictionaries can cause expected skips.

Packaging smoke checks: `powershell -NoProfile -File tests/windows-packaging.ps1` (also supports `pwsh` on macOS/Linux). These use fake executables/compiler, check paths/guards and parse scripts; they do not produce or run a real installer. Run the same checks with Windows PowerShell 5.1 before releasing.

For a manual pass, open [preview-example.md](preview-example.md), [code-highlighting-example.md](code-highlighting-example.md) and [spellchecking-example.md](spellchecking-example.md). Use disposable documents for save/reopen/undo, table cell drafts and Source/Grid, large table scrolling, screenshot attachments/Save As, external changes/recovery, Print/PDF, high-DPI shortcuts and IME. Do not Learn spelling words unless you intend to change the system dictionary.

Performance benchmarks are opt-in; see the `tableGridNativeEditingBenchmark` and long-document cases in `tests/tst_editor.cpp` for environment controls. Use a copy of private files. Frame-submission timings are not display latency or FPS, and spelling/cold-open/scroll workloads need separate checks. Never describe fake Mermaid helpers as an end-to-end Node/browser test.
