# freeink-books

An EPUB reader for the Seeed reTerminal Sticky, built entirely on the
[FreeInk SDK](https://github.com/Free-Ink/freeink-sdk): FreeInkBook does the
parsing, layout, caching, and page rendering; FreeInkUI draws every piece of
chrome; the board libraries own the hardware. This firmware is the glue —
screens, settings, and the main loop (~0.7 MB binary).

## Features

- **Library** — cover-grid of every `.epub` on the card (root or `/Books`),
  with titles/authors/covers pulled from the books themselves.
- **Reading** — justified, hyphenated, kerned, ligature-aware text with
  anti-aliased fonts; embedded book CSS honored (headings, dropcap sizes,
  small caps, sub/superscripts, underline); right-to-left scripts (Hebrew,
  and Arabic with contextual letter joining — use a font with Arabic
  Presentation Forms coverage; mixed Latin/numbers handled per UAX #9);
  CJK typography (kinsoku, inter-character justification, full-width
  punctuation compression, Korean word-space justification — driven by the
  book's declared language); images with proper grayscale dithering;
  instant page turns from the layout cache.
- **Links & footnotes** — tap linked text to follow it (same-chapter
  anchors or cross-chapter references); swipe up from the bottom to return
  to where you jumped from, then again to exit to the Library.
- **Position that survives everything** — progress, font, size, margins,
  spacing, alignment, and orientation changes all land back on the same
  sentence, exactly (character anchors, not estimates).
- **Settings** — reading font (any TTF/OTF on the card, with automatic
  `-Bold`/`-Italic`/`-BoldItalic` sibling loading), size, line spacing,
  page margins, alignment, hyphenation, sharp-vs-AA text, extra paragraph
  spacing, embedded-styles toggle, focus reading (bold word prefixes), UI
  font (CJK/Hangul chrome fallback), and 4-way orientation. All persisted.

## SD card layout

```
/*.epub, /*.txt    your library (card root; a /Books folder also works)
/fonts/*.ttf       reading fonts — pick one in Settings; siblings named
                   <Name>-Bold.ttf / -Italic.ttf / -BoldItalic.ttf load too
/fonts/*.fibh      optional extra hyphenation patterns (English is built in;
                   generate others with freeink-sdk tools/hyphc.py)
/BookCache/        created automatically: page caches, progress, settings
```

## Controls

- **Library**: touch or UP/DOWN + CONFIRM to open a book; gear button for
  Settings.
- **Reading**: tap right/left thirds (or swipe, or UP/DOWN buttons) to turn
  pages; tap linked text to follow it; tap center for chrome (title,
  position, Library / Contents / Aa size); swipe down from the top edge for
  the table of contents; swipe up from the bottom edge to go back
  (link-return first, then Library).
- Hold the power button ~1.5 s to sleep.

## Build

```
pio run -e sticky -t upload
pio device monitor        # boot/status logs via the IDF console
```

The committed config builds against the pinned `freeink-sdk` git submodule.
For SDK development, a gitignored `platformio.local.ini` (matched by
`extra_configs = platformio.local*.ini`) can override `lib_deps` with
**absolute** `symlink:///...` paths to a working checkout — relative
`symlink://` paths resolve against the invoking directory and silently link
the wrong tree from IDE builds.

## Notes

- Big buffers (book/layout arenas, fonts) live in PSRAM — the Sticky is the
  ESP32-S3R8 (8 MB). A 4-5 MB variable font plus all arenas fits; static
  font families are lighter.
- Layout caches are per-settings "generations"; changing typography
  settings relayouts a chapter once (~a second) and is cached thereafter.
- Serial debugging on the Sticky goes through the IDF/ROM console
  (`esp_rom_printf`), not the Arduino CDC object — see `LOGF` in
  `src/main.cpp`.
