#include "bilingual/BilingualTooltip.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <expat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bilingual/PageWindow.h"
#include "bilingual/ParagraphBoundary.h"
#include "bilingual/TextNormalize.h"
#include "bilingual/TooltipHitTest.h"

// The chapter reparse (paragraph pairing, sentence alignment) and the tooltip box drawing are
// CrossLingua's TooltipOverlay; the paragraph-range gate is replaced by PageWindow.

namespace {

struct SentEntry {
  std::string key;          // first 6 words (normalized) of the original sentence
  std::string translation;  // its aligned translation
};

// Strip '.', collapse spaces, trim: the form both page and chapter keys are compared in.
std::string normalizeKey(const std::string& key) {
  std::string out;
  out.reserve(key.size());
  for (const char c : key) {
    if (c == '.') continue;
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// A page sentence may be cut short (hyphenation, page edge), so keys match on their common prefix.
bool keysMatch(const std::string& a, const std::string& b) {
  const size_t cl = std::min(a.size(), b.size());
  return cl >= 3 && a.compare(0, cl, b, 0, cl) == 0;
}

// ── Chapter parsing: extract (original, translation) paragraph pairs ────────────────────────

void addPairToIndex(const std::string& origText, const std::string& transText, SentencePairScratch& scratch,
                    std::vector<SentEntry>& index);

struct ParseCtx {
  SentencePairScratch* scratch = nullptr;
  const std::vector<std::string>* anchors = nullptr;  // normalized page keys the window matches on
  bilingual::PageWindow<SentEntry>* window = nullptr;
  XML_Parser parser = nullptr;
  int blockDepth = 0;
  bool inBlock = false;
  bool isTranslation = false;
  int skipDepth = 0;  // inside a <table> / undecodable-image subtree the layout collapses to one paragraph
  bool currentBlockIsLi = false;
  std::string currentText;
  // A pair is a run of original blocks and the run of translation blocks after it: one block each
  // as Calibre writes them, several when a paragraph was split (the CrossPoint Calibre plugin's
  // transfer optimizer cuts big <p>s into siblings, and a <blockquote> can hold several <p>s).
  std::string origRun;
  std::string transRun;
  bool stopAfterPair = false;  // reading the next file only to finish the pair this one ended in
  bool stopped = false;
  int pairCount = 0;
};

void trimText(std::string& t) {
  while (!t.empty() && (t.front() == ' ' || t.front() == '\n')) t.erase(0, 1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
}

void appendRun(std::string& run, const std::string& t) {
  if (!run.empty()) run += ' ';
  run += t;
}

void stopParse(ParseCtx* ctx) {
  ctx->stopped = true;
  if (ctx->parser) XML_StopParser(ctx->parser, XML_FALSE);
}

void emitPair(ParseCtx* ctx) {
  std::vector<SentEntry> entries;
  addPairToIndex(ctx->origRun, ctx->transRun, *ctx->scratch, entries);
  int matched = 0;
  for (auto& e : entries) {
    e.key = normalizeKey(e.key);
    for (const auto& anchor : *ctx->anchors) {
      if (keysMatch(e.key, anchor)) {
        matched++;
        break;
      }
    }
  }
  ctx->pairCount++;
  ctx->origRun.clear();
  ctx->transRun.clear();
  if (!ctx->window->addPair(std::move(entries), matched) || ctx->stopAfterPair) stopParse(ctx);
}

void flushOriginalParagraph(ParseCtx* ctx) {
  auto& t = ctx->currentText;
  trimText(t);
  if (!t.empty()) {
    if (!ctx->transRun.empty()) {
      emitPair(ctx);  // an original after translations: that pair is complete
    } else if (ctx->stopAfterPair) {
      stopParse(ctx);  // the next file opens on an original: the carried one has no translation
    }
    appendRun(ctx->origRun, t);
  }
  t.clear();
}

bool hasVisibleText(const std::string& s) {
  for (const char c : s) {
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return true;
  }
  return false;
}

void seedLiBulletIfEmpty(ParseCtx* ctx) {
  if (ctx->currentBlockIsLi && !hasVisibleText(ctx->currentText)) ctx->currentText = "\xe2\x80\xa2";
}

bool imgSrcDecodable(const char* src) {
  if (!src || !*src) return false;
  const char* dot = strrchr(src, '.');
  if (!dot) return false;
  std::string ext(dot);
  for (auto& c : ext) {
    if (c >= 'A' && c <= 'Z') c += 32;
  }
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

void XMLCALL chOnStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* ctx = static_cast<ParseCtx*>(ud);
  if (ctx->skipDepth > 0) {
    ctx->skipDepth++;
    return;
  }
  if (paraboundary::isHardBreak(name)) {
    if (ctx->inBlock && !ctx->isTranslation) flushOriginalParagraph(ctx);
    return;
  }
  if (strcmp(name, "table") == 0) {
    if (!ctx->isTranslation) {
      seedLiBulletIfEmpty(ctx);
      flushOriginalParagraph(ctx);
    }
    if (!ctx->inBlock) {
      ctx->inBlock = true;
      ctx->blockDepth = 1;
    }
    ctx->isTranslation = false;
    ctx->currentText = "[Table omitted]";
    ctx->currentBlockIsLi = false;
    ctx->skipDepth = 1;
    return;
  }
  if (strcmp(name, "img") == 0) {
    const char* src = nullptr;
    const char* alt = nullptr;
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0)
          src = atts[i + 1];
        else if (strcmp(atts[i], "alt") == 0)
          alt = atts[i + 1];
      }
    }
    if (!imgSrcDecodable(src) && alt && *alt) {
      if (!ctx->isTranslation) {
        seedLiBulletIfEmpty(ctx);
        flushOriginalParagraph(ctx);
      }
      if (!ctx->inBlock) {
        ctx->inBlock = true;
        ctx->blockDepth = 1;
      }
      ctx->isTranslation = false;
      ctx->currentText = std::string("[Image: ") + alt + "]";
      ctx->currentBlockIsLi = false;
      ctx->skipDepth = 1;
    }
    return;
  }
  if (paraboundary::isContainerBlockTag(name)) {
    if (ctx->inBlock && !ctx->isTranslation) {
      seedLiBulletIfEmpty(ctx);
      flushOriginalParagraph(ctx);
    }
    ctx->inBlock = true;
    ctx->blockDepth = 1;
    ctx->isTranslation = false;
    ctx->currentBlockIsLi = (strcmp(name, "li") == 0);
    ctx->currentText.clear();
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "lang") == 0 || strcmp(atts[i], "xml:lang") == 0) ctx->isTranslation = true;
      }
    }
    return;
  }
  if (ctx->inBlock) ctx->blockDepth++;
}

