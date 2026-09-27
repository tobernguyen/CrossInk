#pragma once

#include <Epub/Page.h>
#include <GfxRenderer.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bilingual/SentencePairing.h"
#include "bilingual/SentenceSplitter.h"

// Sentence translation tooltip for Calibre-style bilingual EPUBs, ported from CrossLingua's
// TooltipOverlay (Lingua "Tooltip" mode). The page is laid out with the translated paragraphs
// hidden; a long-press on a sentence shows its translation in a small box near it, taken from the
// chapter's own HTML, where each original paragraph is followed by its lang-tagged translation.
//
// Differences from CrossLingua:
//  - Touch only: selectAt() picks the step; no button stepping.
//  - CrossInk pages carry no chapter paragraph indices, so the chapter reparse keeps the entries
//    around the pairs whose sentences are on the page (PageWindow) instead of an index range.
//  - The Page is reloaded for every render, so nothing here keeps pointers into it between calls.
class BilingualTooltip {
 public:
  // `nextPath` resolves the next spine item's HTML, read only when a chapter file ends before a
  // paragraph's translation (split files).
  void setChapterHtmlPath(const std::string& path, std::string (*nextPath)() = nullptr) {
    nextPath_ = nextPath;
    if (path != htmlPath_) {
      htmlPath_ = path;
      resetPage();
    }
  }

  // Long-press at screen (x, y); the next render() resolves it against the page geometry and shows
  // that sentence, or hides the tooltip when the touch is off the text or the sentence has no
  // translation.
  void selectAt(int x, int y) {
    touchX_ = static_cast<int16_t>(x);
    touchY_ = static_cast<int16_t>(y);
    touchPending_ = true;
    currentStep_ = 0;
    tipPage_ = 0;
    if (loadFailure_) resetPage();  // the chapter didn't load for this page last time: try again
  }
  void dismiss() {
    currentStep_ = -1;
    touchPending_ = false;
    tipPageCount_ = 0;
  }
  // A tap at screen (x, y) while showing. On a translation too long for its box, a tap inside the box
  // shows the next part (after the last, the first again) and returns true; false means close it.
  bool scrollAt(const int x, const int y) {
    if (!isActive() || tipPageCount_ < 2 || x < boxX_ || x >= boxX_ + boxW_ || y < boxY_ || y >= boxY_ + boxH_) {
      return false;
    }
    tipPage_ = (tipPage_ + 1) % tipPageCount_;
    return true;
  }
  bool isActive() const { return currentStep_ >= 0; }
  // The page on screen changed (turn, relayout): drop the per-page sentence state.
  void resetPage() {
    pagePrepared_ = false;
    stepCount_ = 0;
    splits_.count = 0;
    sentenceTranslations_.clear();
    loadFailure_ = nullptr;
  }

  void render(GfxRenderer& renderer, const Page& page, int fontId, int tooltipFontId, int xOffset, int yOffset,
              int viewportWidth, int viewportHeight, bool foregroundBlack);

 private:
  static constexpr int MAX_WORDS = 500;

  void preparePage(const Page& page);
  int stepAtTouch(const Page& page, int lineHeight, int xOffset, int yOffset) const;

  struct SentenceBounds {
    int firstLineY;
    int startX;
    int endX;
  };
  SentenceBounds findSentenceBounds(const Page& page, const SentenceSpan& span, int xOffset, int yOffset) const;
  void drawSentenceUnderline(GfxRenderer& renderer, const Page& page, const SentenceSpan& span, int fontId, int xOffset,
                             int yOffset, bool foregroundBlack) const;

  std::string htmlPath_;
  std::string (*nextPath_)() = nullptr;
  int8_t currentStep_ = -1;
  bool pagePrepared_ = false;
  bool touchPending_ = false;
  int16_t touchX_ = 0;
  int16_t touchY_ = 0;
  int origWordCount_ = 0;
  SentenceSplitResult splits_;
  // One entry per page sentence; a merged group holds the same string in each member slot.
  std::vector<std::string> sentenceTranslations_;
  SentenceStep steps_[MAX_SENTENCES];
  int stepCount_ = 0;
  // The drawn box (screen px) and which part of its translation it shows.
  int boxX_ = 0, boxY_ = 0, boxW_ = 0, boxH_ = 0;
  int tipPage_ = 0;
  int tipPageCount_ = 0;
  // Why the page's chapter entries didn't load (null when they did), with the heap at that moment. A
  // hold that finds no translation then shows this as a notice instead of nothing.
  const char* loadFailure_ = nullptr;
  uint32_t failFreeHeap_ = 0, failMaxAlloc_ = 0;
  bool notice_ = false;
};
