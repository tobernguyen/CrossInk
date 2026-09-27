#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "bilingual/SentencePairing.h"
#include "bilingual/SentenceSplitter.h"

// Touch-to-sentence lookup for the Tooltip overlay's long-press selection. Pure geometry over the
// page's words, indexed exactly as TooltipOverlay::preparePage collects them (PageLine order, then
// word order), so the word it returns is the same index the sentence spans are cut on. Header-only
// and hardware-free so the host tests exercise the production rule.
namespace tooltip_hit {

struct WordPos {
  int16_t x;        // word's left edge, screen px (line->xPos + wordXpos + xOffset)
  int16_t lineTop;  // its line's top, screen px (line->yPos + yOffset)
};

// Word under a touch at (x, y), or -1. The line is the one whose [top, top + lineHeight) band holds
// y, else the nearest line within one line height (a touch in paragraph spacing still lands on
// text; one on a far margin does not). On that line it is the word with the greatest left edge at
// or before x -- the word under the finger, independent of LTR/RTL order -- or the leftmost word
// when the touch is left of them all.
inline int wordAt(const WordPos* words, const int count, const int x, const int y, const int lineHeight) {
  int bestTop = 0;
  int bestDist = lineHeight + 1;
  for (int i = 0; i < count; i++) {
    const int top = words[i].lineTop;
    const int dist = y < top ? top - y : (y >= top + lineHeight ? y - (top + lineHeight) + 1 : 0);
    if (dist < bestDist) {
      bestDist = dist;
      bestTop = top;
    }
  }
  if (bestDist > lineHeight) return -1;

  int under = -1;
  int leftmost = -1;
  for (int i = 0; i < count; i++) {
    if (words[i].lineTop != bestTop) continue;
    if (leftmost < 0 || words[i].x < words[leftmost].x) leftmost = i;
    if (words[i].x <= x && (under < 0 || words[i].x > words[under].x)) under = i;
  }
  return under >= 0 ? under : leftmost;
}

// Tooltip step whose source-sentence span covers `word`, or -1 (the word is in a sentence with no
// translation, so no step was made for it).
inline int stepForWord(const SentenceSplitResult& splits, const SentenceStep* steps, const int stepCount,
                       const int word) {
  for (int k = 0; k < stepCount; k++) {
    if (word >= splits.spans[steps[k].firstSentence].startWord && word < splits.spans[steps[k].lastSentence].endWord) {
      return k;
    }
  }
  return -1;
}

// Word indices where the page's paragraphs start, from its lines (page order: top y, first word).
// CrossInk lines carry no paragraph index, but inside a paragraph every line advances by the same
// step while a paragraph end adds its margins (and the paragraph-spacing setting), so a step larger
// than the page's smallest one starts a paragraph. Line 0 always does. A page of one-line
// paragraphs has no inner step to compare and finds no others -- its sentences still end at their
// punctuation. ponytail: a ruby-annotated line is taller and reads as a paragraph start.
inline int paragraphRunStarts(const int16_t* lineTops, const uint16_t* lineFirstWord, const int lineCount,
                              uint16_t* runStarts) {
  int pitch = 0;
  for (int i = 1; i < lineCount; i++) {
    const int step = lineTops[i] - lineTops[i - 1];
    if (step > 0 && (pitch == 0 || step < pitch)) pitch = step;
  }
  int count = 0;
  for (int i = 0; i < lineCount; i++) {
    if (i > 0 && (pitch == 0 || lineTops[i] - lineTops[i - 1] <= pitch)) continue;
    if (count > 0 && runStarts[count - 1] == lineFirstWord[i]) continue;  // an empty line
    runStarts[count++] = lineFirstWord[i];
  }
  return count;
}

// Tallest box tipTop can place without covering the sentence: its roomier side. At least 40% of the
// viewport, though, so a sentence that fills the page still gets a readable (overlapping) box. A
// translation taller than this pages inside the box.
inline int tipMaxHeight(const int sentTop, const int sentBottom, const int viewTop, const int viewBottom,
                        const int gap) {
  return std::max({sentTop - gap - viewTop, viewBottom - (sentBottom + gap), (viewBottom - viewTop) * 4 / 10});
}

// Top edge of a tipH-tall tooltip for a sentence spanning [sentTop, sentBottom) in the viewport
// [viewTop, viewBottom): above the sentence when it fits there, else below it. When neither side
// fits, it starts on the roomier side and overlaps the sentence, clamped to the viewport, so a long
// translation is shown whole rather than cut to one side's room.
inline int tipTop(const int tipH, const int sentTop, const int sentBottom, const int viewTop, const int viewBottom,
                  const int gap) {
  const int roomAbove = sentTop - gap - viewTop;
  const int roomBelow = viewBottom - (sentBottom + gap);
  const bool above = tipH <= roomAbove || (tipH > roomBelow && roomAbove > roomBelow);
  const int y = above ? sentTop - gap - tipH : sentBottom + gap;
  return std::max(viewTop, std::min(y, viewBottom - tipH));
}

}  // namespace tooltip_hit
