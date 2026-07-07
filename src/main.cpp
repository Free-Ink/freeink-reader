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
#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <FreeInkApp.h>
#include <FreeInkBook.h>
#include <FreeInkUIBookFont.h>
#include <FreeInkUIDisplayTarget.h>
#include <InputManager.h>
#include <PowerManager.h>
#include <SDCardManager.h>
#include <cache/PageCache.h>
#include <css/Css.h>
#include <layout/ChapterLayout.h>
#include <render/PageRenderer.h>
#include <render/TtfFont.h>
#include <text/Hyphenator.h>

#include "BookStorageAdapters.h"

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
  ActionToc,
  ActionTocJump,
  ActionFontSize,
  ActionSettings,
  ActionPickFont,
};

enum class Screen : uint8_t { Library, Reader, Toc, Settings };

// ---------------------------------------------------------------------------
// Stores

struct Shelf {
  static constexpr int kMax = 64;
  char paths[kMax][160];
  char titles[kMax][64];
  ui::ListItem items[kMax];
  int count = 0;

  void scan() {
    count = 0;
    addFrom("/");       // books anywhere: card root works out of the box
    addFrom("/Books");  // ...and a Books folder for the tidy
  }

  void addFrom(const char* dir) {
    const bool root = dir[0] == '/' && dir[1] == 0;
    for (const String& name : SdMan.listFiles(dir, kMax)) {
      if (name.startsWith(".") || !name.endsWith(".epub") || count >= kMax) continue;
      snprintf(paths[count], sizeof(paths[count]), "%s%s%s", dir, root ? "" : "/", name.c_str());
      snprintf(titles[count], sizeof(titles[count]), "%.*s",
               static_cast<int>(name.length() - 5), name.c_str());
      items[count] = ui::ListItem{};
      items[count].label = titles[count];
      items[count].actionValue = static_cast<int16_t>(count);
      ++count;
    }
  }
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

