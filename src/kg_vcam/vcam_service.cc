#include "vcam_service.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "kgcore/log.h"

namespace kgcore {
namespace {
using clock_type = std::chrono::steady_clock;

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             clock_type::now().time_since_epoch())
      .count();
}
}  // namespace

VcamService::VcamService(std::unique_ptr<IVirtualCameraSink> sink)
    : sink_(std::move(sink)) {
  RefreshRegistration();
  worker_ = std::thread(&VcamService::WorkerLoop, this);
}

VcamService::~VcamService() { Shutdown(); }

void VcamService::Shutdown() {
  stop_requested_.store(true);
  {
    std::lock_guard<std::mutex> lk(mu_);
    cv_.notify_all();
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  if (sink_ && sink_->IsOpen()) {
    sink_->Close();
  }
}

bool VcamService::RefreshRegistration() {
  if (!sink_) return false;
  std::string detail;
  const bool reg = sink_->IsRegistered(&detail);
  std::lock_guard<std::mutex> lk(mu_);
  const bool changed = (reg != stats_.registered) || (detail != reg_detail_);
  stats_.registered = reg;
  reg_detail_ = detail;
  if (changed) {
    Log("vcam", "status registrasi [%s]: %s (%s)", sink_->BackendName(),
        reg ? "TERDAFTAR" : "BELUM TERDAFTAR", detail.c_str());
  }
  prev_registered_ = reg;
  return reg;
}

void VcamService::SetEnabled(bool enabled, const VcamConfig& cfg) {
  VcamConfig norm = cfg;
  NormalizeVcamConfig(&norm);
  std::lock_guard<std::mutex> lk(mu_);
  const bool was = want_enabled_.exchange(enabled);
  const bool dim_changed =
      (norm.width != cfg_.width || norm.height != cfg_.height ||
       norm.fps != cfg_.fps);
  cfg_ = norm;
  if (dim_changed || was != enabled) {
    cfg_dirty_ = true;
  }
  if (enabled && !was) {
    stats_.last_error.clear();
  }
  cv_.notify_all();
}

void VcamService::UpdateConfig(const VcamConfig& cfg) {
  VcamConfig norm = cfg;
  NormalizeVcamConfig(&norm);
  std::lock_guard<std::mutex> lk(mu_);
  const bool dim_changed =
      (norm.width != cfg_.width || norm.height != cfg_.height ||
       norm.fps != cfg_.fps);
  cfg_ = norm;
  if (dim_changed) {
    cfg_dirty_ = true;
    cv_.notify_all();
  }
}

void VcamService::SubmitFrame(const uint8_t* rgba_top_down, int width,
                              int height, int64_t capture_ts_us,
                              float readback_ms) {
  if (!rgba_top_down || width <= 0 || height <= 0 || !want_enabled_.load()) {
    return;
  }
  const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  std::lock_guard<std::mutex> lk(mu_);
  if (has_pending_) {
    ++stats_.frames_dropped;
  }
  pending_rgba_.assign(rgba_top_down, rgba_top_down + bytes);
  pending_w_ = width;
  pending_h_ = height;
  pending_ts_us_ = capture_ts_us > 0 ? capture_ts_us : NowUs();
  last_readback_ms_ = readback_ms;
  has_pending_ = true;
  cv_.notify_one();
}

VcamStats VcamService::GetStats() const {
  std::lock_guard<std::mutex> lk(mu_);
  VcamStats out = stats_;
  out.pbo_busy_skips = pbo_busy_skips_.load(std::memory_order_relaxed);
  out.readback_ms = last_readback_ms_;
  const auto info = ComputeVcamStatus(
      out.registered, out.active, out.consumer_count, !out.last_error.empty());
  out.state = info.state;
  return out;
}

std::string VcamService::RegistrationDetail() const {
  std::lock_guard<std::mutex> lk(mu_);
  return reg_detail_;
}

void VcamService::WorkerLoop() {
  VcamFramePacer pacer(30);
  std::vector<uint8_t> send_rgba;
  std::vector<uint8_t> converted;
  VcamConfig active_cfg{};
  int64_t last_reg_check_us = 0;
  int64_t fps_win_start_us = NowUs();
  uint64_t fps_win_frames = 0;
  double worker_ms_ema = 0.0;
  double latency_ms_ema = 0.0;
  int prev_consumers = -1;
  uint64_t last_log_sent = 0;
  int64_t last_log_us = NowUs();

  while (!stop_requested_.load()) {
    bool enabled = false;
    VcamConfig target_cfg{};
    bool dirty = false;
    std::vector<uint8_t> new_rgba;
    int new_w = 0, new_h = 0;
    int64_t new_ts = 0;
    bool got_new = false;

    {
      std::unique_lock<std::mutex> lk(mu_);
      enabled = want_enabled_.load();
      if (!enabled && !sink_->IsOpen()) {
        cv_.wait_for(lk, std::chrono::milliseconds(250), [&] {
          return stop_requested_.load() || want_enabled_.load();
        });
      }
      if (stop_requested_.load()) break;
      enabled = want_enabled_.load();
      target_cfg = cfg_;
      dirty = cfg_dirty_;
      cfg_dirty_ = false;
      if (has_pending_) {
        new_rgba.swap(pending_rgba_);
        new_w = pending_w_;
        new_h = pending_h_;
        new_ts = pending_ts_us_;
        has_pending_ = false;
        got_new = true;
      }
    }

    const int64_t now_us = NowUs();

    if (!enabled) {
      if (sink_->IsOpen()) {
        sink_->Close();
        pacer.Reset(target_cfg.fps);
        prev_consumers = -1;
        Log("vcam", "kamera virtual dimatikan (sent=%llu repeat=%llu drop=%llu)",
            static_cast<unsigned long long>(pacer.FramesSent()),
            static_cast<unsigned long long>(pacer.FramesRepeated()),
            static_cast<unsigned long long>(stats_.frames_dropped));
      }
      {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.active = false;
        stats_.consumer_count = -1;
        stats_.output_fps = 0.0f;
      }
      if (now_us - last_reg_check_us >= 2000000LL) {
        last_reg_check_us = now_us;
        RefreshRegistration();
      }
      continue;
    }

    // Reopen sink bila belum buka atau resolusi/FPS berubah.
    if (!sink_->IsOpen() || dirty || target_cfg.width != active_cfg.width ||
        target_cfg.height != active_cfg.height || target_cfg.fps != active_cfg.fps) {
      if (sink_->IsOpen()) {
        sink_->Close();
      }
      std::string reg_detail;
      const bool reg = sink_->IsRegistered(&reg_detail);
      std::string open_err;
      const bool ok = reg && sink_->Open(target_cfg.width, target_cfg.height,
                                         static_cast<float>(target_cfg.fps), &open_err);
      if (!reg) {
        open_err = reg_detail.empty() ? "belum terdaftar" : reg_detail;
      }
      active_cfg = target_cfg;
      pacer.Reset(active_cfg.fps);
      fps_win_start_us = now_us;
      fps_win_frames = 0;
      prev_consumers = -1;
      {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.registered = reg;
        reg_detail_ = reg_detail;
        stats_.active = ok;
        stats_.last_error = ok ? "" : open_err;
        stats_.consumer_count = ok ? sink_->ConsumerCount() : -1;
      }
      if (!ok) {
        Log("vcam", "gagal membuka sink %dx%d@%d: %s",
            target_cfg.width, target_cfg.height, target_cfg.fps,
            open_err.c_str());
        // Matikan permintaan agar tidak terus spam retry di loop.
        want_enabled_.store(false);
        continue;
      }
      Log("vcam", "kamera virtual aktif: %dx%d @ %d fps (mirror=%d, backend=%s)",
          active_cfg.width, active_cfg.height, active_cfg.fps,
          active_cfg.mirror ? 1 : 0, sink_->BackendName());
    }

    if (got_new) {
      if (pacer.SubmitFrame(new_rgba.data(), new_w, new_h, new_ts)) {
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.frames_dropped;
      }
    }

    int sw = 0, sh = 0;
    int64_t pts_us = 0, cap_ts_us = 0;
    bool is_repeat = false;
    if (pacer.PollFrame(now_us, &send_rgba, &sw, &sh, &pts_us, &cap_ts_us,
                        &is_repeat)) {
      const auto t0 = clock_type::now();
      const bool ok_conv = ConvertRgbaToVcamFormat(
          send_rgba.data(), sw, sh, sw * 4, active_cfg.width, active_cfg.height,
          sink_->PreferredFormat(), /*flip_vertical=*/false,
          /*mirror_horizontal=*/target_cfg.mirror, &converted);
      bool ok_send = false;
      if (ok_conv) {
        ok_send = sink_->SendFrame(converted.data(), converted.size(),
                                   active_cfg.width, active_cfg.height, pts_us);
      }
      const auto t1 = clock_type::now();
      const double step_ms =
          std::chrono::duration<double, std::milli>(t1 - t0).count();
      const double lat_ms =
          static_cast<double>(std::max<int64_t>(0, NowUs() - cap_ts_us)) / 1000.0;
      worker_ms_ema = worker_ms_ema == 0.0 ? step_ms : worker_ms_ema * 0.9 + step_ms * 0.1;
      latency_ms_ema = latency_ms_ema == 0.0 ? lat_ms : latency_ms_ema * 0.9 + lat_ms * 0.1;
      ++fps_win_frames;

      const int consumers = sink_->ConsumerCount();
      if (consumers != prev_consumers) {
        Log("vcam", "konsumen berubah: %d -> %d", prev_consumers, consumers);
        prev_consumers = consumers;
      }

      const int64_t dt_fps_us = now_us - fps_win_start_us;
      float out_fps = 0.0f;
      if (dt_fps_us >= 1000000LL) {
        out_fps = static_cast<float>(fps_win_frames) * 1000000.0f /
                  static_cast<float>(dt_fps_us);
        fps_win_start_us = now_us;
        fps_win_frames = 0;
      }

      {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.active = ok_send;
        if (ok_send) {
          ++stats_.frames_sent;
          if (is_repeat) ++stats_.frames_repeated;
        }
        stats_.consumer_count = consumers;
        if (out_fps > 0.0f) stats_.output_fps = out_fps;
        stats_.worker_ms = static_cast<float>(worker_ms_ema);
        stats_.latency_ms = static_cast<float>(latency_ms_ema);
      }

      if (now_us - last_log_us >= 2000000LL) {
        const auto snap = GetStats();
        const uint64_t dsent = snap.frames_sent - last_log_sent;
        const float sec = static_cast<float>(now_us - last_log_us) / 1000000.0f;
        last_log_us = now_us;
        last_log_sent = snap.frames_sent;
        Log("vcam",
            "stream %dx%d@%d | fps=%.1f | sent=%llu (+%llu) repeat=%llu drop=%llu pbo_skip=%llu | "
            "readback=%.2fms worker=%.2fms lat=%.1fms | consumers=%d",
            active_cfg.width, active_cfg.height, active_cfg.fps,
            sec > 0.0f ? static_cast<float>(dsent) / sec : snap.output_fps,
            static_cast<unsigned long long>(snap.frames_sent),
            static_cast<unsigned long long>(dsent),
            static_cast<unsigned long long>(snap.frames_repeated),
            static_cast<unsigned long long>(snap.frames_dropped),
            static_cast<unsigned long long>(snap.pbo_busy_skips),
            snap.readback_ms, snap.worker_ms, snap.latency_ms,
            snap.consumer_count);
      }
    } else {
      // Tidur singkat sampai mendekati tick pacer atau ada frame baru.
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait_for(lk, std::chrono::milliseconds(4), [&] {
        return stop_requested_.load() || !want_enabled_.load() || has_pending_;
      });
    }
  }
}

}  // namespace kgcore
