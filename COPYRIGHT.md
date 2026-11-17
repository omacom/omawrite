# Write — copyright and third-party notices

[Write/Omawrite](https://github.com/omacom/omawrite) is copyright © 2026 David Heinemeier Hansson, with additional contributions copyright © 2026 Alfonso Alvarez. The application is provided under the root [MIT license](LICENSE). Both copyright notices, the permission notice and disclaimer remain applicable.

Git history and historical benchmark reports retain their original attribution and environment details; these are historical records, not product branding.

## Separately licensed components

- **IBM Plex Mono**: copyright © 2017 IBM Corp. with Reserved Font Name "Plex". SIL OFL 1.1; see [fonts/OFL.txt](fonts/OFL.txt). Fonts have not been renamed or relicensed.
- **KSyntaxHighlighting 6.13.0**: MIT engine, but embedded definitions/themes have mixed additional terms. See [notices and unresolved resource audit](third_party/ksyntaxhighlighting/README.md).
- **Sonnet Core 6.13.0**: separately licensed KDE code, including LGPL obligations for static linking, source and relinking; see [notices](third_party/sonnet/README.md).
- Qt and platform libraries retain their own licenses. The application MIT license does not relicense dependencies, resources or bundled fonts.

**These notices do not establish clearance for prebuilt distribution.** The syntax-resource audit and static-library distribution obligations remain unresolved; the packaging guard remains enabled. Local development builds are not a declaration of distribution compliance.

## Application identity

The application uses upstream's `omawrite` / `Omacom` / `omacom.io` settings identity and standard data locations. It does not discover or import other applications' preferences or recovery stores. Existing Write preferences remain untouched by startup. Quit other running Write builds before testing this branch so the single-instance broker does not forward file opens to an older build.
