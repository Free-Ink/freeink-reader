// freeink-books — EPUB reader for the Seeed reTerminal Sticky.
//
// Everything heavy is SDK code: FreeInkBook parses/paginates/caches/renders
// books; FreeInkUI draws the chrome and routes input; the board libraries own
// the hardware. This firmware is the glue: screens, stores, and the main loop.
//
// SD layout:  /*.epub               your library (root; /Books also scanned)
//             /fonts/*.ttf           reading font (first found is used)
//             /BookCache/<id>/       page caches + progress (auto-created)

#include <Arduino.h>
#include <ctype.h>
#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <FreeInkApp.h>
#include <FreeInkBook.h>
#include <FreeInkUIBookFont.h>
#include <FreeInkUIDisplayTarget.h>
#include <Icon.h>
#include <InputManager.h>
#include <PowerManager.h>
#include <SDCardManager.h>
#include <cache/PageCache.h>
#include <epub/ImageProbe.h>
#include <css/Css.h>
#include <layout/ChapterLayout.h>
#include <render/ImageRenderer.h>
#include <render/PageRenderer.h>
#include <render/TtfFont.h>
#include <text/Hyphenator.h>
#include <text/hyph_en_us.h>

#include "BookStorageAdapters.h"
#include "icons_gen.h"
#include <FreeInkUIIcon.h>

// Sticky logs reliably through the IDF/ROM console (USB-Serial/JTAG), not the
// Arduino CDC Serial object — BoardConfig.h encodes this per board as
// FREEINK_LOG_TRANSPORT. X3/X4-class boards keep plain Serial.
#if FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF
#include <esp_rom_sys.h>
#define LOGF(...) esp_rom_printf(__VA_ARGS__)
#else
#define LOGF(...) Serial.printf(__VA_ARGS__)
#endif

using namespace freeink;
using book::BookStatus;

// ---------------------------------------------------------------------------
// Actions & screens

enum : ui::ActionId {
  ActionOpenBook = 1,
  ActionPageNext,
  ActionPagePrev,
  ActionBackToLibrary,
  ActionBackToReader,
  ActionToc,
  ActionTocJump,
  ActionFontSize,
  ActionSettings,
  ActionPickFont,
  ActionPickSize,
  ActionFontMenu,
  ActionSizeMenu,
  ActionUiFontMenu,
  ActionPickUiFont,
  ActionLineMenu,
  ActionPickLine,
  ActionMarginMenu,
  ActionPickMargin,
  ActionAlignMenu,
  ActionPickAlign,
  ActionToggleHyphen,
  ActionToggleSharp,
  ActionToggleParaSpace,
  ActionToggleEmbCss,
  ActionToggleFocus,
  ActionOrientMenu,
  ActionPickOrient,
  ActionCloseMenu,
  ActionRefreshLibrary,
};

enum class Screen : uint8_t { Library, Reader, Toc, Settings };

// ---------------------------------------------------------------------------
// Stores

bool containsIgnoreCase(const char* haystack, const char* needle) {
  if (haystack == nullptr || needle == nullptr || needle[0] == 0) return false;
  for (const char* h = haystack; *h; ++h) {
    const char* a = h;
    const char* b = needle;
    while (*a && *b && tolower(static_cast<unsigned char>(*a)) ==
                            tolower(static_cast<unsigned char>(*b))) {
      ++a;
      ++b;
    }
    if (*b == 0) return true;
  }
  return false;
}

struct Shelf {
  static constexpr int kMax = 64;
  static constexpr uint16_t kCoverW = 150;
  static constexpr uint16_t kCoverH = 225;
  char paths[kMax][160];
  char titles[kMax][64];
  char authors[kMax][48];
  char metas[kMax][48];
  char coverHrefs[kMax][160];
  ui::ListItem items[kMax];
  uint8_t* coverBits[kMax];
  bool detailsReady[kMax];
  bool coverTried[kMax];
  int count = 0;

  void scan() {
    releaseCovers();
    count = 0;
    addFrom("/");       // books anywhere: card root works out of the box
    addFrom("/Books");  // ...and a Books folder for the tidy
  }

  void releaseCovers() {
    for (int i = 0; i < kMax; ++i) {
      if (coverBits[i] != nullptr) {
        free(coverBits[i]);
        coverBits[i] = nullptr;
      }
      authors[i][0] = 0;
      metas[i][0] = 0;
      coverHrefs[i][0] = 0;
      detailsReady[i] = false;
      coverTried[i] = false;
    }
  }

  void addFrom(const char* dir) {
    const bool root = dir[0] == '/' && dir[1] == 0;
    for (const String& name : SdMan.listFiles(dir, kMax)) {
      const bool isEpub = name.endsWith(".epub");
      const bool isTxt = name.endsWith(".txt");
      const bool isCrashReport = containsIgnoreCase(name.c_str(), "crash") ||
                                 containsIgnoreCase(name.c_str(), "panic") ||
                                 containsIgnoreCase(name.c_str(), "backtrace");
      if (name.startsWith(".") || isCrashReport || (!isEpub && !isTxt) || count >= kMax) continue;
      snprintf(paths[count], sizeof(paths[count]), "%s%s%s", dir, root ? "" : "/", name.c_str());
      snprintf(titles[count], sizeof(titles[count]), "%.*s",
               static_cast<int>(name.length() - (isEpub ? 5 : 4)), name.c_str());
      snprintf(metas[count], sizeof(metas[count]), "%s", dir);
      items[count] = ui::ListItem{};
      items[count].label = titles[count];
      items[count].actionValue = static_cast<int16_t>(count);
      coverBits[count] = nullptr;
      detailsReady[count] = false;
      coverTried[count] = false;
      ++count;
    }
  }

  void ensureDetails(uint16_t index);
  bool ensureCover(uint16_t index);
  ui::CoverGridItem gridItem(uint16_t index, bool loadDetails = true);
};

struct Progress {  // persisted per book: (spineIndex, charStart, baseSizePx)
  uint16_t spineIndex = 0;
  uint32_t charStart = 0;
  uint16_t baseSizePx = 18;
};

// ---------------------------------------------------------------------------
// Reader session — one open book

struct ReaderSession {
  SdBookSource source;
  SdCacheStorage cache;
  book::Arena bookArena;
  book::Arena scratch;
  book::Book bk;
  book::CssStylesheet sheet{};
  book::LayoutParams params;
  book::PageCacheReader reader;
  book::Arena indexArena;
  Progress pos;
  char cacheDir[96];
  char progressPath[128];
  // PageCacheReader BORROWS this name for every later readPage() — it must
  // live as long as the reader, never on ensureChapter's stack.
  char cacheName[64];
  uint32_t pageInChapter = 0;
  bool open = false;
  bool isTxt = false;  // plain-text document: one chapter, no container
  // Links on the currently rendered page (copied; tap hit-testing).
  struct LinkBox { char target[128]; char fragment[48]; int16_t x, y; uint16_t w, h; };
  LinkBox links[24];
  uint8_t linkCount = 0;
  struct BackEntry { uint16_t spine; uint32_t charStart; };
  BackEntry backStack[8];
  uint8_t backDepth = 0;

  bool begin(const char* epubPath, book::FontChain& fonts, const book::Hyphenator* hyph,
             uint8_t* bookBuf, size_t bookCap, uint8_t* scratchBuf, size_t scratchCap,
             uint8_t* indexBuf, size_t indexCap) {
    bookArena.init(bookBuf, bookCap);
    scratch.init(scratchBuf, scratchCap);
    indexArena.init(indexBuf, indexCap);
    if (!source.open(epubPath)) return false;
    const size_t plen = strlen(epubPath);
    isTxt = plen > 4 && strcmp(epubPath + plen - 4, ".txt") == 0;
    if (!isTxt && bk.open(source, bookArena, scratch) != BookStatus::Ok) return false;

    // Per-book cache directory from a path hash; progress lives beside it.
    const uint32_t id = book::ZipCatalog::hashPath(epubPath);
    snprintf(cacheDir, sizeof(cacheDir), "/BookCache/%08x", id);
    snprintf(progressPath, sizeof(progressPath), "%s/progress.bin", cacheDir);
    cache.setDir(cacheDir);
    loadProgress();

    // Book stylesheet (all text/css manifest items). Plain text has none.
    book::CssStylesheetBuilder builder;
    static uint8_t sheetBuf[48 * 1024];
    static book::Arena sheetArena;
    sheetArena.init(sheetBuf, sizeof(sheetBuf));
    builder.begin(sheetArena);
    for (size_t m = 0; isTxt ? false : m < bk.manifestCount(); ++m) {
      const book::ManifestItem* item = bk.manifestItem(m);
      if (strcmp(item->mediaType, "text/css") != 0) continue;
      if (const book::ZipEntry* e = bk.zip().find(item->href)) {
        builder.addSheet(source, *e, scratch);
      }
    }
    sheet = builder.finish();

    params = book::LayoutParams{};
    extern ui::DisplayTarget* target;
    params.pageWidth = target->logicalWidth();   // follows the orientation setting
    params.pageHeight = target->logicalHeight();
    params.baseSizePx = pos.baseSizePx;
    params.font = &fonts;
    params.stylesheet = isTxt ? nullptr : &sheet;
    extern uint16_t lineSpacingPct;
    extern uint16_t screenMarginPx;
    extern uint8_t paraAlign, extraParaSpacing, embeddedStyles, focusReading;
    params.marginLeft = params.marginRight = static_cast<int16_t>(screenMarginPx);
    params.marginTop = params.marginBottom = static_cast<int16_t>(screenMarginPx > 20 ? 20 : screenMarginPx);
    params.lineSpacingPct = lineSpacingPct;
    params.paragraphSpacingPct = extraParaSpacing ? 150 : 100;
    params.embeddedStyles = embeddedStyles != 0;
    params.focusReading = focusReading != 0;
    params.hyphenator = hyph;
    static const book::TextAlign kAligns[4] = {book::TextAlign::Justify, book::TextAlign::Left,
                                               book::TextAlign::Center, book::TextAlign::Right};
    params.defaultAlign = kAligns[paraAlign];
    params.language = (!isTxt && bk.metadata().language[0]) ? bk.metadata().language : "en";

    open = ensureChapter(pos.spineIndex) == BookStatus::Ok;
    if (open) pageInChapter = reader.pageForChar(pos.charStart);
    return open;
  }

  void end() {
    saveProgress();
    source.close();
    open = false;
  }

  // Font fingerprint distinguishes TTF vs built-in layouts: metrics differ,
  // so caches from one font set must not serve the other.
  uint32_t generation() const;