void XMLCALL chOnEnd(void* ud, const XML_Char* name) {
  auto* ctx = static_cast<ParseCtx*>(ud);
  if (ctx->skipDepth > 0) {
    ctx->skipDepth--;
    return;
  }
  if (paraboundary::isHardBreak(name)) return;
  if (strcmp(name, "img") == 0) return;
  if (!ctx->inBlock) return;
  ctx->blockDepth--;
  if (ctx->blockDepth > 0) return;
  ctx->inBlock = false;

  if (ctx->isTranslation) {
    auto& t = ctx->currentText;
    trimText(t);
    if (!t.empty() && !ctx->origRun.empty()) appendRun(ctx->transRun, t);  // a stray translation has no pair
    t.clear();
  } else {
    seedLiBulletIfEmpty(ctx);
    flushOriginalParagraph(ctx);
  }
  ctx->currentBlockIsLi = false;
}

void XMLCALL chOnChar(void* ud, const XML_Char* data, int len) {
  auto* ctx = static_cast<ParseCtx*>(ud);
  if (ctx->skipDepth > 0 || !ctx->inBlock) return;
  for (int i = 0; i < len; i++) {
    char c = data[i];
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && !ctx->currentText.empty() && ctx->currentText.back() == ' ') continue;
    ctx->currentText += c;
  }
}

// ── Sentence index ──────────────────────────────────────────────────────────────────────────

