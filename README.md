# freeink-books

An EPUB reader for the Seeed reTerminal Sticky, built entirely on the
[FreeInk SDK](../freeink-sdk): FreeInkBook does the parsing, pagination,
caching, and page rendering; FreeInkUI draws every piece of chrome; this
firmware is just screens and glue.

## SD card layout

```
/*.epub            your library (card root; a /Books folder also works)
/fonts/*.ttf       the reading font (first one found is used) — anti-aliased
/fonts/*.fibh      optional hyphenation patterns (freeink-sdk tools/hyphc.py)
/BookCache/        created automatically: page caches + reading progress
```

Generate hyphenation patterns once:

```
python3 ../freeink-sdk/libs/book/FreeInkBook/tools/hyphc.py \
  ../freeink-sdk/libs/book/FreeInkBook/third_party/hyphen-patterns/hyph-en-us.pat.txt \
  hyph-en-us.fibh
```

## Controls

- **Library**: UP/DOWN buttons or touch to select, CONFIRM/tap to open.
- **Reading**: tap right/left thirds (or swipe, or UP/DOWN buttons) to turn
  pages; tap the center to toggle the chrome (title, chapter/page position,
  Library / Contents / font-size controls).
- Reading position and font size persist per book; changing the font size
  relayouts in the background and returns to the exact same sentence.
- Hold the power button ~1.5 s to sleep.

## Build

```
pio run -e sticky -t upload
```

Assumes a `freeink-sdk` checkout as a sibling directory (see platformio.ini).
