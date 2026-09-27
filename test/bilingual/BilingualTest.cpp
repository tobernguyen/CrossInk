#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "EpubReaderMenuModel.h"
#include "bilingual/BilingualLang.h"
#include "bilingual/PageWindow.h"
#include "bilingual/TooltipHitTest.h"

// ── Reader menu ─────────────────────────────────────────────────────────────────────────────

// Upstream grows the drawer tabs over time and the fork adds rows to the Settings tab, which has a
// fixed capacity (ReaderDrawerTabCatalog::items). Building the full catalog in a constant expression
// turns an overflow after a rebase into a compile error instead of silent memory corruption.
constexpr ReaderDrawerAvailability everyOptionalRow() {
  ReaderDrawerAvailability a{};
  a.hasFootnotes = a.hasDictionary = a.hasBookmarks = a.hasClippings = true;
  a.showReadingPaceReset = a.hasStablePageNumbers = a.hasTranslations = true;
  return a;
}
constexpr ReaderDrawerCatalog kFullCatalog = makeReaderDrawerCatalog(everyOptionalRow());

TEST(BilingualMenu, ForkRowsFitTheSettingsTab) {
  const auto& settings = kFullCatalog[static_cast<size_t>(ReaderDrawerTab::Settings)];
  const auto has = [&](const ReaderDrawerCatalogItem item) {
    return std::find(settings.items.begin(), settings.items.begin() + settings.count, item) !=
           settings.items.begin() + settings.count;
  };
  EXPECT_TRUE(has(ReaderDrawerCatalogItem::HoldTime));
  EXPECT_TRUE(has(ReaderDrawerCatalogItem::BilingualTooltip));
  EXPECT_TRUE(has(ReaderDrawerCatalogItem::BilingualTooltipFont));
  for (const auto& tab : kFullCatalog) EXPECT_LE(tab.count, tab.items.size());
}

using bilingual::PageWindow;
using bilingual::TranslationScanner;

// ── Language rule ───────────────────────────────────────────────────────────────────────────

TEST(BilingualLang, PrimarySubtagIsLowercasedPrefix) {
  EXPECT_EQ(bilingual::primaryLang("vi-VN"), "vi");
  EXPECT_EQ(bilingual::primaryLang("EN"), "en");
  EXPECT_EQ(bilingual::primaryLang("zh_Hans"), "zh");
  EXPECT_EQ(bilingual::primaryLang(nullptr), "");
}

TEST(BilingualLang, TranslationIsADifferentPrimaryLanguage) {
  EXPECT_TRUE(bilingual::isTranslationLang("vi", "en"));
  EXPECT_FALSE(bilingual::isTranslationLang("en-GB", "en"));
  EXPECT_FALSE(bilingual::isTranslationLang("", "en"));
  EXPECT_FALSE(bilingual::isTranslationLang("vi", ""));  // unknown book language: never hide anything
}

TEST(BilingualLang, LangAttributeReadsLangAndXmlLang) {
  const char* plain[] = {"class", "calibre_1", "lang", "vi", nullptr};
  const char* xml[] = {"xml:lang", "fr", nullptr};
  const char* none[] = {"class", "x", nullptr};
  EXPECT_STREQ(bilingual::langAttribute(plain), "vi");
  EXPECT_STREQ(bilingual::langAttribute(xml), "fr");
  EXPECT_EQ(bilingual::langAttribute(none), nullptr);
  EXPECT_EQ(bilingual::langAttribute(nullptr), nullptr);
}

// ── First-open scan ─────────────────────────────────────────────────────────────────────────

TEST(TranslationScanner, CountsTranslatedBlocksFromTheCalibrePlugin) {
  TranslationScanner scanner("en");
  const std::string html =
      "<p class=\"calibre_1\">Where is he now?</p>"
      "<p class=\"calibre_1\" dir=\"auto\" lang=\"vi\">Giờ anh ta ở đâu?</p>"
      "<h2 lang='vi'>Chương 1</h2>"
      "<li xml:lang=\"vi-VN\">mục</li>";
  scanner.feed(html.data(), html.size());
  EXPECT_EQ(scanner.count(), 3);
}

TEST(TranslationScanner, IgnoresInlineForeignPhrasesAndSameLanguage) {
  TranslationScanner scanner("en");
  const std::string html =
      "<html lang=\"en\"><body lang=\"en\">"
      "<p>He said <span lang=\"fr\">je ne sais quoi</span>.</p>"
      "<p lang=\"en-GB\">Colour.</p>"
      "<!-- <p lang=\"vi\">commented out</p> -->"
      "</body></html>";
  scanner.feed(html.data(), html.size());
  EXPECT_EQ(scanner.count(), 0);
}

TEST(TranslationScanner, HandlesTagsSplitAcrossStreamChunks) {
  TranslationScanner scanner("en");
  const std::string html = "<p class=\"a\" lang=\"vi\">x</p><p lang=\"vi\">y</p>";
  for (const char c : html) scanner.feed(&c, 1);
  EXPECT_EQ(scanner.count(), 2);
}

// ── Page window ─────────────────────────────────────────────────────────────────────────────

// Each pair contributes one entry named after it; matchedKeys marks pairs whose sentences are on the page.
static std::vector<std::string> run(PageWindow<std::string>& w, const std::vector<int>& matches, int* stoppedAt) {
  *stoppedAt = -1;
  for (size_t i = 0; i < matches.size(); i++) {
    if (!w.addPair({"p" + std::to_string(i)}, matches[i])) {
      *stoppedAt = static_cast<int>(i);
      break;
    }
  }
  return w.take();
}

