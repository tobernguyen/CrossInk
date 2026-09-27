#include "bilingual/Bilingual.h"

#include <Epub.h>
#include <Epub/parsers/ElementSkipHook.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <iterator>
#include <memory>
#include <string>

#include "bilingual/BilingualLang.h"
#include "activities/reader/EpubGrayscale.h"
#include "bilingual/BilingualTooltip.h"
#include "fontIds.h"

namespace bilingual {
namespace {

// A book counts as translated after this many translated block elements: enough that a stray
// foreign-language paragraph in a monolingual book doesn't switch it into tooltip mode.
constexpr int kDetectThreshold = 3;
// First-open scan budget: the first spine items, stopping early on detection. Calibre translations
// start with the first text chapter, so this is normally one or two items.
constexpr int kScanMaxSpineItems = 12;
constexpr size_t kScanMaxBytes = 1536 * 1024;

constexpr uint8_t kStateMagic0 = 'B';
constexpr uint8_t kStateMagic1 = 'L';
constexpr uint8_t kStateVersion = 1;
enum Override : uint8_t { kDefault = 0, kOn = 1, kOff = 2 };

// ponytail: one global book state -- the reader shows one book at a time, and the layout parser
// reads it without extra plumbing through CrossInk's render spec. Per-book objects if that changes.
struct BookState {
  const Epub* epub = nullptr;
  std::string cachePath;
  std::string bookPrimary;
  bool detected = false;
  uint8_t override = kDefault;
  bool hasTouch = false;
  bool enabled = false;
};
BookState g;

std::unique_ptr<BilingualTooltip> gTip;
int gTipSpine = -1;
int gTipPage = -1;
uint32_t gTipFingerprint = 0;
// Where the page's BW pass last drew the tooltip; the grayscale planes redraw it the same way.
struct TipPlacement {
  int bodyFontId, xOffset, yOffset, viewportWidth, viewportHeight;
};
TipPlacement gTipPlacement{};

// Global preferences. One hold time for the tooltip and the dictionary lookups; the default 0.5 s
// is the touch SDK's long-press (what CrossLingua's tooltip used). Stock CrossInk holds for 1 s.
constexpr unsigned long kHoldMs[] = {250, 500, 750, 1000};
constexpr const char* kHoldLabels[] = {"0.25 s", "0.5 s", "0.75 s", "1 s"};
enum TooltipFont : uint8_t { kFontInterface = 0, kFontInterfaceSmall = 1, kFontBook = 2, kFontCount };
constexpr char kPrefsPath[] = "/.crosspoint/bilingual-prefs.bin";
constexpr uint8_t kPrefsVersion = 2;  // v1 indexed a hold list without 0.25 s
struct Prefs {
  bool loaded = false;
  uint8_t hold = 1;
  uint8_t font = kFontInterface;
};
Prefs gPrefs;

Prefs& prefs() {
  if (gPrefs.loaded) return gPrefs;
  gPrefs.loaded = true;
  HalFile f;
  uint8_t b[5] = {};
  if (Storage.openFileForRead("BIL", kPrefsPath, f) && f.read(b, sizeof(b)) == static_cast<int>(sizeof(b)) &&
      b[0] == 'B' && b[1] == 'P' && (b[2] == 1 || b[2] == kPrefsVersion) && b[4] < kFontCount) {
    const uint8_t hold = b[2] == 1 ? static_cast<uint8_t>(b[3] + 1) : b[3];
    if (hold < std::size(kHoldMs)) gPrefs.hold = hold;
    gPrefs.font = b[4];
  }
  return gPrefs;
}

void savePrefs() {
  HalFile f;
  if (!Storage.openFileForWrite("BIL", kPrefsPath, f)) {
    LOG_ERR("BIL", "Cannot write %s", kPrefsPath);
    return;
  }
  const uint8_t b[5] = {'B', 'P', kPrefsVersion, gPrefs.hold, gPrefs.font};
  f.write(b, sizeof(b));
}

int tooltipFontId(const int bodyFontId) {
  switch (prefs().font) {
    case kFontInterfaceSmall:
      return UI_10_FONT_ID;
    case kFontBook:
      return bodyFontId;
    default:
      return UI_12_FONT_ID;
  }
}

bool effectiveEnabled() {
  if (g.override == kOn) return true;
  if (g.override == kOff) return false;
  return g.detected && g.hasTouch;
}

std::string statePath() { return g.cachePath + "/bilingual.bin"; }

bool loadState() {
  HalFile f;
  if (!Storage.openFileForRead("BIL", statePath(), f)) return false;
  uint8_t b[5] = {};
  const bool ok = f.read(b, sizeof(b)) == static_cast<int>(sizeof(b)) && b[0] == kStateMagic0 && b[1] == kStateMagic1 &&
                  b[2] == kStateVersion && b[4] <= kOff;
  if (ok) {
    g.detected = b[3] != 0;
    g.override = b[4];
  }
  return ok;
}

void saveState() {
  HalFile f;
  if (!Storage.openFileForWrite("BIL", statePath(), f)) {
    LOG_ERR("BIL", "Cannot write %s", statePath().c_str());
    return;
  }
  const uint8_t b[5] = {kStateMagic0, kStateMagic1, kStateVersion, static_cast<uint8_t>(g.detected ? 1 : 0),
                        g.override};
  f.write(b, sizeof(b));
}

// Zip-stream sink: feeds the scanner and cuts the stream short once the book is detected or the
// byte budget is spent (a short write stops readItemContentsToStream with allowEarlyStop).
class ScanSink final : public Print {
 public:
  ScanSink(TranslationScanner& scanner, size_t& budget) : scanner_(scanner), budget_(budget) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t size) override {
    scanner_.feed(reinterpret_cast<const char*>(buf), size);
    budget_ -= std::min(budget_, size);
    return (scanner_.count() >= kDetectThreshold || budget_ == 0) ? 0 : size;
  }

