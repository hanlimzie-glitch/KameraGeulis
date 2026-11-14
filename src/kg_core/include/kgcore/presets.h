#pragma once
// Preset definisi efek KameraGeulis (M2). Header-only agar bisa dipakai
// kg_app (Windows) dan diuji kg_tests (Linux). Nilai preset menentukan
// efek global (kulit) yang didukung engine GPUPixel — efek landmark
// TIDAK termasuk (belum tersedia, lihat docs/LANDMARKS.md §8).
#include <string>
#include <string_view>

namespace kgcore {
struct PresetDef {
  const char* name;    // nama internal + ditampilkan (bukan diterjemahkan)
  float smoothing;     // [0,1]
  float whitening;     // [0,1]
};

// M2i (3d): nilai diturunkan agar default terlihat natural - smoothing kini
// juga punya guided-filter sendiri (terukur), whitening cukup angkat luma
// ringan (chroma dipertahankan oleh komposit M2i).
inline constexpr PresetDef kPresets[] = {
    {"Natural",   0.18f, 0.06f},
    {"Soft",      0.30f, 0.12f},
    {"Glam",      0.48f, 0.20f},
    {"ClearSkin", 0.38f, 0.15f},
};

// nullptr bila nama tidak dikenal. Nama "Custom" disengaja bukan preset.
inline const PresetDef* FindPreset(std::string_view name) {
  for (const auto& p : kPresets) {
    if (name == p.name) return &p;
  }
  return nullptr;
}

inline bool IsCustomPreset(std::string_view name) { return name == "Custom"; }
}  // namespace kgcore