  // Opens (building if stale/missing) the page cache for one spine item.
  BookStatus ensureChapter(uint16_t spineIndex) {
    const uint32_t hash = generation();
    book::pageCacheName(spineIndex, hash, cacheName, sizeof(cacheName));
    indexArena.reset();
    BookStatus st = reader.open(cache, cacheName, hash, indexArena);
    if (st == BookStatus::Ok) return st;

    const book::ManifestItem* item = isTxt ? nullptr : bk.spineItem(spineIndex);
    const book::ZipEntry* entry =
        (!isTxt && item != nullptr) ? bk.zip().find(item->href) : nullptr;
    if (!isTxt && entry == nullptr) return BookStatus::NotFound;
    if (isTxt && spineIndex != 0) return BookStatus::NotFound;  // one chapter

    const size_t marked = scratch.mark();
    book::PageCacheWriter writer;
    if (!writer.begin(cache, cacheName, hash, scratch)) {
      scratch.release(marked);
      return BookStatus::IoError;
    }
    uint32_t totalChars = 0;
    st = isTxt ? book::ChapterLayout::layoutPlainText(source, params, scratch, writer,
                                                      nullptr, &totalChars)
               : book::ChapterLayout::layout(source, bk.zip(), *entry, item->href, params,
                                             scratch, writer, nullptr, &totalChars);
    writer.setTotalChars(totalChars);
    if (st == BookStatus::Ok && !writer.finish()) st = BookStatus::IoError;
    scratch.release(marked);
    if (st != BookStatus::Ok) return st;

    indexArena.reset();
    return reader.open(cache, cacheName, hash, indexArena);
  }

  // Renders the current page into the framebuffer (chrome drawn separately).
  // Text always draws; a failed image decode logs and leaves its box blank.
  bool renderCurrent(book::FontChain& fonts, const book::FrameTarget& target) {
    const size_t marked = scratch.mark();
    book::Page page{};
    const BookStatus rs = reader.readPage(pageInChapter, scratch, &page);
    if (rs != BookStatus::Ok) {
      LOGF("[reader] readPage(%u) %s\n", pageInChapter, book::bookStatusName(rs));
      scratch.release(marked);
      return false;
    }
    LOGF("[reader] page %u ok: runs=%u images=%u char=%u\n", pageInChapter, page.runCount,
         page.imageCount, page.charStart);
    uint32_t missingCp = 0;
    const uint32_t missing = book::PageRenderer::renderText(page, fonts, target, &missingCp);
    if (missing != 0) {
      LOGF("[reader] %u glyphs missing on page %u (first U+%04X)\n", missing, pageInChapter,
           missingCp);
    }
    const BookStatus is = book::PageRenderer::renderImages(page, source, bk.zip(), scratch, target);
    if (is != BookStatus::Ok) {
      LOGF("[reader] images on page %u: %s (%u placed)\n", pageInChapter,
           book::bookStatusName(is), page.imageCount);
    }
    pos.charStart = page.charStart;
    scratch.release(marked);
    return true;
  }

  bool turn(int direction) {  // +1 / -1; returns false at book edges
    if (direction > 0) {
      if (pageInChapter + 1 < reader.pageCount()) {
        ++pageInChapter;
        return true;
      }
      const size_t chapterCount = isTxt ? 1 : bk.spineCount();
      if (pos.spineIndex + 1u < chapterCount &&
          ensureChapter(pos.spineIndex + 1) == BookStatus::Ok) {
        ++pos.spineIndex;
        pageInChapter = 0;
        return true;
      }
      return false;
    }
    if (pageInChapter > 0) {
      --pageInChapter;
      return true;
    }
    if (pos.spineIndex > 0 && ensureChapter(pos.spineIndex - 1) == BookStatus::Ok) {
      --pos.spineIndex;
      pageInChapter = reader.pageCount() > 0 ? reader.pageCount() - 1 : 0;
      return true;
    }
    return false;
  }

  // Follows a tapped link: same-chapter fragment or cross-chapter href.
  bool followLink(const LinkBox& link) {
    if (backDepth < 8) backStack[backDepth++] = {pos.spineIndex, pos.charStart};
    uint16_t targetSpine = pos.spineIndex;
    if (link.target[0] != 0) {
      int found = -1;
      for (size_t s = 0; s < bk.spineCount(); ++s) {
        if (strcmp(bk.spineItem(s)->href, link.target) == 0) { found = static_cast<int>(s); break; }
      }
      if (found < 0) { if (backDepth) --backDepth; return false; }
      targetSpine = static_cast<uint16_t>(found);
    }
    if (ensureChapter(targetSpine) != BookStatus::Ok) { if (backDepth) --backDepth; return false; }
    pos.spineIndex = targetSpine;
    uint32_t ch = 0;
    if (link.fragment[0] != 0 &&
        reader.charForAnchor(book::ZipCatalog::hashPath(link.fragment), &ch)) {
      pageInChapter = reader.pageForChar(ch);
    } else {
      pageInChapter = 0;
    }
    return true;
  }

  bool goBack() {
    if (backDepth == 0) return false;
    const BackEntry e = backStack[--backDepth];
    if (ensureChapter(e.spine) != BookStatus::Ok) return false;
    pos.spineIndex = e.spine;
    pageInChapter = reader.pageForChar(e.charStart);
    return true;
  }

  bool jumpToToc(size_t tocIndex) {
    const book::TocEntry* toc = bk.tocEntry(tocIndex);
    if (toc == nullptr) return false;
    for (size_t s = 0; s < bk.spineCount(); ++s) {
      if (strcmp(bk.spineItem(s)->href, toc->href) != 0) continue;
      if (ensureChapter(static_cast<uint16_t>(s)) != BookStatus::Ok) return false;
      pos.spineIndex = static_cast<uint16_t>(s);
      pageInChapter = 0;
      return true;
    }
    return false;
  }

  bool jumpToSpine(uint16_t spineIndex) {
    if (spineIndex >= bk.spineCount()) return false;
    if (ensureChapter(spineIndex) != BookStatus::Ok) return false;
    pos.spineIndex = spineIndex;
    pageInChapter = 0;
    return true;
  }

  // Font-size change: new generation; the anchor carries the position over.
  bool setBaseSize(uint16_t sizePx) {
    const uint32_t anchor = pos.charStart;
    pos.baseSizePx = params.baseSizePx = sizePx;
    if (ensureChapter(pos.spineIndex) != BookStatus::Ok) return false;
    pageInChapter = reader.pageForChar(anchor);
    return true;
  }

  void loadProgress() {
    extern uint16_t defaultSizePx;
    pos.baseSizePx = defaultSizePx;  // global setting; per-book file overrides
    FsFile f = SdMan.open(progressPath, O_RDONLY);
    if (f) {
      f.read(&pos, sizeof(pos));
      f.close();
    }
    if (pos.baseSizePx < 12 || pos.baseSizePx > 32) pos.baseSizePx = 18;
  }
  void saveProgress() {
    FsFile f = SdMan.open(progressPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (f) {
      f.write(&pos, sizeof(pos));
      f.close();
    }
  }
};

uint32_t ReaderSession::generation() const {
  extern bool fontReady;
  extern char currentFontName[48];
  // Fingerprint the actual face: different TTFs have different metrics, so
  // their layouts must not share caches.
  extern book::FontChain fonts;
  return book::layoutGenerationHash(
      params, fontReady ? (book::ZipCatalog::hashPath(currentFontName) ^
                           (static_cast<uint32_t>(fonts.styleCoverage()) << 24))
                        : 1u);
}

// ---------------------------------------------------------------------------
// Globals (static allocation; big buffers in PSRAM)

using App = ui::FreeInkApp<48, 24>;

EInkDisplay display(
    BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.display.mosi,
    BoardConfig::ACTIVE.display.cs, BoardConfig::ACTIVE.display.dc,
    BoardConfig::ACTIVE.display.rst, BoardConfig::ACTIVE.display.busy);
ui::DisplayTarget* target = nullptr;
App* app = nullptr;
InputManager input;
BatteryMonitor battery;

Shelf shelf;
ReaderSession session;
book::TtfFont ttf;
book::TtfFont ttfBold, ttfItalic, ttfBoldItalic;
uint16_t defaultSizePx = 18;  // global font-size setting (per-book Aa overrides)
// Reader typography settings (CrossPoint parity), persisted in settings.bin.
uint16_t lineSpacingPct = 100;   // 100/115/130/150
uint16_t screenMarginPx = 24;    // 16/24/32/40
uint8_t paraAlign = 0;           // 0 justify, 1 left, 2 center, 3 right
uint8_t hyphenateSetting = 1;
uint8_t sharpText = 0;           // 1 = Mono1Sharp (no AA dither)
uint8_t extraParaSpacing = 0;    // 1 = 150% paragraph spacing
uint8_t embeddedStyles = 1;
uint8_t focusReading = 0;        // bold each word's first ~45% (fixation aid)
uint8_t orientationSetting = 0;  // 0 portrait, 1 landscape, 2 portrait flipped, 3 landscape flipped
static const ui::Orientation kUiOrient[4] = {
    ui::Orientation::Portrait, ui::Orientation::LandscapeCounterClockwise,
    ui::Orientation::PortraitInverted, ui::Orientation::LandscapeClockwise};
static const book::FrameRotation kPageRot[4] = {
    book::FrameRotation::Portrait, book::FrameRotation::None,
    book::FrameRotation::PortraitInverted, book::FrameRotation::UpsideDown};
static const char* kOrientNames[4] = {"Portrait", "Landscape", "Portrait (flipped)",
                                      "Landscape (flipped)"};
book::TtfFont uiTtf;
ui::TtfGlyphSource uiGlyphSource;
char uiFontName[48] = "";       // "" = bitmap chrome only
uint8_t* uiFontBuf = nullptr;
uint32_t uiFontCap = 0;
ui::BitmapBookFont builtinFont;  // bundled Noto Sans — always-available fallback
book::FontChain fonts;
book::Hyphenator hyphenator;
bool hyphReady = false;
bool fontReady = false;

Screen screen = Screen::Library;
int16_t librarySelected = 0;
uint16_t libraryTop = 0;
uint16_t libraryVisibleCells = 0;
bool libraryFastScrollFrame = false;
bool libraryHydrateAfterScroll = false;
uint16_t tocTop = 0;
uint16_t tocVisibleRows = 0;
bool tocAnchorSelected = false;
uint16_t settingsTop = 0;
uint16_t settingsVisibleRows = 0;
bool readerChromeVisible = false;
bool libraryRefreshRequested = false;
bool libraryRefreshPainted = false;
char statusText[96];

uint8_t* bookBuf = nullptr;      // 256 KB PSRAM
uint8_t* scratchBuf = nullptr;   // 512 KB PSRAM
uint8_t* indexBuf = nullptr;     // 64 KB PSRAM
uint8_t* glyphBuf = nullptr;     // 128 KB PSRAM
uint8_t* fontFile = nullptr;     // TTF bytes, PSRAM
uint32_t fontFileCap = 0;
uint8_t* hyphData = nullptr;
uint8_t* coverBookBuf = nullptr;     // lazy library metadata/covers
uint8_t* coverScratchBuf = nullptr;

uint8_t* psAlloc(size_t n) {
  uint8_t* p = static_cast<uint8_t*>(ps_malloc(n));
  return p != nullptr ? p : static_cast<uint8_t*>(malloc(n));
}

bool loadSdBlob(const char* dir, const char* ext, uint8_t** out, uint32_t* lenOut) {
  for (const String& name : SdMan.listFiles(dir, 16)) {
    if (name.startsWith(".") || !name.endsWith(ext)) continue;  // skip macOS ._ droppings
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dir, name.c_str());
    FsFile f = SdMan.open(path, O_RDONLY);
    if (!f) continue;
    const uint32_t len = f.fileSize();
    *out = psAlloc(len);
    if (*out == nullptr) return false;
    const bool ok = f.read(*out, len) == static_cast<int>(len);
    f.close();
    if (ok) {
      *lenOut = len;
      return true;
    }
  }
  return false;
}

