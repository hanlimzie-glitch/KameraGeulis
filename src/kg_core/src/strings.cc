#include "kgcore/strings.h"
#include <memory>

namespace kgcore {
namespace {
using Table = std::unordered_map<std::string, std::pair<std::string, std::string>>;
const Table& DefaultTable() {
  static const Table* kTable = new Table{
    {"app.title",            {"KameraGeulis", "KameraGeulis"}},
    {"tab.face",             {"Wajah", "Face"}},
    {"tab.skin",             {"Kulit", "Skin"}},
    {"tab.makeup",           {"Makeup", "Makeup"}},
    {"tab.presets",          {"Preset", "Presets"}},
    {"skin.smoothing",       {"Haluskan Kulit", "Skin Smoothing"}},
    {"skin.whitening",       {"Cerahkan", "Whitening"}},
    {"face.slim",            {"Tiruskan Wajah", "Face Slim"}},
    {"face.bigeyes",         {"Perbesar Mata", "Big Eyes"}},
    {"vcam.toggle",          {"Kamera Virtual", "Virtual Camera"}},
    {"vcam.need_register",   {"Kamera virtual butuh registrasi 1x sebagai admin (softcam/DirectShow).",
                              "Virtual camera requires a one-time admin registration (softcam/DirectShow)."}},
    {"vcam.unavailable",     {"Kamera virtual tidak tersedia (softcam.dll tidak ditemukan / belum terdaftar).",
                              "Virtual camera unavailable (softcam.dll not found / not registered)."}},
    {"landmark.unavailable", {"Efek berbasis landmark tidak tersedia pada build ini (model belum ada).",
                              "Landmark-based effects unavailable on this build (models missing)."}},
    {"landmark.status",      {"Status landmark", "Landmark status"}},
    {"landmark.active",      {"Aktif (ORT CPU)", "Active (ORT CPU)"}},
    {"landmark.inactive",    {"Nonaktif — efek landmark dimatikan", "Inactive — landmark effects disabled"}},
    {"face.not_detected",    {"Wajah tidak terdeteksi", "Face not detected"}},
    {"common.snapshot",      {"Ambil Snapshot", "Snapshot"}},
    {"common.mirror",        {"Cermin (mirror)", "Mirror"}},
    {"common.quit",          {"Keluar", "Quit"}},
    {"status.ready",         {"Siap", "Ready"}},
    {"status.no_camera",     {"Kamera tidak ditemukan", "No camera found"}},
    {"status.starting",      {"Memulai kamera…", "Starting camera…"}},
  };
  return *kTable;
}
}  // namespace

Strings& Strings::Instance() {
  static Strings instance;
  if (instance.table_.empty()) instance.table_ = DefaultTable();
  return instance;
}

void Strings::SetLanguage(Language lang) { lang_ = lang; }

std::string Strings::Get(const char* key) const {
  const std::string k(key);
  auto it = table_.find(k);
  if (it == table_.end()) return k;
  return lang_ == Language::kId ? it->second.first : it->second.second;
}

const char* Strings::kKeys[] = {
    "app.title", "tab.face", "tab.skin", "tab.makeup", "tab.presets",
    "skin.smoothing", "skin.whitening", "face.slim", "face.bigeyes",
    "vcam.toggle", "vcam.need_register", "vcam.unavailable",
    "landmark.unavailable", "landmark.status", "landmark.active", "landmark.inactive",
    "face.not_detected", "common.snapshot", "common.mirror", "common.quit",
    "status.ready", "status.no_camera", "status.starting", nullptr};
}  // namespace kgcore
