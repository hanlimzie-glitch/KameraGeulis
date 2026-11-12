#include <catch2/catch_test_macros.hpp>
#include <string>
#include "kgcore/strings.h"

using kgcore::Language;
using kgcore::Strings;

TEST_CASE("every key has both non-empty id and en strings") {
  auto& s = Strings::Instance();
  for (size_t i = 0; Strings::kKeys[i] != nullptr; ++i) {
    s.SetLanguage(Language::kId);
    std::string id = s.Get(Strings::kKeys[i]);
    s.SetLanguage(Language::kEn);
    std::string en = s.Get(Strings::kKeys[i]);
    REQUIRE_FALSE(id.empty());
    REQUIRE_FALSE(en.empty());
    REQUIRE(id != Strings::kKeys[i]);  // must be a real string, not a fallback
    REQUIRE(en != Strings::kKeys[i]);
  }
  s.SetLanguage(Language::kId);
}

TEST_CASE("unknown key falls back to the key itself") {
  auto& s = Strings::Instance();
  s.SetLanguage(Language::kEn);
  REQUIRE(s.Get("no.such.key") == "no.such.key");
  s.SetLanguage(Language::kId);
}

TEST_CASE("indonesian and english differ for user-facing strings") {
  auto& s = Strings::Instance();
  s.SetLanguage(Language::kId);
  std::string id = s.Get("skin.smoothing");
  s.SetLanguage(Language::kEn);
  std::string en = s.Get("skin.smoothing");
  REQUIRE(id != en);
  s.SetLanguage(Language::kId);
}