bool isSupportedCoverImage(const book::ManifestItem* item) {
  if (item == nullptr || item->mediaType == nullptr) return false;
  return strcmp(item->mediaType, "image/png") == 0 ||
         strcmp(item->mediaType, "image/jpeg") == 0 ||
         strcmp(item->mediaType, "image/jpg") == 0;
}

const book::ManifestItem* chooseCoverItem(const book::Book& bk) {
  const book::ManifestItem* fallback = nullptr;
  for (size_t i = 0; i < bk.manifestCount(); ++i) {
    const book::ManifestItem* item = bk.manifestItem(i);
    if (!isSupportedCoverImage(item)) continue;
    if (item->isCoverImage) return item;
    if (fallback == nullptr && (containsIgnoreCase(item->id, "cover") ||
                                containsIgnoreCase(item->href, "cover"))) {
      fallback = item;
    }
  }
  return fallback;
}

struct CoverDecodeCtx {
  uint8_t* bits = nullptr;
  uint16_t stride = 0;
  uint16_t x = 0;
  uint16_t y = 0;
};

bool coverDecodeRow(void* user, uint16_t y, const uint8_t* gray, uint16_t width) {
  CoverDecodeCtx* ctx = static_cast<CoverDecodeCtx*>(user);
  if (ctx == nullptr || ctx->bits == nullptr) return false;
  const uint16_t py = static_cast<uint16_t>(ctx->y + y);
  uint8_t* row = ctx->bits + static_cast<uint32_t>(py) * ctx->stride;
  for (uint16_t sx = 0; sx < width; ++sx) {
    const uint16_t px = static_cast<uint16_t>(ctx->x + sx);
    const uint8_t level = gray[sx] < 64 ? 0 : gray[sx] < 128 ? 1 : gray[sx] < 192 ? 2 : 3;
    const uint8_t shift = static_cast<uint8_t>((3 - (px & 3)) * 2);
    row[px / 4] = static_cast<uint8_t>((row[px / 4] & ~(0x03 << shift)) | (level << shift));
  }
  return true;
}

void Shelf::ensureDetails(uint16_t index) {
  if (index >= count || detailsReady[index]) return;
  detailsReady[index] = true;
  if (coverBookBuf == nullptr || coverScratchBuf == nullptr) return;

  SdBookSource source;
  if (!source.open(paths[index])) return;

  book::Arena bookArena;
  book::Arena scratch;
  bookArena.init(coverBookBuf, 128 * 1024);
  scratch.init(coverScratchBuf, 192 * 1024);
  book::Book bk;
  if (bk.open(source, bookArena, scratch) == BookStatus::Ok) {
    if (bk.metadata().title != nullptr && bk.metadata().title[0] != 0) {
      snprintf(titles[index], sizeof(titles[index]), "%s", bk.metadata().title);
    }
    if (bk.metadata().author != nullptr && bk.metadata().author[0] != 0) {
      snprintf(authors[index], sizeof(authors[index]), "%s", bk.metadata().author);
    }
    const book::ManifestItem* cover = chooseCoverItem(bk);
    if (cover != nullptr && cover->href != nullptr) {
      snprintf(coverHrefs[index], sizeof(coverHrefs[index]), "%s", cover->href);
    }
    snprintf(metas[index], sizeof(metas[index]), "%u chapters",
             static_cast<unsigned>(bk.spineCount()));
  }
  source.close();
}

bool Shelf::ensureCover(uint16_t index) {
  if (index >= count) return false;
  ensureDetails(index);
  if (coverBits[index] != nullptr) return true;
  if (coverTried[index]) return false;
  coverTried[index] = true;
  if (coverHrefs[index][0] == 0 || coverBookBuf == nullptr || coverScratchBuf == nullptr) return false;

  const uint16_t stride = static_cast<uint16_t>((kCoverW + 3) / 4);
  uint8_t* bits = static_cast<uint8_t*>(psAlloc(static_cast<size_t>(stride) * kCoverH));
  if (bits == nullptr) return false;
  memset(bits, 0xFF, static_cast<size_t>(stride) * kCoverH);

  SdBookSource source;
  if (!source.open(paths[index])) {
    free(bits);
    return false;
  }
  book::Arena bookArena;
  book::Arena scratch;
  bookArena.init(coverBookBuf, 128 * 1024);
  scratch.init(coverScratchBuf, 192 * 1024);
  book::Book bk;
  bool ok = false;
  if (bk.open(source, bookArena, scratch) == BookStatus::Ok) {
    const book::ZipEntry* entry = bk.zip().find(coverHrefs[index]);
    if (entry != nullptr) {
      const size_t mark = scratch.mark();
      uint16_t drawW = kCoverW;
      uint16_t drawH = kCoverH;
      book::ImageInfo info;
      if (book::probeImage(source, *entry, scratch, &info) == BookStatus::Ok &&
          info.width > 0 && info.height > 0) {
        const uint32_t srcW = info.width;
        const uint32_t srcH = info.height;
        if (static_cast<uint32_t>(kCoverW) * srcH <= static_cast<uint32_t>(kCoverH) * srcW) {
          drawW = kCoverW;
          drawH = static_cast<uint16_t>((static_cast<uint32_t>(kCoverW) * srcH + srcW / 2) / srcW);
        } else {
          drawH = kCoverH;
          drawW = static_cast<uint16_t>((static_cast<uint32_t>(kCoverH) * srcW + srcH / 2) / srcH);
        }
        if (drawW == 0) drawW = 1;
        if (drawH == 0) drawH = 1;
        if (drawW > kCoverW) drawW = kCoverW;
        if (drawH > kCoverH) drawH = kCoverH;
      }
      const uint16_t offsetX = static_cast<uint16_t>((kCoverW - drawW) / 2);
      const uint16_t offsetY = static_cast<uint16_t>((kCoverH - drawH) / 2);
      book::PageImage image{coverHrefs[index], 0, 0, drawW, drawH};
      CoverDecodeCtx ctx{bits, stride, offsetX, offsetY};
      ok = book::ImageRenderer::render(source, bk.zip(), image, scratch, coverDecodeRow, &ctx) == BookStatus::Ok;
      scratch.release(mark);
    }
  }
  source.close();

  if (!ok) {
    free(bits);
    return false;
  }
  coverBits[index] = bits;
  return true;
}

ui::CoverGridItem Shelf::gridItem(uint16_t index, bool loadDetails) {
  if (loadDetails) ensureDetails(index);
  ui::CoverGridItem item;
  item.title = index < count ? titles[index] : "";
  item.actionValue = static_cast<int16_t>(index);
  item.enabled = index < count;
  return item;
}

ui::CoverGridItem libraryGridItem(uint16_t index, void*) {
  extern bool libraryFastScrollFrame;
  return shelf.gridItem(index, !libraryFastScrollFrame);
}

ui::BitmapRef iconBook16();

ui::BitmapRef iconRef(const Icon& icon) {
  return ui::BitmapRef{icon.bits, icon.w, icon.h, ui::BitmapFormat::Mask1};
}

ui::BitmapRef iconSettings24() {
  return ui::bitmapFromIcon(icon_settings_48);  // generated (gen_icons.py)
}

uint8_t gray2At(const uint8_t* bits, uint16_t stride, uint16_t x, uint16_t y) {
  const uint8_t packed = bits[static_cast<uint32_t>(y) * stride + x / 4];
  const uint8_t shift = static_cast<uint8_t>((3 - (x & 3)) * 2);
  return static_cast<uint8_t>((packed >> shift) & 0x03);
}

ui::Paint paintForGray2(uint8_t level) {
  switch (level) {
    case 0:
      return ui::Paint::solid(ui::Color::Black);
    case 1:
      return ui::Paint::dither(ui::Color::DarkGray);
    case 2:
      return ui::Paint::dither(ui::Color::LightGray);
    default:
      return ui::Paint::solid(ui::Color::White);
  }
}

void drawGray2Cover(ui::DrawTarget& draw, ui::Rect rect, const uint8_t* bits, uint16_t srcW, uint16_t srcH) {
  if (bits == nullptr || srcW == 0 || srcH == 0 || rect.empty()) return;

  const int32_t byW = (static_cast<int32_t>(rect.width) << 8) / srcW;
  const int32_t byH = (static_cast<int32_t>(rect.height) << 8) / srcH;
  const int32_t scale = byW < byH ? byW : byH;
  int16_t dstW = static_cast<int16_t>((static_cast<uint32_t>(srcW) * scale) >> 8);
  int16_t dstH = static_cast<int16_t>((static_cast<uint32_t>(srcH) * scale) >> 8);
  if (dstW <= 0 || dstH <= 0) return;
  if (dstW > rect.width) dstW = rect.width;
  if (dstH > rect.height) dstH = rect.height;

  const int16_t x0 = static_cast<int16_t>(rect.x + (rect.width - dstW) / 2);
  const int16_t y0 = static_cast<int16_t>(rect.y + (rect.height - dstH) / 2);
  const uint16_t stride = static_cast<uint16_t>((srcW + 3) / 4);

  for (int16_t dy = 0; dy < dstH; ++dy) {
    const uint16_t sy = static_cast<uint16_t>((static_cast<int32_t>(dy) * srcH) / dstH);
    int16_t runX = 0;
    uint8_t runLevel = 3;
    for (int16_t dx = 0; dx <= dstW; ++dx) {
      const uint8_t level = dx < dstW
                                ? gray2At(bits, stride,
                                          static_cast<uint16_t>((static_cast<int32_t>(dx) * srcW) / dstW), sy)
                                : 0xFF;
      if (dx == 0) {
        runX = 0;
        runLevel = level;
        continue;
      }
      if (level == runLevel) continue;
      if (runLevel != 3) {
        draw.fill(ui::Rect{static_cast<int16_t>(x0 + runX), static_cast<int16_t>(y0 + dy),
                           static_cast<int16_t>(dx - runX), 1},
                  paintForGray2(runLevel));
      }
      runX = dx;
      runLevel = level;
    }
  }
}

bool libraryCoverPainter(ui::DrawTarget& draw, ui::Rect rect, const ui::CoverGridItem& item,
                         uint16_t index, void*) {
  extern bool libraryFastScrollFrame;
  draw.fill(rect, ui::Paint::solid(ui::Color::White), 4);
  draw.stroke(rect, ui::Paint::solid(ui::Color::Black), 1, 4);
  const bool hasCover = shelf.coverBits[index] != nullptr ||
                        (!libraryFastScrollFrame && shelf.ensureCover(index));
  if (hasCover) {
    drawGray2Cover(draw, rect.inset(ui::Insets{3, 3, 3, 3}), shelf.coverBits[index], Shelf::kCoverW,
                   Shelf::kCoverH);
    return true;
  }

  ui::Rect cover = rect.inset(ui::Insets{3, 3, 3, 3});
  draw.fill(cover, ui::Paint::solid(ui::Color::Black), 2);
  ui::TextStyle title;
  title.font = app != nullptr ? app->theme().smallText.font : 0;
  title.align = ui::TextAlign::Center;
  title.color = ui::Color::White;
  title.inverted = true;
  title.maxLines = 5;
  draw.text(cover.inset(ui::Insets{18, 24, 92, 24}), item.title, title);
  return true;
}

