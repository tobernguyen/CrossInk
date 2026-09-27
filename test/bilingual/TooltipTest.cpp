// BilingualTooltip end to end on the host: a page's words in, the tooltip text out, with the chapter
// HTML read from temp files (stub Page / GfxRenderer / storage in tooltip_stubs/).
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "bilingual/BilingualTooltip.h"

namespace {

constexpr int kMarginX = 10, kMarginY = 40, kLineHeight = 32;

// One page line: its top (page y) and its text, words 10 px per byte plus a 10 px space.
struct Line {
  int16_t top;
  const char* text;
};

Page makePage(const std::vector<Line>& lines) {
  Page page;
  for (const auto& l : lines) {
    auto line = std::make_unique<PageLine>();
    line->yPos = l.top;
    line->block = std::make_shared<TextBlock>();
    std::istringstream words(l.text);
    std::string w;
    int16_t x = 0;
    while (words >> w) {
      line->block->words.push_back(w);
      line->block->xpos.push_back(x);
      x = static_cast<int16_t>(x + 10 * w.size() + 10);
    }
    page.elements.push_back(std::move(line));
  }
  return page;
}

std::string writeChapter(const char* name, const std::string& body) {
  const std::string path = testing::TempDir() + name;
  std::ofstream(path) << "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>" << body << "</body></html>";
  return path;
}

std::string gNextChapter;  // what the tooltip's next-file resolver returns

// Hold on the start of the page line at `top`; the text the tooltip draws, or "" when none shows.
std::string holdAt(BilingualTooltip& tip, const Page& page, const int16_t top) {
  GfxRenderer renderer;
  tip.selectAt(kMarginX + 1, kMarginY + top + kLineHeight / 2);
  tip.render(renderer, page, 0, 0, kMarginX, kMarginY, 480, 700, true);
  return tip.isActive() ? renderer.drawn : "";
}

TEST(BilingualTooltip, HeadingWithoutAFullStopKeepsItsOwnTranslation) {
  const auto chapter = writeChapter("heading.xhtml",
                                    "<h2>Skeuomorphic and Native Technologies</h2>"
                                    "<h2 lang=\"vi\">Công nghệ mô phỏng và công nghệ bản địa</h2>"
                                    "<p>People use new technologies in one of two ways. The first set is popular.</p>"
                                    "<p lang=\"vi\">Mọi người dùng công nghệ mới theo hai cách. Nhóm đầu phổ biến.</p>");
  // The heading's margin shows as a bigger step between lines than the paragraph's line pitch.
  const Page page = makePage({{0, "Skeuomorphic and Native"},
                              {32, "Technologies"},
                              {80, "People use new technologies in one"},
                              {112, "of two ways. The first set is"},
                              {144, "popular."}});
  BilingualTooltip tip;
  tip.setChapterHtmlPath(chapter);
  EXPECT_NE(holdAt(tip, page, 0).find("Công nghệ mô phỏng"), std::string::npos);
  EXPECT_NE(holdAt(tip, page, 80).find("Mọi người dùng"), std::string::npos);
  EXPECT_NE(holdAt(tip, page, 144).find("Nhóm đầu"), std::string::npos);
}

TEST(BilingualTooltip, TranslationAtTheStartOfTheNextFile) {
  // A transfer optimizer split the chapter file between a paragraph and its translation.
  const auto chapter = writeChapter("part1.xhtml",
                                    "<p>Network effects decide who wins online.</p>"
                                    "<p lang=\"vi\">Hiệu ứng mạng quyết định ai thắng.</p>"
                                    "<p>Some policymakers seek to defang the largest companies.</p>");
  gNextChapter = writeChapter("part2.xhtml",
                              "<p lang=\"vi\">Một số nhà hoạch định chính sách tìm cách kiềm chế.</p>"
                              "<p>Many startups try to build new networks.</p>"
                              "<p lang=\"vi\">Nhiều công ty khởi nghiệp.</p>");
  const Page page = makePage({{0, "Some policymakers seek to defang"}, {32, "the largest companies."}});
  BilingualTooltip tip;
  tip.setChapterHtmlPath(chapter, [] { return gNextChapter; });
  EXPECT_NE(holdAt(tip, page, 0).find("Một số nhà hoạch định"), std::string::npos);
}

TEST(BilingualTooltip, TranslationSplitIntoSeveralParagraphs) {
  // A long translation cut into sibling <p>s, each still tagged lang.
  const auto chapter = writeChapter("split.xhtml",
                                    "<p>The first idea is simple enough. The second idea takes longer to explain.</p>"
                                    "<p lang=\"vi\">Ý tưởng thứ nhất khá đơn giản.</p>"
                                    "<p lang=\"vi\">Ý tưởng thứ hai cần nhiều thời gian hơn.</p>");
  const Page page = makePage({{0, "The first idea is simple enough."},
                              {32, "The second idea takes longer to"},
                              {64, "explain."}});
  BilingualTooltip tip;
  tip.setChapterHtmlPath(chapter);
  EXPECT_NE(holdAt(tip, page, 0).find("Ý tưởng thứ nhất"), std::string::npos);
  EXPECT_NE(holdAt(tip, page, 32).find("Ý tưởng thứ hai"), std::string::npos);
}

}  // namespace