std::vector<std::string> tokenizeWords(const std::string& text) {
  std::vector<std::string> words;
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    int wl;
    while (i < n && (wl = textnorm::whitespaceLenAt(text, i)) > 0) i += static_cast<size_t>(wl);
    if (i >= n) break;
    const size_t start = i;
    while (i < n && textnorm::whitespaceLenAt(text, i) == 0) i++;
    words.emplace_back(text.data() + start, i - start);
  }
  return words;
}

void addPairToIndex(const std::string& origText, const std::string& transText, SentencePairScratch& scratch,
                    std::vector<SentEntry>& index) {
  auto origWords = tokenizeWords(origText);
  auto transWords = tokenizeWords(transText);
  if (origWords.empty() || transWords.empty()) return;

  std::vector<const char*> origPtrs, transPtrs;
  origPtrs.reserve(origWords.size());
  transPtrs.reserve(transWords.size());
  for (auto& w : origWords) origPtrs.push_back(w.c_str());
  for (auto& w : transWords) transPtrs.push_back(w.c_str());

  if (!splitSentencePair(origPtrs.data(), static_cast<int>(origPtrs.size()), transPtrs.data(),
                         static_cast<int>(transPtrs.size()), scratch)) {
    return;
  }
  mapSentenceSpans(origPtrs.data(), transPtrs.data(), scratch);
  for (int os = 0; os < scratch.origSplits.count; os++) {
    std::string trans = joinSpan(transWords, scratch.transFor[os]);
    std::string key =
        sentenceKey(origPtrs.data(), scratch.origSplits.spans[os].startWord, scratch.origSplits.spans[os].endWord);
    if (!key.empty() && !trans.empty()) index.push_back({std::move(key), std::move(trans)});
  }
}

// Stream a file through the parser. False on a parse error (`failure` says why); a stop is not one.
bool feedFile(HalFile& f, XML_Parser parser, const char*& failure) {
  char buf[1024];
  for (bool done = false; !done;) {
    int n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
    if (n < 0) n = 0;
    done = (n < static_cast<int>(sizeof(buf)));
    if (XML_Parse(parser, buf, n, done) == XML_STATUS_ERROR) {
      const XML_Error code = XML_GetErrorCode(parser);
      if (code == XML_ERROR_ABORTED) return true;  // stopped: the window closed behind the page
      LOG_ERR("BLT", "XML parse error at line %lu: %s", XML_GetCurrentLineNumber(parser), XML_ErrorString(code));
      failure = code == XML_ERROR_NO_MEMORY ? "out of memory" : "chapter parse error";
      return false;
    }
  }
  return true;
}

XML_Parser makeChapterParser(ParseCtx& ctx) {
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) return nullptr;
  XML_SetUserData(parser, &ctx);
  XML_SetElementHandler(parser, chOnStart, chOnEnd);
  XML_SetCharacterDataHandler(parser, chOnChar);
  return parser;
}