void drawLibraryCoverPreview(ui::DrawTarget& draw, ui::Rect rect, uint16_t index) {
  extern bool libraryFastScrollFrame;
  if (rect.height <= 0 || rect.width <= 0 || index >= shelf.count) return;
  draw.fill(rect, ui::Paint::solid(ui::Color::White), 4);
  draw.stroke(rect, ui::Paint::solid(ui::Color::Black), 1, 4);
  ui::Rect cover = rect.inset(ui::Insets{3, 3, 0, 3});
  if (cover.height <= 0) return;
  const bool hasCover = shelf.coverBits[index] != nullptr ||
                        (!libraryFastScrollFrame && shelf.ensureCover(index));
  if (hasCover) {
    const uint8_t* bits = shelf.coverBits[index];
    const uint16_t srcW = Shelf::kCoverW;
    const uint16_t srcH = Shelf::kCoverH;
    const uint16_t stride = static_cast<uint16_t>((srcW + 3) / 4);
    const int16_t fullW = static_cast<int16_t>(rect.width - 6);
    const int16_t fullH = static_cast<int16_t>(Shelf::kCoverH - 6);
    for (int16_t dy = 0; dy < cover.height; ++dy) {
      const uint16_t sy = static_cast<uint16_t>((static_cast<int32_t>(dy) * srcH) / fullH);
      int16_t runX = 0;
      uint8_t runLevel = 3;
      bool haveRun = false;
      for (int16_t dx = 0; dx <= cover.width; ++dx) {
        uint8_t level = 3;
        if (dx < cover.width) {
          const uint16_t sx = static_cast<uint16_t>((static_cast<int32_t>(dx) * srcW) / fullW);
          const uint8_t byte = bits[static_cast<uint32_t>(sy) * stride + sx / 4];
          const uint8_t shift = static_cast<uint8_t>((3 - (sx & 3)) * 2);
          level = static_cast<uint8_t>((byte >> shift) & 0x03);
        }
        if (!haveRun) {
          runX = dx;
          runLevel = level;
          haveRun = true;
          continue;
        }
        if (level == runLevel) continue;
        if (runLevel != 3) {
          draw.fill(ui::Rect{static_cast<int16_t>(cover.x + runX), static_cast<int16_t>(cover.y + dy),
                             static_cast<int16_t>(dx - runX), 1},
                    paintForGray2(runLevel));
        }
        runX = dx;
        runLevel = level;
      }
    }
  } else {
    draw.fill(cover, ui::Paint::solid(ui::Color::Black), 2);
  }
}

ui::BitmapRef iconBook16() {
  return ui::bitmapFromIcon(icon_book_22);  // generated (gen_icons.py)
}

