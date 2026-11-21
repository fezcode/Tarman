# Stone theme typefaces

Instrument Serif (regular) and DM Mono (regular and medium) match the typography
of the [Tarman showcase](https://fezcode.com/projects/tarman). Segoe UI remains the
body face for readable process tables and Windows glyph coverage.

The unmodified TTF files come from the Google Fonts repository:

- [Instrument Serif](https://github.com/google/fonts/tree/main/ofl/instrumentserif)
- [DM Mono](https://github.com/google/fonts/tree/main/ofl/dmmono)

Both families use the SIL Open Font License, version 1.1; their copyright and
license notices are alongside the fonts. CMake embeds the fonts into the
executable and appends both licenses to the shipped `build/LICENSE.txt`.
No font installation or network access is needed at runtime.
