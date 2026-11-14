#pragma once
#include <string>
#include <unordered_map>

namespace kgcore {
// UI string table (Indonesian default, English fallback). Table lives in
// strings.cc so it travels with the binary — no external files to lose.
enum class Language { kId, kEn };

class Strings {
 public:
  static Strings& Instance();
  void SetLanguage(Language lang);
  Language GetLanguage() const { return lang_; }
  // Returns the localized string for `key`; falls back to English, then to
  // the key itself (never crashes, never empty).
  std::string Get(const char* key) const;
  static const char* kKeys[];  // sentinel-terminated list of all keys (tests)

 private:
  Strings() = default;
  Language lang_ = Language::kId;
  std::unordered_map<std::string, std::pair<std::string, std::string>> table_;
};
}  // namespace kgcore