// `failure` names why the chapter couldn't be read (whatever was read before stays usable).
// `nextPath` gives the following spine item's HTML: the CrossPoint Calibre plugin splits chapter
// files at ~7 KB, sometimes between a paragraph and its translation.
std::vector<SentEntry> parseAndBuildIndex(const std::string& path, const std::vector<std::string>& anchors,
                                          const char*& failure, std::string (*nextPath)()) {
  std::vector<SentEntry> none;
  if (anchors.empty()) return none;
  if (path.empty()) {
    failure = "chapter not found";
    return none;
  }

  HalFile f;
  if (!Storage.openFileForRead("BLT", path, f)) {
    LOG_ERR("BLT", "Cannot open %s", path.c_str());
    failure = "can't open chapter";
    return none;
  }
  const auto scratch = makeUniqueNoThrow<SentencePairScratch>();
  if (!scratch) {
    LOG_ERR("BLT", "OOM: sentence pairing scratch");
    failure = "out of memory";
    return none;
  }
  bilingual::PageWindow<SentEntry> window(static_cast<int>(anchors.size()));
  ParseCtx ctx;
  ctx.scratch = scratch.get();
  ctx.anchors = &anchors;
  ctx.window = &window;
  ctx.parser = makeChapterParser(ctx);
  if (!ctx.parser) {
    LOG_ERR("BLT", "Failed to create expat parser");
    failure = "out of memory";
    return none;
  }
  bool ok = feedFile(f, ctx.parser, failure);
  XML_ParserFree(ctx.parser);
  ctx.parser = nullptr;
  f.close();

  // The file ended on an original still waiting for its translation: finish that pair from the
  // start of the next file (the split file's continuation).
  if (ok && !ctx.stopped && !ctx.origRun.empty() && ctx.transRun.empty() && nextPath) {
    const std::string next = nextPath();
    HalFile nf;
    if (!next.empty() && Storage.openFileForRead("BLT", next, nf)) {
      ctx.inBlock = ctx.isTranslation = ctx.currentBlockIsLi = false;
      ctx.blockDepth = ctx.skipDepth = 0;
      ctx.stopAfterPair = true;
      ctx.parser = makeChapterParser(ctx);
      if (ctx.parser) {
        ok = feedFile(nf, ctx.parser, failure);
        XML_ParserFree(ctx.parser);
        ctx.parser = nullptr;
      }
    }
  }
  if (ok && !ctx.stopped && !ctx.origRun.empty() && !ctx.transRun.empty()) emitPair(&ctx);

  auto index = window.take();
  LOG_DBG("BLT", "Index: %d entries kept from %d pairs", static_cast<int>(index.size()), ctx.pairCount);
  return index;
}

// ── Tooltip box helpers ─────────────────────────────────────────────────────────────────────

int findLastLineY(const Page& page, const SentenceSpan& span, const int yOffset) {
  int idx = 0, lastY = 0;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const uint16_t n = line->getBlock()->wordCount();
    for (uint16_t i = 0; i < n; i++) {
      if (idx >= span.startWord && idx < span.endWord) lastY = line->yPos + yOffset;
      idx++;
    }
  }
  return lastY;
}

int utf8CharLen(const char* s, const int maxLen) {
  if (maxLen <= 0) return 0;
  const unsigned char c = static_cast<unsigned char>(s[0]);
  int n = 1;
  if ((c & 0xE0) == 0xC0)
    n = 2;
  else if ((c & 0xF0) == 0xE0)
    n = 3;
  else if ((c & 0xF8) == 0xF0)
    n = 4;
  return n > maxLen ? maxLen : n;
}

int fitPrefixBytes(GfxRenderer& renderer, const int fontId, const char* s, const int len, const int avail) {
  char buf[512];
  int fit = 0;
  int i = 0;
  while (i < len) {
    const int cand = i + utf8CharLen(s + i, len - i);
    if (cand > static_cast<int>(sizeof(buf)) - 1) break;
    memcpy(buf, s, cand);
    buf[cand] = '\0';
    if (renderer.getTextWidth(fontId, buf) > avail) break;
    fit = cand;
    i = cand;
  }
  return fit;
}

struct TipLine {
  const char* start;
  int len;
};

// Wrap into lines measured with the same getTextWidth call the draw uses, so box height and drawn
// lines agree exactly; an over-long word is hard-broken on a UTF-8 boundary. Returns the total line
// count and stores lines [first, first + maxOut) in out.
int wrapTooltipLines(GfxRenderer& renderer, const int fontId, const char* text, int avail, TipLine* out,
                     const int first, const int maxOut) {
  if (avail < 1) avail = 1;
  char buf[512];
  int count = 0;
  const char* p = text;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* lineStart = p;
    int lineLen = 0;
    while (*p) {
      const char* wordStart = p;
      while (*p && *p != ' ') p++;
      const int cand = static_cast<int>(p - lineStart);
      const int measLen = cand < static_cast<int>(sizeof(buf)) - 1 ? cand : static_cast<int>(sizeof(buf)) - 1;
      memcpy(buf, lineStart, measLen);
      buf[measLen] = '\0';
      if (renderer.getTextWidth(fontId, buf) <= avail) {
        lineLen = cand;
        while (*p == ' ') p++;
        continue;
      }
      if (lineLen > 0) {
        p = wordStart;
        break;
      }
      const int wordLen = static_cast<int>(p - wordStart);
      int fit = fitPrefixBytes(renderer, fontId, wordStart, wordLen, avail);
      if (fit <= 0) fit = utf8CharLen(wordStart, wordLen);
      lineLen = fit;
      p = wordStart + fit;
      break;
    }
    if (count >= first && count < first + maxOut) out[count - first] = {lineStart, lineLen};
    count++;
  }
  return count;
}