ui::BitmapRef iconText16() {
  static constexpr uint8_t bits[] = {
      0x00, 0x00, 0x7F, 0xFE, 0x04, 0x20, 0x04, 0x20,
      0x04, 0x20, 0x04, 0x20, 0x04, 0x20, 0x04, 0x20,
      0x04, 0x20, 0x04, 0x20, 0x04, 0x20, 0x04, 0x20,
      0x0E, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  return ui::BitmapRef{bits, 16, 16, ui::BitmapFormat::BW1};
}

ui::BitmapRef iconRefresh16() {
  return ui::bitmapFromIcon(icon_refresh_22);  // generated (gen_icons.py)
}

ui::StyleSet roundedRowStyles(uint8_t radius = 8) {
  ui::StyleSet styles = ui::selectedOutlineListRowStyles(radius);
  styles.normal.border = ui::Paint::solid(ui::Color::Black);
  styles.normal.borderWidth = 1;
  styles.normal.radius = radius;
  styles.focused = styles.normal;
  styles.focused.background = ui::Paint::dither(ui::Color::LightGray);
  styles.active = styles.selected;
  return styles;
}

// Reading-font selection: the Settings screen lists /fonts/*.ttf plus the
// built-in bitmap font; the choice persists in /BookCache/settings.bin.
struct FontShelf {
  static constexpr int kMax = 12;
  char names[kMax][48];
  ui::ListItem items[kMax + 1];
  int count = 0;
  void scan() {
    count = 0;
    items[0] = ui::ListItem{};
    items[0].label = "Built-in (Noto Sans)";
    items[0].actionValue = -1;
    for (const String& name : SdMan.listFiles("/fonts", kMax)) {
      if (name.startsWith(".") || !name.endsWith(".ttf") || count >= kMax) continue;
      snprintf(names[count], sizeof(names[count]), "%s", name.c_str());
      items[count + 1] = ui::ListItem{};
      items[count + 1].label = names[count];
      items[count + 1].actionValue = static_cast<int16_t>(count);
      ++count;
    }
  }
};
FontShelf fontShelf;
char currentFontName[48] = "";  // "" = built-in
uint8_t settingsMenu = 0;       // 0 = none, 1 = font dialog, 2 = size dialog
static const uint16_t kFontSizes[6] = {14, 16, 18, 21, 24, 28};

void saveFontSetting() {
  SdMan.ensureDirectoryExists("/BookCache");
  FsFile f = SdMan.open("/BookCache/settings.bin", O_WRONLY | O_CREAT | O_TRUNC);
  if (f) {
    f.write(currentFontName, sizeof(currentFontName));
    f.write(&defaultSizePx, sizeof(defaultSizePx));
    f.write(uiFontName, sizeof(uiFontName));
    f.write(&lineSpacingPct, sizeof(lineSpacingPct));
    f.write(&screenMarginPx, sizeof(screenMarginPx));
    f.write(&paraAlign, 1);
    f.write(&hyphenateSetting, 1);
    f.write(&sharpText, 1);
    f.write(&extraParaSpacing, 1);
    f.write(&embeddedStyles, 1);
    f.write(&orientationSetting, 1);
    f.write(&focusReading, 1);
    f.close();
  }
}
void loadFontSetting() {
  FsFile f = SdMan.open("/BookCache/settings.bin", O_RDONLY);
  if (f) {
    f.read(currentFontName, sizeof(currentFontName));
    if (f.read(&defaultSizePx, sizeof(defaultSizePx)) != sizeof(defaultSizePx)) {
      defaultSizePx = 18;  // pre-size settings file
    }
    if (f.read(uiFontName, sizeof(uiFontName)) != sizeof(uiFontName)) uiFontName[0] = 0;
    uiFontName[sizeof(uiFontName) - 1] = 0;
    f.read(&lineSpacingPct, sizeof(lineSpacingPct));
    f.read(&screenMarginPx, sizeof(screenMarginPx));
    f.read(&paraAlign, 1);
    f.read(&hyphenateSetting, 1);
    f.read(&sharpText, 1);
    f.read(&extraParaSpacing, 1);
    f.read(&embeddedStyles, 1);
    f.read(&orientationSetting, 1);
    if (f.read(&focusReading, 1) != 1) focusReading = 0;  // pre-focus settings file
    if (focusReading > 1) focusReading = 0;
    if (orientationSetting > 3) orientationSetting = 0;
    f.close();
    if (lineSpacingPct < 90 || lineSpacingPct > 200) lineSpacingPct = 100;
    if (screenMarginPx < 8 || screenMarginPx > 64) screenMarginPx = 24;
    if (paraAlign > 3) paraAlign = 0;
    currentFontName[sizeof(currentFontName) - 1] = 0;
  }
  if (defaultSizePx < 12 || defaultSizePx > 32) defaultSizePx = 18;
}

// Loads one TTF from /fonts into PSRAM and initializes the engine.
// Loads one face file into its own PSRAM buffer + glyph arena.
bool loadFaceFile(const char* name, book::TtfFont& face, uint8_t*& buf, uint32_t& cap,
                  bool quiet) {
  char path[96];
  snprintf(path, sizeof(path), "/fonts/%s", name);
  FsFile f = SdMan.open(path, O_RDONLY);
  if (!f) {
    if (!quiet) LOGF("[font] open failed: %s\n", path);
    return false;
  }
  const uint32_t len = f.fileSize();
  // Size the buffer to the font (variable fonts run 4-5 MB); PSRAM has room.
  if (len > 6 * 1024 * 1024) {
    LOGF("[font] %s too large (%u bytes, cap 6MB)\n", name, len);
    f.close();
    return false;
  }
  if (buf == nullptr || cap < len) {
    if (buf != nullptr) free(buf);
    buf = psAlloc(len);
    cap = buf != nullptr ? len : 0;
  }
  if (buf == nullptr) {
    LOGF("[font] %u-byte alloc failed (free PSRAM %u)\n", len, ESP.getFreePsram());
    f.close();
    return false;
  }
  const int got = f.read(buf, len);
  f.close();
  if (got != static_cast<int>(len)) {
    LOGF("[font] short read %d/%u for %s\n", got, len, name);
    return false;
  }
  uint8_t* arena = psAlloc(64 * 1024);
  if (arena == nullptr) return false;
  static freeink::book::Arena arenas[4];
  static uint8_t arenaUsed = 0;
  book::Arena& glyphArena = arenas[arenaUsed++ & 3];
  glyphArena.init(arena, 64 * 1024);
  const bool ok = face.init(buf, len, glyphArena);
  if (!ok) LOGF("[font] %s failed to parse (AppleDouble junk? not a TTF?)\n", name);
  return ok;
}

bool tryLoadTtf(const char* name) { return loadFaceFile(name, ttf, fontFile, fontFileCap, false); }

// Tries "<stem>-Bold.ttf" style siblings of the regular face.
void loadVariantFaces(const char* regularName) {
  char stem[64];
  snprintf(stem, sizeof(stem), "%s", regularName);
  char* dot = strrchr(stem, '.');
  if (dot != nullptr) *dot = 0;
  char* reg = strstr(stem, "-Regular");
  if (reg != nullptr) *reg = 0;
  struct Variant {
    const char* suffix;
    uint8_t flags;
    book::TtfFont* face;
  };
  static uint8_t* bufs[3] = {nullptr, nullptr, nullptr};
  static uint32_t caps[3] = {0, 0, 0};
  const Variant variants[3] = {{"-Bold", book::StyleBold, &ttfBold},
                               {"-Italic", book::StyleItalic, &ttfItalic},
                               {"-BoldItalic",
                                static_cast<uint8_t>(book::StyleBold | book::StyleItalic),
                                &ttfBoldItalic}};
  for (int v = 0; v < 3; ++v) {
    char name[96];
    snprintf(name, sizeof(name), "%s%s.ttf", stem, variants[v].suffix);
    if (loadFaceFile(name, *variants[v].face, bufs[v], caps[v], true)) {
      fonts.add(variants[v].face, variants[v].flags);
      LOGF("[font] +%s\n", name);
    }
  }
}

// (Re)builds the font chain for the selected font; built-in stays the tail.
// A selection that fails to load (stale ._file setting, corrupt font) heals
// itself: the first loadable TTF on the card is adopted and persisted.
void applyFont() {
  fonts = book::FontChain{};
  fontReady = false;
  if (currentFontName[0] != 0 && tryLoadTtf(currentFontName)) {
    fontReady = fonts.add(&ttf);
    if (fontReady) loadVariantFaces(currentFontName);
  }
  if (!fontReady) {
    for (int i = 0; i < fontShelf.count && !fontReady; ++i) {
      if (strcmp(fontShelf.names[i], currentFontName) == 0) continue;
      if (tryLoadTtf(fontShelf.names[i])) {
        snprintf(currentFontName, sizeof(currentFontName), "%s", fontShelf.names[i]);
        saveFontSetting();
        fontReady = fonts.add(&ttf);
      }
    }
  }
  fonts.add(&builtinFont);
  LOGF("[font] active: %s\n", fontReady ? currentFontName : "built-in Noto Sans");
}

// UI chrome fallback font: bitmap Noto covers Latin; a chosen TTF supplies
// everything else (Korean titles, CJK metadata, ...) sized per UI slot.
void applyOrientation() {
  target->setOrientation(kUiOrient[orientationSetting]);
  app->setDevice(target->deviceContext());
}

void applyUiFont() {
  if (uiFontName[0] != 0 && loadFaceFile(uiFontName, uiTtf, uiFontBuf, uiFontCap, false)) {
    uiGlyphSource.setFont(&uiTtf);
    target->setGlyphFallback(&uiGlyphSource);
    LOGF("[font] ui fallback: %s\n", uiFontName);
  } else {
    target->setGlyphFallback(nullptr);
    if (uiFontName[0] != 0) LOGF("[font] ui fallback %s failed\n", uiFontName);
  }
}

// ---------------------------------------------------------------------------
// Screens (FreeInkUI)

void libraryScreen(App::ScreenType& s, void*) {
  const uint16_t pct = battery.readPercentage();
  snprintf(statusText, sizeof(statusText), "%u books", static_cast<unsigned>(shelf.count));
  char batteryMeta[12];
  snprintf(batteryMeta, sizeof(batteryMeta), "%u%%", pct);
  s.insetContent(ui::Insets{0, 14, 0, 16});

  ui::HeaderProps h1;
  h1.title = statusText;
  h1.rightLabel = batteryMeta;
  h1.titleOffsetY = -4;
  h1.borderEdges = 0;  // borderless: it is a headline, not chrome
  s.header(h1);
  s.spacer(12);
  ui::Rect actions = s.takeBottom(18);
  ui::ButtonProps settings;
  settings.icon = iconSettings24();
  settings.iconSize = 34;  // scaled up from the 24px asset, but keep footer compact
  settings.action = ActionSettings;
  settings.styles = s.theme().button;
  settings.radius = 8;
  settings.minTouchSize = 44;
  settings.hitPadding = {2, 4, 2, 4};
  ui::Rect settingsRect{static_cast<int16_t>(actions.right() - 48),
                        static_cast<int16_t>(actions.bottom() - 44), 44, 44};
  ui::button(s.frame(), settingsRect, settings);
  if (shelf.count == 0) {
    s.centeredText("No books found.\nCopy .epub files to /Books on the SD card.");
    return;
  }
  const ui::Rect gridBounds = s.body();
  ui::Rect body = gridBounds;
  const uint8_t columns = 2;
  const int16_t rowHeight = static_cast<int16_t>(Shelf::kCoverH + 8);
  const int16_t rowGap = 7;
  const int16_t columnGap = 36;
  const int16_t packedGridW =
      static_cast<int16_t>(columns * (Shelf::kCoverW + 8) + (columns - 1) * columnGap);
  if (packedGridW < body.width) {
    body.x = static_cast<int16_t>(body.x + (body.width - packedGridW) / 2);
    body.width = packedGridW;
  }
  const uint16_t visible = ui::coverGridVisibleCells(body, columns, rowHeight, rowGap);
  libraryVisibleCells = visible;
  libraryTop = ui::coverGridTopIndexFor(static_cast<uint16_t>(librarySelected),
                                        static_cast<uint16_t>(shelf.count), columns, visible);
  ui::CoverGridProps grid;
  grid.itemProvider = libraryGridItem;
  grid.count = static_cast<uint16_t>(shelf.count);
  grid.topIndex = libraryTop;
  grid.selectedIndex = librarySelected;
  grid.action = ActionOpenBook;
  grid.columns = columns;
  grid.coverSize = {Shelf::kCoverW, Shelf::kCoverH};
  grid.rowHeight = rowHeight;
  grid.rowGap = rowGap;
  grid.gap = columnGap;
  grid.cellInset = {4, 0, 4, 0};
  grid.labelHeight = 0;
  grid.cellStyles = ui::selectedPlainListRowStyles();
  grid.selectionIndicator = ui::CoverGridSelectionIndicator::CoverFrame;
  grid.selectedCoverFrameGap = 4;
  grid.selectedCoverFrameWidth = 2;
  grid.selectedCoverFrameRadius = 5;
  grid.coverPainter = libraryCoverPainter;
  grid.scrollIndicator = false;
  ui::coverGrid(s.frame(), body, grid);

  const uint16_t visibleRows = static_cast<uint16_t>(visible / columns);
  const int16_t strideY = static_cast<int16_t>(rowHeight + rowGap);
  const int16_t partialY = static_cast<int16_t>(body.y + visibleRows * strideY);
  const int16_t partialH = static_cast<int16_t>(body.bottom() - partialY);
  const uint16_t previewStart = static_cast<uint16_t>(libraryTop + visible);
  if (partialH >= 24 && previewStart < shelf.count && visibleRows > 0) {
    const int16_t cellW = static_cast<int16_t>((body.width - (columns - 1) * columnGap) / columns);
    const int16_t previewH = partialH;
    for (uint8_t col = 0; col < columns && previewStart + col < shelf.count; ++col) {
      ui::Rect cell{static_cast<int16_t>(body.x + col * (cellW + columnGap)), partialY, cellW, previewH};
      ui::Rect cover{static_cast<int16_t>(cell.x + (cell.width - Shelf::kCoverW) / 2), cell.y,
                     Shelf::kCoverW, previewH};
      drawLibraryCoverPreview(s.frame().target(), cover, static_cast<uint16_t>(previewStart + col));
    }
  }

  if (shelf.count > visible && visible > 0) {
    const int16_t trackW = 3;
    const int16_t trackX = static_cast<int16_t>(gridBounds.right() - trackW);
    s.frame().target().fill(ui::Rect{trackX, gridBounds.y, trackW, gridBounds.height},
                            ui::Paint::dither(ui::Color::LightGray));
    int16_t thumbH = static_cast<int16_t>((static_cast<int32_t>(gridBounds.height) * visible) / shelf.count);
    if (thumbH < 18) thumbH = 18;
    const uint16_t range = static_cast<uint16_t>(shelf.count - visible);
    int16_t thumbY = static_cast<int16_t>(
        gridBounds.y + (range > 0 ? (static_cast<int32_t>(gridBounds.height - thumbH) * libraryTop) / range : 0));
    if (libraryTop >= range) thumbY = static_cast<int16_t>(gridBounds.bottom() - thumbH);
    s.frame().target().fill(ui::Rect{trackX, thumbY, trackW, thumbH},
                            ui::Paint::solid(ui::Color::Black));
  }
}

void readerScreen(App::ScreenType& s, void*) {
  // Page content is composited after app render; the screen contributes the
  // full-page tap zones. Contents is opened with a top-edge swipe.
  const ui::Rect body = s.body();
  const int16_t zoneW = static_cast<int16_t>(body.width / 3);
  // Leave inputMask/state/enabled at their defaults (InputTouch) — zeroing
  // the mask makes a zone accept no input at all.
  const ui::TapZone zones[] = {
      {.rect = {body.x, body.y, zoneW, body.height}, .action = ActionPagePrev},
      {.rect = {static_cast<int16_t>(body.x + zoneW), body.y, zoneW, body.height},
       .action = ActionToc},
      {.rect = {static_cast<int16_t>(body.x + 2 * zoneW), body.y, zoneW, body.height},
       .action = ActionPageNext},
  };
  ui::TapZonesProps props;
  props.zones = zones;
  props.count = 3;
  props.swipeLeft = ActionPageNext;
  props.swipeRight = ActionPagePrev;
  props.back = ActionBackToLibrary;
  ui::tapZones(s.frame(), body, props);
}

void tocScreen(App::ScreenType& s, void*) {
  static ui::ListItem items[128];
  static char fallbackLabels[128][32];
  const bool hasToc = session.bk.tocCount() > 0;
  const uint16_t n = static_cast<uint16_t>(
      (hasToc ? session.bk.tocCount() : session.bk.spineCount()) < 128
          ? (hasToc ? session.bk.tocCount() : session.bk.spineCount())
          : 128);
  int16_t selected = -1;
  for (uint16_t i = 0; i < n; ++i) {
    items[i] = ui::ListItem{};
    if (hasToc) {
      const book::TocEntry* toc = session.bk.tocEntry(i);
      items[i].label = toc != nullptr ? toc->title : "";
      if (selected < 0 && toc != nullptr) {
        const book::ManifestItem* spine = session.bk.spineItem(session.pos.spineIndex);
        if (spine != nullptr && strcmp(spine->href, toc->href) == 0) selected = static_cast<int16_t>(i);
      }
    } else {
      snprintf(fallbackLabels[i], sizeof(fallbackLabels[i]), "Chapter %u", static_cast<unsigned>(i + 1));
      items[i].label = fallbackLabels[i];
      if (i == session.pos.spineIndex) selected = static_cast<int16_t>(i);
    }
    items[i].actionValue = static_cast<int16_t>(i);
  }
  if (n == 0) {
    s.navHeader("Contents", ActionBackToReader, ui::BitmapRef{}, nullptr, ui::EdgesNone);
    s.centeredText("No table of contents found.");
    return;
  }
  s.navHeader("Contents", ActionBackToReader, ui::BitmapRef{}, nullptr, ui::EdgesNone);
  s.insetContent({8, 12, 0, 12});
  tocVisibleRows = ui::listVisibleRows(s.body(), s.theme().rowHeight, 0);
  LOGF("[toc] open=%d n=%u visible=%u rowH=%d body=%dx%d first='%s'\n", session.open ? 1 : 0,
       n, tocVisibleRows, s.theme().rowHeight, s.body().width, s.body().height,
       n > 0 ? items[0].label : "-");
  tocTop = ui::listTopIndexFor(tocAnchorSelected ? selected : -1, tocTop, tocVisibleRows, n);
  tocAnchorSelected = false;
  s.list(items, n, selected, ActionTocJump, tocTop);
}

void settingsScreen(App::ScreenType& s, void*) {
  char summary[20];
  snprintf(summary, sizeof(summary), "%d books", shelf.count);
  s.navHeader("Settings", ActionBackToLibrary, ui::BitmapRef{}, nullptr, ui::EdgesNone);
  s.insetContent({8, 12, 0, 12});
  if (libraryRefreshRequested) {
    s.centeredText("Scanning Library...\nChecking the SD card for books.");
    return;
  }
  const ui::Rect settingsBody = s.body();

  ui::TextStyle label = s.theme().bodyText;
  label.bold = true;
  const int16_t rowH = static_cast<int16_t>(s.target().lineHeight(label.font) +
                                            s.target().lineHeight(s.theme().smallText.font) + 16);
  const int16_t rowGap = static_cast<int16_t>(s.theme().spaceMd * rowH / s.theme().rowHeight);
  static constexpr uint16_t kSettingsRows = 12;
  settingsVisibleRows = ui::listVisibleRows(settingsBody, rowH, rowGap);
  if (settingsVisibleRows == 0) settingsVisibleRows = 1;
  if (settingsVisibleRows > kSettingsRows) settingsVisibleRows = kSettingsRows;
  const uint16_t settingsMaxTop = kSettingsRows > settingsVisibleRows
                                      ? static_cast<uint16_t>(kSettingsRows - settingsVisibleRows)
                                      : 0;
  if (settingsTop > settingsMaxTop) settingsTop = settingsMaxTop;
  uint16_t settingsRowIndex = 0;
  static constexpr int16_t kSettingsLeftInset = 14;
  static constexpr int16_t kSettingsRightInset = 26;
  auto takeSettingRect = [&]() {
    return s.takeTop(rowH, rowGap).inset({0, kSettingsRightInset, 0, kSettingsLeftInset});
  };
  auto shouldDrawSetting = [&]() {
    return settingsRowIndex >= settingsTop &&
           settingsRowIndex < static_cast<uint16_t>(settingsTop + settingsVisibleRows);
  };
  auto settingsRow = [&](ui::SettingRowProps& row, ui::BitmapRef icon) {
    if (shouldDrawSetting()) {
      row.labelText = label;
      row.subtitleText = s.theme().smallText;
      row.icon = icon;
      row.iconSize = 22;
      ui::settingRow(s.frame(), takeSettingRect(), row);
    }
    ++settingsRowIndex;
  };

  ui::SettingRowProps refresh;
  refresh.label = "Refresh Library";
  refresh.subtitle = "Rescan the SD card for EPUB files";
  refresh.action = ActionRefreshLibrary;
  refresh.value = summary;
  settingsRow(refresh, iconRefresh16());

  int16_t selected = -1;  // built-in
  for (int i = 0; i < fontShelf.count; ++i) {
    if (strcmp(fontShelf.names[i], currentFontName) == 0) selected = static_cast<int16_t>(i);
  }
  const char* fontValue = selected < 0 ? "Built-in Font" : fontShelf.names[selected];
  ui::DropdownProps font;
  font.label = "Selected Reading Font";
  font.subtitle = fontValue;
  font.subtitleText = s.theme().smallText;
  font.icon = iconBook16();
  font.iconSize = 22;
  font.action = ActionFontMenu;
  font.labelText = label;
  font.valueText = s.theme().bodyText;
  font.styles = s.theme().button;
  font.radius = 8;
  font.padding = {6, 8, 6, 8};
  if (shouldDrawSetting()) ui::dropdown(s.frame(), takeSettingRect(), font);
  ++settingsRowIndex;

  static char sizeValue[12];
  snprintf(sizeValue, sizeof(sizeValue), "%u px", defaultSizePx);
  ui::DropdownProps size;
  size.label = "Reading Size";
  size.subtitle = sizeValue;
  size.subtitleText = s.theme().smallText;
  size.icon = iconBook16();
  size.iconSize = 22;
  size.action = ActionSizeMenu;
  size.labelText = label;
  size.valueText = s.theme().bodyText;
  size.styles = s.theme().button;
  size.radius = 8;
  size.padding = {6, 8, 6, 8};
  if (shouldDrawSetting()) ui::dropdown(s.frame(), takeSettingRect(), size);
  ++settingsRowIndex;

  ui::DropdownProps uiFont;
  uiFont.label = "UI Font";
  uiFont.subtitle = uiFontName[0] ? uiFontName : "Built-in (Latin only)";
  uiFont.subtitleText = s.theme().smallText;
  uiFont.icon = iconBook16();
  uiFont.iconSize = 22;
  uiFont.action = ActionUiFontMenu;
  uiFont.labelText = label;
  uiFont.valueText = s.theme().bodyText;
  uiFont.styles = s.theme().button;
  uiFont.radius = 8;
  uiFont.padding = {6, 8, 6, 8};
  if (shouldDrawSetting()) ui::dropdown(s.frame(), takeSettingRect(), uiFont);
  ++settingsRowIndex;

  static char lineValue[12], marginValue[12];
  snprintf(lineValue, sizeof(lineValue), "%u%%", lineSpacingPct);
  snprintf(marginValue, sizeof(marginValue), "%u px", screenMarginPx);
  static const char* kAlignNames[4] = {"Justified", "Left", "Center", "Right"};
  auto pickerRow = [&](const char* lbl, const char* val, ui::ActionId action) {
    ui::DropdownProps d;
    d.label = lbl;
    d.subtitle = val;
    d.subtitleText = s.theme().smallText;
    d.icon = iconBook16();
    d.iconSize = 22;
    d.action = action;
    d.labelText = label;
    d.styles = s.theme().button;
    d.radius = 8;
    d.padding = {6, 8, 6, 8};
    if (shouldDrawSetting()) ui::dropdown(s.frame(), takeSettingRect(), d);
    ++settingsRowIndex;
  };
  pickerRow("Line Spacing", lineValue, ActionLineMenu);
  pickerRow("Page Margin", marginValue, ActionMarginMenu);
  pickerRow("Alignment", kAlignNames[paraAlign], ActionAlignMenu);
  pickerRow("Orientation", kOrientNames[orientationSetting], ActionOrientMenu);

  auto toggle = [&](const char* lbl, bool on, ui::ActionId action) {
    ui::ToggleRowProps t;
    t.row.label = lbl;
    t.row.labelText = label;
    t.checked = on;
    t.toggleAction = action;
    if (shouldDrawSetting()) ui::toggleRow(s.frame(), takeSettingRect(), t);
    ++settingsRowIndex;
  };
  toggle("Hyphenation", hyphenateSetting != 0, ActionToggleHyphen);
  toggle("Sharp Text (no AA)", sharpText != 0, ActionToggleSharp);
  toggle("Extra Paragraph Spacing", extraParaSpacing != 0, ActionToggleParaSpace);
  toggle("Embedded Book Styles", embeddedStyles != 0, ActionToggleEmbCss);
  toggle("Focus Reading", focusReading != 0, ActionToggleFocus);

  if (kSettingsRows > settingsVisibleRows) {
    const int16_t trackW = 3;
    const int16_t trackX = static_cast<int16_t>(settingsBody.right() - trackW);
    s.frame().target().fill(ui::Rect{trackX, settingsBody.y, trackW, settingsBody.height},
                            ui::Paint::dither(ui::Color::LightGray));
    int16_t thumbH = static_cast<int16_t>((static_cast<int32_t>(settingsBody.height) * settingsVisibleRows) /
                                          kSettingsRows);
    if (thumbH < 12) thumbH = 12;
    const int16_t range = static_cast<int16_t>(kSettingsRows - settingsVisibleRows);
    int16_t thumbY = static_cast<int16_t>(
        settingsBody.y + (range > 0 ? (static_cast<int32_t>(settingsBody.height - thumbH) * settingsTop) / range
                                    : 0));
    if (settingsTop >= settingsMaxTop) {
      thumbY = static_cast<int16_t>(settingsBody.bottom() - thumbH);
    }
    s.frame().target().fill(ui::Rect{trackX, thumbY, trackW, thumbH},
                            ui::Paint::solid(ui::Color::Black));
  }

  // Dropdowns open option dialogs; tapping outside dismisses them.
  if (settingsMenu == 1) {
    static ui::DialogOption opts[FontShelf::kMax + 2];
    uint8_t n = 0;
    opts[n++] = {"Built-in Font", ActionPickFont, -1,
                 currentFontName[0] == 0 ? ui::StateSelected : ui::StateNormal, true};
    for (int i = 0; i < fontShelf.count && n < FontShelf::kMax + 1; ++i) {
      opts[n++] = {fontShelf.names[i], ActionPickFont, static_cast<int16_t>(i),
                   strcmp(fontShelf.names[i], currentFontName) == 0 ? ui::StateSelected : ui::StateNormal,
                   true};
    }
    ui::OptionDialogProps d;
    d.title = "Reading Font";
    d.options = opts;
    d.optionCount = n;
    d.verticalOptions = true;
    d.dimBackground = true;
    d.buttonStyles = s.theme().button;
    s.frame().hit(s.frame().screen(), ActionCloseMenu);
    s.dialog(d);
  } else if (settingsMenu >= 4 && settingsMenu <= 7) {
    static char labels[6][12];
    static ui::DialogOption opts[7];
    uint8_t n = 0;
    const char* title = "";
    if (settingsMenu == 4) {
      title = "Line Spacing";
      static const uint16_t v[4] = {100, 115, 130, 150};
      for (int i = 0; i < 4; ++i) {
        snprintf(labels[i], sizeof(labels[i]), "%u%%", v[i]);
        opts[n++] = {labels[i], ActionPickLine, static_cast<int16_t>(v[i]),
                     lineSpacingPct == v[i] ? ui::StateSelected : ui::StateNormal, true};
      }
    } else if (settingsMenu == 5) {
      title = "Page Margin";
      static const uint16_t v[4] = {16, 24, 32, 40};
      for (int i = 0; i < 4; ++i) {
        snprintf(labels[i], sizeof(labels[i]), "%u px", v[i]);
        opts[n++] = {labels[i], ActionPickMargin, static_cast<int16_t>(v[i]),
                     screenMarginPx == v[i] ? ui::StateSelected : ui::StateNormal, true};
      }
    } else if (settingsMenu == 6) {
      title = "Alignment";
      static const char* names[4] = {"Justified", "Left", "Center", "Right"};
      for (int i = 0; i < 4; ++i) {
        opts[n++] = {names[i], ActionPickAlign, static_cast<int16_t>(i),
                     paraAlign == i ? ui::StateSelected : ui::StateNormal, true};
      }
    } else {
      title = "Orientation";
      for (int i = 0; i < 4; ++i) {
        opts[n++] = {kOrientNames[i], ActionPickOrient, static_cast<int16_t>(i),
                     orientationSetting == i ? ui::StateSelected : ui::StateNormal, true};
      }
    }
    ui::OptionDialogProps d;
    d.title = title;
    d.options = opts;
    d.optionCount = n;
    d.verticalOptions = true;
    d.dimBackground = true;
    d.buttonStyles = s.theme().button;
    s.frame().hit(s.frame().screen(), ActionCloseMenu);
    s.dialog(d);
  } else if (settingsMenu == 3) {
    static ui::DialogOption opts[FontShelf::kMax + 2];
    uint8_t n = 0;
    opts[n++] = {"Built-in (Latin only)", ActionPickUiFont, -1,
                 uiFontName[0] == 0 ? ui::StateSelected : ui::StateNormal, true};
    for (int i = 0; i < fontShelf.count && n < FontShelf::kMax + 1; ++i) {
      opts[n++] = {fontShelf.names[i], ActionPickUiFont, static_cast<int16_t>(i),
                   strcmp(fontShelf.names[i], uiFontName) == 0 ? ui::StateSelected : ui::StateNormal,
                   true};
    }
    ui::OptionDialogProps d;
    d.title = "UI Font";
    d.options = opts;
    d.optionCount = n;
    d.verticalOptions = true;
    d.dimBackground = true;
    d.buttonStyles = s.theme().button;
    s.frame().hit(s.frame().screen(), ActionCloseMenu);
    s.dialog(d);
  } else if (settingsMenu == 2) {
    static char sizeLabels[6][8];
    static ui::DialogOption opts[7];
    for (int i = 0; i < 6; ++i) {
      snprintf(sizeLabels[i], sizeof(sizeLabels[i]), "%u px", kFontSizes[i]);
      opts[i] = {sizeLabels[i], ActionPickSize, static_cast<int16_t>(kFontSizes[i]),
                 defaultSizePx == kFontSizes[i] ? ui::StateSelected : ui::StateNormal, true};
    }
    ui::OptionDialogProps d;
    d.title = "Reading Size";
    d.options = opts;
    d.optionCount = 6;
    d.verticalOptions = true;
    d.dimBackground = true;
    d.buttonStyles = s.theme().button;
    s.frame().hit(s.frame().screen(), ActionCloseMenu);
    s.dialog(d);
  }
}

void goToPage(Screen next, bool initialPaint = false) {
  float sx0, sy0, sx1, sy1;
  while (input.popSwipe(sx0, sy0, sx1, sy1)) {
    // Drop gestures completed on the previous screen; otherwise an edge swipe
    // can immediately act on the screen it just opened.
  }
  screen = next;
  app->clearTapFlash();
  switch (next) {
    case Screen::Library: app->setScreen(libraryScreen, nullptr, ui::RefreshHint::None); break;
    case Screen::Reader: app->setScreen(readerScreen, nullptr, ui::RefreshHint::None); break;
    case Screen::Toc: app->setScreen(tocScreen, nullptr, ui::RefreshHint::None); break;
    case Screen::Settings: app->setScreen(settingsScreen, nullptr, ui::RefreshHint::None); break;
  }
  if (initialPaint) {
    app->invalidate(ui::RefreshHint::Full);
  } else {
    app->invalidateTransition();
  }
}

// ---------------------------------------------------------------------------
// Action handlers

void onOpenBook(const ui::ActionEvent& e, void*) {
  librarySelected = e.value;
  if (session.begin(shelf.paths[e.value], fonts, (hyphReady && hyphenateSetting) ? &hyphenator : nullptr, bookBuf,
                    512 * 1024, scratchBuf, 512 * 1024, indexBuf, 64 * 1024)) {
    readerChromeVisible = false;
    tocTop = 0;
    goToPage(Screen::Reader);
  }
}

void onPageTurn(const ui::ActionEvent& e, void*) {
  session.turn(e.action == ActionPageNext ? 1 : -1);
  app->invalidate(ui::RefreshHint::Fast);
}

void onCenterTap(const ui::ActionEvent&, void*) {
  readerChromeVisible = false;
  tocAnchorSelected = true;
  goToPage(Screen::Toc);
}

void onTocJump(const ui::ActionEvent& e, void*) {
  const bool ok = session.bk.tocCount() > 0
                      ? session.jumpToToc(static_cast<size_t>(e.value))
                      : session.jumpToSpine(static_cast<uint16_t>(e.value));
  if (ok) {
    readerChromeVisible = false;
    goToPage(Screen::Reader);
  }
}

void onBackToReader(const ui::ActionEvent&, void*) {
  readerChromeVisible = false;
  goToPage(Screen::Reader);
}

void onFontSize(const ui::ActionEvent&, void*) {
  static const uint16_t kSizes[] = {14, 16, 18, 21, 24, 28};
  uint16_t next = kSizes[0];
  for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); ++i) {
    if (kSizes[i] > session.pos.baseSizePx) {
      next = kSizes[i];
      break;
    }
  }
  session.setBaseSize(next);
  readerChromeVisible = false;
  app->invalidate(ui::RefreshHint::Full);
}