 private:
  TranslationScanner& scanner_;
  size_t& budget_;
};

bool scanForTranslations(const Epub& epub) {
  if (g.bookPrimary.empty()) return false;
  TranslationScanner scanner(g.bookPrimary);
  size_t budget = kScanMaxBytes;
  ScanSink sink(scanner, budget);
  const int items = std::min(epub.getSpineItemsCount(), kScanMaxSpineItems);
  for (int i = 0; i < items && scanner.count() < kDetectThreshold && budget > 0; i++) {
    epub.readItemContentsToStream(epub.getSpineItem(i).href, sink, 1024, /*allowEarlyStop=*/true);
  }
  LOG_INF("BIL", "Scan: %d translated blocks (lang != %s)", scanner.count(), g.bookPrimary.c_str());
  return scanner.count() >= kDetectThreshold;
}

// The chapter HTML the tooltip re-parses: the section build's unzipped copy, or a one-off
// extraction when the build didn't keep one.
std::string chapterHtmlPath(const int spineIndex) {
  if (!g.epub || spineIndex < 0 || spineIndex >= g.epub->getSpineItemsCount()) return {};
  const std::string cached = g.cachePath + "/html/" + std::to_string(spineIndex) + ".html";
  if (Storage.exists(cached.c_str())) return cached;
  const std::string own = g.cachePath + "/bilingual/" + std::to_string(spineIndex) + ".html";
  if (Storage.exists(own.c_str())) return own;
  Storage.mkdir((g.cachePath + "/bilingual").c_str());
  return g.epub->extractItemToFile(g.epub->getSpineItem(spineIndex).href, own) ? own : std::string{};
}

void clearLayoutCache() { Storage.removeDir((g.cachePath + "/sections").c_str()); }

// Word count + first words: tells a relaid-out page (font, margins) from the one a tooltip was
// resolved on, since the Page object itself is reloaded for every render.
uint32_t pageFingerprint(const Page& page) {
  uint32_t h = 2166136261u;
  int words = 0;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& block = static_cast<const PageLine*>(el.get())->getBlock();
    for (uint16_t i = 0; i < block->wordCount(); i++, words++) {
      if (words < 8) {
        for (const char* p = block->wordText(i); *p; p++) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
      }
    }
  }
  return (h ^ static_cast<uint32_t>(words)) * 16777619u;
}

}  // namespace