  bool begin(const char* epubPath, book::FontChain& fonts, const book::Hyphenator* hyph,
             uint8_t* bookBuf, size_t bookCap, uint8_t* scratchBuf, size_t scratchCap,
             uint8_t* indexBuf, size_t indexCap) {
    bookArena.init(bookBuf, bookCap);
    scratch.init(scratchBuf, scratchCap);
    indexArena.init(indexBuf, indexCap);
    if (!source.open(epubPath)) return false;
    if (bk.open(source, bookArena, scratch) != BookStatus::Ok) return false;

    // Per-book cache directory from a path hash; progress lives beside it.
    const uint32_t id = book::ZipCatalog::hashPath(epubPath);
    snprintf(cacheDir, sizeof(cacheDir), "/BookCache/%08x", id);
    snprintf(progressPath, sizeof(progressPath), "%s/progress.bin", cacheDir);
    cache.setDir(cacheDir);
    loadProgress();

    // Book stylesheet (all text/css manifest items).
    book::CssStylesheetBuilder builder;
    static uint8_t sheetBuf[48 * 1024];
    static book::Arena sheetArena;
    sheetArena.init(sheetBuf, sizeof(sheetBuf));
    builder.begin(sheetArena);
    for (size_t m = 0; m < bk.manifestCount(); ++m) {
      const book::ManifestItem* item = bk.manifestItem(m);
      if (strcmp(item->mediaType, "text/css") != 0) continue;
      if (const book::ZipEntry* e = bk.zip().find(item->href)) {
        builder.addSheet(source, *e, scratch);
      }
    }
    sheet = builder.finish();

    params = book::LayoutParams{};
    params.pageWidth = 480;   // portrait logical page (panel held tall)
    params.pageHeight = 800;
    params.baseSizePx = pos.baseSizePx;
    params.font = &fonts;
    params.stylesheet = &sheet;
    params.hyphenator = hyph;
    params.defaultAlign = book::TextAlign::Justify;
    params.language = bk.metadata().language[0] ? bk.metadata().language : "en";

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

    const book::ManifestItem* item = bk.spineItem(spineIndex);
    const book::ZipEntry* entry = item ? bk.zip().find(item->href) : nullptr;
    if (entry == nullptr) return BookStatus::NotFound;

    const size_t marked = scratch.mark();
    book::PageCacheWriter writer;
    if (!writer.begin(cache, cacheName, hash, scratch)) {
      scratch.release(marked);
      return BookStatus::IoError;
    }
    st = book::ChapterLayout::layout(source, bk.zip(), *entry, item->href, params, scratch,
                                     writer, nullptr);
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
    book::PageRenderer::renderText(page, fonts, target);
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
      if (pos.spineIndex + 1u < bk.spineCount() &&
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

  // Font-size change: new generation; the anchor carries the position over.
  bool setBaseSize(uint16_t sizePx) {
    const uint32_t anchor = pos.charStart;
    pos.baseSizePx = params.baseSizePx = sizePx;
    if (ensureChapter(pos.spineIndex) != BookStatus::Ok) return false;
    pageInChapter = reader.pageForChar(anchor);
    return true;
  }

  void loadProgress() {
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
  return book::layoutGenerationHash(params, fontReady ? 2u : 1u);
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
ui::BitmapBookFont builtinFont;  // bundled Noto Sans — always-available fallback
book::FontChain fonts;
book::Hyphenator hyphenator;
bool hyphReady = false;
bool fontReady = false;

Screen screen = Screen::Library;
int16_t librarySelected = 0;
uint16_t libraryTop = 0;
bool readerChromeVisible = false;
char statusText[96];

uint8_t* bookBuf = nullptr;      // 256 KB PSRAM
uint8_t* scratchBuf = nullptr;   // 512 KB PSRAM
uint8_t* indexBuf = nullptr;     // 64 KB PSRAM
uint8_t* glyphBuf = nullptr;     // 128 KB PSRAM
uint8_t* fontFile = nullptr;     // TTF bytes, PSRAM
uint8_t* hyphData = nullptr;

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

void saveFontSetting() {
  SdMan.ensureDirectoryExists("/BookCache");
  FsFile f = SdMan.open("/BookCache/settings.bin", O_WRONLY | O_CREAT | O_TRUNC);
  if (f) {
    f.write(currentFontName, sizeof(currentFontName));
    f.close();
  }
}
void loadFontSetting() {
  FsFile f = SdMan.open("/BookCache/settings.bin", O_RDONLY);
  if (f) {
    f.read(currentFontName, sizeof(currentFontName));
    f.close();
    currentFontName[sizeof(currentFontName) - 1] = 0;
  }
}

// (Re)builds the font chain for the selected font; built-in stays the tail.
void applyFont() {
  fonts = book::FontChain{};
  fontReady = false;
  if (currentFontName[0] != 0) {
    char path[96];
    snprintf(path, sizeof(path), "/fonts/%s", currentFontName);
    FsFile f = SdMan.open(path, O_RDONLY);
    if (f) {
      const uint32_t len = f.fileSize();
      if (fontFile == nullptr) fontFile = psAlloc(2 * 1024 * 1024);
      if (fontFile != nullptr && len <= 2 * 1024 * 1024 &&
          f.read(fontFile, len) == static_cast<int>(len)) {
        static freeink::book::Arena glyphArena;
        glyphArena.init(glyphBuf, 128 * 1024);
        fontReady = ttf.init(fontFile, len, glyphArena) && fonts.add(&ttf);
      }
      f.close();
    }
  }
  fonts.add(&builtinFont);
}

// ---------------------------------------------------------------------------
// Screens (FreeInkUI)

void libraryScreen(App::ScreenType& s, void*) {
  const uint16_t pct = battery.readPercentage();
  snprintf(statusText, sizeof(statusText), "%u%%", pct);
  // Slim status band (battery), then a roomy left-aligned title above the list.
  ui::StatusBarProps status;
  status.trailing = statusText;
  s.status(status);
  s.spacer(18);
  ui::HeaderProps h1;
  h1.title = "Library";
  h1.borderEdges = 0;  // borderless: it is a headline, not chrome
  s.header(h1);
  s.spacer(14);
  const ui::FooterAction footer[] = {{.label = "Settings", .action = ActionSettings}};
  s.footer(footer, 1);
  if (shelf.count == 0) {
    s.centeredText("No books found.\nCopy .epub files to /Books on the SD card.");
    return;
  }
  s.list(shelf.items, static_cast<uint16_t>(shelf.count), librarySelected, ActionOpenBook,
         libraryTop);
}

void readerScreen(App::ScreenType& s, void*) {
  // Page content is composited after app render; the screen contributes the
  // full-page tap zones (and chrome when toggled).
  if (readerChromeVisible) {
    snprintf(statusText, sizeof(statusText), "%s  ·  ch %u  p %u/%u",
             session.bk.metadata().title, session.pos.spineIndex + 1,
             session.pageInChapter + 1, session.reader.pageCount());
    s.header(statusText, nullptr, nullptr);
    const ui::FooterAction footer[] = {
        {.label = "Library", .action = ActionBackToLibrary},
        {.label = "Contents", .action = ActionToc},
        {.label = "Aa", .action = ActionFontSize},
    };
    s.footer(footer, 3);
  }
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
  const uint16_t n = static_cast<uint16_t>(
      session.bk.tocCount() < 128 ? session.bk.tocCount() : 128);
  for (uint16_t i = 0; i < n; ++i) {
    items[i] = ui::ListItem{};
    items[i].label = session.bk.tocEntry(i)->title;
    items[i].actionValue = static_cast<int16_t>(i);
  }
  s.navHeader("Contents", ActionBackToLibrary, ui::BitmapRef{});
  s.list(items, n, -1, ActionTocJump);
}

void settingsScreen(App::ScreenType& s, void*) {
  s.navHeader("Settings", ActionBackToLibrary, ui::BitmapRef{});
  int16_t selected = 0;  // built-in
  for (int i = 0; i < fontShelf.count; ++i) {
    if (strcmp(fontShelf.names[i], currentFontName) == 0) selected = static_cast<int16_t>(i + 1);
  }
  s.list(fontShelf.items, static_cast<uint16_t>(fontShelf.count + 1), selected, ActionPickFont);
}

void goToPage(Screen next, bool initialPaint = false) {
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
  if (session.begin(shelf.paths[e.value], fonts, hyphReady ? &hyphenator : nullptr, bookBuf,
                    256 * 1024, scratchBuf, 512 * 1024, indexBuf, 64 * 1024)) {
    readerChromeVisible = false;
    goToPage(Screen::Reader);
  }
}

void onPageTurn(const ui::ActionEvent& e, void*) {
  if (readerChromeVisible) {
    readerChromeVisible = false;
    app->invalidate(ui::RefreshHint::Full);
    return;
  }
  session.turn(e.action == ActionPageNext ? 1 : -1);
  app->invalidate(ui::RefreshHint::Fast);
}

void onCenterTap(const ui::ActionEvent&, void*) {
  if (screen == Screen::Reader) {
    readerChromeVisible = !readerChromeVisible;
    app->invalidate(ui::RefreshHint::Full);
  } else {
    goToPage(Screen::Toc);
  }
}

void onTocJump(const ui::ActionEvent& e, void*) {
  if (session.jumpToToc(static_cast<size_t>(e.value))) {
    readerChromeVisible = false;
    goToPage(Screen::Reader);
  }
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
  goToPage(Screen::Settings);
}

void onPickFont(const ui::ActionEvent& e, void*) {
  if (e.value < 0) currentFontName[0] = 0;
  else snprintf(currentFontName, sizeof(currentFontName), "%s", fontShelf.names[e.value]);
  saveFontSetting();
  applyFont();
  goToPage(Screen::Library);
}

void onBackToLibrary(const ui::ActionEvent&, void*) {
  if (screen == Screen::Settings) {
    goToPage(Screen::Library);
    return;
  }
  if (screen == Screen::Toc) {
    goToPage(Screen::Reader);
    return;
  }
  session.end();
  shelf.scan();
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
  LOGF("[freeink-books] boot\n");

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

  bookBuf = psAlloc(256 * 1024);
  scratchBuf = psAlloc(512 * 1024);
  indexBuf = psAlloc(64 * 1024);
  glyphBuf = psAlloc(128 * 1024);

  // Reading font: the persisted Settings choice, defaulting to the first TTF
  // found (or built-in when none). applyFont() always chains the built-in.
  loadFontSetting();
  fontShelf.scan();
  if (currentFontName[0] == 0 && fontShelf.count > 0) {
    snprintf(currentFontName, sizeof(currentFontName), "%s", fontShelf.names[0]);
  }
  applyFont();
  uint32_t len = 0;
  if (loadSdBlob("/fonts", ".fibh", &hyphData, &len)) {
    hyphReady = hyphenator.init(hyphData, len);
  }

  shelf.scan();
  LOGF("[freeink-books] sd=%d books=%d font=%s\n", SdMan.ready() ? 1 : 0, shelf.count,
       currentFontName[0] ? currentFontName : "(built-in)");
  app->on(ActionOpenBook, onOpenBook);
  app->on(ActionPageNext, onPageTurn);
  app->on(ActionPagePrev, onPageTurn);
  app->on(ActionToc, onCenterTap);
  app->on(ActionTocJump, onTocJump);
  app->on(ActionFontSize, onFontSize);
  app->on(ActionSettings, onSettings);
  app->on(ActionPickFont, onPickFont);
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
        readerChromeVisible = true;
        app->invalidate(ui::RefreshHint::Full);
      }
      continue;
    }
    ui::InputSnapshot b;
    b.focusNext = btn == InputManager::BTN_DOWN;
    b.focusPrev = btn == InputManager::BTN_UP;
    b.confirm = btn == InputManager::BTN_CONFIRM;
    app->route(b);
  }

  // Swipe up from the bottom edge returns to the Library (reader only).
  float sx0, sy0, sx1, sy1;
  if (screen == Screen::Reader && input.wasSwipe(sx0, sy0, sx1, sy1)) {
    const ui::Point a = ui::touchToLogical(app->device(), sx0, sy0);
    const ui::Point b = ui::touchToLogical(app->device(), sx1, sy1);
    const int16_t h = app->device().height;
    if (a.y > (h * 3) / 4 && (a.y - b.y) > h / 4) {
      session.end();
      shelf.scan();
      goToPage(Screen::Library);
    }
  }

  // Touch taps (queued): route against the last rendered frame.
  float nx, ny;
  while (input.popTouchTap(nx, ny)) {
    ui::InputSnapshot tap;
    const ui::Point p = ui::touchToLogical(app->device(), nx, ny);
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
                                       freeink::book::FrameFormat::Mono1Dithered,
                                       freeink::book::FrameRotation::Portrait};
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
    pending = ui::RefreshHint::None;
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
