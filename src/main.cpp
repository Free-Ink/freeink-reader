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
  ActionLibraryTab,
};

enum class Screen : uint8_t { Boot, Library, Reader, Toc, Sleep };

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

bool isBookFileName(const char* name) {
  if (name == nullptr) return false;
  const size_t len = strlen(name);
  if (len > 5 && strcasecmp(name + len - 5, ".epub") == 0) return true;
  if (len > 4 && strcasecmp(name + len - 4, ".txt") == 0) return true;
  return false;
}

bool isCrashReportName(const char* name) {
  return containsIgnoreCase(name, "crash") || containsIgnoreCase(name, "panic") ||
         containsIgnoreCase(name, "backtrace");
}

bool isHiddenOrSystemDir(const char* name) {
  if (name == nullptr || name[0] == 0 || name[0] == '.') return true;
  return strcasecmp(name, "BookCache") == 0 || strcasecmp(name, "System Volume Information") == 0 ||
         strcasecmp(name, "fonts") == 0 || strcasecmp(name, "sleep") == 0 ||
         strcasecmp(name, "screenshots") == 0 || strcasecmp(name, "themes") == 0;
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
    SdMan.ensureDirectoryExists("/BookCache");
    SdMan.ensureDirectoryExists("/BookCache/shelf");
    releaseCovers();
    count = 0;
    addFrom("/", 0);  // books anywhere: card root works out of the box
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

  void addFrom(const char* dir, uint8_t depth) {
    const bool root = dir[0] == '/' && dir[1] == 0;
    FsFile d = SdMan.open(dir, O_RDONLY);
    if (!d || !d.isDirectory()) {
      if (d) d.close();
      return;
    }
    char name[128];
    for (FsFile f = d.openNextFile(); f && count < kMax; f = d.openNextFile()) {
      f.getName(name, sizeof(name));
      if (f.isDirectory()) {
        if (depth < 6 && !isHiddenOrSystemDir(name)) {
          char child[160];
          snprintf(child, sizeof(child), "%s%s%s", dir, root ? "" : "/", name);
          f.close();
          addFrom(child, static_cast<uint8_t>(depth + 1));
          continue;
        }
        f.close();
        continue;
      }
      f.close();
      const bool isBook = isBookFileName(name);
      const bool isCrashReport = isCrashReportName(name);
      if (name[0] == '.' || isCrashReport || !isBook || count >= kMax) continue;
      snprintf(paths[count], sizeof(paths[count]), "%s%s%s", dir, root ? "" : "/", name);
      const size_t nameLen = strlen(name);
      const bool isEpub = nameLen > 5 && strcasecmp(name + nameLen - 5, ".epub") == 0;
      snprintf(titles[count], sizeof(titles[count]), "%.*s",
               static_cast<int>(nameLen - (isEpub ? 5 : 4)), name);
      snprintf(metas[count], sizeof(metas[count]), "%s", dir);
      items[count] = ui::ListItem{};
      items[count].label = titles[count];
      items[count].subtitle = metas[count];
      items[count].actionValue = static_cast<int16_t>(count);
      coverBits[count] = nullptr;
      detailsReady[count] = false;
      coverTried[count] = false;
      loadCachedEntry(static_cast<uint16_t>(count));
      ++count;
    }
    d.close();
  }

  bool loadCachedEntry(uint16_t index);
  void saveCachedEntry(uint16_t index);
  void ensureDetails(uint16_t index);
  bool ensureCover(uint16_t index);
  ui::CoverGridItem gridItem(uint16_t index, bool loadDetails = true);
};