// One wrapped box above the touch, for a hold that found no translation because the chapter didn't
// load (see BilingualTooltip::loadFailure_).
void drawNotice(GfxRenderer& renderer, const int fontId, const char* text, const int xOffset, const int yOffset,
                const int viewportWidth, const int viewportHeight, const int touchY, const bool foregroundBlack) {
  constexpr int PAD = 6, MAX_LINES = 4;
  TipLine lines[MAX_LINES];
  const int n = std::clamp(wrapTooltipLines(renderer, fontId, text, viewportWidth - 4 * PAD, lines, 0, MAX_LINES), 1,
                           MAX_LINES);
  const int tlh = renderer.getLineHeight(fontId);
  const int w = viewportWidth - 2 * PAD, h = n * tlh + 2 * PAD;
  const int y = std::clamp(touchY - h - tlh, yOffset + PAD, yOffset + viewportHeight - PAD - h);
  const bool grayPlane = renderer.getRenderMode() != GfxRenderer::BW;
  renderer.fillRect(xOffset + PAD - 1, y - 1, w + 2, h + 2, grayPlane || !foregroundBlack);
  renderer.drawRoundedRect(xOffset + PAD, y, w, h, 1, 3, foregroundBlack);
  for (int i = 0; i < n && i < MAX_LINES; i++) {
    char lb[512];
    const int cl = std::min(lines[i].len, 511);
    memcpy(lb, lines[i].start, cl);
    lb[cl] = '\0';
    renderer.drawText(fontId, xOffset + 2 * PAD, y + PAD + i * tlh, lb, foregroundBlack, EpdFontFamily::REGULAR);
  }
}

}  // namespace

// ── Page preparation ────────────────────────────────────────────────────────────────────────

