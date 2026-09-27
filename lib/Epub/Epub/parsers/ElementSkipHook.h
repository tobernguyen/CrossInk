#pragma once

// bilingual (fork): lets the app hide elements from chapter layout -- the translated paragraphs of
// a bilingual book in tooltip mode -- without lib/Epub depending on src/. Set by src/bilingual.
namespace epub_hooks {

using SkipElementFn = bool (*)(const char* const* atts);
inline SkipElementFn skipElement = nullptr;

}  // namespace epub_hooks
