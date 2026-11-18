# KSyntaxHighlighting 6.13.0: provenance and distribution status

Write builds the standalone KDE syntax engine, not Kate or KTextEditor. The engine source is MIT-licensed; **this does not mean all bundled syntax definitions are MIT**. `QRC_SYNTAX=ON` embeds upstream language definitions and themes in the private static library.

## Pinned source

- Engine and resource source: <https://download.kde.org/stable/frameworks/6.13/syntax-highlighting-6.13.0.tar.xz>
- SHA-256: `d6f19bb886d6e1c7251d813497f14ecc9dac413253764eb61a8f3a836806450d`
- Build-only Extra CMake Modules: <https://download.kde.org/stable/frameworks/6.13/extra-cmake-modules-6.13.0.tar.xz>
- ECM SHA-256: `7006017c00c817ff4c056995146d271791d1487a398d39ea6cac1cd59a8bf402`
- Reproducible configuration: [`../../dependencies/CMakeLists.txt`](../../dependencies/CMakeLists.txt).

The dependency bootstrap verifies both archives. Extracted original source, including definition authorship/license headers, is retained under `.deps/syntax-highlighting-prefix/src/syntax-highlighting/`. Upstream source remains authoritative. Translated language-name catalogs, examples, and the optional QML plugin are not needed by this integration.

`LICENSES/` is an unmodified copy of **all five license texts supplied in that upstream source archive**, not a claim that those texts cover every legacy resource declaration. `resource-license-inventory.tsv` records the XML `license` and `author` attributes verbatim for the pinned release; empty declarations and legacy names are not resolved or relicensed by this inventory. Individual file headers may provide additional terms. Theme/source notices also remain in the original archive and embedded upstream resources.

## No distributable-binary clearance

This feature has been exercised as a local development build. **A complete resource-license/source-compliance audit has not been performed.** No prebuilt release is being published. Do not describe the resulting binary as MIT-only, or assume static linking removes resource obligations. Before distributing a binary, determine the applicable terms for all included resources, preserve the required notices, and provide the corresponding sources/build information required by those terms. Merely linking to this repository or copying the engine's MIT text is not a substitute for that work.

The historical Arch `pkgbuild/PKGBUILD` is intentionally disabled until that audit and an updated packaging recipe are available. Published packages remain separate from this development branch.
