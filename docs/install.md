# Build, dependencies and deployment

## Windows

From CMD in the checkout:

```bat
setup.bat -Install -Installer
build.bat --check --installer
build.bat --test --installer
```

`setup.bat` alone only checks; `-Install` opts into winget downloads, license agreements and possible UAC prompts. It installs missing tools, downloads a reproducible **Qt 6.12.0 MSVC 2022 x64** kit through aqt/Python, and records that kit in ignored `build/windows-setup.json`. Later setup runs reuse the recorded kit unless overridden; existing SDK directories are not moved when the app is renamed. Override with `-QtVersion 6.x.y -QtRoot C:\Qt`, or set `QT_DIR` to an existing kit. Qt downloads depend on archive availability; review Qt licensing before use. Reopen terminals after installation. Automatic setup supports x64 only.

Dependencies: Git, CMake **3.24+**, Ninja, native Windows Perl (Strawberry), Visual Studio **2022 Build Tools with C++ workload and Windows SDK**, Qt **6.7+** desktop, and the official **VC++ x64 Redistributable**. Python 3.12/aqt is needed only to download Qt; Inno Setup **6.3+** only for installers. Qt Creator is an optional IDE, not a compiler or dependency.

| Qt kit | Compiler / application build tool | Architecture |
| --- | --- | --- |
| `msvc2022_64` | VS 2022 MSVC / `nmake` | x64 Developer Prompt (`VsDevCmd.bat -arch=x64 -host_arch=x64`, one line) |
| `mingw_64` | **matching Qt-supplied MinGW version** / `mingw32-make` | x64; set `MINGW_DIR` to compiler directory, without `\bin` |

For an existing Qt 6.12 installation, set `QT_DIR=C:\Qt\6.12.0\msvc2022_64` (use its actual installed path, or a matching MinGW kit); explicit `QT_DIR`/`QMAKE` overrides the recorded kit. Changing the setup default does not override an existing selection. The application minimum remains Qt 6.7. The script initializes MSVC x64 if no compiler is on PATH. A pre-existing x86 prompt must be replaced with the correct Developer Prompt. **An x86 compiler cannot link x64 Qt**; CMake's resulting “incompatible version” error can be an architecture mismatch, not a Qt-version problem. Keep the Qt compiler ahead of Strawberry's unrelated GCC. Do not mix MSVC/MinGW, Debug/Release, CRTs or architecture caches; toolchain stamps reject changed Windows kits. Missing Vulkan is optional, not a build blocker.

`build.bat --check` validates tools/Qt/compiler architecture without building. Normal builds bootstrap pinned KDE dependencies privately and build Release. Windows still uses the qmake project `omawrite.pro`; macOS/Linux default to CMake. Custom static dependency installations use `OMAWRITE_SYNTAX_PREFIX` and `OMAWRITE_SPELL_PREFIX`. `--test` runs the main and Mermaid suites; it does **not** run all Unix suites. `--deploy` collects the matching Qt runtime, QML plugins and notices into `build/windows-<kit>-qt<version>-<arch>/dist`. Keep the **entire folder**. Existing dist folders are refused: move/remove only generated output before rebuilding.

`--installer` implies `--deploy` and compiles `packaging/windows.iss`, producing `installer/Write-<version>-windows-x64-setup.exe` beside `dist`. It is an **unsigned, per-user x64 installer** with Start Menu/optional desktop shortcut and uninstaller; it does not change file associations. Compiler discovery verifies the required 6.3+ features with a syntax-only probe in an isolated temporary directory; it does not rely on the executable's often-zero file version or create/run an installer. For a custom path use CMD's `set "ISCC=C:\Tools\Inno Setup 6\ISCC.exe"`; clear an override with `set ISCC=` to use automatic discovery. The installed app still needs the **VC++ Redistributable** for MSVC builds; setup installs it on the developer machine, but the generated installer does not bundle it and blocks MSVC installation when its registry prerequisite is absent. Use the latest supported redistributable; the presence check does not certify that an older runtime is sufficient. Static Qt is not used: this is a bundled deployment, like a typical macOS `.app`.

Optional diagrams: `setup.bat -Install -Mermaid` installs Node LTS and global Mermaid CLI (which may download Chromium). Alternatively run `npm install --global @mermaid-js/mermaid-cli` with an existing Node installation. Ensure Node and the npm global bin directory (normally `%APPDATA%\npm`) are in the app's PATH; restart the app/sign out if Explorer has stale environment. Node/Mermaid/browser remain separate from the installer.

## macOS and Linux

Dependencies: Qt **6.7+** (Core/Gui/Widgets/Concurrent/Network/PrintSupport/QML/Quick/QuickControls2/QuickDialogs2/Test), C++17 compiler, make, CMake **3.24+**, Ninja, Perl. Linux also needs Qt DBus, Hunspell development libraries and installed language dictionaries. macOS uses system NSSpellChecker; Windows uses ISpellChecker, not Hunspell.

On macOS with Homebrew and Apple's command-line tools installed:

```sh
brew install qt cmake ninja perl
./bin/build
./bin/test
open -a "$PWD/build/cmake/Write.app" /absolute/path/note.md
```

On Linux, install the equivalent packages through your distribution (check that its Qt is new enough), then `./bin/build`; run `./build/cmake/omawrite note.md`. `bin/build` uses upstream's CMake build and bootstraps SHA-pinned ECM/KSyntaxHighlighting/Sonnet into `.deps`; no manual KDE install is needed. Perl and Qt Test are required even when dependency tests are disabled. These Unix scripts do not detect all stale toolchains: move generated `build`, `build-tests` and `.deps` aside when changing Qt/compiler/architecture. Do not remove app data or documents. See [test commands](testing.md).

## Checks before distributing

- Test on a clean Windows account/machine **without Qt on PATH**: install, launch, open/save LF UTF-8, shortcuts, high-DPI layout, then uninstall. Check Start Menu, taskbar/Explorer icons and missing-runtime behavior. Native Windows installer validation is still required; a macOS build is not evidence that it works.
- Exercise tables, attachments/recovery, printing/PDF, spelling with an installed dictionary, and real Mermaid/browser separately. Fake-CLI/offscreen tests do not validate printers, GUI focus, IME, or browser deployment.
- `windeployqt`/`macdeployqt` collect runtime files; they do not sign/notarize or grant redistribution clearance. Retain `LICENSE`, `COPYRIGHT.md`, font license and `third_party` notices. **Sonnet's static LGPL linking requires corresponding source/relinking compliance**; KSyntaxHighlighting embeds mixed-license resources with an incomplete audit. Read [COPYRIGHT](../COPYRIGHT.md) and linked notices before publishing binaries. An installer is not a claim that these obligations are satisfied.
- Identify builds by git commit as well as `--version`; development commits can share a version number.

Canonical Markdown owns save/undo; Edit/Read are disposable projections and printing uses an independent snapshot. Native tables are virtualized, but parsing, Read import, cold sizing and global operations can still do document-sized work. There is no constant-time editing guarantee.