void onSettings(const ui::ActionEvent&, void*) {
  fontShelf.scan();
  settingsTop = 0;
  goToPage(Screen::Settings);
}

void onRefreshLibrary(const ui::ActionEvent&, void*) {
  settingsMenu = 0;
  libraryRefreshRequested = true;
  libraryRefreshPainted = false;
  app->invalidate(ui::RefreshHint::Full);
}

void onFontMenu(const ui::ActionEvent&, void*) {
  fontShelf.scan();
  settingsMenu = 1;
  app->invalidate(ui::RefreshHint::Fast);
}

void onSizeMenu(const ui::ActionEvent&, void*) {
  settingsMenu = 2;
  app->invalidate(ui::RefreshHint::Fast);
}

void onUiFontMenu(const ui::ActionEvent&, void*) {
  fontShelf.scan();
  settingsMenu = 3;
  app->invalidate(ui::RefreshHint::Fast);
}

void onPickUiFont(const ui::ActionEvent& e, void*) {
  if (e.value < 0) uiFontName[0] = 0;
  else snprintf(uiFontName, sizeof(uiFontName), "%s", fontShelf.names[e.value]);
  saveFontSetting();
  applyUiFont();
  settingsMenu = 0;
  app->invalidate(ui::RefreshHint::Full);
}

