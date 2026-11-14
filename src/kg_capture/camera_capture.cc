#include "camera_capture.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfobjects.h>
#include <propvarutil.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "kgcore/frame_convert.h"
#include "kgcore/log.h"
#include "kgcore/reader_fallback.h"

namespace kgcore {
namespace {
constexpr char kTag[] = "capture";

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

template <typename T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

struct MfStartup {
  bool ok = false;
  MfStartup() { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)); }
  ~MfStartup() {
    if (ok) MFShutdown();
  }
};

// Enumerates video capture devices; returns count (devices must be freed by
// caller via SafeRelease each). nullptr on failure.
HRESULT EnumerateDevicesRaw(IMFActivate*** out_devices, UINT32* out_count) {
  *out_devices = nullptr;
  *out_count = 0;
  IMFAttributes* attrs = nullptr;
  HRESULT hr = MFCreateAttributes(&attrs, 1);
  if (FAILED(hr)) return hr;
  hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (SUCCEEDED(hr)) {
    hr = MFEnumDeviceSources(attrs, out_devices, out_count);
  }
  attrs->Release();
  return hr;
}

HRESULT SelectRgb32Output(IMFSourceReader* reader) {
  IMFMediaType* type = nullptr;
  HRESULT hr = MFCreateMediaType(&type);
  if (FAILED(hr)) return hr;
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);  // BGRA byte order
  hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                   nullptr, type);
  type->Release();
  return hr;
}

// Requests RGB32 output at the given size/rate. Returns true when the
// camera accepted it; false -> caller must fall back (SelectRgb32Output).
HRESULT SelectRgb32OutputAt(IMFSourceReader* reader, int width, int height,
                            int fps) {
  IMFMediaType* type = nullptr;
  HRESULT hr = MFCreateMediaType(&type);
  if (FAILED(hr)) return hr;
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);  // BGRA byte order
  type->SetUINT64(MF_MT_FRAME_SIZE,
                  (static_cast<UINT64>(static_cast<UINT32>(width)) << 32) |
                      static_cast<UINT32>(height));
  type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (fps > 0) {
    type->SetUINT64(MF_MT_FRAME_RATE,
                    (static_cast<UINT64>(static_cast<UINT32>(fps)) << 32) | 1u);
  }
  hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                   nullptr, type);
  type->Release();
  return hr;
}

// Human-readable subtype: GUID Data1 is either a fourcc (NV12, MJPG...) or a
// D3D format number (RGB32 = 22) - print fourcc chars when printable.
std::string SubtypeToString(int data1) {
  char c[4];
  bool printable = true;
  for (int i = 0; i < 4; ++i) {
    c[i] = static_cast<char>((data1 >> (8 * i)) & 0xFF);
    if (c[i] < 32 || c[i] > 126) printable = false;
  }
  char buf[32];
  if (printable) {
    std::snprintf(buf, sizeof(buf), "%.4s", c);
  } else {
    // Known D3D format numbers used by MF RGB GUIDs.
    switch (data1) {
      case 22: return "RGB32";
      case 21: return "ARGB32";
      case 20: return "RGB24";
      default:
        std::snprintf(buf, sizeof(buf), "fmt%u", static_cast<unsigned>(data1 & 0xFFFF));
        return buf;
    }
  }
  return buf;
}

std::string FriendlyName(IMFActivate* activate) {
  LPWSTR name = nullptr;
  UINT32 len = 0;
  if (SUCCEEDED(activate->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                             &name, &len)) && name) {
    std::wstring ws(name);
    CoTaskMemFree(name);
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n > 0) {
      std::string out(n, '\0');
      WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), n,
                          nullptr, nullptr);
      return out;
    }
  }
  return "Camera";
}
}  // namespace

std::vector<VideoDeviceInfo> CameraCapture::EnumerateDevices() {
  std::vector<VideoDeviceInfo> out;
  MfStartup mf;
  if (!mf.ok) return out;
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  HRESULT hr = EnumerateDevicesRaw(&devices, &count);
  if (FAILED(hr)) return out;
  out.reserve(count);
  for (UINT32 i = 0; i < count; ++i) {
    VideoDeviceInfo info;
    info.index = static_cast<int>(i);
    info.name = FriendlyName(devices[i]);
    out.push_back(std::move(info));
    SafeRelease(devices[i]);
  }
  CoTaskMemFree(devices);
  return out;
}

