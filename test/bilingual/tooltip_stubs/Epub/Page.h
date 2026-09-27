#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../EpdFontFamily.h"

// Just the slice of CrossInk's Page the tooltip reads.
enum PageElementTag : uint8_t { TAG_PageLine = 1 };

struct TextBlock {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
  uint16_t wordCount() const { return static_cast<uint16_t>(words.size()); }
  const char* wordText(uint16_t i) const { return words[i].c_str(); }
  int16_t wordXpos(uint16_t i) const { return xpos[i]; }
  EpdFontFamily::Style wordStyle(uint16_t) const { return EpdFontFamily::REGULAR; }
};

struct PageElement {
  int16_t xPos = 0;
  int16_t yPos = 0;
  virtual ~PageElement() = default;
  virtual PageElementTag getTag() const = 0;
};

struct PageLine final : PageElement {
  std::shared_ptr<TextBlock> block;
  const std::shared_ptr<TextBlock>& getBlock() const { return block; }
  PageElementTag getTag() const override { return TAG_PageLine; }
};

struct Page {
  std::vector<std::unique_ptr<PageElement>> elements;
};