void BilingualTooltip::preparePage(const Page& page) {
  if (pagePrepared_) return;
  pagePrepared_ = true;
  sentenceTranslations_.clear();
  stepCount_ = 0;

  // 1. The page's words, in PageLine order (the indexing TooltipHitTest and the spans share), cut
  //    into paragraph runs so a heading without a full stop stays its own sentence.
  std::vector<const char*> words;
  words.reserve(MAX_WORDS);
  std::vector<int16_t> lineTops;
  std::vector<uint16_t> lineFirstWord;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine || static_cast<int>(words.size()) >= MAX_WORDS) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    lineTops.push_back(line->yPos);
    lineFirstWord.push_back(static_cast<uint16_t>(words.size()));
    const auto& block = line->getBlock();
    for (uint16_t i = 0; i < block->wordCount() && static_cast<int>(words.size()) < MAX_WORDS; i++) {
      words.push_back(block->wordText(i));
    }
  }
  origWordCount_ = static_cast<int>(words.size());
  std::vector<uint16_t> runStarts(lineTops.size());
  const int runCount = origWordCount_ > 0 ? tooltip_hit::paragraphRunStarts(lineTops.data(), lineFirstWord.data(),
                                                                            static_cast<int>(lineTops.size()),
                                                                            runStarts.data())
                                          : 0;
  splits_ = splitSentencesByParagraph(words.data(), origWordCount_, runStarts.data(), runCount);
  mergeJunkSentences(splits_, words.data(), runStarts.data(), runCount);

  // 2. Page sentence keys, and the anchors the chapter window opens on (longer keys, so a short
  //    common sentence elsewhere in the chapter can't open it; fall back to all keys).
  std::vector<std::string> pageKeys(splits_.count);
  std::vector<std::string> anchors;
  for (int s = 0; s < splits_.count; s++) {
    pageKeys[s] = normalizeKey(sentenceKey(words.data(), splits_.spans[s].startWord, splits_.spans[s].endWord));
    if (pageKeys[s].size() >= 8) anchors.push_back(pageKeys[s]);
  }
  if (anchors.empty()) {
    for (const auto& k : pageKeys) {
      if (!k.empty()) anchors.push_back(k);
    }
  }

  // 3. The chapter entries around this page.
  const auto index = parseAndBuildIndex(htmlPath_, anchors, loadFailure_, nextPath_);
  if (index.empty() && !loadFailure_ && !anchors.empty()) loadFailure_ = "page not found in chapter";
  if (loadFailure_) {
    const auto heap = MemoryBudget::snapshot();
    failFreeHeap_ = heap.freeHeap;
    failMaxAlloc_ = heap.maxAllocHeap;
    LOG_ERR("BLT", "Page entries not loaded: %s (free=%u maxAlloc=%u)", loadFailure_, heap.freeHeap,
            heap.maxAllocHeap);
  }
  if (index.empty()) return;

  // 4. Match page sentences to entries by key (sequential hint first), then gap-fill unmatched
  //    sentences from their keyed neighbours' index positions -- CrossLingua's rule unchanged.
  sentenceTranslations_.resize(splits_.count);
  std::vector<int> matchedIdx(splits_.count, -1);
  std::vector<int> keyed;
  int lastIdx = -1;
  for (int s = 0; s < splits_.count; s++) {
    const std::string& np = pageKeys[s];
    if (np.empty()) continue;
    keyed.push_back(s);
    int foundIdx = -1;
    if (lastIdx >= 0 && lastIdx + 1 < static_cast<int>(index.size()) && keysMatch(np, index[lastIdx + 1].key)) {
      foundIdx = lastIdx + 1;
    }
    if (foundIdx < 0) {
      size_t bestLen = 0;
      for (int j = 0; j < static_cast<int>(index.size()); j++) {
        const size_t cl = std::min(np.size(), index[j].key.size());
        if (cl > bestLen && keysMatch(np, index[j].key)) {
          bestLen = cl;
          foundIdx = j;
        }
      }
    }
    if (foundIdx >= 0) {
      sentenceTranslations_[s] = index[foundIdx].translation;
      matchedIdx[s] = foundIdx;
      lastIdx = foundIdx;
    }
  }
  for (int k = static_cast<int>(keyed.size()) - 2; k >= 0; k--) {
    const int s = keyed[k], next = keyed[k + 1];
    if (matchedIdx[s] < 0 && matchedIdx[next] > 0) {
      matchedIdx[s] = matchedIdx[next] - 1;
      sentenceTranslations_[s] = index[matchedIdx[s]].translation;
    }
  }
  for (size_t k = 1; k < keyed.size(); k++) {
    const int s = keyed[k], prev = keyed[k - 1];
    if (matchedIdx[s] < 0 && matchedIdx[prev] >= 0 && matchedIdx[prev] + 1 < static_cast<int>(index.size())) {
      matchedIdx[s] = matchedIdx[prev] + 1;
      sentenceTranslations_[s] = index[matchedIdx[s]].translation;
    }
  }

  // 5. Consecutive sentences sharing one translation become one step.
  stepCount_ = groupTranslationSteps(sentenceTranslations_, steps_, MAX_SENTENCES);
  LOG_DBG("BLT", "Page: %d words, %d sentences, %d steps", origWordCount_, splits_.count, stepCount_);
}

int BilingualTooltip::stepAtTouch(const Page& page, const int lineHeight, const int xOffset, const int yOffset) const {
  std::vector<tooltip_hit::WordPos> words;
  words.reserve(origWordCount_);
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getBlock();
    for (uint16_t i = 0; i < block->wordCount() && static_cast<int>(words.size()) < origWordCount_; i++) {
      words.push_back({static_cast<int16_t>(block->wordXpos(i) + line->xPos + xOffset),
                       static_cast<int16_t>(line->yPos + yOffset)});
    }
  }
  const int word = tooltip_hit::wordAt(words.data(), static_cast<int>(words.size()), touchX_, touchY_, lineHeight);
  return word < 0 ? -1 : tooltip_hit::stepForWord(splits_, steps_, stepCount_, word);
}

