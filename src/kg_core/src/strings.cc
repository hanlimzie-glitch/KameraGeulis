#include "kgcore/strings.h"
#include <memory>

namespace kgcore {
namespace {
using Table = std::unordered_map<std::string, std::pair<std::string, std::string>>;
const Table& DefaultTable() {
  static const Table* kTable = new Table{
    {"app.title",            {"KameraGeulis", "KameraGeulis"}},
    {"tab.skin",             {"Kulit", "Skin"}},
    {"tab.face",             {"Wajah", "Face"}},
    {"tab.makeup",           {"Makeup", "Makeup"}},
    {"tab.presets",          {"Preset", "Presets"}},
    {"tab.camera",           {"Kamera", "Camera"}},
    {"tab.about",            {"Info", "Info"}},
    {"skin.smoothing",       {"Haluskan Kulit", "Skin Smoothing"}},
    {"skin.whitening",       {"Cerahkan Kulit", "Skin Whitening"}},
    {"skin.only",            {"Terapkan hanya pada kulit wajah", "Apply to face skin only"}},
    {"skin.only_tip",
     {"Efek Haluskan/Cerahkan hanya berlaku di dalam mask kulit wajah (oval dikurangi mata, alis, bibir, lubang hidung + deteksi warna kulit). Tekan F6 untuk melihat mask sebagai lapisan merah. Saat wajah tidak terdeteksi, efek memudar ke nol.",
      "Smooth/Whitening apply only inside the face skin mask (oval minus eyes, brows, lips, nostrils + skin-color check). Press F6 to see the mask as a red overlay. When no face is detected the effect fades to zero."}},
    {"skin.only_off_note",
     {"Peralatan lama: efek berlaku ke SELURUH frame (dinding/baju ikut terpengaruh).",
      "Legacy mode: the effect applies to the WHOLE frame (walls/clothes included)."}},
    {"camera.black_frames",
     {"Gambar nyaris hitam beberapa detik - perangkat mungkin salah (atau tertutup). Coba perangkat lain di tab Kamera; perangkat terakhir dipakai akan jadi default berikutnya.",
      "Picture near-black for several seconds - wrong device (or covered lens). Try another device in the Camera tab; the last used device becomes the next default."}},
    {"skin.coming_soon_note",
     {"Efek korektif wajah/makeup sedang dibangun - lihat tab masing-masing.",
      "Corrective face/makeup effects are being built - see their tabs."}},
    {"face.slim",            {"Tiruskan Wajah", "Face Slim"}},
    {"face.bigeyes",         {"Perbesar Mata", "Big Eyes"}},
    {"makeup.lipstick",      {"Lipstik", "Lipstick"}},
    {"makeup.blusher",       {"Blush On", "Blusher"}},
    {"makeup.foundation",    {"Foundation", "Foundation"}},
    {"makeup.eyeliner",      {"Eyeliner", "Eyeliner"}},
    {"makeup.eyeshadow",     {"Eyeshadow", "Eyeshadow"}},
    {"makeup.browtint",      {"Brow Tint", "Brow Tint"}},
    {"common.coming_soon",   {"Segera hadir", "Coming soon"}},
    {"common.coming_soon_tip",
     {"Efek ini menunggu integrasi landmark yang terverifikasi - lihat docs/LANDMARKS.md S.8. UI sudah final.",
      "This effect waits on a verified landmark integration - see docs/LANDMARKS.md S.8. The UI is final."}},
    {"common.reset",         {"Reset", "Reset"}},
    {"common.save",          {"Simpan Pengaturan", "Save Settings"}},
    {"common.load",          {"Muat Pengaturan", "Load Settings"}},
    {"common.mirror",        {"Cermin (mirror)", "Mirror"}},
    {"common.snapshot",      {"Ambil Snapshot", "Snapshot"}},
    {"common.quit",          {"Keluar", "Quit"}},
    {"common.language",      {"Bahasa", "Language"}},
    {"compare.hold",         {"Tahan untuk Bandingkan", "Hold to Compare"}},
    {"compare.active",       {"Mode bandingkan - efek dimatikan sementara", "Compare mode - effects bypassed"}},
    {"presets.reset",        {"Kembalikan Default", "Restore Defaults"}},
    {"presets.custom",       {"Kustom", "Custom"}},
    {"presets.active",       {"Preset aktif", "Active preset"}},
    {"presets.saved",        {"Pengaturan tersimpan.", "Settings saved."}},
    {"presets.loaded",       {"Pengaturan dimuat.", "Settings loaded."}},
    {"presets.save_failed",  {"Gagal menyimpan pengaturan.", "Failed to save settings."}},
    {"presets.load_failed",  {"Gagal memuat pengaturan.", "Failed to load settings."}},
    {"camera.device",        {"Perangkat Kamera", "Camera Device"}},
    {"camera.resolution",    {"Resolusi", "Resolution"}},
    {"camera.fps",           {"Frame per Detik", "Frames per Second"}},
    {"camera.apply",         {"Terapkan", "Apply"}},
    {"camera.refresh",       {"Segarkan Daftar", "Refresh List"}},
    {"camera.none",          {"Tidak ada kamera ditemukan.", "No camera found."}},
    {"camera.applied",       {"Kamera diterapkan.", "Camera applied."}},
    {"camera.apply_failed",  {"Gagal menerapkan kamera - memakai mode sebelumnya.", "Failed to apply camera - keeping previous mode."}},
    {"camera.no_modes",      {"Mode tidak terdeteksi - memakai mode bawaan.", "No modes detected - using built-in mode."}},
    {"camera.current",       {"Aktual", "Current"}},
    {"vcam.toggle",          {"Kamera Virtual", "Virtual Camera"}},
    {"vcam.need_register",   {"Kamera virtual butuh registrasi 1x sebagai admin (softcam/DirectShow).",
                              "Virtual camera requires a one-time admin registration (softcam/DirectShow)."}},
    {"vcam.unavailable",     {"Kamera virtual tidak tersedia (softcam.dll tidak ditemukan / belum terdaftar).",
                              "Virtual camera unavailable (softcam.dll not found / not registered)."}},
    {"vcam.coming_soon",     {"Segera hadir (M3)", "Coming soon (M3)"}},
    {"landmark.unavailable", {"Efek berbasis landmark tidak tersedia pada build ini (model belum ada).",
                              "Landmark-based effects unavailable on this build (models missing)."}},
    {"landmark.status",      {"Status landmark", "Landmark status"}},
    {"landmark.active",      {"Aktif (ORT CPU)", "Active (ORT CPU)"}},
    {"landmark.inactive",    {"Nonaktif - efek landmark dimatikan", "Inactive - landmark effects disabled"}},
    {"face.not_detected",    {"Wajah tidak terdeteksi", "Face not detected"}},
    {"status.camera",        {"Kamera", "Camera"}},
    {"status.ready",         {"Siap", "Ready"}},
    {"status.no_camera",     {"Kamera tidak ditemukan", "No camera found"}},
    {"status.starting",      {"Memulai kamera...", "Starting camera..."}},
    {"debug.overlay",        {"Overlay Debug (F3)", "Debug Overlay (F3)"}},
    {"debug.note",           {"Menggambar 468 titik landmark, bbox wajah, FPS dan timings per tahap di atas preview. Untuk verifikasi akurasi sebelum efek dibangun di atasnya.",
                              "Draws the 468 landmark points, face bbox, FPS and per-stage timings over the preview. For verifying accuracy before effects are built on top."}},
    {"debug.capture_ms",     {"Capture", "Capture"}},
    {"debug.detect_ms",      {"Deteksi wajah", "Face detection"}},
    {"debug.mesh_ms",        {"Mesh 468", "Mesh 468"}},
    {"debug.track_ms",       {"Track (total)", "Track (total)"}},
    {"debug.render_ms",      {"Render", "Render"}},
    {"debug.since_detect",   {"Frame sejak deteksi", "Frames since detect"}},
    {"debug.unverified",     {"Belum diuji di Windows nyata (GPU/webcam/ORT performa).",
                              "Not yet tested on real Windows (GPU/webcam/ORT perf)."}},
    {"about.license_note",   {"Tanpa file LICENSE - keputusan pemilik repo (lihat STATUS.md). Model: MediaPipe Apache-2.0 via tf2onnx, lihat docs/LANDMARKS.md.",
                              "No LICENSE file yet - repo owner decision (see STATUS.md). Models: MediaPipe Apache-2.0 via tf2onnx, see docs/LANDMARKS.md."}},
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
    "app.title",
    "tab.skin", "tab.face", "tab.makeup", "tab.presets", "tab.camera", "tab.about",
    "skin.smoothing", "skin.whitening", "skin.only", "skin.only_tip",
    "skin.only_off_note", "skin.coming_soon_note",
    "face.slim", "face.bigeyes",
    "makeup.lipstick", "makeup.blusher", "makeup.foundation", "makeup.eyeliner",
    "makeup.eyeshadow", "makeup.browtint",
    "common.coming_soon", "common.coming_soon_tip", "common.reset", "common.save",
    "common.load", "common.mirror", "common.snapshot", "common.quit", "common.language",
    "compare.hold", "compare.active",
    "presets.reset", "presets.custom", "presets.active", "presets.saved",
    "presets.loaded", "presets.save_failed", "presets.load_failed",
    "camera.device", "camera.resolution", "camera.fps", "camera.apply",
    "camera.refresh", "camera.none", "camera.applied", "camera.apply_failed",
    "camera.no_modes", "camera.current", "camera.black_frames",
    "vcam.toggle", "vcam.need_register", "vcam.unavailable", "vcam.coming_soon",
    "landmark.unavailable", "landmark.status", "landmark.active", "landmark.inactive",
    "face.not_detected",
    "status.camera", "status.ready", "status.no_camera", "status.starting",
    "debug.overlay", "debug.note", "debug.capture_ms", "debug.detect_ms",
    "debug.mesh_ms", "debug.track_ms", "debug.render_ms", "debug.since_detect",
    "debug.unverified",
    "about.license_note",
    nullptr};
}  // namespace kgcore
