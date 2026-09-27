#pragma once
#include <cstring>
#include <string>

#include "EpdFontFamily.h"

// Fixed-pitch fake: 10 px per byte, 32 px lines. Records the text it draws.
class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  std::string drawn;
  RenderMode getRenderMode() const { return BW; }
  bool isFontCacheScanning() const { return false; }
  int getTextWidth(int, const char* t, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return 10 * static_cast<int>(strlen(t));
  }
  int getLineHeight(int) const { return 32; }
  int getFontAscenderSize(int) const { return 24; }
  void drawLine(int, int, int, int, bool) const {}
  void fillRect(int, int, int, int, bool) const {}
  void drawRoundedRect(int, int, int, int, int, int, bool) const {}
  void drawText(int, int, int, const char* t, bool, EpdFontFamily::Style) { drawn += std::string(t) + " "; }
};