template <typename T>
void pickSetting(T& slot, T value) {
  slot = value;
  saveFontSetting();
  settingsMenu = 0;
  app->invalidate(ui::RefreshHint::Full);
}
void onLineMenu(const ui::ActionEvent&, void*) { settingsMenu = 4; app->invalidate(ui::RefreshHint::Fast); }
void onMarginMenu(const ui::ActionEvent&, void*) { settingsMenu = 5; app->invalidate(ui::RefreshHint::Fast); }
void onAlignMenu(const ui::ActionEvent&, void*) { settingsMenu = 6; app->invalidate(ui::RefreshHint::Fast); }
void onPickLine(const ui::ActionEvent& e, void*) { pickSetting(lineSpacingPct, static_cast<uint16_t>(e.value)); }
void onPickMargin(const ui::ActionEvent& e, void*) { pickSetting(screenMarginPx, static_cast<uint16_t>(e.value)); }
void onPickAlign(const ui::ActionEvent& e, void*) { pickSetting(paraAlign, static_cast<uint8_t>(e.value)); }
void onOrientMenu(const ui::ActionEvent&, void*) { settingsMenu = 7; app->invalidate(ui::RefreshHint::Fast); }
void onPickOrient(const ui::ActionEvent& e, void*) {
  pickSetting(orientationSetting, static_cast<uint8_t>(e.value));
  applyOrientation();  // swap the logical frame + touch mapping now
}
void onToggleHyphen(const ui::ActionEvent&, void*) { hyphenateSetting ^= 1; saveFontSetting(); app->invalidate(ui::RefreshHint::Fast); }
void onToggleSharp(const ui::ActionEvent&, void*) { sharpText ^= 1; saveFontSetting(); app->invalidate(ui::RefreshHint::Fast); }
void onToggleParaSpace(const ui::ActionEvent&, void*) { extraParaSpacing ^= 1; saveFontSetting(); app->invalidate(ui::RefreshHint::Fast); }
void onToggleEmbCss(const ui::ActionEvent&, void*) { embeddedStyles ^= 1; saveFontSetting(); app->invalidate(ui::RefreshHint::Fast); }
void onToggleFocus(const ui::ActionEvent&, void*) { focusReading ^= 1; saveFontSetting(); app->invalidate(ui::RefreshHint::Fast); }

void onCloseMenu(const ui::ActionEvent&, void*) {
  settingsMenu = 0;
  app->invalidate(ui::RefreshHint::Full);  // clear the dimmed backdrop
}

void onPickFont(const ui::ActionEvent& e, void*) {
  if (e.value < 0) currentFontName[0] = 0;
  else snprintf(currentFontName, sizeof(currentFontName), "%s", fontShelf.names[e.value]);
  saveFontSetting();
  applyFont();
  settingsMenu = 0;
  app->invalidate(ui::RefreshHint::Full);
}

void onPickSize(const ui::ActionEvent& e, void*) {
  defaultSizePx = static_cast<uint16_t>(e.value);
  saveFontSetting();
  settingsMenu = 0;
  app->invalidate(ui::RefreshHint::Full);
}

void onBackToLibrary(const ui::ActionEvent&, void*) {
  if (screen == Screen::Settings) {
    goToPage(Screen::Library);
    return;
  }
  session.end();
  goToPage(Screen::Library);
}

// ---------------------------------------------------------------------------