struct Progress {  // persisted per book: (spineIndex, charStart, baseSizePx)
  uint16_t spineIndex = 0;
  uint32_t charStart = 0;
  uint16_t baseSizePx = 18;
  uint8_t percent = 0;
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
    if (open) {
      const uint32_t chapters = isTxt ? 1 : static_cast<uint32_t>(bk.spineCount());
      const uint32_t pages = reader.pageCount() > 0 ? reader.pageCount() : 1;
      uint32_t pct = chapters > 0
                         ? (static_cast<uint32_t>(pos.spineIndex) * 100u +
                            (static_cast<uint32_t>(pageInChapter) * 100u) / pages) /
                               chapters
                         : 0;
      if (pct > 100) pct = 100;
      pos.percent = static_cast<uint8_t>(pct);
    }
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
enum class LibraryTab : uint8_t { Recent = 0, AllBooks = 1, Settings = 2 };
LibraryTab libraryTab = LibraryTab::Recent;
int16_t librarySelected = 0;
uint16_t libraryTop = 0;
uint16_t libraryVisibleCells = 0;
uint16_t allBooksTop = 0;
uint16_t allBooksVisibleRows = 0;
static constexpr uint16_t kAllBooksMaxRows = 48;
enum class BrowserEntryKind : uint8_t { Up, Folder, Book };
char allBooksPath[160] = "/";
char allBooksEntryNames[kAllBooksMaxRows][64];
char allBooksEntrySubtitles[kAllBooksMaxRows][48];
int16_t allBooksEntryShelfIndex[kAllBooksMaxRows];
BrowserEntryKind allBooksEntryKind[kAllBooksMaxRows];
ui::ListItem allBooksItems[kAllBooksMaxRows];
uint16_t allBooksEntryCount = 0;
int16_t allBooksSelected = 0;
bool allBooksBrowserDirty = true;
static constexpr uint16_t kRecentBookLimit = 4;
uint32_t recentHashes[Shelf::kMax];
int16_t recentShelfForSlot[Shelf::kMax];
uint8_t recentHashCount = 0;
uint8_t recentVisibleCount = 0;
uint16_t tocTop = 0;
uint16_t tocVisibleRows = 0;
bool tocAnchorSelected = false;
uint16_t settingsTop = 0;
uint16_t settingsVisibleRows = 0;
bool readerChromeVisible = false;
bool libraryRefreshRequested = false;
bool libraryRefreshPainted = false;
int16_t pendingOpenShelfIndex = -1;
bool pendingOpenToastVisible = false;
bool pendingOpenToastPresented = false;
uint32_t pendingOpenToastShownAt = 0;
char statusText[96];
uint8_t loadingPhase = 0;
bool ignorePowerUntilRelease = true;

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

struct ShelfCacheHeader {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t headerSize = 0;
  uint32_t pathHash = 0;
  uint32_t sampleHash = 0;
  uint64_t fileSize = 0;
  uint16_t coverW = 0;
  uint16_t coverH = 0;
  uint16_t coverStride = 0;
  uint16_t coverBytes = 0;
  uint8_t hasCover = 0;
  uint8_t coverTried = 0;
  char title[64];
  char author[48];
  char meta[48];
  char coverHref[160];
};

static constexpr uint32_t kShelfCacheMagic = 0x46425348;  // FBSH
static constexpr uint16_t kShelfCacheVersion = 2;

uint32_t shelfPathHash(const char* path) {
  return book::ZipCatalog::hashPath(path);
}

void loadRecentHashes() {
  recentHashCount = 0;
  FsFile f = SdMan.open("/BookCache/recent.bin", O_RDONLY);
  if (!f) return;
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t count = 0;
  if (f.read(&magic, sizeof(magic)) != sizeof(magic) ||
      f.read(&version, sizeof(version)) != sizeof(version) ||
      f.read(&count, sizeof(count)) != sizeof(count) ||
      magic != 0x46425243 || version != 1) {
    f.close();
    return;
  }
  if (count > Shelf::kMax) count = Shelf::kMax;
  for (uint16_t i = 0; i < count; ++i) {
    uint32_t hash = 0;
    if (f.read(&hash, sizeof(hash)) != sizeof(hash)) break;
    recentHashes[recentHashCount++] = hash;
  }
  f.close();
}

void saveRecentHashes() {
  SdMan.ensureDirectoryExists("/BookCache");
  FsFile f = SdMan.open("/BookCache/recent.tmp", O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return;
  const uint32_t magic = 0x46425243;  // FBRC
  const uint16_t version = 1;
  const uint16_t count = recentHashCount;
  bool ok = f.write(&magic, sizeof(magic)) == sizeof(magic) &&
            f.write(&version, sizeof(version)) == sizeof(version) &&
            f.write(&count, sizeof(count)) == sizeof(count);
  for (uint16_t i = 0; ok && i < count; ++i) {
    ok = f.write(&recentHashes[i], sizeof(recentHashes[i])) == sizeof(recentHashes[i]);
  }
  f.close();
  if (!ok) {
    SdMan.remove("/BookCache/recent.tmp");
    return;
  }
  SdMan.remove("/BookCache/recent.bin");
  SdMan.rename("/BookCache/recent.tmp", "/BookCache/recent.bin");
}

int16_t shelfIndexForHash(uint32_t hash) {
  for (int i = 0; i < shelf.count; ++i) {
    if (shelfPathHash(shelf.paths[i]) == hash) return static_cast<int16_t>(i);
  }
  return -1;
}

void rebuildRecentShelfSlots() {
  recentVisibleCount = 0;
  for (uint8_t i = 0; i < Shelf::kMax; ++i) recentShelfForSlot[i] = -1;
  for (uint8_t i = 0; i < recentHashCount && recentVisibleCount < kRecentBookLimit; ++i) {
    const int16_t shelfIndex = shelfIndexForHash(recentHashes[i]);
    if (shelfIndex >= 0) recentShelfForSlot[recentVisibleCount++] = shelfIndex;
  }
  for (int16_t shelfIndex = 0; shelfIndex < shelf.count && recentVisibleCount < kRecentBookLimit; ++shelfIndex) {
    bool alreadyShown = false;
    for (uint8_t i = 0; i < recentVisibleCount; ++i) {
      if (recentShelfForSlot[i] == shelfIndex) {
        alreadyShown = true;
        break;
      }
    }
    if (!alreadyShown) recentShelfForSlot[recentVisibleCount++] = shelfIndex;
  }
}

void promoteRecentBook(uint16_t shelfIndex) {
  if (shelfIndex >= shelf.count) return;
  const uint32_t hash = shelfPathHash(shelf.paths[shelfIndex]);
  uint8_t out = 0;
  uint32_t next[Shelf::kMax];
  next[out++] = hash;
  for (uint8_t i = 0; i < recentHashCount && out < Shelf::kMax; ++i) {
    if (recentHashes[i] == hash) continue;
    if (shelfIndexForHash(recentHashes[i]) < 0) continue;
    next[out++] = recentHashes[i];
  }
  memcpy(recentHashes, next, out * sizeof(uint32_t));
  recentHashCount = out;
  rebuildRecentShelfSlots();
  saveRecentHashes();
}

int16_t shelfIndexForPath(const char* path) {
  for (int i = 0; i < shelf.count; ++i) {
    if (strcmp(shelf.paths[i], path) == 0) return static_cast<int16_t>(i);
  }
  return -1;
}

bool browserAtRoot() {
  return allBooksPath[0] == '/' && allBooksPath[1] == 0;
}

void browserParentPath(char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) return;
  snprintf(out, outLen, "%s", allBooksPath);
  if (out[0] == '/' && out[1] == 0) return;
  char* slash = strrchr(out, '/');
  if (slash == nullptr || slash == out) {
    snprintf(out, outLen, "/");
  } else {
    *slash = 0;
  }
}

void browserJoinPath(const char* dir, const char* name, char* out, size_t outLen) {
  const bool root = dir != nullptr && dir[0] == '/' && dir[1] == 0;
  snprintf(out, outLen, "%s%s%s", dir != nullptr ? dir : "/", root ? "" : "/", name != nullptr ? name : "");
}

void resetAllBooksBrowser(const char* path = "/") {
  snprintf(allBooksPath, sizeof(allBooksPath), "%s", (path != nullptr && path[0]) ? path : "/");
  allBooksTop = 0;
  allBooksSelected = 0;
  allBooksBrowserDirty = true;
}

void addBrowserEntry(BrowserEntryKind kind, const char* label, const char* subtitle, int16_t shelfIndex) {
  if (allBooksEntryCount >= kAllBooksMaxRows) return;
  const uint16_t i = allBooksEntryCount++;
  snprintf(allBooksEntryNames[i], sizeof(allBooksEntryNames[i]), "%s", label != nullptr ? label : "");
  snprintf(allBooksEntrySubtitles[i], sizeof(allBooksEntrySubtitles[i]), "%s",
           subtitle != nullptr ? subtitle : "");
  allBooksEntryShelfIndex[i] = shelfIndex;
  allBooksEntryKind[i] = kind;
  allBooksItems[i] = ui::ListItem{};
  allBooksItems[i].label = allBooksEntryNames[i];
  allBooksItems[i].subtitle = allBooksEntrySubtitles[i][0] ? allBooksEntrySubtitles[i] : nullptr;
  allBooksItems[i].actionValue = static_cast<int16_t>(i);
}

uint8_t savedProgressPercentForShelf(uint16_t shelfIndex) {
  if (shelfIndex >= shelf.count) return 0;
  char path[128];
  snprintf(path, sizeof(path), "/BookCache/%08x/progress.bin",
           static_cast<unsigned>(shelfPathHash(shelf.paths[shelfIndex])));
  FsFile f = SdMan.open(path, O_RDONLY);
  if (!f) return 0;
  Progress p{};
  const int n = f.read(&p, sizeof(p));
  f.close();
  if (n < static_cast<int>(sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t))) return 0;
  return p.percent <= 100 ? p.percent : 0;
}

void bookListSubtitle(uint16_t shelfIndex, char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) return;
  const uint8_t pct = savedProgressPercentForShelf(shelfIndex);
  if (shelfIndex < shelf.count && shelf.authors[shelfIndex][0]) {
    snprintf(out, outLen, "%u%% - %s", static_cast<unsigned>(pct), shelf.authors[shelfIndex]);
  } else {
    snprintf(out, outLen, "%u%%", static_cast<unsigned>(pct));
  }
}

void rebuildAllBooksBrowser() {
  allBooksEntryCount = 0;
  if (!browserAtRoot()) {
    addBrowserEntry(BrowserEntryKind::Up, "Back", "Parent folder", -1);
  }

  FsFile dir = SdMan.open(allBooksPath, O_RDONLY);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    resetAllBooksBrowser("/");
    dir = SdMan.open(allBooksPath, O_RDONLY);
  }
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    allBooksBrowserDirty = false;
    return;
  }

  char name[128];
  for (FsFile f = dir.openNextFile(); f && allBooksEntryCount < kAllBooksMaxRows; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    const bool isDir = f.isDirectory();
    f.close();
    if (isDir) {
      if (isHiddenOrSystemDir(name)) continue;
      addBrowserEntry(BrowserEntryKind::Folder, name, "Folder", -1);
    }
  }
  dir.close();

  dir = SdMan.open(allBooksPath, O_RDONLY);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    allBooksBrowserDirty = false;
    return;
  }
  for (FsFile f = dir.openNextFile(); f && allBooksEntryCount < kAllBooksMaxRows; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    const bool isDir = f.isDirectory();
    f.close();
    if (isDir || name[0] == '.' || !isBookFileName(name) || isCrashReportName(name)) continue;
    char path[160];
    browserJoinPath(allBooksPath, name, path, sizeof(path));
    const int16_t shelfIndex = shelfIndexForPath(path);
    if (shelfIndex < 0) continue;
    char subtitle[48];
    bookListSubtitle(static_cast<uint16_t>(shelfIndex), subtitle, sizeof(subtitle));
    addBrowserEntry(BrowserEntryKind::Book, shelf.titles[shelfIndex], subtitle, shelfIndex);
  }
  dir.close();

  if (allBooksSelected >= static_cast<int16_t>(allBooksEntryCount)) {
    allBooksSelected = allBooksEntryCount > 0 ? static_cast<int16_t>(allBooksEntryCount - 1) : 0;
  }
  allBooksBrowserDirty = false;
}

uint16_t shelfCoverStride() {
  return static_cast<uint16_t>((Shelf::kCoverW + 3) / 4);
}

uint16_t shelfCoverBytes() {
  return static_cast<uint16_t>(shelfCoverStride() * Shelf::kCoverH);
}