BilingualTooltip::SentenceBounds BilingualTooltip::findSentenceBounds(const Page& page, const SentenceSpan& span,
                                                                      const int xOffset, const int yOffset) const {
  SentenceBounds bounds = {0, 0, 0};
  int idx = 0;
  bool found = false;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getBlock();
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      if (idx >= span.startWord && idx < span.endWord) {
        const int wx = block->wordXpos(i) + line->xPos + xOffset;
        const int wy = line->yPos + yOffset;
        if (!found) {
          bounds = {wy, wx, wx};
          found = true;
        }
        if (wy == bounds.firstLineY) {
          bounds.startX = std::min(bounds.startX, wx);
          bounds.endX = std::max(bounds.endX, wx);
        }
      }
      idx++;
    }
  }
  return bounds;
}

void BilingualTooltip::drawSentenceUnderline(GfxRenderer& renderer, const Page& page, const SentenceSpan& span,
                                             const int fontId, const int xOffset, const int yOffset,
                                             const bool foregroundBlack) const {
  int idx = 0, curY = -1, sx = 0, ex = 0;
  const int ulOff = renderer.getFontAscenderSize(fontId) + 2;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getBlock();
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      if (idx >= span.startWord && idx < span.endWord) {
        const int wx = block->wordXpos(i) + line->xPos + xOffset;
        const int wy = line->yPos + yOffset;
        const int ww = renderer.getTextWidth(fontId, block->wordText(i), block->wordStyle(i));
        if (wy != curY) {
          if (curY >= 0) renderer.drawLine(sx, curY + ulOff, ex, curY + ulOff, foregroundBlack);
          curY = wy;
          sx = wx;
        }
        ex = wx + ww;
      }
      idx++;
    }
  }
  if (curY >= 0) renderer.drawLine(sx, curY + ulOff, ex, curY + ulOff, foregroundBlack);
}

// ── Rendering ───────────────────────────────────────────────────────────────────────────────