std::vector<VideoMode> CameraCapture::EnumerateModes(int device_index) {
  std::vector<VideoMode> modes;
  MfStartup mf;
  if (!mf.ok) return modes;
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  HRESULT hr = EnumerateDevicesRaw(&devices, &count);
  if (FAILED(hr) || static_cast<UINT32>(device_index) >= count) {
    if (devices) {
      for (UINT32 i = 0; i < count; ++i) SafeRelease(devices[i]);
      CoTaskMemFree(devices);
    }
    return modes;
  }
  IMFMediaSource* source = nullptr;
  hr = devices[device_index]->ActivateObject(IID_PPV_ARGS(&source));
  for (UINT32 i = 0; i < count; ++i) SafeRelease(devices[i]);
  CoTaskMemFree(devices);
  if (FAILED(hr)) return modes;

  IMFAttributes* reader_attrs = nullptr;
  MFCreateAttributes(&reader_attrs, 1);
  if (reader_attrs) {
    reader_attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  }
  IMFSourceReader* reader = nullptr;
  hr = MFCreateSourceReaderFromMediaSource(source, reader_attrs, &reader);
  SafeRelease(reader_attrs);
  if (FAILED(hr)) {
    source->Shutdown();
    SafeRelease(source);
    return modes;
  }
  for (DWORD type_index = 0;; ++type_index) {
    IMFMediaType* type = nullptr;
    hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                    type_index, &type);
    // End of the list is signalled by MF_E_NO_MORE_TYPES (an HRESULT error),
    // so ANY failure ends the enumeration — no constant needed.
    if (FAILED(hr)) break;
    GUID major{};
    if (SUCCEEDED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) &&
        IsEqualGUID(major, MFMediaType_Video)) {
      UINT32 w = 0, h = 0;
      if (SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) &&
          w > 0 && h > 0) {
        UINT32 num = 0, den = 0;
        int fps = 0;
        if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) &&
            den > 0) {
          fps = static_cast<int>((num + den / 2) / den);
        }
        VideoMode m{static_cast<int>(w), static_cast<int>(h), fps};
        const bool dup =
            std::any_of(modes.begin(), modes.end(), [&](const VideoMode& o) {
              return o.width == m.width && o.height == m.height && o.fps == m.fps;
            });
        if (!dup) modes.push_back(m);
      }
    }
    type->Release();
  }
  SafeRelease(reader);
  source->Shutdown();
  SafeRelease(source);
  std::sort(modes.begin(), modes.end(), [](const VideoMode& a, const VideoMode& b) {
    const int64_t pa = static_cast<int64_t>(a.width) * a.height;
    const int64_t pb = static_cast<int64_t>(b.width) * b.height;
    if (pa != pb) return pa > pb;
    return a.fps > b.fps;
  });
  return modes;
}

bool CameraCapture::Start(int device_index, int width, int height, int fps,
                          bool plain_attrs) {
  if (running_.load()) return true;
  plain_attrs_ = plain_attrs;

  // Preflight: enumerate devices synchronously so the UI can report "no
  // camera" immediately instead of failing silently on a worker thread.
  IMFActivate** devices = nullptr;
  UINT32 device_count = 0;
  HRESULT hr = EnumerateDevicesRaw(&devices, &device_count);
  bool any = SUCCEEDED(hr) && device_count > 0 &&
             device_index >= 0 &&
             static_cast<UINT32>(device_index) < device_count;
  if (!any) {
    if (SUCCEEDED(hr)) {
      SetError("no video capture device at index %d (%u found)", device_index,
               device_count);
    } else {
      SetError("MFEnumDeviceSources failed (0x%08lx)", static_cast<unsigned long>(hr));
    }
    if (devices) {
      for (UINT32 i = 0; i < device_count; ++i) SafeRelease(devices[i]);
      CoTaskMemFree(devices);
    }
    return false;
  }
  for (UINT32 i = 0; i < device_count; ++i) SafeRelease(devices[i]);
  CoTaskMemFree(devices);

  queue_ = std::make_unique<BoundedQueue<Frame>>(2);
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = CameraStats{};  // fresh diagnosis counters for this session
  }
  first_frame_seen_.store(false);
  open_attempt_.store(0);
  open_failure_count_.store(0);
  last_open_hr_.store(0);
  first_open_hr_.store(0);
  running_.store(true);
  thread_ = std::thread(&CameraCapture::CaptureThread, this, device_index, width,
                        height, fps);
  return true;
}

