// Standalone DirectShow verification tool run in Windows CI (admin available):
// 1. --verify-registered <dll_dir>:
//    - Registers check via SoftcamSink::IsRegistered()
//    - Enumerates CLSID_VideoInputDeviceCategory via ICreateDevEnum and verifies
//      "KameraGeulis Camera" appears
//    - Opens SoftcamSink (128x96@30), connects DirectShow graph to a minimal
//      COM IMemInputPin receiver, sends synthetic BGR24 frames, verifies
//      scIsConnected()==true and pixel values read back from IMediaSample!
// 2. --verify-absent <dll_dir>:
//    - Verifies "KameraGeulis Camera" is gone from ICreateDevEnum and registry.

#include <windows.h>
#include <dshow.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kgcore/virtual_camera.h"
#include "softcam_sink.h"

namespace {

constexpr wchar_t kExpectedDeviceName[] = L"KameraGeulis Camera";

void FreeMediaTypeFields(AM_MEDIA_TYPE& mt) {
  if (mt.cbFormat != 0 && mt.pbFormat) {
    CoTaskMemFree(mt.pbFormat);
    mt.cbFormat = 0;
    mt.pbFormat = nullptr;
  }
  if (mt.pUnk) {
    mt.pUnk->Release();
    mt.pUnk = nullptr;
  }
}

HRESULT CopyMediaTypeLocal(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src) {
  *dst = *src;
  if (src->cbFormat != 0 && src->pbFormat) {
    dst->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src->cbFormat));
    if (!dst->pbFormat) return E_OUTOFMEMORY;
    std::memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
  }
  if (dst->pUnk) dst->pUnk->AddRef();
  return S_OK;
}

class ReceiverFilter;

class ReceiverPin final : public IPin, public IMemInputPin {
 public:
  explicit ReceiverPin(ReceiverFilter* owner) : owner_(owner) {}
  ~ReceiverPin() { FreeMediaTypeFields(mt_); }

