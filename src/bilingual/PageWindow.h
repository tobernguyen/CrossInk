#pragma once

#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

// CrossInk pages don't record which chapter paragraphs they show, so the tooltip finds the page's
// stretch of the chapter by content: while streaming the chapter's (original, translation) pairs,
// keep only the entries around the run of pairs whose sentence keys appear on the page. Bounded
// memory, no section-cache format change. Hardware-free for the host tests.
namespace bilingual {

template <typename Entry>
class PageWindow {
 public:
  // pageAnchors: how many page keys can match; the scan stops once most have been seen.
  explicit PageWindow(const int pageAnchors, const size_t maxEntries = 240) : anchors_(pageAnchors), max_(maxEntries) {}

  // Feed one pair's entries in chapter order with how many page keys it matched. Returns false once
  // the page's run is over, so the caller can stop parsing.
  bool addPair(std::vector<Entry>&& entries, const int matchedKeys) {
    if (!open_) {
      if (matchedKeys == 0) {
        prev_ = std::move(entries);  // margin: holds the page-top sentence that began on the previous page
        return true;
      }
      open_ = true;
      window_ = std::move(prev_);
      append(std::move(entries), matchedKeys);
      return true;
    }
    if (matchedKeys > 0) {
      if (misses_ >= kResetGap) {
        // The window opened on an early chance match ("Yes."), far from the page: restart at the
        // pair before this one.
        window_.erase(window_.begin(), window_.begin() + static_cast<std::ptrdiff_t>(lastPairStart_));
        matched_ = 0;
      }
      append(std::move(entries), matchedKeys);
      return true;
    }
    misses_++;
    append(std::move(entries), 0);
    return !(misses_ > kCloseGap && matched_ * 2 >= anchors_);
  }

  std::vector<Entry> take() { return std::move(window_); }

 private:
  static constexpr int kResetGap = 20;  // pairs without a match before a new match restarts the window
  static constexpr int kCloseGap = 2;   // trailing misses after most anchors matched: the page is behind us

  void append(std::vector<Entry>&& entries, const int matchedKeys) {
    if (matchedKeys > 0) misses_ = 0;
    matched_ += matchedKeys;
    lastPairStart_ = window_.size();
    window_.insert(window_.end(), std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
    if (window_.size() > max_) {
      const size_t drop = window_.size() - max_;  // keep the newest entries: the page is ahead, not behind
      window_.erase(window_.begin(), window_.begin() + static_cast<std::ptrdiff_t>(drop));
      lastPairStart_ = lastPairStart_ > drop ? lastPairStart_ - drop : 0;
    }
  }

  int anchors_;
  size_t max_;
  std::vector<Entry> prev_;
  std::vector<Entry> window_;
  size_t lastPairStart_ = 0;
  bool open_ = false;
  int misses_ = 0;
  int matched_ = 0;
};

}  // namespace bilingual