void shelfCachePath(const char* bookPath, char* out, size_t outLen) {
  snprintf(out, outLen, "/BookCache/shelf/%08x.bin", static_cast<unsigned>(shelfPathHash(bookPath)));
}

uint64_t fileSizeForPath(const char* path) {
  FsFile f = SdMan.open(path, O_RDONLY);
  if (!f) return 0;
  const uint64_t size = f.fileSize();
  f.close();
  return size;
}

uint32_t fnv1aUpdate(uint32_t hash, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t fileSampleHash(const char* path) {
  FsFile f = SdMan.open(path, O_RDONLY);
  if (!f) return 0;
  uint32_t hash = 2166136261UL;
  const uint64_t size = f.fileSize();
  hash = fnv1aUpdate(hash, reinterpret_cast<const uint8_t*>(&size), sizeof(size));
  uint8_t buf[128];
  const uint32_t firstLen = static_cast<uint32_t>(size < sizeof(buf) ? size : sizeof(buf));
  if (firstLen > 0 && f.seekSet(0)) {
    const int n = f.read(buf, firstLen);
    if (n > 0) hash = fnv1aUpdate(hash, buf, static_cast<size_t>(n));
  }
  if (size > sizeof(buf) && f.seekSet(size > sizeof(buf) ? size - sizeof(buf) : 0)) {
    const int n = f.read(buf, sizeof(buf));
    if (n > 0) hash = fnv1aUpdate(hash, buf, static_cast<size_t>(n));
  }
  f.close();
  return hash;
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

bool Shelf::loadCachedEntry(uint16_t index) {
  if (index >= count) return false;
  char path[96];
  shelfCachePath(paths[index], path, sizeof(path));
  FsFile f = SdMan.open(path, O_RDONLY);
  if (!f) return false;

  ShelfCacheHeader h;
  const bool headerOk = f.read(&h, sizeof(h)) == static_cast<int>(sizeof(h));
  if (!headerOk || h.magic != kShelfCacheMagic || h.version != kShelfCacheVersion ||
      h.headerSize != sizeof(ShelfCacheHeader) || h.pathHash != shelfPathHash(paths[index]) ||
      h.fileSize != fileSizeForPath(paths[index]) || h.sampleHash != fileSampleHash(paths[index]) ||
      h.coverW != kCoverW || h.coverH != kCoverH || h.coverStride != shelfCoverStride() ||
      h.coverBytes != shelfCoverBytes()) {
    f.close();
    return false;
  }

  snprintf(titles[index], sizeof(titles[index]), "%s", h.title);
  snprintf(authors[index], sizeof(authors[index]), "%s", h.author);
  snprintf(metas[index], sizeof(metas[index]), "%s", h.meta);
  snprintf(coverHrefs[index], sizeof(coverHrefs[index]), "%s", h.coverHref);
  items[index].label = titles[index];
  items[index].subtitle = authors[index][0] ? authors[index] : metas[index];
  detailsReady[index] = true;
  coverTried[index] = h.coverTried != 0;

  if (h.hasCover) {
    uint8_t* bits = static_cast<uint8_t*>(psAlloc(h.coverBytes));
    if (bits != nullptr && f.read(bits, h.coverBytes) == h.coverBytes) {
      coverBits[index] = bits;
      coverTried[index] = true;
    } else if (bits != nullptr) {
      free(bits);
    }
  }
  f.close();
  return true;
}

void Shelf::saveCachedEntry(uint16_t index) {
  if (index >= count || !detailsReady[index]) return;
  SdMan.ensureDirectoryExists("/BookCache");
  SdMan.ensureDirectoryExists("/BookCache/shelf");

  char path[96];
  char tmpPath[104];
  shelfCachePath(paths[index], path, sizeof(path));
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);

  ShelfCacheHeader h;
  h.magic = kShelfCacheMagic;
  h.version = kShelfCacheVersion;
  h.headerSize = sizeof(ShelfCacheHeader);
  h.pathHash = shelfPathHash(paths[index]);
  h.sampleHash = fileSampleHash(paths[index]);
  h.fileSize = fileSizeForPath(paths[index]);
  h.coverW = kCoverW;
  h.coverH = kCoverH;
  h.coverStride = shelfCoverStride();
  h.coverBytes = shelfCoverBytes();
  h.hasCover = coverBits[index] != nullptr ? 1 : 0;
  h.coverTried = coverTried[index] ? 1 : 0;
  snprintf(h.title, sizeof(h.title), "%s", titles[index]);
  snprintf(h.author, sizeof(h.author), "%s", authors[index]);
  snprintf(h.meta, sizeof(h.meta), "%s", metas[index]);
  snprintf(h.coverHref, sizeof(h.coverHref), "%s", coverHrefs[index]);

  FsFile f = SdMan.open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return;
  bool ok = f.write(&h, sizeof(h)) == sizeof(h);
  if (ok && h.hasCover) {
    ok = f.write(coverBits[index], h.coverBytes) == h.coverBytes;
  }
  f.close();
  if (!ok) {
    SdMan.remove(tmpPath);
    return;
  }
  SdMan.remove(path);
  SdMan.rename(tmpPath, path);
}

void Shelf::ensureDetails(uint16_t index) {
  if (index >= count || detailsReady[index]) return;
  detailsReady[index] = true;
  if (coverBookBuf == nullptr || coverScratchBuf == nullptr) return;

  SdBookSource source;
  if (!source.open(paths[index])) {
    LOGF("[cover] %s: SD open FAILED\n", paths[index]);
    return;
  }

  book::Arena bookArena;
  book::Arena scratch;
  bookArena.init(coverBookBuf, 512 * 1024);
  scratch.init(coverScratchBuf, 192 * 1024);
  book::Book bk;
  const BookStatus openStatus = bk.open(source, bookArena, scratch);
  if (openStatus != BookStatus::Ok) {
    LOGF("[cover] %s: book open %s\n", paths[index], book::bookStatusName(openStatus));
  }
  if (openStatus == BookStatus::Ok) {
    if (bk.metadata().title != nullptr && bk.metadata().title[0] != 0) {
      snprintf(titles[index], sizeof(titles[index]), "%s", bk.metadata().title);
    }
    if (bk.metadata().author != nullptr && bk.metadata().author[0] != 0) {
      snprintf(authors[index], sizeof(authors[index]), "%s", bk.metadata().author);
    }
    const book::ManifestItem* cover = chooseCoverItem(bk);
    if (cover != nullptr && cover->href != nullptr) {
      snprintf(coverHrefs[index], sizeof(coverHrefs[index]), "%s", cover->href);
    } else {
      LOGF("[cover] %s: no cover item in manifest\n", paths[index]);
    }
    snprintf(metas[index], sizeof(metas[index]), "%u chapters",
             static_cast<unsigned>(bk.spineCount()));
  }
  source.close();
  items[index].label = titles[index];
  items[index].subtitle = authors[index][0] ? authors[index] : metas[index];
  saveCachedEntry(index);
}

bool Shelf::ensureCover(uint16_t index) {
  if (index >= count) return false;
  ensureDetails(index);
  if (coverBits[index] != nullptr) return true;
  if (coverTried[index]) return false;
  coverTried[index] = true;
  if (coverHrefs[index][0] == 0 || coverBookBuf == nullptr || coverScratchBuf == nullptr) {
    saveCachedEntry(index);
    return false;
  }

  const uint16_t stride = static_cast<uint16_t>((kCoverW + 3) / 4);
  uint8_t* bits = static_cast<uint8_t*>(psAlloc(static_cast<size_t>(stride) * kCoverH));
  if (bits == nullptr) {
    saveCachedEntry(index);
    return false;
  }
  memset(bits, 0xFF, static_cast<size_t>(stride) * kCoverH);

  SdBookSource source;
  if (!source.open(paths[index])) {
    free(bits);
    saveCachedEntry(index);
    return false;
  }
  book::Arena bookArena;
  book::Arena scratch;
  bookArena.init(coverBookBuf, 512 * 1024);
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
      const BookStatus rs =
          book::ImageRenderer::render(source, bk.zip(), image, scratch, coverDecodeRow, &ctx);
      ok = rs == BookStatus::Ok;
      if (!ok) {
        LOGF("[cover] %s: render %s (probe kind=%d %ux%u prog=%d)\n", coverHrefs[index],
             book::bookStatusName(rs), (int)info.kind, info.width, info.height,
             (int)info.progressive);
      }
      scratch.release(mark);
    } else {
      LOGF("[cover] %s: zip entry not found\n", coverHrefs[index]);
    }
  }
  source.close();

  if (!ok) {
    free(bits);
    saveCachedEntry(index);
    return false;
  }
  coverBits[index] = bits;
  saveCachedEntry(index);
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
  return shelf.gridItem(index);
}