void BilingualTooltip::render(GfxRenderer& renderer, const Page& page, const int fontId, const int tooltipFontId,
                              const int xOffset, const int yOffset, const int viewportWidth, const int viewportHeight,
                              const bool foregroundBlack) {
  if (currentStep_ < 0) return;
  preparePage(page);

  if (touchPending_) {
    touchPending_ = false;
    const int step = stepAtTouch(page, renderer.getLineHeight(fontId), xOffset, yOffset);
    notice_ = step < 0 && loadFailure_;
    if (step < 0 && !notice_) {
      currentStep_ = -1;
      return;
    }
    if (step >= 0) currentStep_ = static_cast<int8_t>(step);
  }
  if (notice_) {
    tipPageCount_ = 0;  // a tap anywhere closes it
    char text[128];
    snprintf(text, sizeof(text), "No translation: %s (free %uK, max %uK)", loadFailure_,
             static_cast<unsigned>(failFreeHeap_ / 1024), static_cast<unsigned>(failMaxAlloc_ / 1024));
    if (renderer.isFontCacheScanning()) renderer.drawText(tooltipFontId, 0, 0, text, true, EpdFontFamily::REGULAR);
    drawNotice(renderer, tooltipFontId, text, xOffset, yOffset, viewportWidth, viewportHeight, touchY_,
               foregroundBlack);
    return;
  }
  if (stepCount_ == 0) {
    currentStep_ = -1;
    return;
  }
  if (currentStep_ >= stepCount_) currentStep_ = static_cast<int8_t>(stepCount_ - 1);

  const SentenceStep& st = steps_[currentStep_];
  const SentenceSpan span{splits_.spans[st.firstSentence].startWord, splits_.spans[st.lastSentence].endWord};
  const char* text = sentenceTranslations_[st.firstSentence].c_str();
  if (!text[0]) return;

  // Font prewarm scan: record the whole translation, not only the lines drawn below. Before the
  // prewarm, some SD-font widths can resolve differently than in the real pass, so the text may wrap
  // (and page) differently there, and glyphs only on its later lines would draw as replacement boxes.
  if (renderer.isFontCacheScanning()) renderer.drawText(tooltipFontId, 0, 0, text, true, EpdFontFamily::REGULAR);

  const int lh = renderer.getLineHeight(fontId);
  const auto bounds = findSentenceBounds(page, span, xOffset, yOffset);
  if (bounds.firstLineY == 0 && bounds.startX == 0) return;
  const int lastY = findLastLineY(page, span, yOffset);

  constexpr int PAD = 6, RAD = 3, GAP = 4;
  const int maxW = viewportWidth - 2 * PAD;
  const int tlh = renderer.getLineHeight(tooltipFontId);
  const int viewTop = yOffset + PAD, viewBottom = yOffset + viewportHeight - PAD, sentBottom = lastY + lh;
  static constexpr int MAX_TIP_LINES = 30;
  const int rows = std::clamp(
      (tooltip_hit::tipMaxHeight(bounds.firstLineY, sentBottom, viewTop, viewBottom, GAP) - 2 * PAD) / tlh, 1,
      MAX_TIP_LINES);
  TipLine lines[MAX_TIP_LINES];
  lines[0] = {text, 0};
  const int total = std::max(1, wrapTooltipLines(renderer, tooltipFontId, text, maxW - 2 * PAD, lines, 0, 0));
  // Too long for the box: it pages, one tap at a time, keeping its bottom row for the "k/n" marker.
  const int perPage = total > rows && rows > 1 ? rows - 1 : rows;
  tipPageCount_ = (total + perPage - 1) / perPage;
  if (tipPage_ >= tipPageCount_) tipPage_ = 0;
  wrapTooltipLines(renderer, tooltipFontId, text, maxW - 2 * PAD, lines, tipPage_ * perPage, perPage);
  const int nLines = std::min(perPage, total - tipPage_ * perPage);
  const int boxRows = tipPageCount_ > 1 ? rows : nLines;  // every part in the same box

  int tipW = maxW;
  if (total == 1) {
    char lb[512];
    const int cl = std::min(lines[0].len, 511);
    memcpy(lb, lines[0].start, cl);
    lb[cl] = '\0';
    tipW = std::min(renderer.getTextWidth(tooltipFontId, lb) + 2 * PAD, maxW);
  }
  const int tipH = boxRows * tlh + 2 * PAD;
  const int tipX = xOffset + PAD;
  const int tipY = tooltip_hit::tipTop(tipH, bounds.firstLineY, sentBottom, viewTop, viewBottom, GAP);
  boxX_ = tipX, boxY_ = tipY, boxW_ = tipW, boxH_ = tipH;

  // Underline first: a tall box may overlap the sentence, and must cover the underline there.
  drawSentenceUnderline(renderer, page, span, fontId, xOffset, yOffset, foregroundBlack);
  // In a grayscale plane a set pixel adds gray, so the box is cleared there (drawn "black"): the page
  // text it covers leaves no anti-aliased edges inside it.
  const bool grayPlane = renderer.getRenderMode() != GfxRenderer::BW;
  renderer.fillRect(tipX - 1, tipY - 1, tipW + 2, tipH + 2, grayPlane || !foregroundBlack);
  renderer.drawRoundedRect(tipX, tipY, tipW, tipH, 1, RAD, foregroundBlack);
  int textY = tipY + PAD;
  for (int i = 0; i < nLines; i++) {
    if (lines[i].len > 0) {
      char lb[512];
      const int cl = std::min(lines[i].len, 511);
      memcpy(lb, lines[i].start, cl);
      lb[cl] = '\0';
      renderer.drawText(tooltipFontId, tipX + PAD, textY, lb, foregroundBlack, EpdFontFamily::REGULAR);
    }
    textY += tlh;
  }
  if (tipPageCount_ > 1) {
    char marker[12];
    snprintf(marker, sizeof(marker), "%d/%d", tipPage_ + 1, tipPageCount_);
    renderer.drawText(tooltipFontId, tipX + tipW - PAD - renderer.getTextWidth(tooltipFontId, marker),
                      tipY + PAD + (rows - 1) * tlh, marker, foregroundBlack, EpdFontFamily::REGULAR);
  }
}