TEST(PageWindow, KeepsThePagesPairsPlusThePairBefore) {
  PageWindow<std::string> w(/*pageAnchors=*/3);
  int stopped = 0;
  const auto kept = run(w, {0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0}, &stopped);
  EXPECT_EQ(kept, (std::vector<std::string>{"p2", "p3", "p4", "p5", "p6", "p7", "p8"}));
  EXPECT_EQ(stopped, 8);  // stops after the trailing misses: the rest of the chapter isn't parsed
}

TEST(PageWindow, RestartsWhenAnEarlyChanceMatchIsFarFromThePage) {
  PageWindow<std::string> w(/*pageAnchors=*/4);
  std::vector<int> matches(40, 0);
  matches[2] = 1;  // "Yes." early in the chapter
  matches[30] = matches[31] = matches[32] = 1;
  int stopped = 0;
  const auto kept = run(w, matches, &stopped);
  ASSERT_FALSE(kept.empty());
  EXPECT_EQ(kept.front(), "p29");  // the margin pair before the real page, not the early match
  EXPECT_EQ(stopped, 35);
}

TEST(PageWindow, NoMatchKeepsNothing) {
  PageWindow<std::string> w(/*pageAnchors=*/2);
  int stopped = 0;
  EXPECT_TRUE(run(w, {0, 0, 0}, &stopped).empty());
  EXPECT_EQ(stopped, -1);
}

TEST(PageWindow, CapsMemoryKeepingTheNewestEntries) {
  PageWindow<std::string> w(/*pageAnchors=*/100, /*maxEntries=*/4);
  int stopped = 0;
  const auto kept = run(w, std::vector<int>(10, 1), &stopped);
  EXPECT_EQ(kept, (std::vector<std::string>{"p6", "p7", "p8", "p9"}));
}

// ── Touch hit-test (same rule as the CrossLingua port) ──────────────────────────────────────

using tooltip_hit::WordPos;
static const WordPos kPage[] = {{0, 100}, {40, 100}, {80, 100}, {0, 130}, {30, 130}};

TEST(TooltipHit, WordUnderTheFinger) {
  EXPECT_EQ(tooltip_hit::wordAt(kPage, 5, 45, 110, 20), 1);
  EXPECT_EQ(tooltip_hit::wordAt(kPage, 5, 79, 105, 20), 1);   // gap: the word to the left
  EXPECT_EQ(tooltip_hit::wordAt(kPage, 5, 5, 122, 20), 0);    // paragraph gap snaps to the nearest line
  EXPECT_EQ(tooltip_hit::wordAt(kPage, 5, 10, 300, 20), -1);  // far margin: no hit
}

TEST(TooltipHit, WordToStepThroughMergedSentences) {
  SentenceSplitResult splits;
  splits.spans[0] = {0, 3};
  splits.spans[1] = {3, 5};
  splits.spans[2] = {5, 8};
  splits.count = 3;
  const SentenceStep steps[] = {{1, 2}};  // sentence 0 untranslated; 1..2 share one translation
  EXPECT_EQ(tooltip_hit::stepForWord(splits, steps, 1, 1), -1);
  EXPECT_EQ(tooltip_hit::stepForWord(splits, steps, 1, 6), 0);
}

// Viewport [10, 790), sentence [300, 400), gap 4: 286 px of room above, 386 below.
TEST(TooltipHit, TipAboveWhenItFitsElseBelow) {
  EXPECT_EQ(tooltip_hit::tipTop(100, 300, 400, 10, 790, 4), 196);  // fits above
  EXPECT_EQ(tooltip_hit::tipTop(350, 300, 400, 10, 790, 4), 404);  // only fits below
}

TEST(TooltipHit, TallTipOverlapsTheSentenceFromTheRoomierSide) {
  EXPECT_EQ(tooltip_hit::tipTop(500, 300, 400, 10, 790, 4), 290);  // below is roomier: slides up over it
  EXPECT_EQ(tooltip_hit::tipTop(500, 500, 600, 10, 790, 4), 10);   // above is roomier: slides down over it
  EXPECT_EQ(tooltip_hit::tipTop(780, 300, 400, 10, 790, 4), 10);   // full height: whole viewport
}

TEST(TooltipHit, BoxTakesTheRoomierSideButAtLeast40Percent) {
  EXPECT_EQ(tooltip_hit::tipMaxHeight(300, 400, 10, 790, 4), 386);  // below
  EXPECT_EQ(tooltip_hit::tipMaxHeight(500, 600, 10, 790, 4), 486);  // above
  EXPECT_EQ(tooltip_hit::tipMaxHeight(100, 700, 10, 790, 4), 312);  // sentence fills the page: 40% of 780
}

TEST(TooltipHit, ParagraphsStartWhereALineStepsFurtherThanTheLinePitch) {
  // Two-line heading, a margin, a three-line paragraph, paragraph spacing, then one more line.
  const int16_t tops[] = {0, 32, 80, 112, 144, 192};
  const uint16_t firstWord[] = {0, 3, 4, 10, 16, 18};
  uint16_t runs[6];
  ASSERT_EQ(tooltip_hit::paragraphRunStarts(tops, firstWord, 6, runs), 3);
  EXPECT_EQ(runs[0], 0);
  EXPECT_EQ(runs[1], 4);
  EXPECT_EQ(runs[2], 18);

  const int16_t oneLineParagraphs[] = {0, 48, 96};  // no inner step to compare against
  EXPECT_EQ(tooltip_hit::paragraphRunStarts(oneLineParagraphs, firstWord, 3, runs), 1);
}
