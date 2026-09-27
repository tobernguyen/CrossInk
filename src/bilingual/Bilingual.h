#pragma once

#include <cstdint>

class Epub;
class GfxRenderer;
class Page;

// Bilingual reading for Calibre-translated EPUBs (fork feature; the only header upstream CrossInk
// code includes). A translated book is one whose chapters hold lang-tagged translation paragraphs
// after their originals. With its per-book "bilingual tooltip" setting on, the translations are hidden
// from the page and a long-press on a sentence shows its translation instead of a dictionary lookup.
// The setting defaults to on for books detected as translated, on touch boards only.
namespace bilingual {

// Reader lifecycle. onBookOpened runs a one-time scan of a book it hasn't seen (state is kept in
// <book cache>/bilingual.bin), and clears the book's layout cache when the result changes what the
// pages must show.
void onBookOpened(const Epub& epub, bool hasTouch);
void onBookClosed();

bool bookHasTranslations();
bool tooltipEnabled();
// Persists the per-book choice and clears the book's layout cache; the caller must re-lay out.
void setTooltipEnabled(bool enabled);

// ChapterHtmlSlimParser: true for an element to skip (a translation, while the tooltip is on).
bool hidesElement(const char* const* atts);

// Preferences, global to all books (/.crosspoint/bilingual-prefs.bin); a tap in the reader menu
// cycles to the next value. The hold time is shared by every hold-to-look-up gesture: the sentence
// tooltip and both dictionary lookups (on the page and inside a definition).
unsigned long holdMs();
const char* holdLabel();
void cycleHold();
const char* tooltipFontLabel();
void cycleTooltipFont();

// Touch long-press on the current page at screen (x, y).
void selectAt(int spineIndex, int pageIndex, int x, int y);
bool tooltipActive();
void dismissTooltip();
// A tap at screen (x, y) while the tooltip shows: true when it paged a long translation (the tap was
// inside its box); false means the tap should close it.
bool scrollTooltipAt(int x, int y);
// Draw over a finished page buffer; a no-op unless a tooltip is showing on this very page.
void drawTooltip(GfxRenderer& renderer, const Page& page, int spineIndex, int pageIndex, int bodyFontId, int xOffset,
                 int yOffset, int viewportWidth, int viewportHeight, bool foregroundBlack);

}  // namespace bilingual