ui::CoverGridItem recentGridItem(uint16_t index, void*) {
  if (index >= recentVisibleCount || recentShelfForSlot[index] < 0) return ui::CoverGridItem{};
  return shelf.gridItem(static_cast<uint16_t>(recentShelfForSlot[index]));
}

ui::BitmapRef iconBook16();
ui::BitmapRef iconFolder16();
ui::BitmapRef iconSettings16();
ui::BitmapRef iconRecent16();
ui::BitmapRef settingIconForIndex(uint16_t index);

ui::BitmapRef iconRef(const Icon& icon) {
  return ui::BitmapRef{icon.bits, icon.w, icon.h, ui::BitmapFormat::Mask1};
}

ui::BitmapRef iconSettings16() {
  return ui::bitmapFromIcon(icon_settings_22);
}

ui::BitmapRef iconRecent16() {
  return ui::bitmapFromIcon(icon_clock_22);
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
  draw.fill(rect, ui::Paint::solid(ui::Color::White), 4);
  draw.stroke(rect, ui::Paint::solid(ui::Color::Black), 1, 4);
  const bool hasCover = shelf.ensureCover(index);
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

bool recentCoverPainter(ui::DrawTarget& draw, ui::Rect rect, const ui::CoverGridItem& item,
                        uint16_t index, void*) {
  if (index >= recentVisibleCount || recentShelfForSlot[index] < 0) return false;
  return libraryCoverPainter(draw, rect, item, static_cast<uint16_t>(recentShelfForSlot[index]), nullptr);
}

void drawRecentManualCell(App::ScreenType& s, uint8_t slot, ui::Rect coverRect, bool selected) {
  if (slot >= recentVisibleCount || recentShelfForSlot[slot] < 0) return;
  ui::CoverGridItem item = recentGridItem(slot, nullptr);
  ui::Rect hitRect = ui::ensureMinTouchRect(coverRect, s.theme().minTouchSize, s.frame().screen());
  ui::State state = selected ? ui::StateSelected : ui::StateNormal;
  s.frame().hit(hitRect, ActionOpenBook, item.actionValue, ui::InputDefault, state);
  state = s.frame().stateFor(ActionOpenBook, item.actionValue, state);
  const bool active = (state & ui::StateSelected) != 0;
  recentCoverPainter(s.frame().target(), coverRect, item, slot, nullptr);
  if (active) {
    const int16_t gap = 4;
    s.frame().target().stroke(
        ui::Rect{static_cast<int16_t>(coverRect.x - gap), static_cast<int16_t>(coverRect.y - gap),
                 static_cast<int16_t>(coverRect.width + gap * 2),
                 static_cast<int16_t>(coverRect.height + gap * 2)},
        ui::Paint::solid(ui::Color::Black), 2, 5);
  }
}

void drawLibraryCoverPreview(ui::DrawTarget& draw, ui::Rect rect, uint16_t index) {
  if (rect.height <= 0 || rect.width <= 0 || index >= shelf.count) return;
  draw.fill(rect, ui::Paint::solid(ui::Color::White), 4);
  draw.stroke(rect, ui::Paint::solid(ui::Color::Black), 1, 4);
  ui::Rect cover = rect.inset(ui::Insets{3, 3, 0, 3});
  if (cover.height <= 0) return;
  const bool hasCover = shelf.ensureCover(index);
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

ui::BitmapRef iconFolder16() {
  return ui::bitmapFromIcon(icon_folder_22);
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

ui::BitmapRef iconType16() { return ui::bitmapFromIcon(icon_type_22); }
ui::BitmapRef iconTextSize16() { return ui::bitmapFromIcon(icon_text_size_22); }
ui::BitmapRef iconUiFont16() { return ui::bitmapFromIcon(icon_ui_font_22); }
ui::BitmapRef iconLineSpacing16() { return ui::bitmapFromIcon(icon_line_spacing_22); }
ui::BitmapRef iconMargin16() { return ui::bitmapFromIcon(icon_margin_22); }
ui::BitmapRef iconAlign16() { return ui::bitmapFromIcon(icon_align_22); }
ui::BitmapRef iconOrientation16() { return ui::bitmapFromIcon(icon_orientation_22); }
ui::BitmapRef iconHyphen16() { return ui::bitmapFromIcon(icon_hyphen_22); }
ui::BitmapRef iconSharp16() { return ui::bitmapFromIcon(icon_sharp_22); }
ui::BitmapRef iconParagraph16() { return ui::bitmapFromIcon(icon_paragraph_22); }
ui::BitmapRef iconEmbedded16() { return ui::bitmapFromIcon(icon_embedded_22); }
ui::BitmapRef iconFocus16() { return ui::bitmapFromIcon(icon_focus_22); }

ui::BitmapRef settingIconForIndex(uint16_t index) {
  switch (index) {
    case 0: return iconRefresh16();
    case 1: return iconType16();
    case 2: return iconTextSize16();
    case 3: return iconUiFont16();
    case 4: return iconLineSpacing16();
    case 5: return iconMargin16();
    case 6: return iconAlign16();
    case 7: return iconOrientation16();
    case 8: return iconHyphen16();
    case 9: return iconSharp16();
    case 10: return iconParagraph16();
    case 11: return iconEmbedded16();
    case 12: return iconFocus16();
    default: return iconSettings16();
  }
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

void drawSettingsContent(App::ScreenType& s, bool withNavHeader);

static constexpr int16_t kLibraryTabHeight = 104;

void drawLibraryTabs(App::ScreenType& s, ui::Rect rect) {
  ui::TabItem tabs[3] = {
      ui::tabItem(static_cast<int>(LibraryTab::Recent), libraryTab == LibraryTab::Recent, true, "Recent"),
      ui::tabItem(static_cast<int>(LibraryTab::AllBooks), libraryTab == LibraryTab::AllBooks, true, "All Books"),
      ui::tabItem(static_cast<int>(LibraryTab::Settings), libraryTab == LibraryTab::Settings, true, "Settings"),
  };
  tabs[0].icon = iconRecent16();
  tabs[1].icon = iconBook16();
  tabs[2].icon = iconSettings16();
  ui::TabBarProps props;
  props.tabs = tabs;
  props.count = 3;
  props.action = ActionLibraryTab;
  props.text = s.theme().smallText;
  props.tabStyles = ui::flatButtonStyles(8);
  props.tabStyles.normal.background = ui::Paint::none();
  props.tabStyles.normal.foreground = ui::Paint::solid(ui::Color::Black);
  props.tabStyles.selected.background = ui::Paint::none();
  props.tabStyles.selected.foreground = ui::Paint::solid(ui::Color::Black);
  props.tabStyles.focused = props.tabStyles.normal;
  props.tabStyles.active = props.tabStyles.normal;
  props.tabInset = {6, 2, 6, 2};
  props.contentInset = {12, 16, 20, 16};
  props.iconSize = 34;
  props.selectedDotSize = 8;
  props.selectedDotInsetBottom = 5;
  props.divider = true;
  ui::tabBar(s.frame(), rect, props);
}

void drawRecentGrid(App::ScreenType& s) {
  if (shelf.count == 0) {
    s.centeredText("No books found.\nCopy .epub files to /Books on the SD card.");
    libraryVisibleCells = 0;
    return;
  }
  const ui::Rect gridBounds = s.body();
  ui::Rect body = gridBounds;
  const uint8_t columns = 2;
  const int16_t rowGap = 10;
  int16_t recentCoverH = static_cast<int16_t>((body.height - rowGap - 8) / 2);
  if (recentCoverH > Shelf::kCoverH) recentCoverH = Shelf::kCoverH;
  if (recentCoverH < 190) recentCoverH = 190;
  const int16_t recentCoverW = static_cast<int16_t>((static_cast<int32_t>(recentCoverH) * 2) / 3);
  const int16_t rowHeight = static_cast<int16_t>(recentCoverH + 4);
  const int16_t columnGap = 36;
  rebuildRecentShelfSlots();
  const int16_t packedGridW =
      static_cast<int16_t>(columns * (recentCoverW + 8) + (columns - 1) * columnGap);
  if (packedGridW < body.width) {
    body.x = static_cast<int16_t>(body.x + (body.width - packedGridW) / 2);
    body.width = packedGridW;
  }
  const int16_t gridRows = recentVisibleCount <= 2 ? recentVisibleCount : 2;
  if (gridRows > 0) {
    const int16_t gridH = static_cast<int16_t>(gridRows * rowHeight + (gridRows - 1) * rowGap);
    if (gridH < body.height) {
      body.y = static_cast<int16_t>(body.y + (body.height - gridH) / 2);
      body.height = gridH;
    }
  }
  const uint16_t visible = ui::coverGridVisibleCells(body, columns, rowHeight, rowGap);
  libraryVisibleCells = visible;
  int16_t selectedSlot = -1;
  for (uint8_t i = 0; i < recentVisibleCount; ++i) {
    if (recentShelfForSlot[i] == librarySelected) {
      selectedSlot = i;
      break;
    }
  }
  if (selectedSlot < 0 && recentVisibleCount > 0) selectedSlot = 0;
  libraryTop = 0;
  if (recentVisibleCount <= 2) {
    const int16_t singleCoverH = recentVisibleCount == 1
                                     ? recentCoverH
                                     : static_cast<int16_t>((body.height - rowGap - 12) / 2);
    const int16_t coverH = singleCoverH > Shelf::kCoverH ? Shelf::kCoverH : singleCoverH;
    const int16_t coverW = static_cast<int16_t>((static_cast<int32_t>(coverH) * 2) / 3);
    const int16_t x = static_cast<int16_t>(body.x + (body.width - coverW) / 2);
    if (recentVisibleCount == 1) {
      const int16_t y = static_cast<int16_t>(body.y + (body.height - coverH) / 2);
      drawRecentManualCell(s, 0, ui::Rect{x, y, coverW, coverH}, selectedSlot == 0);
    } else {
      const int16_t totalH = static_cast<int16_t>(coverH * 2 + rowGap);
      int16_t y = static_cast<int16_t>(body.y + (body.height - totalH) / 2);
      drawRecentManualCell(s, 0, ui::Rect{x, y, coverW, coverH}, selectedSlot == 0);
      y = static_cast<int16_t>(y + coverH + rowGap);
      drawRecentManualCell(s, 1, ui::Rect{x, y, coverW, coverH}, selectedSlot == 1);
    }
    return;
  }
  ui::CoverGridProps grid;
  grid.itemProvider = recentGridItem;
  grid.count = recentVisibleCount;
  grid.topIndex = 0;
  grid.selectedIndex = selectedSlot;
  grid.action = ActionOpenBook;
  grid.columns = columns;
  grid.coverSize = {recentCoverW, recentCoverH};
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
  grid.coverPainter = recentCoverPainter;
  grid.scrollIndicator = false;
  ui::coverGrid(s.frame(), body, grid);
}

void drawAllBooksList(App::ScreenType& s) {
  if (allBooksBrowserDirty) rebuildAllBooksBrowser();
  if (allBooksEntryCount == 0) {
    s.centeredText("No books found.\nCopy .epub files to /Books on the SD card.");
    allBooksVisibleRows = 0;
    return;
  }
  for (uint16_t i = 0; i < allBooksEntryCount; ++i) {
    if (allBooksEntryKind[i] == BrowserEntryKind::Book) {
      allBooksItems[i].icon = iconBook16();
    } else {
      allBooksItems[i].icon = iconFolder16();
    }
  }
  ui::Rect listRect = s.body().inset({0, 2, 0, 12});
  ui::ListProps list;
  list.items = allBooksItems;
  list.count = allBooksEntryCount;
  list.selectedIndex = allBooksSelected;
  list.action = ActionOpenBook;
  list.topIndex = allBooksTop;
  list.labelText = s.theme().bodyText;
  list.subtitleText = s.theme().smallText;
  list.rowStyles = ui::selectedPlainListRowStyles();
  list.rowHeight = static_cast<int16_t>(s.target().lineHeight(list.labelText.font) +
                                        s.target().lineHeight(list.subtitleText.font) + 22);
  list.rowGap = 6;
  list.rowRadius = 8;
  list.sidePadding = 14;
  list.iconSize = 24;
  list.scrollIndicator = true;
  list.partialTrailingRow = true;
  list.partialTrailingMinHeight = s.target().lineHeight(list.labelText.font);
  allBooksVisibleRows = ui::listVisibleRows(listRect, list.rowHeight, list.rowGap);
  const uint16_t maxTop = allBooksEntryCount > allBooksVisibleRows
                              ? static_cast<uint16_t>(allBooksEntryCount - allBooksVisibleRows)
                              : 0;
  if (allBooksTop > maxTop) allBooksTop = maxTop;
  if (allBooksSelected < static_cast<int16_t>(allBooksTop) ||
      allBooksSelected >= static_cast<int16_t>(allBooksTop + allBooksVisibleRows)) {
    allBooksSelected = static_cast<int16_t>(allBooksTop);
  }
  list.topIndex = allBooksTop;
  ui::list(s.frame(), listRect, list);
}

void libraryScreen(App::ScreenType& s, void*) {
  const uint16_t pct = battery.readPercentage();
  if (libraryTab == LibraryTab::Settings) {
    snprintf(statusText, sizeof(statusText), "Settings");
  } else {
    snprintf(statusText, sizeof(statusText), "%u books", static_cast<unsigned>(shelf.count));
  }
  char batteryMeta[12];
  snprintf(batteryMeta, sizeof(batteryMeta), "%u%%", pct);
  s.insetContent(ui::Insets{0, 10, 0, 12});
  ui::Rect tabs = s.takeBottom(kLibraryTabHeight);

  ui::HeaderProps h1;
  h1.title = statusText;
  h1.rightLabel = batteryMeta;
  h1.titleOffsetY = -4;
  h1.borderEdges = 0;  // borderless: it is a headline, not chrome
  s.header(h1);
  s.spacer(4);
  if (libraryTab == LibraryTab::Recent) {
    drawRecentGrid(s);
  } else if (libraryTab == LibraryTab::AllBooks) {
    drawAllBooksList(s);
  } else {
    drawSettingsContent(s, false);
  }
  drawLibraryTabs(s, tabs);
}

void drawFreeInkLogoDots(ui::DrawTarget& draw, const ui::Rect rect, const uint8_t phase,
                         const bool activeAnimation) {
  const int16_t cell = static_cast<int16_t>(rect.width / 5);
  const int16_t dot = static_cast<int16_t>(cell / 2);
  const int16_t radius = static_cast<int16_t>(dot / 2);
  const int16_t gridW = static_cast<int16_t>(cell * 4 + dot);
  const int16_t x0 = static_cast<int16_t>(rect.x + (rect.width - gridW) / 2);
  const int16_t y0 = static_cast<int16_t>(rect.y + (rect.height - gridW) / 2);
  for (uint8_t r = 0; r < 5; ++r) {
    for (uint8_t c = 0; c < 5; ++c) {
      const bool escaping = r <= 1 && c >= 3;
      ui::Paint ink = ui::Paint::solid(ui::Color::Black);
      if (escaping) {
        if (!activeAnimation) {
          ink = (r == 0 && c == 4) ? ui::Paint::dither(ui::Color::LightGray)
                                   : ui::Paint::dither(ui::Color::DarkGray);
        } else {
          const uint8_t dotIndex = static_cast<uint8_t>((r * 2) + (c - 3));
          const bool active = dotIndex == (phase % 4);
          ink = active ? ui::Paint::solid(ui::Color::Black)
                       : ui::Paint::dither(dotIndex == 1 ? ui::Color::LightGray : ui::Color::DarkGray);
        }
      }
      const int16_t cx = static_cast<int16_t>(x0 + c * cell);
      const int16_t cy = static_cast<int16_t>(y0 + r * cell);
      draw.fill(ui::Rect{cx, cy, dot, dot}, ink, radius);
    }
  }
}

void drawProgressDots(ui::DrawTarget& draw, const ui::Rect rect, const uint8_t phase) {
  const int16_t dot = 8;
  const int16_t gap = 12;
  const int16_t total = static_cast<int16_t>(dot * 3 + gap * 2);
  const int16_t x0 = static_cast<int16_t>(rect.x + (rect.width - total) / 2);
  const int16_t y = static_cast<int16_t>(rect.y + (rect.height - dot) / 2);
  for (uint8_t i = 0; i < 3; ++i) {
    const bool active = i == (phase % 3);
    draw.fill(ui::Rect{static_cast<int16_t>(x0 + i * (dot + gap)), y, dot, dot},
              active ? ui::Paint::solid(ui::Color::Black)
                     : ui::Paint::dither(ui::Color::LightGray),
              static_cast<uint8_t>(dot / 2));
  }
}

void loadingMessageScreen(App::ScreenType& s, const char* message) {
  ui::DrawTarget& draw = s.frame().target();
  const ui::Rect body = s.body();
  const int16_t logoSize = 190;
  const int16_t logoY = static_cast<int16_t>(body.y + (body.height - 300) / 2);
  drawFreeInkLogoDots(draw,
                      ui::Rect{static_cast<int16_t>(body.x + (body.width - logoSize) / 2), logoY,
                               logoSize, logoSize},
                      loadingPhase, true);

  ui::TextStyle title = s.theme().bodyText;
  title.align = ui::TextAlign::Center;
  title.bold = true;
  draw.text(ui::Rect{body.x, static_cast<int16_t>(logoY + logoSize + 26), body.width, 28},
            message, title);
  drawProgressDots(draw,
                   ui::Rect{body.x, static_cast<int16_t>(logoY + logoSize + 62), body.width, 24},
                   loadingPhase);
}

void bootScreen(App::ScreenType& s, void*) {
  loadingMessageScreen(s, "Loading your library");
}

void sleepScreen(App::ScreenType& s, void*) {
  ui::DrawTarget& draw = s.frame().target();
  const ui::Rect body = s.body();
  const int16_t logoSize = 164;
  const int16_t logoY = static_cast<int16_t>(body.y + (body.height - 270) / 2);
  drawFreeInkLogoDots(draw,
                      ui::Rect{static_cast<int16_t>(body.x + (body.width - logoSize) / 2), logoY,
                               logoSize, logoSize},
                      0, false);

  ui::TextStyle title = s.theme().bodyText;
  title.align = ui::TextAlign::Center;
  title.bold = true;
  draw.text(ui::Rect{body.x, static_cast<int16_t>(logoY + logoSize + 30), body.width, 28},
            "Sleeping", title);

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

void drawSettingsContent(App::ScreenType& s, bool withNavHeader) {
  char summary[20];
  snprintf(summary, sizeof(summary), "%d books", shelf.count);
  if (withNavHeader) s.navHeader("Settings", ActionBackToLibrary, ui::BitmapRef{}, nullptr, ui::EdgesNone);
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
  static constexpr uint16_t kSettingsRows = 13;
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
  static constexpr int16_t kSettingsIconSize = 26;
  static constexpr int16_t kSettingsTitleSubtitleGap = 4;
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
      row.iconSize = kSettingsIconSize;
      row.titleSubtitleGap = kSettingsTitleSubtitleGap;
      ui::settingRow(s.frame(), takeSettingRect(), row);
    }
    ++settingsRowIndex;
  };

  ui::SettingRowProps refresh;
  refresh.label = "Refresh Library";
  refresh.subtitle = "Rescan the SD card for EPUB files";
  refresh.action = ActionRefreshLibrary;
  refresh.value = summary;
  settingsRow(refresh, settingIconForIndex(0));

  int16_t selected = -1;  // built-in
  for (int i = 0; i < fontShelf.count; ++i) {
    if (strcmp(fontShelf.names[i], currentFontName) == 0) selected = static_cast<int16_t>(i);
  }
  const char* fontValue = selected < 0 ? "Built-in Font" : fontShelf.names[selected];
  ui::DropdownProps font;
  font.label = "Selected Reading Font";
  font.subtitle = fontValue;
  font.subtitleText = s.theme().smallText;
  font.icon = settingIconForIndex(1);
  font.iconSize = kSettingsIconSize;
  font.titleSubtitleGap = kSettingsTitleSubtitleGap;
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
  size.icon = settingIconForIndex(2);
  size.iconSize = kSettingsIconSize;
  size.titleSubtitleGap = kSettingsTitleSubtitleGap;
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
  uiFont.icon = settingIconForIndex(3);
  uiFont.iconSize = kSettingsIconSize;
  uiFont.titleSubtitleGap = kSettingsTitleSubtitleGap;
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
  auto pickerRow = [&](const char* lbl, const char* val, ui::ActionId action, ui::BitmapRef icon) {
    ui::DropdownProps d;
    d.label = lbl;
    d.subtitle = val;
    d.subtitleText = s.theme().smallText;
    d.icon = icon;
    d.iconSize = kSettingsIconSize;
    d.titleSubtitleGap = kSettingsTitleSubtitleGap;
    d.action = action;
    d.labelText = label;
    d.styles = s.theme().button;
    d.radius = 8;
    d.padding = {6, 8, 6, 8};
    if (shouldDrawSetting()) ui::dropdown(s.frame(), takeSettingRect(), d);
    ++settingsRowIndex;
  };
  pickerRow("Line Spacing", lineValue, ActionLineMenu, settingIconForIndex(4));
  pickerRow("Page Margin", marginValue, ActionMarginMenu, settingIconForIndex(5));
  pickerRow("Alignment", kAlignNames[paraAlign], ActionAlignMenu, settingIconForIndex(6));
  pickerRow("Orientation", kOrientNames[orientationSetting], ActionOrientMenu, settingIconForIndex(7));

  auto toggle = [&](const char* lbl, const char* subtitle, bool on, ui::ActionId action, ui::BitmapRef icon) {
    ui::ToggleRowProps t;
    t.row.label = lbl;
    t.row.subtitle = subtitle;
    t.row.labelText = label;
    t.row.subtitleText = s.theme().smallText;
    t.row.icon = icon;
    t.row.iconSize = kSettingsIconSize;
    t.row.titleSubtitleGap = kSettingsTitleSubtitleGap;
    t.checked = on;
    t.toggleAction = action;
    if (shouldDrawSetting()) ui::toggleRow(s.frame(), takeSettingRect(), t);
    ++settingsRowIndex;
  };
  toggle("Hyphenation", "Break long words", hyphenateSetting != 0,
         ActionToggleHyphen, settingIconForIndex(8));
  toggle("Sharp Text (no AA)", "Crisp one-bit text", sharpText != 0,
         ActionToggleSharp, settingIconForIndex(9));
  toggle("Extra Paragraph Spacing", "More paragraph space", extraParaSpacing != 0,
         ActionToggleParaSpace, settingIconForIndex(10));
  toggle("Embedded Book Styles", "Use publisher CSS", embeddedStyles != 0,
         ActionToggleEmbCss, settingIconForIndex(11));
  toggle("Focus Reading", "Bold word starts", focusReading != 0,
         ActionToggleFocus, settingIconForIndex(12));

  const uint16_t previewIndex = static_cast<uint16_t>(settingsTop + settingsVisibleRows);
  const char* previewLabel = nullptr;
  const char* previewSubtitle = nullptr;
  switch (previewIndex) {
    case 0: previewLabel = "Refresh Library"; break;
    case 1: previewLabel = "Selected Reading Font"; break;
    case 2: previewLabel = "Reading Size"; break;
    case 3: previewLabel = "UI Font"; break;
    case 4: previewLabel = "Line Spacing"; break;
    case 5: previewLabel = "Page Margin"; break;
    case 6: previewLabel = "Alignment"; break;
    case 7: previewLabel = "Orientation"; break;
    case 8:
      previewLabel = "Hyphenation";
      previewSubtitle = "Break long words";
      break;
    case 9:
      previewLabel = "Sharp Text (no AA)";
      previewSubtitle = "Crisp one-bit text";
      break;
    case 10:
      previewLabel = "Extra Paragraph Spacing";
      previewSubtitle = "More paragraph space";
      break;
    case 11:
      previewLabel = "Embedded Book Styles";
      previewSubtitle = "Use publisher CSS";
      break;
    case 12:
      previewLabel = "Focus Reading";
      previewSubtitle = "Bold word starts";
      break;
  }
  const ui::Rect remaining = s.body();
  if (previewLabel != nullptr && remaining.height >= s.target().lineHeight(label.font)) {
    ui::SettingRowProps preview;
    preview.label = previewLabel;
    preview.subtitle = previewSubtitle;
    preview.labelText = label;
    preview.subtitleText = s.theme().smallText;
    preview.icon = settingIconForIndex(previewIndex);
    preview.iconSize = kSettingsIconSize;
    preview.titleSubtitleGap = kSettingsTitleSubtitleGap;
    preview.styles = s.theme().button;
    preview.radius = 8;
    ui::settingRow(s.frame(), remaining.inset({0, kSettingsRightInset, 0, kSettingsLeftInset}), preview);
  }

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
    case Screen::Boot: app->setScreen(bootScreen, nullptr, ui::RefreshHint::None); break;
    case Screen::Library: app->setScreen(libraryScreen, nullptr, ui::RefreshHint::None); break;
    case Screen::Reader: app->setScreen(readerScreen, nullptr, ui::RefreshHint::None); break;
    case Screen::Toc: app->setScreen(tocScreen, nullptr, ui::RefreshHint::None); break;
    case Screen::Sleep: app->setScreen(sleepScreen, nullptr, ui::RefreshHint::None); break;
  }
  if (initialPaint) {
    app->invalidate(ui::RefreshHint::Full);
  } else {
    app->invalidateTransition();
  }
}

void paintLoadingScreen(const bool waitForRefresh = true) {
  loadingPhase = static_cast<uint8_t>(loadingPhase + 1);
  if (screen != Screen::Boot) {
    goToPage(Screen::Boot, /*initialPaint=*/true);
  } else {
    app->invalidate(ui::RefreshHint::Fast);
  }
  if (display.refreshBusy()) {
    if (!waitForRefresh) return;
    while (display.refreshBusy()) {
      delay(10);
    }
  }
  app->render();
  ui::presentAsync(display, app->lastRenderRefreshHint());
  if (waitForRefresh) {
    while (display.refreshBusy()) {
      delay(10);
    }
  }
}

bool presentIndexingToast() {
  if (screen != Screen::Library || target == nullptr || app == nullptr || display.refreshBusy()) return false;

  ui::InteractionBuffer<1> interactions;
  ui::InputSnapshot input;
  ui::Frame<1> frame(*target, app->device(), input, interactions, app->assets());
  const ui::ThemeTokens& theme = app->theme();
  ui::Rect bounds = frame.safeRect().inset(ui::Insets{0, 10, 0, 12});
  bounds.height = static_cast<int16_t>(bounds.height - kLibraryTabHeight);
  const int16_t gridTop = static_cast<int16_t>(theme.headerHeight + 4);
  bounds.y = static_cast<int16_t>(bounds.y + gridTop);
  bounds.height = static_cast<int16_t>(bounds.height > gridTop ? bounds.height - gridTop : 0);

  ui::ToastProps toast;
  toast.message = "Indexing...";
  toast.text = theme.bodyText;
  toast.styles = theme.popup;
  toast.anchor = ui::ToastAnchor::Top;
  toast.margin = 0;
  toast.padding = ui::Insets{10, 18, 10, 18};
  ui::toast(frame, bounds, toast);
  ui::presentAsync(display, ui::RefreshHint::Fast);
  return true;
}

void performPendingOpen() {
  const int16_t shelfIndex = pendingOpenShelfIndex;
  pendingOpenShelfIndex = -1;
  pendingOpenToastVisible = false;
  pendingOpenToastPresented = false;
  if (shelfIndex < 0 || shelfIndex >= shelf.count) {
    app->invalidate(ui::RefreshHint::Full);
    return;
  }
  promoteRecentBook(static_cast<uint16_t>(shelfIndex));
  if (session.begin(shelf.paths[shelfIndex], fonts, (hyphReady && hyphenateSetting) ? &hyphenator : nullptr, bookBuf,
                    512 * 1024, scratchBuf, 512 * 1024, indexBuf, 64 * 1024)) {
    readerChromeVisible = false;
    tocTop = 0;
    goToPage(Screen::Reader);
  } else {
    goToPage(Screen::Library, /*initialPaint=*/true);
  }
}

bool libraryNeedsPreloadWork() {
  for (uint8_t i = 0; i < recentVisibleCount; ++i) {
    const int16_t shelfIndex = recentShelfForSlot[i];
    if (shelfIndex < 0 || shelfIndex >= shelf.count) continue;
    if (!shelf.detailsReady[shelfIndex] || !shelf.coverTried[shelfIndex]) return true;
  }
  return false;
}

void scanAndPreloadLibrary() {
  loadRecentHashes();
  shelf.scan();
  rebuildRecentShelfSlots();
  const bool showLoading = libraryNeedsPreloadWork();
  if (showLoading) paintLoadingScreen(false);
  for (uint8_t i = 0; i < recentVisibleCount; ++i) {
    const int16_t shelfIndex = recentShelfForSlot[i];
    if (shelfIndex < 0 || shelfIndex >= shelf.count) continue;
    shelf.ensureDetails(static_cast<uint16_t>(shelfIndex));
    shelf.ensureCover(static_cast<uint16_t>(shelfIndex));
    if (showLoading) paintLoadingScreen(false);
  }
  rebuildRecentShelfSlots();
  allBooksBrowserDirty = true;
}

void showSleepScreenAndPowerOff() {
  session.end();
  goToPage(Screen::Sleep, /*initialPaint=*/true);
  app->render();
  ui::presentAsync(display, ui::RefreshHint::Full);
  while (display.refreshBusy()) {
    delay(10);
  }
  delay(150);
  PowerManager::deepSleepUntilPowerButton();
}

void scrollSettingsByRow(int8_t dir);

void scrollLibraryByPage(int8_t dir) {
  if (libraryTab == LibraryTab::Settings) {
    scrollSettingsByRow(dir);
    return;
  }
  if (libraryTab == LibraryTab::Recent) return;
  if (allBooksBrowserDirty) rebuildAllBooksBrowser();
  if (allBooksEntryCount <= allBooksVisibleRows || allBooksVisibleRows == 0) return;
  const uint16_t step = allBooksVisibleRows > 1 ? static_cast<uint16_t>(allBooksVisibleRows - 1) : 1;
  const uint16_t maxTop = static_cast<uint16_t>(allBooksEntryCount - allBooksVisibleRows);
  if (dir > 0) {
    allBooksTop = static_cast<uint16_t>(allBooksTop + step > maxTop ? maxTop : allBooksTop + step);
  } else {
    allBooksTop = allBooksTop > step ? static_cast<uint16_t>(allBooksTop - step) : 0;
  }
  allBooksSelected = static_cast<int16_t>(allBooksTop);
  app->invalidate(ui::RefreshHint::Fast);
}

void scrollTocByPage(int8_t dir) {
  const uint16_t tocCount = static_cast<uint16_t>(
      (session.bk.tocCount() > 0 ? session.bk.tocCount() : session.bk.spineCount()) < 128
          ? (session.bk.tocCount() > 0 ? session.bk.tocCount() : session.bk.spineCount())
          : 128);
  if (tocCount <= tocVisibleRows || tocVisibleRows == 0) return;
  const uint16_t maxTop = static_cast<uint16_t>(tocCount - tocVisibleRows);
  const uint16_t step = tocVisibleRows > 1 ? static_cast<uint16_t>(tocVisibleRows - 1) : 1;
  if (dir > 0) {
    tocTop = static_cast<uint16_t>(tocTop + step > maxTop ? maxTop : tocTop + step);
  } else {
    tocTop = tocTop > step ? static_cast<uint16_t>(tocTop - step) : 0;
  }
  app->invalidate(ui::RefreshHint::Fast);
}

void scrollSettingsByRow(int8_t dir) {
  if (settingsMenu != 0) return;
  static constexpr uint16_t kSettingsRows = 13;
  if (kSettingsRows <= settingsVisibleRows || settingsVisibleRows == 0) return;
  const uint16_t maxTop = static_cast<uint16_t>(kSettingsRows - settingsVisibleRows);
  if (dir > 0) {
    settingsTop = settingsTop < maxTop ? static_cast<uint16_t>(settingsTop + 1) : maxTop;
  } else {
    settingsTop = settingsTop > 0 ? static_cast<uint16_t>(settingsTop - 1) : 0;
  }
  app->invalidate(ui::RefreshHint::Fast);
}

// ---------------------------------------------------------------------------
// Action handlers

void onOpenBook(const ui::ActionEvent& e, void*) {
  int16_t shelfIndex = e.value;
  if (screen == Screen::Library && libraryTab == LibraryTab::AllBooks) {
    if (allBooksBrowserDirty) rebuildAllBooksBrowser();
    if (e.value < 0 || e.value >= static_cast<int16_t>(allBooksEntryCount)) return;
    allBooksSelected = e.value;
    const uint16_t row = static_cast<uint16_t>(e.value);
    if (allBooksEntryKind[row] == BrowserEntryKind::Up || allBooksEntryKind[row] == BrowserEntryKind::Folder) {
      char nextPath[160];
      if (allBooksEntryKind[row] == BrowserEntryKind::Up) {
        browserParentPath(nextPath, sizeof(nextPath));
      } else {
        browserJoinPath(allBooksPath, allBooksEntryNames[row], nextPath, sizeof(nextPath));
      }
      snprintf(allBooksPath, sizeof(allBooksPath), "%s", nextPath);
      allBooksTop = 0;
      allBooksSelected = 0;
      allBooksBrowserDirty = true;
      app->invalidate(ui::RefreshHint::Fast);
      return;
    }
    shelfIndex = allBooksEntryShelfIndex[row];
  }
  if (shelfIndex < 0 || shelfIndex >= shelf.count) return;
  librarySelected = shelfIndex;
  pendingOpenShelfIndex = shelfIndex;
  pendingOpenToastVisible = true;
  pendingOpenToastPresented = false;
  pendingOpenToastShownAt = 0;
  app->clearTapFlash();
  app->invalidate(ui::RefreshHint::Fast);
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

void onLibraryTab(const ui::ActionEvent& e, void*) {
  libraryTab = static_cast<LibraryTab>(e.value);
  settingsMenu = 0;
  if (libraryTab == LibraryTab::Settings) {
    fontShelf.scan();
  }
  app->invalidate(ui::RefreshHint::Fast);
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
  coverBookBuf = psAlloc(512 * 1024);  // omnibus ZIP catalogs need the same room as reading
  coverScratchBuf = psAlloc(192 * 1024);

  app->on(ActionOpenBook, onOpenBook);
  app->on(ActionPageNext, onPageTurn);
  app->on(ActionPagePrev, onPageTurn);
  app->on(ActionBackToReader, onBackToReader);
  app->on(ActionToc, onCenterTap);
  app->on(ActionTocJump, onTocJump);
  app->on(ActionFontSize, onFontSize);
  app->on(ActionLibraryTab, onLibraryTab);
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

  scanAndPreloadLibrary();
  goToPage(Screen::Library, /*initialPaint=*/true);
  app->render();
  ui::presentAsync(display, app->lastRenderRefreshHint());

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

  LOGF("[freeink-books] build " __DATE__ " " __TIME__ " engine: %s\n", book::vendorVersions());
  LOGF("[freeink-books] sd=%d books=%d font=%s\n", SdMan.ready() ? 1 : 0, shelf.count,
       currentFontName[0] ? currentFontName : "(built-in)");
}

void loop() {
  bool releasedWakePowerThisLoop = false;
  if (ignorePowerUntilRelease && !input.isPressed(InputManager::BTN_POWER)) {
    ignorePowerUntilRelease = false;
    releasedWakePowerThisLoop = true;
  }

  // Hardware buttons (queued by the input task).
  uint8_t btn;
  while (input.popPress(btn)) {
    if (btn == InputManager::BTN_POWER) {
      if (ignorePowerUntilRelease || releasedWakePowerThisLoop) {
        continue;
      }
      showSleepScreenAndPowerOff();
      continue;
    }
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
    if (btn == InputManager::BTN_DOWN || btn == InputManager::BTN_UP) {
      const int8_t dir = btn == InputManager::BTN_DOWN ? 1 : -1;
      if (screen == Screen::Library) {
        scrollLibraryByPage(dir);
        continue;
      }
      if (screen == Screen::Toc) {
        scrollTocByPage(dir);
        continue;
      }
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
      if (libraryTab == LibraryTab::Settings && settingsMenu == 0) {
        scrollSettingsByRow(dy < 0 ? 1 : -1);
      } else if (libraryTab == LibraryTab::AllBooks &&
                 allBooksEntryCount > allBooksVisibleRows && allBooksVisibleRows > 0) {
        const uint16_t step = allBooksVisibleRows > 1 ? static_cast<uint16_t>(allBooksVisibleRows - 1) : 1;
        const uint16_t maxTop = static_cast<uint16_t>(allBooksEntryCount - allBooksVisibleRows);
        if (dy < 0) {
          allBooksTop = static_cast<uint16_t>(allBooksTop + step > maxTop ? maxTop : allBooksTop + step);
        } else {
          allBooksTop = allBooksTop > step ? static_cast<uint16_t>(allBooksTop - step) : 0;
        }
        allBooksSelected = static_cast<int16_t>(allBooksTop);
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
  if (pendingOpenShelfIndex >= 0 && pendingOpenToastVisible && !pendingOpenToastPresented) {
    if (presentIndexingToast()) {
      pendingOpenToastPresented = true;
      pendingOpenToastShownAt = millis();
      pending = ui::RefreshHint::None;
      performPendingOpen();
    }
  }

  if (pendingOpenShelfIndex >= 0 && pendingOpenToastPresented && !display.refreshBusy() &&
      millis() - pendingOpenToastShownAt >= 80) {
    performPendingOpen();
  }

  if (app->invalidated() && pendingOpenShelfIndex < 0) {
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
    if (libraryRefreshRequested && !libraryRefreshPainted) {
      libraryRefreshPainted = true;
    }
    pending = ui::RefreshHint::None;
  }

  if (libraryRefreshRequested && libraryRefreshPainted && !display.refreshBusy()) {
    scanAndPreloadLibrary();
    fontShelf.scan();
    if (librarySelected >= shelf.count) {
      librarySelected = shelf.count > 0 ? static_cast<int16_t>(shelf.count - 1) : 0;
    }
    libraryTop = 0;
    allBooksTop = 0;
    allBooksSelected = 0;
    allBooksBrowserDirty = true;
    libraryRefreshRequested = false;
    libraryRefreshPainted = false;
    goToPage(Screen::Library, /*initialPaint=*/true);
  }

  // Deep sleep only on a genuine live hold: GPIO4 is the shared
  // CONFIRM/POWER button, and a stale held-time reading here would put the
  // device to sleep right after boot (dead USB CDC, frozen panel).
  if (!ignorePowerUntilRelease && input.isPressed(InputManager::BTN_POWER) &&
      input.getPowerButtonHeldTime() > 1500) {
    showSleepScreenAndPowerOff();
  }
  delay(10);
}
