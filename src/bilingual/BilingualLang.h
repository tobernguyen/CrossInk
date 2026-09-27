#pragma once

#include <cstddef>
#include <cstring>
#include <string>

// Language rules for Calibre-style bilingual EPUBs (bookfere Ebook Translator and similar), where each
// translated paragraph sits after its original as <p lang="vi">. Hardware-free so the host tests
// exercise the production rule.
namespace bilingual {

// Lowercased primary subtag of a BCP 47 tag: "vi-VN" -> "vi", "EN" -> "en".
inline std::string primaryLang(const char* tag) {
  std::string out;
  if (!tag) return out;
  for (; *tag && *tag != '-' && *tag != '_'; tag++) {
    const char c = *tag;
    out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  }
  return out;
}

// Content is a translation iff it carries a language tag whose primary subtag differs from the
// book's (dc:language) primary language. Unknown book language: nothing is a translation.
inline bool isTranslationLang(const char* lang, const std::string& bookPrimary) {
  if (!lang || !*lang || bookPrimary.empty()) return false;
  return primaryLang(lang) != bookPrimary;
}

// The value of lang= / xml:lang= in an expat attribute list, or nullptr.
inline const char* langAttribute(const char* const* atts) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (std::strcmp(atts[i], "lang") == 0 || std::strcmp(atts[i], "xml:lang") == 0) return atts[i + 1];
  }
  return nullptr;
}

// Streams chapter HTML and counts block-level elements (p, div, li, h1-h6, blockquote) tagged with
// a translation language. Block-level only, so a monolingual book with an inline foreign phrase
// (<span lang="fr">) is not mistaken for a translated one.
class TranslationScanner {
 public:
  explicit TranslationScanner(std::string bookPrimary) : book_(std::move(bookPrimary)) {}

  void feed(const char* data, const size_t len) {
    for (size_t i = 0; i < len; i++) {
      const char c = data[i];
      if (!inTag_) {
        if (c == '<') {
          inTag_ = true;
          tag_.clear();
        }
        continue;
      }
      if (c == '>') {
        inTag_ = false;
        examine();
        continue;
      }
      if (tag_.size() < kMaxTag) tag_ += c;
    }
  }

  int count() const { return count_; }

 private:
  static constexpr size_t kMaxTag = 256;  // attributes past this are ignored; lang comes early in practice

  static bool isBlock(const std::string& name) {
    return name == "p" || name == "div" || name == "li" || name == "blockquote" ||
           (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6');
  }

  void examine() {
    if (tag_.empty() || tag_[0] == '/' || tag_[0] == '!' || tag_[0] == '?') return;
    size_t end = 0;
    while (end < tag_.size() && tag_[end] != ' ' && tag_[end] != '\t' && tag_[end] != '\n' && tag_[end] != '\r' &&
           tag_[end] != '/') {
      end++;
    }
    std::string name = primaryLang(tag_.substr(0, end).c_str());  // lowercases; tag names have no '-'
    if (!isBlock(name)) return;
    for (size_t at = tag_.find("lang=", end); at != std::string::npos; at = tag_.find("lang=", at + 5)) {
      const char before = tag_[at - 1];
      if (before != ' ' && before != ':' && before != '\t' && before != '\n') continue;
      const size_t q = at + 5;
      if (q >= tag_.size() || (tag_[q] != '"' && tag_[q] != '\'')) continue;
      const size_t close = tag_.find(tag_[q], q + 1);
      if (close == std::string::npos) return;
      if (isTranslationLang(tag_.substr(q + 1, close - q - 1).c_str(), book_)) count_++;
      return;
    }
  }

  std::string book_;
  std::string tag_;
  bool inTag_ = false;
  int count_ = 0;
};

}  // namespace bilingual