void setup() {
  // Power-rail latch FIRST (Sticky PWR_HOLD/PWR_LOCK) — and release the SD
  // rail before touching the display: SD shares the display's SPI bus, and a
  // rail latched off by a previous sleep clamps SCLK/MOSI (blank panel).
  BoardConfig::holdPowerRails();
  BoardConfig::releaseSdRail();
  delay(10);

  delay(250);  // let USB-Serial/JTAG power up before CDC begin
  Serial.begin(115200);
  Serial.setTxTimeoutMs(1);  // never block when no host is reading
  LOGF("[freeink-books] boot (psram %u KB)\n", ESP.getPsramSize() / 1024);

  // SD before display: they share the SPI bus, and SDCardManager::begin()
  // expects to probe the card before the display driver claims the bus (it
  // deselects the panel CS itself for exactly this window).
  SdMan.begin();
  display.begin();
  input.begin();
  // Input on its own task: taps/presses queue while renders and panel
  // refreshes keep loop() busy. loop() must NOT call input.update().
  input.beginAsync(/*taskPriority=*/2, /*pollMs=*/10);

  // Portrait: the landscape-native panel is held tall; DisplayTarget rotates
  // UI drawing, and PageRenderer rotates page content to match.
  static ui::DisplayTarget tgt(display.getFrameBuffer(), display.getDisplayWidth(),
                               display.getDisplayHeight(), display.getDisplayWidthBytes(),
                               ui::Orientation::Portrait);
  static App application(tgt, tgt.deviceContext());
  target = &tgt;
  app = &application;
  app->setClearColor(ui::Color::White);  // start every frame from white

  // Borderless look: no boxes around buttons/rows/dropdowns; selection
  // feedback is a light fill. One theme assignment, every component inherits.
  ui::ThemeTokens theme = app->theme();
  theme.button = ui::flatButtonStyles(8);
  app->setTheme(theme);

  bookBuf = psAlloc(512 * 1024);  // webnovel omnibuses: 1800+ entries need >256KB
  scratchBuf = psAlloc(512 * 1024);
  indexBuf = psAlloc(64 * 1024);
  glyphBuf = psAlloc(128 * 1024);
  coverBookBuf = psAlloc(128 * 1024);
  coverScratchBuf = psAlloc(192 * 1024);

  // Reading font: the persisted Settings choice, defaulting to the first TTF
  // found (or built-in when none). applyFont() always chains the built-in.
  loadFontSetting();
  fontShelf.scan();
  if (currentFontName[0] == 0 && fontShelf.count > 0) {
    snprintf(currentFontName, sizeof(currentFontName), "%s", fontShelf.names[0]);
  }
  applyFont();
  applyUiFont();
  applyOrientation();
  uint32_t len = 0;
  if (loadSdBlob("/fonts", ".fibh", &hyphData, &len)) {
    hyphReady = hyphenator.init(hyphData, len);
  }
  // No pattern file on the card: fall back to the English patterns embedded
  // in the SDK (tools/hyphc.py --header). Hyphenation is what keeps justified
  // lines from stretching into word-gap canyons.
  if (!hyphReady) hyphReady = hyphenator.init(book::k_hyph_en_us, book::k_hyph_en_us_size);

  shelf.scan();
  LOGF("[freeink-books] sd=%d books=%d font=%s\n", SdMan.ready() ? 1 : 0, shelf.count,
       currentFontName[0] ? currentFontName : "(built-in)");
  app->on(ActionOpenBook, onOpenBook);
  app->on(ActionPageNext, onPageTurn);
  app->on(ActionPagePrev, onPageTurn);
  app->on(ActionBackToReader, onBackToReader);
  app->on(ActionToc, onCenterTap);
  app->on(ActionTocJump, onTocJump);
  app->on(ActionFontSize, onFontSize);
  app->on(ActionSettings, onSettings);
  app->on(ActionPickFont, onPickFont);
  app->on(ActionPickSize, onPickSize);
  app->on(ActionFontMenu, onFontMenu);
  app->on(ActionSizeMenu, onSizeMenu);
  app->on(ActionUiFontMenu, onUiFontMenu);
  app->on(ActionPickUiFont, onPickUiFont);
  app->on(ActionLineMenu, onLineMenu);
  app->on(ActionPickLine, onPickLine);
  app->on(ActionMarginMenu, onMarginMenu);
  app->on(ActionPickMargin, onPickMargin);
  app->on(ActionAlignMenu, onAlignMenu);
  app->on(ActionPickAlign, onPickAlign);
  app->on(ActionToggleHyphen, onToggleHyphen);
  app->on(ActionToggleSharp, onToggleSharp);
  app->on(ActionToggleParaSpace, onToggleParaSpace);
  app->on(ActionToggleEmbCss, onToggleEmbCss);
  app->on(ActionToggleFocus, onToggleFocus);
  app->on(ActionOrientMenu, onOrientMenu);
  app->on(ActionPickOrient, onPickOrient);
  app->on(ActionCloseMenu, onCloseMenu);
  app->on(ActionRefreshLibrary, onRefreshLibrary);
  app->on(ActionBackToLibrary, onBackToLibrary);
  goToPage(Screen::Library, /*initialPaint=*/true);
}

void loop() {
  // Hardware buttons (queued by the input task).
  uint8_t btn;
  while (input.popPress(btn)) {
    if (screen == Screen::Reader && !readerChromeVisible) {
      if (btn == InputManager::BTN_DOWN && session.turn(1)) {
        app->invalidate(ui::RefreshHint::Fast);
      } else if (btn == InputManager::BTN_UP && session.turn(-1)) {
        app->invalidate(ui::RefreshHint::Fast);
      } else if (btn == InputManager::BTN_CONFIRM) {
        tocAnchorSelected = true;
        goToPage(Screen::Toc);
      }
      continue;
    }
    ui::InputSnapshot b;
    b.focusNext = btn == InputManager::BTN_DOWN;
    b.focusPrev = btn == InputManager::BTN_UP;
    b.confirm = btn == InputManager::BTN_CONFIRM;
    app->route(b);
  }

  // Edge gestures mirror CrossPoint: a bottom-edge upward swipe exits, and a
  // top-edge downward swipe opens contents. Regular list swipes are handled only
  // when they do not originate in those edge bands.
  float sx0, sy0, sx1, sy1;
  while (input.popSwipe(sx0, sy0, sx1, sy1)) {
    const ui::Point a = ui::touchToLogical(app->device(), sx0, sy0);
    const ui::Point b = ui::touchToLogical(app->device(), sx1, sy1);
    const int16_t h = app->device().height;
    const int16_t dx = static_cast<int16_t>(b.x - a.x);
    const int16_t dy = static_cast<int16_t>(b.y - a.y);
    const int16_t adx = dx < 0 ? static_cast<int16_t>(-dx) : dx;
    const int16_t ady = dy < 0 ? static_cast<int16_t>(-dy) : dy;
    const int16_t topEdgeBand = static_cast<int16_t>((h * 14) / 100);
    const int16_t homeEdgeBand = 40;
    const int16_t lowerEdge = static_cast<int16_t>(h - homeEdgeBand);
    const bool verticalSwipe = ady > adx;
    if (screen == Screen::Reader && verticalSwipe && a.y <= topEdgeBand && dy > 0) {
      readerChromeVisible = false;
      tocAnchorSelected = true;
      goToPage(Screen::Toc);
      break;
    } else if (screen != Screen::Library && verticalSwipe && a.y >= lowerEdge && dy < 0) {
      if (screen == Screen::Toc) {
        goToPage(Screen::Reader);
        break;
      } else {
        if (screen == Screen::Reader) {
          session.end();
        }
        goToPage(Screen::Library);
        break;
      }
    } else if (screen == Screen::Library && verticalSwipe && dy != 0) {
      if (shelf.count > libraryVisibleCells && libraryVisibleCells > 0) {
        const uint16_t step = libraryVisibleCells;
        const uint16_t maxSelected = static_cast<uint16_t>(shelf.count - 1);
        if (dy < 0) {
          librarySelected = static_cast<int16_t>(
              librarySelected + step > maxSelected ? maxSelected : librarySelected + step);
        } else {
          librarySelected = librarySelected > step ? static_cast<int16_t>(librarySelected - step) : 0;
        }
        libraryFastScrollFrame = true;
        libraryHydrateAfterScroll = true;
        app->invalidate(ui::RefreshHint::Fast);
      }
    } else if (screen == Screen::Toc && dy != 0) {
      const uint16_t tocCount = static_cast<uint16_t>(
          (session.bk.tocCount() > 0 ? session.bk.tocCount() : session.bk.spineCount()) < 128
              ? (session.bk.tocCount() > 0 ? session.bk.tocCount() : session.bk.spineCount())
              : 128);
      if (tocCount > tocVisibleRows && tocVisibleRows > 0) {
        const uint16_t maxTop = static_cast<uint16_t>(tocCount - tocVisibleRows);
        const uint16_t step = tocVisibleRows > 1 ? static_cast<uint16_t>(tocVisibleRows - 1) : 1;
        if (dy < 0) {
          tocTop = static_cast<uint16_t>(tocTop + step > maxTop ? maxTop : tocTop + step);
        } else {
          tocTop = tocTop > step ? static_cast<uint16_t>(tocTop - step) : 0;
        }
        app->invalidate(ui::RefreshHint::Fast);
      }
    } else if (screen == Screen::Settings && settingsMenu == 0 && dy != 0) {
      static constexpr uint16_t kSettingsRows = 12;
      if (kSettingsRows > settingsVisibleRows && settingsVisibleRows > 0) {
        const uint16_t maxTop = static_cast<uint16_t>(kSettingsRows - settingsVisibleRows);
        if (dy < 0) {
          settingsTop = settingsTop < maxTop ? static_cast<uint16_t>(settingsTop + 1) : maxTop;
        } else {
          settingsTop = settingsTop > 0 ? static_cast<uint16_t>(settingsTop - 1) : 0;
        }
        app->invalidate(ui::RefreshHint::Fast);
      }
    }
  }

  // Touch taps (queued): route against the last rendered frame.
  float nx, ny;
  while (input.popTouchTap(nx, ny)) {
    ui::InputSnapshot tap;
    const ui::Point p = ui::touchToLogical(app->device(), nx, ny);
    // Links win over page-turn zones (small targets, deliberate taps).
    if (screen == Screen::Reader && !readerChromeVisible && session.open) {
      bool hit = false;
      for (uint8_t l = 0; l < session.linkCount && !hit; ++l) {
        const auto& b = session.links[l];
        if (p.x >= b.x - 4 && p.x < b.x + b.w + 4 && p.y >= b.y - 4 && p.y < b.y + b.h + 4) {
          if (session.followLink(b)) app->invalidate(ui::RefreshHint::Fast);
          hit = true;
        }
      }
      if (hit) continue;
    }
    tap.touchReleased = true;
    tap.touchX = p.x;
    tap.touchY = p.y;
    app->route(tap);
  }

  static ui::RefreshHint pending = ui::RefreshHint::None;
  if (app->invalidated()) {
    app->render();
    // Composite the book page into the same framebuffer under the chrome.
    if (screen == Screen::Reader && session.open && !readerChromeVisible) {
      freeink::book::FrameTarget frame{display.getFrameBuffer(),
                                       static_cast<int16_t>(display.getDisplayWidth()),
                                       static_cast<int16_t>(display.getDisplayHeight()),
                                       static_cast<int16_t>(display.getDisplayWidthBytes()),
                                       sharpText ? freeink::book::FrameFormat::Mono1Sharp
                                                 : freeink::book::FrameFormat::Mono1Dithered,
                                       kPageRot[orientationSetting]};
      if (!session.renderCurrent(fonts, frame)) {
        LOGF("[reader] page %u/%u render FAILED (ch %u)\n", session.pageInChapter + 1,
             session.reader.pageCount(), session.pos.spineIndex);
      }
    }
    const ui::RefreshHint hint = app->lastRenderRefreshHint();
    if (static_cast<uint8_t>(hint) > static_cast<uint8_t>(pending)) pending = hint;
  }

  // Push the newest frame whenever the panel is idle.
  if (pending != ui::RefreshHint::None && !display.refreshBusy()) {
    ui::presentAsync(display, pending);
    if (libraryHydrateAfterScroll) {
      libraryFastScrollFrame = false;
      libraryHydrateAfterScroll = false;
      app->invalidate(ui::RefreshHint::Fast);
    }
    if (libraryRefreshRequested && !libraryRefreshPainted) {
      libraryRefreshPainted = true;
    }
    pending = ui::RefreshHint::None;
  }

  if (libraryRefreshRequested && libraryRefreshPainted && !display.refreshBusy()) {
    shelf.scan();
    fontShelf.scan();
    if (librarySelected >= shelf.count) {
      librarySelected = shelf.count > 0 ? static_cast<int16_t>(shelf.count - 1) : 0;
    }
    libraryTop = 0;
    libraryRefreshRequested = false;
    libraryRefreshPainted = false;
    app->invalidate(ui::RefreshHint::Full);
  }

  // Deep sleep only on a genuine live hold: GPIO4 is the shared
  // CONFIRM/POWER button, and a stale held-time reading here would put the
  // device to sleep right after boot (dead USB CDC, frozen panel).
  if (input.isPressed(InputManager::BTN_POWER) && input.getPowerButtonHeldTime() > 1500) {
    session.end();
    PowerManager::deepSleepUntilPowerButton();
  }
  delay(10);
}
