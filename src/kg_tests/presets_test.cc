#include <catch2/catch_test_macros.hpp>
#include "kgcore/presets.h"

using namespace kgcore;

TEST_CASE("presets: Natural/Soft/Glam/ClearSkin dikenal dan monoton", "[presets]") {
  const PresetDef* natural = FindPreset("Natural");
  const PresetDef* soft = FindPreset("Soft");
  const PresetDef* glam = FindPreset("Glam");
  const PresetDef* clearskin = FindPreset("ClearSkin");
  REQUIRE(natural != nullptr);
  REQUIRE(soft != nullptr);
  REQUIRE(glam != nullptr);
  REQUIRE(clearskin != nullptr);
  REQUIRE(glam->smoothing > natural->smoothing);
  REQUIRE(glam->whitening > natural->whitening);
  REQUIRE(soft->smoothing > natural->smoothing);
  for (const PresetDef* p : {natural, soft, glam, clearskin}) {
    REQUIRE(p->smoothing >= 0.0f);
    REQUIRE(p->smoothing <= 1.0f);
    REQUIRE(p->whitening >= 0.0f);
    REQUIRE(p->whitening <= 1.0f);
  }
}

TEST_CASE("presets: nama tidak dikenal / Custom bukan preset", "[presets]") {
  REQUIRE(FindPreset("custom") == nullptr);   // case-sensitive disengaja
  REQUIRE(FindPreset("Natur") == nullptr);
  REQUIRE(FindPreset("") == nullptr);
  REQUIRE(FindPreset("Custom") == nullptr);
  REQUIRE(IsCustomPreset("Custom"));
  REQUIRE_FALSE(IsCustomPreset("Natural"));
}