  // IUnknown
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) {
      *ppv = static_cast<IPin*>(this);
      AddRef();
      return S_OK;
    }
    if (riid == IID_IMemInputPin) {
      *ppv = static_cast<IMemInputPin*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  // IPin
  HRESULT STDMETHODCALLTYPE Connect(IPin*, const AM_MEDIA_TYPE*) override {
    return E_UNEXPECTED;
  }
  HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin* connector,
                                              const AM_MEDIA_TYPE* pmt) override {
    if (!connector || !pmt) return E_POINTER;
    if (pmt->majortype != MEDIATYPE_Video || pmt->subtype != MEDIASUBTYPE_RGB24) {
      return VFW_E_TYPE_NOT_ACCEPTED;
    }
    FreeMediaTypeFields(mt_);
    CopyMediaTypeLocal(&mt_, pmt);
    connected_ = connector;
    connected_->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Disconnect() override {
    if (connected_) {
      connected_->Release();
      connected_ = nullptr;
    }
    FreeMediaTypeFields(mt_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pPin) override {
    if (!pPin) return E_POINTER;
    *pPin = connected_;
    if (connected_) connected_->AddRef();
    return connected_ ? S_OK : VFW_E_NOT_CONNECTED;
  }
  HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE* pmt) override {
    if (!pmt) return E_POINTER;
    if (!connected_) return VFW_E_NOT_CONNECTED;
    return CopyMediaTypeLocal(pmt, &mt_);
  }
  HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* pInfo) override;
  HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* pPinDir) override {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_INPUT;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* Id) override {
    if (!Id) return E_POINTER;
    *Id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(L"In")));
    if (!*Id) return E_OUTOFMEMORY;
    std::memcpy(*Id, L"In", sizeof(L"In"));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE* pmt) override {
    if (!pmt) return E_POINTER;
    return (pmt->majortype == MEDIATYPE_Video && pmt->subtype == MEDIASUBTYPE_RGB24)
               ? S_OK
               : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes** ppEnum) override {
    if (ppEnum) *ppEnum = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin**, ULONG*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE EndOfStream() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE BeginFlush() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE EndFlush() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME, REFERENCE_TIME,
                                       double) override {
    return S_OK;
  }

  // IMemInputPin
  HRESULT STDMETHODCALLTYPE GetAllocator(IMemAllocator** ppAllocator) override {
    if (!ppAllocator) return E_POINTER;
    if (!alloc_) {
      HRESULT hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                                    reinterpret_cast<void**>(&alloc_));
      if (FAILED(hr)) return hr;
    }
    *ppAllocator = alloc_;
    alloc_->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE NotifyAllocator(IMemAllocator* pAllocator,
                                            BOOL) override {
    if (alloc_) alloc_->Release();
    alloc_ = pAllocator;
    if (alloc_) alloc_->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetAllocatorRequirements(
      ALLOCATOR_PROPERTIES*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Receive(IMediaSample* pSample) override {
    if (!pSample) return E_POINTER;
    BYTE* data = nullptr;
    const long len = pSample->GetActualDataLength();
    if (SUCCEEDED(pSample->GetPointer(&data)) && data && len >= 3) {
      std::lock_guard<std::mutex> lk(sample_mu_);
      last_sample_.assign(data, data + len);
      ++received_count_;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ReceiveMultiple(IMediaSample** pSamples,
                                            long nSamples,
                                            long* nProcessed) override {
    long done = 0;
    for (long i = 0; i < nSamples; ++i) {
      if (SUCCEEDED(Receive(pSamples[i]))) ++done;
    }
    if (nProcessed) *nProcessed = done;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ReceiveCanBlock() override { return S_FALSE; }

  uint64_t ReceivedCount() const { return received_count_.load(); }
  std::vector<uint8_t> LastSample() const {
    std::lock_guard<std::mutex> lk(sample_mu_);
    return last_sample_;
  }

 private:
  std::atomic<ULONG> ref_{1};
  ReceiverFilter* owner_ = nullptr;
  IPin* connected_ = nullptr;
  IMemAllocator* alloc_ = nullptr;
  AM_MEDIA_TYPE mt_{};
  mutable std::mutex sample_mu_;
  std::vector<uint8_t> last_sample_;
  std::atomic<uint64_t> received_count_{0};
};

class SinglePinEnum final : public IEnumPins {
 public:
  explicit SinglePinEnum(IPin* pin) : pin_(pin) {
    if (pin_) pin_->AddRef();
  }
  ~SinglePinEnum() {
    if (pin_) pin_->Release();
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IEnumPins) {
      *ppv = static_cast<IEnumPins*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Next(ULONG cPins, IPin** ppPins,
                                 ULONG* pcFetched) override {
    if (!ppPins) return E_POINTER;
    ULONG fetched = 0;
    if (cPins > 0 && idx_ == 0 && pin_) {
      ppPins[0] = pin_;
      pin_->AddRef();
      ++idx_;
      fetched = 1;
    }
    if (pcFetched) *pcFetched = fetched;
    return fetched == cPins ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Skip(ULONG cPins) override {
    idx_ += cPins;
    return idx_ <= 1 ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Reset() override {
    idx_ = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IEnumPins** ppEnum) override {
    if (!ppEnum) return E_POINTER;
    auto* e = new SinglePinEnum(pin_);
    e->idx_ = idx_;
    *ppEnum = e;
    return S_OK;
  }

 private:
  std::atomic<ULONG> ref_{1};
  IPin* pin_ = nullptr;
  ULONG idx_ = 0;
};

class ReceiverFilter final : public IBaseFilter {
 public:
  ReceiverFilter() : pin_(new ReceiverPin(this)) {}
  ~ReceiverFilter() {
    if (pin_) pin_->Release();
  }
  ReceiverPin* Pin() const { return pin_; }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPersist ||
        riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
      *ppv = static_cast<IBaseFilter*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetClassID(CLSID* pClsID) override {
    if (!pClsID) return E_POINTER;
    *pClsID = GUID_NULL;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Stop() override {
    state_ = State_Stopped;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Pause() override {
    state_ = State_Paused;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME) override {
    state_ = State_Running;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetState(DWORD, FILTER_STATE* State) override {
    if (!State) return E_POINTER;
    *State = state_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock* pClock) override {
    clock_ = pClock;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock** pClock) override {
    if (!pClock) return E_POINTER;
    *pClock = clock_;
    if (clock_) clock_->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins** ppEnum) override {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new SinglePinEnum(pin_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR Id, IPin** ppPin) override {
    if (!ppPin) return E_POINTER;
    if (Id && std::wcscmp(Id, L"In") == 0) {
      *ppPin = pin_;
      pin_->AddRef();
      return S_OK;
    }
    *ppPin = nullptr;
    return VFW_E_NOT_FOUND;
  }
  HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* pInfo) override {
    if (!pInfo) return E_POINTER;
    std::wcscpy(pInfo->achName, L"KgTestReceiver");
    pInfo->pGraph = graph_;
    if (graph_) graph_->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph* pGraph,
                                            LPCWSTR) override {
    graph_ = pGraph;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR* pVendorInfo) override {
    if (pVendorInfo) *pVendorInfo = nullptr;
    return E_NOTIMPL;
  }

 private:
  std::atomic<ULONG> ref_{1};
  ReceiverPin* pin_ = nullptr;
  FILTER_STATE state_ = State_Stopped;
  IReferenceClock* clock_ = nullptr;
  IFilterGraph* graph_ = nullptr;
};

HRESULT STDMETHODCALLTYPE ReceiverPin::QueryPinInfo(PIN_INFO* pInfo) {
  if (!pInfo) return E_POINTER;
  pInfo->pFilter = owner_;
  if (owner_) owner_->AddRef();
  pInfo->dir = PINDIR_INPUT;
  std::wcscpy(pInfo->achName, L"In");
  return S_OK;
}

bool FindDirectShowDevice(const wchar_t* target_name, IMoniker** out_moniker) {
  if (out_moniker) *out_moniker = nullptr;
  ICreateDevEnum* dev_enum = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
                                reinterpret_cast<void**>(&dev_enum));
  if (FAILED(hr) || !dev_enum) return false;

  IEnumMoniker* enum_mon = nullptr;
  hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                       &enum_mon, 0);
  bool found = false;
  if (hr == S_OK && enum_mon) {
    IMoniker* mon = nullptr;
    while (enum_mon->Next(1, &mon, nullptr) == S_OK) {
      IPropertyBag* bag = nullptr;
      if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_IPropertyBag,
                                       reinterpret_cast<void**>(&bag)))) {
        VARIANT var;
        VariantInit(&var);
        if (SUCCEEDED(bag->Read(L"FriendlyName", &var, nullptr)) &&
            var.vt == VT_BSTR && var.bstrVal) {
          std::printf("[ci-vcam] DShow device: %ls\n", var.bstrVal);
          if (std::wcscmp(var.bstrVal, target_name) == 0) {
            found = true;
            if (out_moniker && !*out_moniker) {
              *out_moniker = mon;
              mon->AddRef();
            }
          }
        }
        VariantClear(&var);
        bag->Release();
      }
      mon->Release();
    }
    enum_mon->Release();
  }
  dev_enum->Release();
  return found;
}

IPin* FindOutputPin(IBaseFilter* filter) {
  IEnumPins* ep = nullptr;
  if (FAILED(filter->EnumPins(&ep)) || !ep) return nullptr;
  IPin* out = nullptr;
  IPin* pin = nullptr;
  while (ep->Next(1, &pin, nullptr) == S_OK) {
    PIN_DIRECTION dir{};
    if (SUCCEEDED(pin->QueryDirection(&dir)) && dir == PINDIR_OUTPUT) {
      out = pin;
      break;
    }
    pin->Release();
  }
  ep->Release();
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "Usage: kg_vcam_ci_test <--verify-registered|--verify-absent> <dll_dir>\n");
    return 2;
  }
  const std::string mode = argv[1];
  const std::string dll_dir = argv[2];

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) {
    std::fprintf(stderr, "CoInitializeEx failed: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
    return 3;
  }

  kgcore::SoftcamSink sink(dll_dir);
  std::string reg_detail;
  const bool is_reg = sink.IsRegistered(&reg_detail);
  std::printf("[ci-vcam] IsRegistered=%d (%s)\n", is_reg ? 1 : 0,
              reg_detail.c_str());

  if (mode == "--verify-absent") {
    const bool found = FindDirectShowDevice(kExpectedDeviceName, nullptr);
    CoUninitialize();
    if (found) {
      std::fprintf(stderr, "FAIL: KameraGeulis Camera still present in DirectShow!\n");
      return 10;
    }
    if (is_reg) {
      std::fprintf(stderr, "FAIL: Registry still reports SoftcamSink registered!\n");
      return 11;
    }
    std::printf("[ci-vcam] PASS: KameraGeulis Camera cleanly unregistered.\n");
    return 0;
  }

  if (mode != "--verify-registered") {
    CoUninitialize();
    return 2;
  }

  if (!is_reg) {
    std::fprintf(stderr, "FAIL: SoftcamSink::IsRegistered returned false!\n");
    CoUninitialize();
    return 4;
  }

  IMoniker* mon = nullptr;
  if (!FindDirectShowDevice(kExpectedDeviceName, &mon) || !mon) {
    std::fprintf(stderr, "FAIL: KameraGeulis Camera not found in DirectShow!\n");
    CoUninitialize();
    return 5;
  }

  // 1. Buka sender (128x96 @ 30 fps)
  constexpr int kW = 128, kH = 96;
  std::string open_err;
  if (!sink.Open(kW, kH, 30.0f, &open_err)) {
    std::fprintf(stderr, "FAIL: SoftcamSink::Open failed: %s\n",
                 open_err.c_str());
    mon->Release();
    CoUninitialize();
    return 6;
  }

  // Kirim frame sintetis dua warna: separuh atas MERAH (240,20,10), separuh
  // bawah BIRU (10,30,240) agar urutan kanal BGR dan orientasi atas/bawah ikut teruji.
  std::vector<uint8_t> rgba(static_cast<size_t>(kW) * kH * 4);
  for (int y = 0; y < kH; ++y) {
    const bool top = (y < kH / 2);
    for (int x = 0; x < kW; ++x) {
      const size_t i = (static_cast<size_t>(y) * kW + x) * 4;
      rgba[i + 0] = top ? 240 : 10;
      rgba[i + 1] = top ? 20 : 30;
      rgba[i + 2] = top ? 10 : 240;
      rgba[i + 3] = 255;
    }
  }
  std::vector<uint8_t> bgr;
  kgcore::ConvertRgbaToVcamFormat(rgba.data(), kW, kH, kW * 4, kW, kH,
                                  kgcore::VcamPixelFormat::kBgr24TopDown,
                                  false, false, &bgr);
  sink.SendFrame(bgr.data(), bgr.size(), kW, kH, 1000);

  // 2. Bangun graf DirectShow: Softcam filter -> ReceiverFilter
  IBaseFilter* src_filter = nullptr;
  hr = mon->BindToObject(nullptr, nullptr, IID_IBaseFilter,
                         reinterpret_cast<void**>(&src_filter));
  mon->Release();
  if (FAILED(hr) || !src_filter) {
    std::fprintf(stderr, "FAIL: BindToObject failed: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
    sink.Close();
    CoUninitialize();
    return 7;
  }

  IFilterGraph2* graph = nullptr;
  hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                        IID_IFilterGraph2, reinterpret_cast<void**>(&graph));
  if (FAILED(hr) || !graph) {
    std::fprintf(stderr, "FAIL: CoCreateInstance(CLSID_FilterGraph) failed\n");
    src_filter->Release();
    sink.Close();
    CoUninitialize();
    return 8;
  }

  auto* recv = new ReceiverFilter();
  graph->AddFilter(src_filter, kExpectedDeviceName);
  graph->AddFilter(recv, L"KgTestReceiver");

  IPin* out_pin = FindOutputPin(src_filter);
  if (!out_pin) {
    std::fprintf(stderr, "FAIL: Softcam Output pin not found\n");
    recv->Release();
    graph->Release();
    src_filter->Release();
    sink.Close();
    CoUninitialize();
    return 9;
  }

  hr = graph->ConnectDirect(out_pin, recv->Pin(), nullptr);
  out_pin->Release();
  if (FAILED(hr)) {
    std::fprintf(stderr, "FAIL: ConnectDirect failed: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
    recv->Release();
    graph->Release();
    src_filter->Release();
    sink.Close();
    CoUninitialize();
    return 10;
  }

  IMediaControl* ctrl = nullptr;
  graph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&ctrl));
  hr = ctrl ? ctrl->Run() : E_FAIL;
  if (FAILED(hr)) {
    std::fprintf(stderr, "FAIL: IMediaControl::Run failed: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
    if (ctrl) ctrl->Release();
    recv->Release();
    graph->Release();
    src_filter->Release();
    sink.Close();
    CoUninitialize();
    return 11;
  }

  // Kirim frame berulang di loop singkat sampai penerima membaca frame sintetis.
  bool connected_seen = false;
  for (int i = 0; i < 40; ++i) {
    sink.SendFrame(bgr.data(), bgr.size(), kW, kH,
                   static_cast<int64_t>(i + 2) * 33333);
    if (sink.ConsumerCount() > 0) connected_seen = true;
    if (recv->Pin()->ReceivedCount() >= 2 && connected_seen) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }

  const uint64_t recv_cnt = recv->Pin()->ReceivedCount();
  const auto sample = recv->Pin()->LastSample();

  if (ctrl) {
    ctrl->Stop();
    ctrl->Release();
  }
  recv->Release();
  graph->Release();
  src_filter->Release();
  sink.Close();
  CoUninitialize();

  std::printf("[ci-vcam] received_frames=%llu connected_seen=%d sample_bytes=%zu\n",
              static_cast<unsigned long long>(recv_cnt), connected_seen ? 1 : 0,
              sample.size());

  if (recv_cnt == 0 || sample.size() < static_cast<size_t>(kW) * kH * 3) {
    std::fprintf(stderr, "FAIL: DirectShow receiver got no valid frame!\n");
    return 12;
  }
  if (!connected_seen) {
    std::fprintf(stderr, "FAIL: scIsConnected() never reported connected!\n");
    return 13;
  }
  // Periksa nilai piksel DIB bottom-up: baris 0 = bawah (BIRU: BGR 240,30,10),
  // baris kH-1 = atas (MERAH: BGR 10,20,240).
  const size_t bot_row = 0;
  const size_t top_row = static_cast<size_t>(kH - 1) * kW * 3;
  if (sample[bot_row + 0] != 240 || sample[bot_row + 1] != 30 ||
      sample[bot_row + 2] != 10 || sample[top_row + 0] != 10 ||
      sample[top_row + 1] != 20 || sample[top_row + 2] != 240) {
    std::fprintf(stderr,
                 "FAIL: Pixel/orientation mismatch! bot=(%u,%u,%u) top=(%u,%u,%u)\n",
                 sample[bot_row + 0], sample[bot_row + 1], sample[bot_row + 2],
                 sample[top_row + 0], sample[top_row + 1], sample[top_row + 2]);
    return 14;
  }

  std::printf("[ci-vcam] PASS: DirectShow registration + enumeration + BGR24 frame readback verified!\n");
  return 0;
}
