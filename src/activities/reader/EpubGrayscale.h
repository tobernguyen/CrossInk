#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;
class Page;

namespace EpubGrayscale {
constexpr int GRAYSCALE_STRIP_ROWS = 80;

// bilingual (fork): drawn over the page text in each text-grayscale plane -- the translation
// tooltip -- so it is anti-aliased like the page and covers the page text under it. Set by src/bilingual.
using GrayscaleOverlayFn = void (*)(GfxRenderer& renderer, const Page& page);
inline GrayscaleOverlayFn grayscaleOverlay = nullptr;

// Preserves the live BW buffer and existing controller synchronization. False
// leaves the caller responsible for its existing BW-snapshot fallback.
bool runTiledGrayscalePass(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                           bool foregroundBlack, bool needsTextGrayscale, bool needsImageGrayscale, uint8_t* scratch,
                           size_t scratchSize, bool asyncRefreshPending);
}  // namespace EpubGrayscale