void onBookOpened(const Epub& epub, const bool hasTouch) {
  g = BookState{};
  g.epub = &epub;
  g.cachePath = epub.getCachePath();
  g.bookPrimary = primaryLang(epub.getLanguage().c_str());
  g.hasTouch = hasTouch;
  if (!loadState()) {
    g.detected = scanForTranslations(epub);
    saveState();
    // Layouts cached before this book was scanned (by stock CrossInk, or before the fork) still
    // show the translations inline.
    if (effectiveEnabled()) clearLayoutCache();
  }
  g.enabled = effectiveEnabled();
  epub_hooks::skipElement = &hidesElement;
  // Text grayscale runs only with black text (foregroundBlack).
  EpubGrayscale::grayscaleOverlay = [](GfxRenderer& renderer, const Page& page) {
    const TipPlacement& p = gTipPlacement;
    drawTooltip(renderer, page, gTipSpine, gTipPage, p.bodyFontId, p.xOffset, p.yOffset, p.viewportWidth,
                p.viewportHeight, true);
  };
  LOG_INF("BIL", "Book: lang=%s translated=%d tooltip=%d", g.bookPrimary.c_str(), g.detected, g.enabled);
}

void onBookClosed() {
  epub_hooks::skipElement = nullptr;
  EpubGrayscale::grayscaleOverlay = nullptr;
  g = BookState{};
  gTip.reset();
  gTipSpine = gTipPage = -1;
  gTipFingerprint = 0;
}

bool bookHasTranslations() { return g.detected || g.override != kDefault; }

bool tooltipEnabled() { return g.enabled; }

void setTooltipEnabled(const bool enabled) {
  if (g.cachePath.empty()) return;
  g.override = enabled ? kOn : kOff;
  saveState();
  g.enabled = effectiveEnabled();
  dismissTooltip();
  clearLayoutCache();
}

unsigned long holdMs() { return kHoldMs[prefs().hold]; }

const char* holdLabel() { return kHoldLabels[prefs().hold]; }

void cycleHold() {
  prefs().hold = static_cast<uint8_t>((prefs().hold + 1) % std::size(kHoldMs));
  savePrefs();
}

const char* tooltipFontLabel() {
  switch (prefs().font) {
    case kFontInterfaceSmall:
      return tr(STR_TOOLTIP_FONT_INTERFACE_SMALL);
    case kFontBook:
      return tr(STR_TOOLTIP_FONT_BOOK);
    default:
      return tr(STR_TOOLTIP_FONT_INTERFACE);
  }
}

void cycleTooltipFont() {
  prefs().font = static_cast<uint8_t>((prefs().font + 1) % kFontCount);
  savePrefs();
}

bool hidesElement(const char* const* atts) {
  return g.enabled && isTranslationLang(langAttribute(atts), g.bookPrimary);
}

void selectAt(const int spineIndex, const int pageIndex, const int x, const int y) {
  if (!g.enabled) return;
  if (!gTip) {
    gTip = makeUniqueNoThrow<BilingualTooltip>();
    if (!gTip) {
      LOG_ERR("BIL", "OOM: tooltip");
      return;
    }
  }
  if (spineIndex != gTipSpine) {
    gTip->setChapterHtmlPath(chapterHtmlPath(spineIndex), [] { return chapterHtmlPath(gTipSpine + 1); });
  }
  if (spineIndex != gTipSpine || pageIndex != gTipPage) {
    gTip->resetPage();
    gTipFingerprint = 0;
  }
  gTipSpine = spineIndex;
  gTipPage = pageIndex;
  gTip->selectAt(x, y);
}

bool tooltipActive() { return gTip && gTip->isActive(); }

void dismissTooltip() {
  if (gTip) gTip->dismiss();
}

bool scrollTooltipAt(const int x, const int y) { return gTip && gTip->scrollAt(x, y); }

void drawTooltip(GfxRenderer& renderer, const Page& page, const int spineIndex, const int pageIndex,
                 const int bodyFontId, const int xOffset, const int yOffset, const int viewportWidth,
                 const int viewportHeight, const bool foregroundBlack) {
  if (!gTip || !gTip->isActive()) return;
  const uint32_t fingerprint = pageFingerprint(page);
  if (spineIndex != gTipSpine || pageIndex != gTipPage || (gTipFingerprint != 0 && fingerprint != gTipFingerprint)) {
    gTip->dismiss();  // the page under the tooltip turned or was relaid out
    gTip->resetPage();
    gTipFingerprint = 0;  // or every later hold on this page would be dismissed here too
    return;
  }
  gTipFingerprint = fingerprint;
  gTipPlacement = {bodyFontId, xOffset, yOffset, viewportWidth, viewportHeight};
  gTip->render(renderer, page, bodyFontId, tooltipFontId(bodyFontId), xOffset, yOffset, viewportWidth, viewportHeight,
               foregroundBlack);
}

}  // namespace bilingual