CameraCapture::CaptureStatus CameraCapture::GetStatus() const {
  CaptureStatus st;
  st.thread_active = running_.load();
  st.streaming = first_frame_seen_.load();
  st.failure_count = open_failure_count_.load();
  st.had_failure = st.failure_count > 0;
  st.hr = last_open_hr_.load();
  st.first_hr = first_open_hr_.load();
  st.attempt = open_attempt_.load();
  return st;
}

void CameraCapture::Stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
  queue_.reset();
}

bool CameraCapture::LatestFrame(Frame& out) {
  if (!queue_) return false;
  return queue_->TryPop(out);
}

void CameraCapture::SetError(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = buf;
  }
  Log(kTag, "%s", buf);
}

void CameraCapture::CaptureThread(int device_index, int req_width, int req_height,
                                  int req_fps) {
  MfStartup mf;
  if (!mf.ok) {
    open_failure_count_.fetch_add(1);
    last_open_hr_.store(static_cast<int64_t>(0x80004005));  // E_FAIL
    first_open_hr_.store(static_cast<int64_t>(0x80004005));
    SetError("MFStartup failed - kamera tidak bisa dimulai");
    running_.store(false);
    return;
  }

  // M2f: loop percobaan dengan backoff (1s -> 2s -> 4s -> 8s cap). Pembuatan
  // reader memakai tangga fallback atribut (a) penuh -> (b) basic -> (c) polos,
  // atau HANYA (c) bila --capture-plain. Setiap percobaan mencatat HRESULT.
  // "MF capture start: OK" hanya dicetak setelah reader jadi DAN frame
  // pertama diterima (lihat blok first-frame di bawah).
  const int64_t thread_start_ms = NowMs();
  int backoff_ms = 1000;
  while (running_.load()) {
    const unsigned attempt = open_attempt_.fetch_add(1) + 1;
    IMFSourceReader* reader = nullptr;
    IMFMediaSource* source = nullptr;
    IMFActivate* activate = nullptr;   // disimpan lintas tingkat (M2g)
    const char* last_stage = nullptr;  // tahap kegagalan terakhir di ladder
    int used_level = -1;
    int64_t fail_hr = 0;
    const char* fail_stage = nullptr;
    bool opened = false;
    // Negotiated mode (diisi saat berhasil dibuka; dipakai read loop).
    int width = 0, height = 0;
    int raw_stride = 0;  // SIGNED - sign is diagnosis data, never clamp it away
    int abs_stride = 0;
    int subtype_data1 = 0, fps_num = 0, fps_den = 0;

    // ---- satu percobaan buka (do{}while(0) pengganti goto) ----
    do {
      IMFActivate** devices = nullptr;
      UINT32 device_count = 0;
      auto release_devices = [&] {
        if (!devices) return;
        for (UINT32 i = 0; i < device_count; ++i) SafeRelease(devices[i]);
        CoTaskMemFree(devices);
        devices = nullptr;
      };
      const HRESULT hr_enum = EnumerateDevicesRaw(&devices, &device_count);
      if (FAILED(hr_enum) || device_count == 0 || device_index < 0 ||
          static_cast<UINT32>(device_index) >= device_count) {
        // Tidak ada device pada index: HRESULT_FROM_WIN32(ERROR_NOT_FOUND).
        fail_hr = FAILED(hr_enum)
                      ? static_cast<int64_t>(static_cast<uint32_t>(hr_enum))
                      : static_cast<int64_t>(0x80070490);
        fail_stage = "enumerasi device";
        release_devices();
        break;
      }
      // M2g requirement 3: jumlah device + nama dipakai dicatat di sini agar
      // masalah PERANGKAT terlihat terpisah dari masalah READER. ActivateObject
      // dipindah ke dalam ladder - setiap tingkat membuat objek source BARU.
      activate = devices[device_index];
      activate->AddRef();
      Log(kTag, "device: %u ditemukan, memakai index %d \"%s\"", device_count,
          device_index, FriendlyName(activate).c_str());
      release_devices();  // semua (terpilih sudah dipegang ref sendiri)

      // ---- tangga fallback atribut reader (M2f requirement 1) ----
      size_t ladder_n = 0;
      const int* ladder = nullptr;
      const int plain_only[] = {kReaderPlain};
      if (plain_attrs_) {
        ladder = plain_only;  // --capture-plain: langsung tingkat (c)
        ladder_n = 1;
      } else {
        ladder = DefaultReaderLadder(&ladder_n);
      }
      // M2g requirement 1: objek IMFMediaSource BARU untuk SETIAP tingkat.
      // Pembuatan reader yang gagal me-release reader internalnya, dan "by
      // default ... the source reader shuts down the media source" (docs MS)
      // -> sumber mati (MF_E_SHUTDOWN) bila dipakai ulang. Jadi: matikan sisa
      // objek gagal, ActivateObject baru, baru coba reader di tingkat ini.
      used_level = RunReaderFallbackLadder(
          ladder, ladder_n,
          [&](int level, int64_t* lhr) -> bool {
            last_stage = "MFCreateSourceReaderFromMediaSource";
            if (source) {  // pertahanan: jangan pernah memakai ulang objek gagal
              source->Shutdown();
              SafeRelease(source);
            }
            const HRESULT hr_act = activate->ActivateObject(IID_PPV_ARGS(&source));
            Log(kTag, "ActivateObject (objek baru, tingkat %d): hr=0x%08llX %s",
                level, static_cast<unsigned long long>(
                           static_cast<uint32_t>(hr_act)),
                SUCCEEDED(hr_act) ? "OK" : "GAGAL");
            if (FAILED(hr_act)) {
              last_stage = "ActivateObject";
              *lhr = static_cast<int64_t>(static_cast<uint32_t>(hr_act));
              SafeRelease(source);
              return false;
            }
            IMFAttributes* attrs = nullptr;
            if (level != kReaderPlain) {
              const HRESULT hr_a =
                  MFCreateAttributes(&attrs, level == kReaderFull ? 2 : 1);
              if (FAILED(hr_a)) {
                *lhr = static_cast<int64_t>(static_cast<uint32_t>(hr_a));
                source->Shutdown();
                SafeRelease(source);
                return false;
              }
              attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
              if (level == kReaderFull) {
                attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,
                                 TRUE);
              }
            }
            const HRESULT hr_r =
                MFCreateSourceReaderFromMediaSource(source, attrs, &reader);
            SafeRelease(attrs);
            *lhr = static_cast<int64_t>(static_cast<uint32_t>(hr_r));
            if (FAILED(hr_r)) {
              // Objek ini sudah mati/mati-matian setelah kegagalan reader
              // (lihat catatan di atas) - buang sekarang, jangan dipakai lagi.
              source->Shutdown();
              SafeRelease(source);
              return false;
            }
            return true;
          },
          [&](int level, int64_t lhr, bool ok) {
            if (!ok && last_stage && std::strcmp(last_stage, "ActivateObject") == 0) {
              return;  // hr ActivateObject sudah dicetak di barisnya sendiri
            }
            Log(kTag, "reader percobaan tingkat %d (%s): hr=0x%08llX %s", level,
                ReaderLevelName(level), static_cast<unsigned long long>(lhr),
                ok ? "OK" : "GAGAL");
          },
          &fail_hr);
      SafeRelease(activate);  // ladder selesai; source (bila sukses) dipegang sendiri
      if (used_level < 0) {
        fail_stage = last_stage ? last_stage : "MFCreateSourceReaderFromMediaSource";
        break;
      }
      Log(kTag, "MF capture: tingkat dipakai = %d (%s)", used_level,
          ReaderLevelName(used_level));

      // ---- pilih mode output RGB32 (M2e, tanpa perubahan perilaku) ----
      HRESULT hr = S_OK;  // (dulu diisi ActivateObject; kini hr per tingkat)
      if (req_width > 0 && req_height > 0) {
        hr = SelectRgb32OutputAt(reader, req_width, req_height, req_fps);
        if (FAILED(hr)) {
          Log(kTag,
              "requested mode %dx%d@%d not accepted (0x%08lx) - falling back to default RGB32",
              req_width, req_height, req_fps, static_cast<unsigned long>(hr));
        }
      }
      if (FAILED(hr) || req_width <= 0 || req_height <= 0) {
        hr = SelectRgb32Output(reader);
      }
      if (FAILED(hr)) {
        fail_hr = static_cast<int64_t>(static_cast<uint32_t>(hr));
        fail_stage = "SetCurrentMediaType RGB32";
        SafeRelease(reader);
        break;
      }

      // Read back the media type ACTUALLY negotiated (M2e requirement 5):
      // size, signed stride (negative = bottom-up rows), subtype, frame rate.
      {
        IMFMediaType* current = nullptr;
        if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                  &current))) {
          UINT32 w = 0, h = 0;
          MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &w, &h);
          width = static_cast<int>(w);
          height = static_cast<int>(h);
          UINT32 s = 0;
          if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) {
            raw_stride = static_cast<int>(s);  // bit pattern = signed value
          }
          GUID sub{};
          if (SUCCEEDED(current->GetGUID(MF_MT_SUBTYPE, &sub))) {
            subtype_data1 = static_cast<int>(sub.Data1);
          }
          UINT64 fr = 0;
          if (SUCCEEDED(current->GetUINT64(MF_MT_FRAME_RATE, &fr))) {
            fps_num = static_cast<int>(fr >> 32);
            fps_den = static_cast<int>(fr & 0xFFFFFFFFu);
          }
          current->Release();
        }
      }
      if (width <= 0 || height <= 0) {
        fail_hr = static_cast<int64_t>(0x80004005);  // E_FAIL: tipe tak terbaca
        fail_stage = "GetCurrentMediaType";
        SafeRelease(reader);
        break;
      }
      if (raw_stride == 0) raw_stride = width * 4;  // RGB32 default: top-down
      abs_stride = raw_stride < 0 ? -raw_stride : raw_stride;
      const std::string sub_str = SubtypeToString(subtype_data1);
      Log(kTag,
          "capture started (aktual): %dx%d stride=%d (abs=%d,%s) subtype=%s "
          "(0x%08X) fps=%d/%d RGB32/BGRA | diminta: %dx%d@%d | reader tingkat %d (%s)",
          width, height, raw_stride, abs_stride,
          raw_stride < 0 ? "bottom-up" : "top-down", sub_str.c_str(),
          static_cast<unsigned>(subtype_data1), fps_num, fps_den, req_width,
          req_height, req_fps, used_level, ReaderLevelName(used_level));
      if (req_width > 0 && (width != req_width || height != req_height)) {
        Log(kTag,
            "catatan: permintaan %dx%d tidak dipenuhi - memakai %dx%d (kamera/"
            "driver menolak scaling; lihat flag advanced processing)",
            req_width, req_height, width, height);
      }
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.width = width;
        stats_.height = height;
        stats_.stride = raw_stride;
        stats_.subtype = subtype_data1;
        stats_.fps_num = fps_num;
        stats_.fps_den = fps_den;
        stats_.mode_from_device = true;
      }
      opened = true;
    } while (false);

    // ---- gagal buka: catat, backoff, ulangi ----
    if (!opened) {
      SafeRelease(reader);
      SafeRelease(activate);  // bila jalur gagal sebelum ladder melepasnya
      if (source) {
        source->Shutdown();
        SafeRelease(source);
      }
      open_failure_count_.fetch_add(1);
      last_open_hr_.store(fail_hr);
      {  // hr PERTAMA siklus ini (untuk UI: kode pertama -> terakhir)
        int64_t expected = 0;
        first_open_hr_.compare_exchange_strong(expected, fail_hr);
      }
      const int wait_ms = backoff_ms;
      SetError("MF capture start: GAGAL (%s) hr=0x%08llX - percobaan %u, mencoba lagi dalam %d ms",
               fail_stage ? fail_stage : "tidak diketahui",
               static_cast<unsigned long long>(fail_hr), attempt, wait_ms);
      for (int w = 0; w < wait_ms && running_.load(); w += 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      backoff_ms = std::min(backoff_ms * 2, 8000);
      continue;
    }

    Log(kTag,
        "MF capture start: reader jadi (tingkat %d, %s, percobaan %u) - menunggu frame pertama",
        used_level, ReaderLevelName(used_level), attempt);

    // ---- read loop (kode M2e; ditambah log OK saat frame pertama) ----
    bool read_failed = false;
    int64_t read_hr = 0;
    while (running_.load()) {
      DWORD stream_index = 0, flags = 0;
      LONGLONG timestamp = 0;
      IMFSample* sample = nullptr;
      const HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                            &stream_index, &flags, &timestamp, &sample);
      if (FAILED(hr)) {
        read_failed = true;
        read_hr = static_cast<int64_t>(static_cast<uint32_t>(hr));
        SetError("ReadSample failed (0x%08lx)", static_cast<unsigned long>(hr));
        break;
      }
      if (flags & MF_SOURCE_READERF_STREAMTICK) {
        SafeRelease(sample);
        continue;
      }
      if (!sample) continue;  // not fatal; keep waiting

      IMFMediaBuffer* buffer = nullptr;
      HRESULT hr_buf = sample->ConvertToContiguousBuffer(&buffer);
      if (SUCCEEDED(hr_buf)) {
        BYTE* data = nullptr;
        DWORD max_len = 0, current_len = 0;
        hr_buf = buffer->Lock(&data, &max_len, &current_len);
        if (SUCCEEDED(hr_buf) && data) {
          // Normalise every frame to TOP-DOWN rows with a positive stride:
          // MF negative stride (bottom-up) is flipped during this copy, so
          // everything downstream (pipeline, F4 raw preview, F5 dump) sees a
          // single layout. Padded stride (> row_bytes) is preserved.
          const bool flip = raw_stride < 0;
          const size_t stride_bytes = static_cast<size_t>(abs_stride);
          const size_t row_bytes = static_cast<size_t>(width) * 4;
          Frame frame;
          frame.width = width;
          frame.height = height;
          frame.stride_bytes = static_cast<int>(stride_bytes);
          frame.format = kFormatBGRA;
          frame.timestamp_ms = NowMs();
          frame.pixels.resize(stride_bytes * static_cast<size_t>(height));
          const size_t mem_rows =
              std::min<size_t>(current_len / stride_bytes,
                               static_cast<size_t>(height));
          if (!flip && stride_bytes == row_bytes &&
              current_len >= frame.pixels.size()) {
            std::memcpy(frame.pixels.data(), data, frame.pixels.size());
          } else {
            for (size_t mi = 0; mi < mem_rows; ++mi) {
              const size_t visual_row = flip ? (height - 1 - mi) : mi;
              std::memcpy(frame.pixels.data() + visual_row * stride_bytes,
                          data + mi * stride_bytes, row_bytes);
            }
          }
          // M2e per-frame stats (luma + center alpha) for the 1 s diag log.
          {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.received++;
            stats_.width = width;
            stats_.height = height;
            stats_.stride = raw_stride;
            stats_.subtype = subtype_data1;
            const FrameStats fst = ComputeFrameStats(
                frame.pixels.data(), width, height, stride_bytes,
                /*flip_vertical=*/false, /*src_is_bgra=*/true);
            stats_.mean_luma = fst.mean_luma;
            stats_.center_alpha = fst.center_alpha;
          }
          frames_captured_.fetch_add(1);
          if (queue_) queue_->Push(std::move(frame));
          // M2f: OK HANYA setelah reader jadi (di atas) + frame pertama ini.
          if (!first_frame_seen_.load()) {
            first_frame_seen_.store(true);
            Log(kTag,
                "MF capture start: OK - frame pertama diterima %lld ms setelah thread mulai (tingkat %d, percobaan %u)",
                static_cast<long long>(NowMs() - thread_start_ms), used_level,
                attempt);
          }
        }
        if (data) buffer->Unlock();
        SafeRelease(buffer);
      }
      SafeRelease(sample);
    }

    SafeRelease(reader);
    if (source) {
      source->Shutdown();
      SafeRelease(source);
    }
    if (!running_.load()) break;
    if (read_failed) {
      open_failure_count_.fetch_add(1);
      last_open_hr_.store(read_hr);
      {  // hr PERTAMA siklus ini
        int64_t expected = 0;
        first_open_hr_.compare_exchange_strong(expected, read_hr);
      }
      const int wait_ms = backoff_ms;
      SetError("MF capture start: GAGAL (ReadSample) hr=0x%08llX - percobaan %u, mencoba lagi dalam %d ms",
               static_cast<unsigned long long>(read_hr), attempt, wait_ms);
      for (int w = 0; w < wait_ms && running_.load(); w += 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      backoff_ms = std::min(backoff_ms * 2, 8000);
      continue;
    }
  }

  running_.store(false);
}
}  // namespace kgcore
