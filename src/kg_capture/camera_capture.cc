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
#include "kgcore/log.h"

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
HRESULT EnumerateDevices(IMFActivate*** out_devices, UINT32* out_count) {
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
  HRESULT hr = EnumerateDevices(&devices, &count);
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
  HRESULT hr = EnumerateDevices(&devices, &count);
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
    if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;
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

bool CameraCapture::Start(int device_index, int width, int height, int fps) {
  if (running_.load()) return true;

  // Preflight: enumerate devices synchronously so the UI can report "no
  // camera" immediately instead of failing silently on a worker thread.
  IMFActivate** devices = nullptr;
  UINT32 device_count = 0;
  HRESULT hr = EnumerateDevices(&devices, &device_count);
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
  running_.store(true);
  thread_ = std::thread(&CameraCapture::CaptureThread, this, device_index, width,
                        height, fps);
  return true;
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
    SetError("MFStartup failed");
    running_.store(false);
    return;
  }

  IMFActivate** devices = nullptr;
  UINT32 device_count = 0;
  HRESULT hr = EnumerateDevices(&devices, &device_count);
  if (FAILED(hr) || static_cast<UINT32>(device_index) >= device_count) {
    SetError("device disappeared between preflight and open (hr=0x%08lx)",
             static_cast<unsigned long>(hr));
    goto cleanup_devices;
  }

  {
    IMFMediaSource* source = nullptr;
    hr = devices[device_index]->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr)) {
      SetError("ActivateObject failed (0x%08lx)", static_cast<unsigned long>(hr));
      goto cleanup_devices;
    }

    // Enable the video processor so MF can transcode NV12 -> RGB32.
    IMFAttributes* reader_attrs = nullptr;
    MFCreateAttributes(&reader_attrs, 1);
    if (reader_attrs) {
      reader_attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    IMFSourceReader* reader = nullptr;
    hr = MFCreateSourceReaderFromMediaSource(source, reader_attrs, &reader);
    SafeRelease(reader_attrs);
    if (FAILED(hr)) {
      SetError("MFCreateSourceReaderFromMediaSource failed (0x%08lx)",
               static_cast<unsigned long>(hr));
      source->Shutdown();
      SafeRelease(source);
      goto cleanup_devices;
    }

    if (req_width > 0 && req_height > 0) {
      hr = SelectRgb32OutputAt(reader, req_width, req_height, req_fps);
      if (FAILED(hr)) {
        Log(kTag, "requested mode %dx%d@%d not accepted (0x%08lx) — falling back to default RGB32",
            req_width, req_height, req_fps, static_cast<unsigned long>(hr));
      }
    }
    if (FAILED(hr) || req_width <= 0 || req_height <= 0) {
      hr = SelectRgb32Output(reader);
    }
    if (FAILED(hr)) {
      SetError("camera does not provide RGB32/BGRA output (0x%08lx); "
               "NV12 fallback not implemented yet",
               static_cast<unsigned long>(hr));
      SafeRelease(reader);
      source->Shutdown();
      SafeRelease(source);
      goto cleanup_devices;
    }

    // Discover negotiated frame size + stride.
    int width = 0, height = 0, stride = 0;
    {
      IMFMediaType* current = nullptr;
      if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current))) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &w, &h);
        width = static_cast<int>(w);
        height = static_cast<int>(h);
        UINT32 s = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) {
          stride = static_cast<int>(s);
        }
        current->Release();
      }
    }
    if (width <= 0 || height <= 0) {
      UINT32 stride_dword = 0;
      if (SUCCEEDED(MFCalculateImageSize(MFVideoFormat_RGB32,
                                         static_cast<UINT32>(width > 0 ? width : 640),
                                         static_cast<UINT32>(height > 0 ? height : 480),
                                         &stride_dword))) {
        stride = static_cast<int>(stride_dword);
      }
    }
    if (stride < width * 4) stride = width * 4;
    Log(kTag, "capture started: %dx%d stride=%d (RGB32/BGRA)", width, height, stride);

    while (running_.load()) {
      DWORD stream_index = 0, flags = 0;
      LONGLONG timestamp = 0;
      IMFSample* sample = nullptr;
      hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                              &stream_index, &flags, &timestamp, &sample);
      if (FAILED(hr)) {
        SetError("ReadSample failed (0x%08lx)", static_cast<unsigned long>(hr));
        break;
      }
      if (flags & MF_SOURCE_READERF_STREAMTICK) {
        SafeRelease(sample);
        continue;
      }
      if (!sample) continue;  // not fatal; keep waiting

      IMFMediaBuffer* buffer = nullptr;
      hr = sample->ConvertToContiguousBuffer(&buffer);
      if (SUCCEEDED(hr)) {
        BYTE* data = nullptr;
        DWORD max_len = 0, current_len = 0;
        hr = buffer->Lock(&data, &max_len, &current_len);
        if (SUCCEEDED(hr) && data) {
          Frame frame;
          frame.width = width;
          frame.height = height;
          frame.stride_bytes = stride;
          frame.format = kFormatBGRA;
          frame.timestamp_ms = NowMs();
          const size_t row_bytes = static_cast<size_t>(width) * 4;
          frame.pixels.resize(static_cast<size_t>(stride) * height);
          const size_t copy_rows =
              std::min<size_t>(current_len / row_bytes, static_cast<size_t>(height));
          if (stride == static_cast<int>(row_bytes)) {
            if (current_len >= frame.pixels.size()) {
              std::memcpy(frame.pixels.data(), data, frame.pixels.size());
            }
          } else {
            for (size_t y = 0; y < copy_rows; ++y) {
              std::memcpy(frame.pixels.data() + y * stride, data + y * stride, row_bytes);
            }
          }
          frames_captured_.fetch_add(1);
          if (queue_) queue_->Push(std::move(frame));
        }
        if (data) buffer->Unlock();
        SafeRelease(buffer);
      }
      SafeRelease(sample);
    }

    SafeRelease(reader);
    source->Shutdown();
    SafeRelease(source);
  }

cleanup_devices:
  for (UINT32 i = 0; i < device_count; ++i) SafeRelease(devices[i]);
  CoTaskMemFree(devices);
  running_.store(false);
}
}  // namespace kgcore
