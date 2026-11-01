#define NOMINMAX
#include "minimix/frame_protocol.h"
#include "minimix/virtual_camera.h"
#include "shared_memory_win.h"

#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

DWORD WINAPI output_debug_log(void* context) {
  std::unique_ptr<std::string> line(static_cast<std::string*>(context));
  OutputDebugStringA(line->c_str());
  return 0;
}

void debug_log(const std::string& message) {
  auto* line = new (std::nothrow) std::string("[MiniMix VCAM] " + message + "\n");
  if (!line) return;
  if (!QueueUserWorkItem(output_debug_log, line, WT_EXECUTEDEFAULT)) {
    delete line;
  }
}

void fill_color_bars(std::uint8_t* pixels, UINT32 width, UINT32 height) {
  static constexpr std::array<std::array<std::uint8_t, 3>, 7> colors{{
      {{191, 191, 191}}, {{191, 191, 0}}, {{0, 191, 191}}, {{0, 191, 0}},
      {{191, 0, 191}}, {{191, 0, 0}}, {{0, 0, 191}},
  }};
  for (UINT32 y = 0; y < height; ++y) {
    for (UINT32 x = 0; x < width; ++x) {
      const auto color_index = std::min<std::size_t>(
          colors.size() - 1, static_cast<std::size_t>(x) * colors.size() / width);
      auto* pixel = pixels + (static_cast<std::size_t>(y) * width + x) * 4;
      pixel[0] = colors[color_index][2];
      pixel[1] = colors[color_index][1];
      pixel[2] = colors[color_index][0];
      pixel[3] = 255;
    }
  }
}

HRESULT set_camera_stream_attributes(IMFAttributes* attributes) {
  if (!attributes) return E_POINTER;
  HRESULT hr = attributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
  if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
  if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
  if (SUCCEEDED(hr)) {
    hr = attributes->SetUINT32(
        MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
  }
  return hr;
}

class MiniMixMediaSource;

class MiniMixMediaStream final : public IMFMediaStream {
 public:
  explicit MiniMixMediaStream(MiniMixMediaSource* source);
  HRESULT Initialize();
  HRESULT Start();
  HRESULT Stop();
  HRESULT Pause();
  void DetachSource();
  void Shutdown();

  STDMETHODIMP QueryInterface(REFIID iid, void** object) override;
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG references = --references_;
    if (!references) delete this;
    return references;
  }
  STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
  STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
  STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
  STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended_type, HRESULT status,
                          const PROPVARIANT* value) override;
  STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
  STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
  STDMETHODIMP RequestSample(IUnknown* token) override;

  IMFStreamDescriptor* descriptor() const { return descriptor_; }

 private:
  IMFMediaEventQueue* AcquireEventQueue();
  MiniMixMediaSource* AcquireSource();
  ~MiniMixMediaStream();
  std::atomic<ULONG> references_{1};
  std::mutex event_lock_;
  std::mutex source_lock_;
  MiniMixMediaSource* source_;
  IMFMediaEventQueue* events_ = nullptr;
  IMFStreamDescriptor* descriptor_ = nullptr;
};

enum class SourceState { stopped, started, paused, shutdown };

class MiniMixMediaSource final : public IMFMediaSourceEx,
                                 public IMFGetService,
                                 public IKsControl,
                                 public IMFSampleAllocatorControl {
  struct SampleRequest {
    MiniMixMediaSource* source;
    IStream* token_stream;
    std::uint64_t generation;
  };

 public:
  explicit MiniMixMediaSource(std::uint8_t output_id) : output_id_(output_id) {}
  HRESULT Initialize() {
    const int frame_channel = minimix::vcam::internal::initialize_frame_channel(output_id_);
    if (frame_channel != 0) {
      return HRESULT_FROM_WIN32(frame_channel == 5 ? ERROR_BUSY : ERROR_NOT_READY);
    }
    HRESULT hr = MFCreateEventQueue(&events_);
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&source_attributes_, 1);
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&stream_attributes_, 1);
    if (SUCCEEDED(hr)) hr = set_camera_stream_attributes(stream_attributes_);
    if (FAILED(hr)) return hr;
    stream_ = new (std::nothrow) MiniMixMediaStream(this);
    if (!stream_) return E_OUTOFMEMORY;
    hr = stream_->Initialize();
    if (FAILED(hr)) {
      stream_->Release();
      stream_ = nullptr;
    }
    return hr;
  }
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == __uuidof(IMFMediaEventGenerator) ||
        iid == __uuidof(IMFMediaSource) || iid == __uuidof(IMFMediaSourceEx)) {
      *object = static_cast<IMFMediaSourceEx*>(this);
      AddRef();
      return S_OK;
    }
    if (iid == __uuidof(IMFGetService)) {
      *object = static_cast<IMFGetService*>(this);
      AddRef();
      return S_OK;
    }
    if (iid == __uuidof(IKsControl)) {
      *object = static_cast<IKsControl*>(this);
      AddRef();
      return S_OK;
    }
    if (iid == __uuidof(IMFSampleAllocatorControl)) {
      *object = static_cast<IMFSampleAllocatorControl*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG references = --references_;
    if (!references) delete this;
    return references;
  }
  bool TryAddRef() {
    ULONG references = references_.load(std::memory_order_relaxed);
    while (references != 0) {
      if (references_.compare_exchange_weak(
              references, references + 1, std::memory_order_acquire,
              std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }
  STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override {
    auto* events = AcquireEventQueue();
    if (!events) return MF_E_SHUTDOWN;
    const HRESULT hr = events->GetEvent(flags, event);
    events->Release();
    return hr;
  }
  STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override {
    auto* events = AcquireEventQueue();
    if (!events) return MF_E_SHUTDOWN;
    const HRESULT hr = events->BeginGetEvent(callback, state);
    events->Release();
    return hr;
  }
  STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
    auto* events = AcquireEventQueue();
    if (!events) return MF_E_SHUTDOWN;
    const HRESULT hr = events->EndGetEvent(result, event);
    events->Release();
    return hr;
  }
  STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended_type, HRESULT status,
                          const PROPVARIANT* value) override {
    auto* events = AcquireEventQueue();
    if (!events) return MF_E_SHUTDOWN;
    const HRESULT hr = events->QueueEventParamVar(type, extended_type, status, value);
    events->Release();
    return hr;
  }
  STDMETHODIMP GetCharacteristics(DWORD* characteristics) override {
    if (!characteristics) return E_POINTER;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
  }
  STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
  STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* time_format,
                    const PROPVARIANT* start_position) override;
  STDMETHODIMP Stop() override;
  STDMETHODIMP Pause() override;
  STDMETHODIMP Shutdown() override;
  STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
  STDMETHODIMP GetStreamAttributes(DWORD stream_id, IMFAttributes** attributes) override;
  STDMETHODIMP SetD3DManager(IUnknown* manager) override;
  STDMETHODIMP GetService(REFGUID service, REFIID iid, LPVOID* object) override;
  STDMETHODIMP KsProperty(PKSPROPERTY property, ULONG property_length, LPVOID property_data,
                         ULONG data_length, ULONG* bytes_returned) override;
  STDMETHODIMP KsMethod(PKSMETHOD method, ULONG method_length, LPVOID method_data,
                        ULONG data_length, ULONG* bytes_returned) override;
  STDMETHODIMP KsEvent(PKSEVENT event, ULONG event_length, LPVOID event_data,
                       ULONG data_length, ULONG* bytes_returned) override;
  STDMETHODIMP SetDefaultAllocator(DWORD output_stream_id, IUnknown* allocator) override;
  STDMETHODIMP GetAllocatorUsage(DWORD output_stream_id, DWORD* input_stream_id,
                                 MFSampleAllocatorUsage* usage) override;
  HRESULT QueueSampleRequest(IUnknown* token);
  void QueueSampleError(HRESULT error);
  HRESULT ScheduleSampleRequest(SampleRequest* request);
  void ReleaseSampleRequest(SampleRequest* request);
  HRESULT BuildAndQueueSampleLocked(IUnknown* token, std::uint64_t generation);
  IMFMediaEventQueue* AcquireEventQueue() const;

  SourceState state() const {
    std::lock_guard<std::mutex> guard(lock_);
    return state_;
  }

 private:
  static DWORD WINAPI ProcessSampleRequest(void* context);
  ~MiniMixMediaSource() { Shutdown(); }
  HRESULT output_dimensions(UINT32* width, UINT32* height);
  std::atomic<ULONG> references_{1};
  const std::uint8_t output_id_;
  IMFMediaEventQueue* events_ = nullptr;
  MiniMixMediaStream* stream_ = nullptr;
  IMFAttributes* source_attributes_ = nullptr;
  IMFAttributes* stream_attributes_ = nullptr;
  mutable std::mutex lock_;
  std::mutex sample_lock_;
  std::deque<SampleRequest*> paused_requests_;
  std::atomic<std::uint32_t> pending_samples_{0};
  std::vector<std::uint8_t> last_frame_;
  bool has_valid_frame_ = false;
  bool stream_announced_ = false;
  bool stream_selected_ = false;
  std::uint64_t sample_generation_ = 0;
  HRESULT last_sample_error_ = S_OK;
  SourceState state_ = SourceState::stopped;
  LONGLONG next_sample_time_ = 0;
};

class MiniMixMediaSourceActivate final : public IMFActivate {
 public:
  explicit MiniMixMediaSourceActivate(std::uint8_t output_id) : output_id_(output_id) {}

  HRESULT Initialize() { return MFCreateAttributes(&attributes_, 1); }

  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == __uuidof(IMFAttributes) ||
        iid == __uuidof(IMFActivate)) {
      *object = static_cast<IMFActivate*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG references = --references_;
    if (!references) delete this;
    return references;
  }
  STDMETHODIMP ActivateObject(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    debug_log("ActivateObject output " + std::to_string(output_id_));
    std::lock_guard<std::mutex> guard(lock_);
    if (!source_) {
      source_ = new (std::nothrow) MiniMixMediaSource(output_id_);
      if (!source_) return E_OUTOFMEMORY;
      const HRESULT hr = source_->Initialize();
      if (FAILED(hr)) {
        debug_log("ActivateObject initialization failed: " +
                  std::to_string(static_cast<unsigned long>(hr)));
        source_->Release();
        source_ = nullptr;
        return hr;
      }
    }
    const HRESULT hr = source_->QueryInterface(iid, object);
    if (FAILED(hr)) {
      debug_log("ActivateObject QueryInterface failed: " +
                std::to_string(static_cast<unsigned long>(hr)));
    }
    return hr;
  }
  STDMETHODIMP ShutdownObject() override {
    std::lock_guard<std::mutex> guard(lock_);
    if (!source_) return S_OK;
    const HRESULT hr = source_->Shutdown();
    source_->Release();
    source_ = nullptr;
    return hr;
  }
  STDMETHODIMP DetachObject() override {
    std::lock_guard<std::mutex> guard(lock_);
    if (source_) source_->Release();
    source_ = nullptr;
    return S_OK;
  }

  STDMETHODIMP GetItem(REFGUID key, PROPVARIANT* value) override {
    return attributes_->GetItem(key, value);
  }
  STDMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) override {
    return attributes_->GetItemType(key, type);
  }
  STDMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result) override {
    return attributes_->CompareItem(key, value, result);
  }
  STDMETHODIMP Compare(IMFAttributes* theirs, MF_ATTRIBUTES_MATCH_TYPE type, BOOL* result) override {
    return attributes_->Compare(theirs, type, result);
  }
  STDMETHODIMP GetUINT32(REFGUID key, UINT32* value) override {
    return attributes_->GetUINT32(key, value);
  }
  STDMETHODIMP GetUINT64(REFGUID key, UINT64* value) override {
    return attributes_->GetUINT64(key, value);
  }
  STDMETHODIMP GetDouble(REFGUID key, double* value) override {
    return attributes_->GetDouble(key, value);
  }
  STDMETHODIMP GetGUID(REFGUID key, GUID* value) override {
    return attributes_->GetGUID(key, value);
  }
  STDMETHODIMP GetStringLength(REFGUID key, UINT32* length) override {
    return attributes_->GetStringLength(key, length);
  }
  STDMETHODIMP GetString(REFGUID key, LPWSTR value, UINT32 size, UINT32* length) override {
    return attributes_->GetString(key, value, size, length);
  }
  STDMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* value, UINT32* length) override {
    return attributes_->GetAllocatedString(key, value, length);
  }
  STDMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override {
    return attributes_->GetBlobSize(key, size);
  }
  STDMETHODIMP GetBlob(REFGUID key, UINT8* buffer, UINT32 size, UINT32* blob_size) override {
    return attributes_->GetBlob(key, buffer, size, blob_size);
  }
  STDMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** buffer, UINT32* size) override {
    return attributes_->GetAllocatedBlob(key, buffer, size);
  }
  STDMETHODIMP GetUnknown(REFGUID key, REFIID iid, LPVOID* object) override {
    return attributes_->GetUnknown(key, iid, object);
  }
  STDMETHODIMP SetItem(REFGUID key, REFPROPVARIANT value) override {
    return attributes_->SetItem(key, value);
  }
  STDMETHODIMP DeleteItem(REFGUID key) override { return attributes_->DeleteItem(key); }
  STDMETHODIMP DeleteAllItems() override { return attributes_->DeleteAllItems(); }
  STDMETHODIMP SetUINT32(REFGUID key, UINT32 value) override {
    return attributes_->SetUINT32(key, value);
  }
  STDMETHODIMP SetUINT64(REFGUID key, UINT64 value) override {
    return attributes_->SetUINT64(key, value);
  }
  STDMETHODIMP SetDouble(REFGUID key, double value) override {
    return attributes_->SetDouble(key, value);
  }
  STDMETHODIMP SetGUID(REFGUID key, REFGUID value) override {
    return attributes_->SetGUID(key, value);
  }
  STDMETHODIMP SetString(REFGUID key, LPCWSTR value) override {
    return attributes_->SetString(key, value);
  }
  STDMETHODIMP SetBlob(REFGUID key, const UINT8* buffer, UINT32 size) override {
    return attributes_->SetBlob(key, buffer, size);
  }
  STDMETHODIMP SetUnknown(REFGUID key, IUnknown* value) override {
    return attributes_->SetUnknown(key, value);
  }
  STDMETHODIMP LockStore() override { return attributes_->LockStore(); }
  STDMETHODIMP UnlockStore() override { return attributes_->UnlockStore(); }
  STDMETHODIMP GetCount(UINT32* count) override { return attributes_->GetCount(count); }
  STDMETHODIMP GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* value) override {
    return attributes_->GetItemByIndex(index, key, value);
  }
  STDMETHODIMP CopyAllItems(IMFAttributes* destination) override {
    return attributes_->CopyAllItems(destination);
  }

 private:
  ~MiniMixMediaSourceActivate() {
    if (source_) source_->Release();
    if (attributes_) attributes_->Release();
  }

  std::atomic<ULONG> references_{1};
  const std::uint8_t output_id_;
  IMFAttributes* attributes_ = nullptr;
  MiniMixMediaSource* source_ = nullptr;
  std::mutex lock_;
};

HRESULT make_video_type(UINT32 width, UINT32 height, IMFMediaType** media_type) {
  if (!media_type) return E_POINTER;
  *media_type = nullptr;
  IMFMediaType* result = nullptr;
  HRESULT hr = MFCreateMediaType(&result);
  if (SUCCEEDED(hr)) hr = result->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(hr)) hr = result->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  if (SUCCEEDED(hr)) hr = MFSetAttributeSize(result, MF_MT_FRAME_SIZE, width, height);
  if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(result, MF_MT_FRAME_RATE, 30, 1);
  if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(result, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (SUCCEEDED(hr)) hr = result->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (SUCCEEDED(hr)) hr = result->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  if (SUCCEEDED(hr)) {
    const UINT64 bitrate = static_cast<UINT64>(width) * height * 4 * 8 * 30;
    hr = result->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));
  }
  if (FAILED(hr) && result) result->Release();
  else *media_type = result;
  return hr;
}

MiniMixMediaStream::MiniMixMediaStream(MiniMixMediaSource* source) : source_(source) {}

MiniMixMediaSource* MiniMixMediaStream::AcquireSource() {
  std::lock_guard<std::mutex> guard(source_lock_);
  return source_ && source_->TryAddRef() ? source_ : nullptr;
}

IMFMediaEventQueue* MiniMixMediaStream::AcquireEventQueue() {
  std::lock_guard<std::mutex> guard(event_lock_);
  if (events_) events_->AddRef();
  return events_;
}

void MiniMixMediaStream::Shutdown() {
  IMFMediaEventQueue* events = nullptr;
  {
    std::lock_guard<std::mutex> guard(event_lock_);
    events = events_;
    events_ = nullptr;
  }
  if (events) {
    events->Shutdown();
    events->Release();
  }
}

void MiniMixMediaStream::DetachSource() {
  std::lock_guard<std::mutex> guard(source_lock_);
  source_ = nullptr;
}

HRESULT MiniMixMediaStream::Initialize() {
  HRESULT hr = MFCreateEventQueue(&events_);
  IMFMediaType* types[2] = {};
  if (SUCCEEDED(hr)) hr = make_video_type(1280, 720, &types[0]);
  if (SUCCEEDED(hr)) hr = make_video_type(1920, 1080, &types[1]);
  if (SUCCEEDED(hr)) hr = MFCreateStreamDescriptor(0, 2, types, &descriptor_);
  if (SUCCEEDED(hr)) {
    IMFMediaTypeHandler* handler = nullptr;
    hr = descriptor_->GetMediaTypeHandler(&handler);
    if (SUCCEEDED(hr)) {
      hr = handler->SetCurrentMediaType(types[0]);
      handler->Release();
    }
  }
  if (SUCCEEDED(hr)) hr = set_camera_stream_attributes(descriptor_);
  if (types[0]) types[0]->Release();
  if (types[1]) types[1]->Release();
  return hr;
}
MiniMixMediaStream::~MiniMixMediaStream() {
  Shutdown();
  DetachSource();
  if (descriptor_) descriptor_->Release();
}
HRESULT MiniMixMediaStream::Start() {
  debug_log("MEStreamStarted");
  return QueueEvent(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}
HRESULT MiniMixMediaStream::Stop() {
  debug_log("MEStreamStopped");
  return QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}
HRESULT MiniMixMediaStream::Pause() {
  debug_log("MEStreamPaused");
  return QueueEvent(MEStreamPaused, GUID_NULL, S_OK, nullptr);
}
STDMETHODIMP MiniMixMediaStream::QueryInterface(REFIID iid, void** object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  if (iid == IID_IUnknown || iid == __uuidof(IMFMediaEventGenerator) || iid == __uuidof(IMFMediaStream)) {
    *object = static_cast<IMFMediaStream*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}
STDMETHODIMP MiniMixMediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
  auto* events = AcquireEventQueue();
  if (!events) return MF_E_SHUTDOWN;
  const HRESULT hr = events->GetEvent(flags, event);
  events->Release();
  return hr;
}
STDMETHODIMP MiniMixMediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
  auto* events = AcquireEventQueue();
  if (!events) return MF_E_SHUTDOWN;
  const HRESULT hr = events->BeginGetEvent(callback, state);
  events->Release();
  return hr;
}
STDMETHODIMP MiniMixMediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
  auto* events = AcquireEventQueue();
  if (!events) return MF_E_SHUTDOWN;
  const HRESULT hr = events->EndGetEvent(result, event);
  events->Release();
  return hr;
}
STDMETHODIMP MiniMixMediaStream::QueueEvent(MediaEventType type, REFGUID extended_type, HRESULT status,
                                             const PROPVARIANT* value) {
  auto* events = AcquireEventQueue();
  if (!events) return MF_E_SHUTDOWN;
  const HRESULT hr = events->QueueEventParamVar(type, extended_type, status, value);
  events->Release();
  return hr;
}
STDMETHODIMP MiniMixMediaStream::GetMediaSource(IMFMediaSource** source) {
  if (!source) return E_POINTER;
  *source = nullptr;
  auto* media_source = AcquireSource();
  if (!media_source) return MF_E_SHUTDOWN;
  const HRESULT hr = media_source->QueryInterface(
      __uuidof(IMFMediaSource), reinterpret_cast<void**>(source));
  media_source->Release();
  return hr;
}
STDMETHODIMP MiniMixMediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
  if (!descriptor) return E_POINTER;
  *descriptor = descriptor_;
  if (descriptor_) descriptor_->AddRef();
  return descriptor_ ? S_OK : MF_E_SHUTDOWN;
}
STDMETHODIMP MiniMixMediaStream::RequestSample(IUnknown* token) {
  auto* source = AcquireSource();
  if (!source) return MF_E_SHUTDOWN;
  const HRESULT hr = source->QueueSampleRequest(token);
  source->Release();
  return hr;
}

STDMETHODIMP MiniMixMediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
  if (!descriptor) return E_POINTER;
  *descriptor = nullptr;
  IMFStreamDescriptor* current_descriptor = nullptr;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (state_ == SourceState::shutdown || !stream_) return MF_E_SHUTDOWN;
    current_descriptor = stream_->descriptor();
    if (!current_descriptor) return MF_E_SHUTDOWN;
    current_descriptor->AddRef();
  }
  HRESULT hr = MFCreatePresentationDescriptor(1, &current_descriptor, descriptor);
  current_descriptor->Release();
  if (SUCCEEDED(hr)) hr = (*descriptor)->SelectStream(0);
  if (FAILED(hr) && *descriptor) {
    (*descriptor)->Release();
    *descriptor = nullptr;
  }
  return hr;
}
STDMETHODIMP MiniMixMediaSource::Start(IMFPresentationDescriptor* presentation,
                                       const GUID* time_format, const PROPVARIANT*) {
  if (time_format && *time_format != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
  if (!presentation) return E_POINTER;

  DWORD descriptor_count = 0;
  HRESULT hr = presentation->GetStreamDescriptorCount(&descriptor_count);
  if (FAILED(hr)) return hr;
  if (descriptor_count != 1) return MF_E_INVALIDSTREAMNUMBER;

  BOOL selected = FALSE;
  IMFStreamDescriptor* descriptor = nullptr;
  hr = presentation->GetStreamDescriptorByIndex(0, &selected, &descriptor);
  if (FAILED(hr)) return hr;
  DWORD stream_id = 0;
  hr = descriptor->GetStreamIdentifier(&stream_id);
  descriptor->Release();
  if (FAILED(hr)) return hr;
  if (stream_id != 0) return MF_E_INVALIDSTREAMNUMBER;

  std::vector<SampleRequest*> requests_to_schedule;
  std::vector<SampleRequest*> requests_to_discard;
  std::unique_lock<std::mutex> sample_guard(sample_lock_);
  SourceState previous_state;
  bool previous_stream_selected = false;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (state_ == SourceState::shutdown) return MF_E_SHUTDOWN;
    previous_state = state_;
    previous_stream_selected = stream_selected_;
    state_ = SourceState::started;
    stream_selected_ = selected != FALSE;
    if (previous_state == SourceState::stopped) next_sample_time_ = 0;
  }

  if (stream_selected_) {
    const auto stream_event = stream_announced_ ? MEUpdatedStream : MENewStream;
    hr = events_->QueueEventParamUnk(stream_event, GUID_NULL, S_OK, stream_);
    if (SUCCEEDED(hr)) {
      stream_announced_ = true;
      debug_log(stream_event == MENewStream ? "MENewStream" : "MEUpdatedStream");
    }
  }
  if (SUCCEEDED(hr)) {
    hr = QueueEvent(MESourceStarted, GUID_NULL, S_OK, nullptr);
    if (SUCCEEDED(hr)) debug_log("MESourceStarted");
  }
  if (SUCCEEDED(hr) && stream_selected_) hr = stream_->Start();
  if (FAILED(hr)) {
    std::lock_guard<std::mutex> guard(lock_);
    state_ = previous_state;
    stream_selected_ = previous_stream_selected;
    debug_log("Start failed: " + std::to_string(static_cast<unsigned long>(hr)));
    return hr;
  }

  debug_log("Start output " + std::to_string(output_id_) +
            (stream_selected_ ? " with selected video stream" : " with no selected stream"));
  {
    std::lock_guard<std::mutex> guard(lock_);
    last_sample_error_ = S_OK;
    while (!paused_requests_.empty()) {
      auto* request = paused_requests_.front();
      paused_requests_.pop_front();
      if (stream_selected_) {
        request->generation = sample_generation_;
        requests_to_schedule.push_back(request);
      } else {
        requests_to_discard.push_back(request);
      }
    }
  }
  sample_guard.unlock();

  for (auto* request : requests_to_discard) ReleaseSampleRequest(request);
  for (auto* request : requests_to_schedule) {
    const HRESULT schedule_result = ScheduleSampleRequest(request);
    if (FAILED(schedule_result)) {
      debug_log("Could not resume a queued sample request: " +
                std::to_string(static_cast<unsigned long>(schedule_result)));
      QueueSampleError(schedule_result);
    }
  }
  return S_OK;
}
STDMETHODIMP MiniMixMediaSource::Stop() {
  debug_log("Stop output " + std::to_string(output_id_));
  std::deque<SampleRequest*> paused_requests;
  bool stop_stream = false;
  std::unique_lock<std::mutex> sample_guard(sample_lock_);
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (state_ == SourceState::shutdown) return MF_E_SHUTDOWN;
    stop_stream = stream_selected_;
    stream_selected_ = false;
    state_ = SourceState::stopped;
    ++sample_generation_;
    paused_requests.swap(paused_requests_);
  }
  HRESULT hr = stop_stream ? stream_->Stop() : S_OK;
  if (SUCCEEDED(hr)) hr = QueueEvent(MESourceStopped, GUID_NULL, S_OK, nullptr);
  sample_guard.unlock();
  for (auto* request : paused_requests) ReleaseSampleRequest(request);
  return hr;
}
STDMETHODIMP MiniMixMediaSource::Pause() {
  debug_log("Pause output " + std::to_string(output_id_));
  bool pause_stream = false;
  std::unique_lock<std::mutex> sample_guard(sample_lock_);
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (state_ == SourceState::shutdown) return MF_E_SHUTDOWN;
    pause_stream = stream_selected_;
    state_ = SourceState::paused;
  }
  HRESULT hr = pause_stream ? stream_->Pause() : S_OK;
  if (SUCCEEDED(hr)) hr = QueueEvent(MESourcePaused, GUID_NULL, S_OK, nullptr);
  sample_guard.unlock();
  return hr;
}
STDMETHODIMP MiniMixMediaSource::Shutdown() {
  debug_log("Shutdown output " + std::to_string(output_id_));
  std::deque<SampleRequest*> paused_requests;
  MiniMixMediaStream* stream = nullptr;
  {
    std::lock_guard<std::mutex> sample_guard(sample_lock_);
    std::lock_guard<std::mutex> guard(lock_);
    if (state_ == SourceState::shutdown) return S_OK;
    state_ = SourceState::shutdown;
    stream_selected_ = false;
    ++sample_generation_;
    paused_requests.swap(paused_requests_);
    if (events_) events_->Shutdown();
    if (events_) events_->Release();
    events_ = nullptr;
    stream = stream_;
    stream_ = nullptr;
    if (source_attributes_) source_attributes_->Release();
    source_attributes_ = nullptr;
    if (stream_attributes_) stream_attributes_->Release();
    stream_attributes_ = nullptr;
  }
  for (auto* request : paused_requests) ReleaseSampleRequest(request);
  if (stream) {
    stream->Shutdown();
    stream->DetachSource();
    stream->Release();
  }
  return S_OK;
}
IMFMediaEventQueue* MiniMixMediaSource::AcquireEventQueue() const {
  std::lock_guard<std::mutex> guard(lock_);
  if (events_) events_->AddRef();
  return events_;
}

STDMETHODIMP MiniMixMediaSource::GetSourceAttributes(IMFAttributes** attributes) {
  if (!attributes) return E_POINTER;
  *attributes = nullptr;
  std::lock_guard<std::mutex> guard(lock_);
  if (state_ == SourceState::shutdown || !source_attributes_) return MF_E_SHUTDOWN;
  source_attributes_->AddRef();
  *attributes = source_attributes_;
  return S_OK;
}
STDMETHODIMP MiniMixMediaSource::GetStreamAttributes(DWORD stream_id, IMFAttributes** attributes) {
  if (!attributes) return E_POINTER;
  *attributes = nullptr;
  std::lock_guard<std::mutex> guard(lock_);
  if (state_ == SourceState::shutdown || !stream_attributes_) return MF_E_SHUTDOWN;
  if (stream_id != 0) return MF_E_INVALIDSTREAMNUMBER;
  stream_attributes_->AddRef();
  *attributes = stream_attributes_;
  return S_OK;
}
STDMETHODIMP MiniMixMediaSource::SetD3DManager(IUnknown*) { return E_NOTIMPL; }
STDMETHODIMP MiniMixMediaSource::GetService(REFGUID, REFIID, LPVOID* object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  return MF_E_UNSUPPORTED_SERVICE;
}
STDMETHODIMP MiniMixMediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MiniMixMediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MiniMixMediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MiniMixMediaSource::SetDefaultAllocator(DWORD, IUnknown*) { return E_NOTIMPL; }
STDMETHODIMP MiniMixMediaSource::GetAllocatorUsage(
    DWORD output_stream_id, DWORD* input_stream_id, MFSampleAllocatorUsage* usage) {
  if (!input_stream_id || !usage) return E_POINTER;
  if (output_stream_id != 0) return MF_E_INVALIDSTREAMNUMBER;
  *input_stream_id = output_stream_id;
  *usage = MFSampleAllocatorUsage_UsesCustomAllocator;
  return S_OK;
}
HRESULT MiniMixMediaSource::output_dimensions(UINT32* width, UINT32* height) {
  if (!width || !height) return E_POINTER;
  IMFMediaTypeHandler* handler = nullptr;
  IMFMediaType* type = nullptr;
  HRESULT hr = stream_->descriptor()->GetMediaTypeHandler(&handler);
  if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(&type);
  if (SUCCEEDED(hr)) hr = MFGetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
  if (type) type->Release();
  if (handler) handler->Release();
  return hr;
}
void MiniMixMediaSource::ReleaseSampleRequest(SampleRequest* request) {
  if (request->token_stream) request->token_stream->Release();
  pending_samples_.fetch_sub(1, std::memory_order_release);
  Release();
  delete request;
}

HRESULT MiniMixMediaSource::ScheduleSampleRequest(SampleRequest* request) {
  if (!QueueUserWorkItem(ProcessSampleRequest, request, WT_EXECUTEDEFAULT)) {
    const DWORD error = GetLastError();
    ReleaseSampleRequest(request);
    return HRESULT_FROM_WIN32(error);
  }
  return S_OK;
}

HRESULT MiniMixMediaSource::QueueSampleRequest(IUnknown* token) {
  static std::atomic<std::uint64_t> request_count{0};
  const auto request_number = ++request_count;
  if (request_number <= 3 || request_number % 30 == 0) {
    debug_log("RequestSample output " + std::to_string(output_id_) +
              " (#" + std::to_string(request_number) + ")");
  }

  {
    std::lock_guard<std::mutex> guard(lock_);
    if (state_ == SourceState::shutdown) return MF_E_SHUTDOWN;
    if (state_ == SourceState::stopped || !stream_selected_) {
      return MF_E_MEDIA_SOURCE_WRONGSTATE;
    }
  }

  constexpr std::uint32_t kMaxPendingSamples = 8;
  std::uint32_t pending = pending_samples_.load(std::memory_order_relaxed);
  do {
    if (pending >= kMaxPendingSamples) return MF_E_NOTACCEPTING;
  } while (!pending_samples_.compare_exchange_weak(
      pending, pending + 1, std::memory_order_acq_rel, std::memory_order_relaxed));

  IStream* token_stream = nullptr;
  if (token) {
    const HRESULT marshal_result =
        CoMarshalInterThreadInterfaceInStream(IID_IUnknown, token, &token_stream);
    if (FAILED(marshal_result)) {
      pending_samples_.fetch_sub(1, std::memory_order_release);
      return marshal_result;
    }
  }
  auto* request = new (std::nothrow) SampleRequest{this, token_stream, 0};
  if (!request) {
    if (token_stream) token_stream->Release();
    pending_samples_.fetch_sub(1, std::memory_order_release);
    return E_OUTOFMEMORY;
  }

  {
    std::lock_guard<std::mutex> guard(lock_);
    if (state_ == SourceState::shutdown || state_ == SourceState::stopped || !stream_selected_) {
      const HRESULT state_error = state_ == SourceState::shutdown
          ? MF_E_SHUTDOWN : MF_E_MEDIA_SOURCE_WRONGSTATE;
      if (token_stream) token_stream->Release();
      delete request;
      pending_samples_.fetch_sub(1, std::memory_order_release);
      return state_error;
    }
    request->generation = sample_generation_;
    AddRef();
    if (state_ == SourceState::paused) {
      paused_requests_.push_back(request);
      return S_OK;
    }
  }
  return ScheduleSampleRequest(request);
}

DWORD WINAPI MiniMixMediaSource::ProcessSampleRequest(void* context) {
  auto* request = static_cast<SampleRequest*>(context);
  MiniMixMediaSource* source = request->source;

  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  HRESULT result = apartment;
  bool requeued = false;
  if (SUCCEEDED(apartment)) {
    std::lock_guard<std::mutex> sample_guard(source->sample_lock_);
    {
      std::lock_guard<std::mutex> guard(source->lock_);
      if (source->state_ == SourceState::paused) {
        source->paused_requests_.push_back(request);
        requeued = true;
      } else if (source->state_ != SourceState::started || !source->stream_selected_) {
        result = MF_E_INVALIDREQUEST;
      }
    }
    if (!requeued && result != MF_E_INVALIDREQUEST) {
      IUnknown* token = nullptr;
      result = request->token_stream
          ? CoGetInterfaceAndReleaseStream(
                request->token_stream, IID_IUnknown, reinterpret_cast<void**>(&token))
          : S_OK;
      request->token_stream = nullptr;
      if (SUCCEEDED(result)) {
        result = source->BuildAndQueueSampleLocked(token, request->generation);
        if (FAILED(result) && result != MF_E_INVALIDREQUEST) source->QueueSampleError(result);
      } else {
        source->QueueSampleError(result);
      }
      if (token) token->Release();
    }
    CoUninitialize();
  } else {
    source->QueueSampleError(result);
  }

  if (!requeued) source->ReleaseSampleRequest(request);
  return 0;
}

void MiniMixMediaSource::QueueSampleError(HRESULT error) {
  IMFMediaEventQueue* events = nullptr;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (state_ == SourceState::shutdown || !events_ || last_sample_error_ == error) return;
    last_sample_error_ = error;
    events = events_;
    events->AddRef();
  }
  debug_log("Asynchronous sample error: " +
            std::to_string(static_cast<unsigned long>(error)));
  events->QueueEventParamVar(MEError, GUID_NULL, error, nullptr);
  events->Release();
}

HRESULT MiniMixMediaSource::BuildAndQueueSampleLocked(IUnknown* token, std::uint64_t generation) {
  static std::atomic<std::uint64_t> sample_count{0};
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (state_ != SourceState::started || generation != sample_generation_) {
      return MF_E_INVALIDREQUEST;
    }
  }
  UINT32 width = 0, height = 0;
  HRESULT hr = output_dimensions(&width, &height);
  if (SUCCEEDED(hr) && (!width || !height ||
      width > minimix::vcam::kMaxWidth || height > minimix::vcam::kMaxHeight)) {
    hr = E_INVALIDARG;
  }
  IMFSample* sample = nullptr;
  IMFMediaBuffer* buffer = nullptr;
  BYTE* data = nullptr;
  DWORD max_length = 0, current_length = 0;
  if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
  if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(width * height * 4, &buffer);
  if (SUCCEEDED(hr)) hr = buffer->Lock(&data, &max_length, &current_length);
  if (SUCCEEDED(hr)) {
    const auto frame_bytes = static_cast<std::size_t>(width) * height * 4;
    if (last_frame_.size() != frame_bytes) {
      last_frame_.resize(frame_bytes);
      fill_color_bars(last_frame_.data(), width, height);
      has_valid_frame_ = false;
    }
    const bool copied = minimix::vcam::internal::copy_latest_frame(output_id_, data, width, height);
    if (copied) {
      std::memcpy(last_frame_.data(), data, frame_bytes);
      has_valid_frame_ = true;
    } else {
      if (has_valid_frame_) {
        std::memcpy(data, last_frame_.data(), frame_bytes);
      } else {
        fill_color_bars(data, width, height);
        std::memcpy(last_frame_.data(), data, frame_bytes);
      }
    }
    const auto frame_number = ++sample_count;
    if (frame_number <= 3 || frame_number % 30 == 0) {
      debug_log("BuildAndQueueSample output " + std::to_string(output_id_) +
                ": copy_latest_frame=" + (copied ? "success" : "failure") +
                ", dimensions=" + std::to_string(width) + "x" + std::to_string(height) +
                ", timestamp=" + std::to_string(next_sample_time_));
    }
    hr = buffer->Unlock();
    data = nullptr;
  }
  if (SUCCEEDED(hr)) hr = buffer->SetCurrentLength(width * height * 4);
  if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
  if (SUCCEEDED(hr) && token) hr = sample->SetUnknown(MFSampleExtension_Token, token);
  if (SUCCEEDED(hr)) hr = sample->SetSampleTime(next_sample_time_);
  if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(333333);
  if (SUCCEEDED(hr)) hr = sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
  if (SUCCEEDED(hr)) next_sample_time_ += 333333;
  PROPVARIANT value;
  PropVariantInit(&value);
  if (SUCCEEDED(hr)) {
    value.vt = VT_UNKNOWN;
    value.punkVal = sample;
    sample->AddRef();
    hr = stream_->QueueEvent(MEMediaSample, GUID_NULL, S_OK, &value);
  }
  PropVariantClear(&value);
  if (data && buffer) buffer->Unlock();
  if (buffer) buffer->Release();
  if (sample) sample->Release();
  if (SUCCEEDED(hr)) {
    std::lock_guard<std::mutex> guard(lock_);
    last_sample_error_ = S_OK;
  }
  return hr;
}

class ClassFactory final : public IClassFactory {
 public:
  explicit ClassFactory(std::uint8_t output_id) : output_id_(output_id) {}
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_IClassFactory) {
      *object = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG references = --references_;
    if (!references) delete this;
    return references;
  }
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!object) return E_POINTER;
    *object = nullptr;
    auto* activate = new (std::nothrow) MiniMixMediaSourceActivate(output_id_);
    if (!activate) return E_OUTOFMEMORY;
    HRESULT hr = activate->Initialize();
    if (SUCCEEDED(hr)) hr = activate->QueryInterface(iid, object);
    activate->Release();
    return hr;
  }
  STDMETHODIMP LockServer(BOOL) override { return S_OK; }
 private:
  ~ClassFactory() = default;
  std::atomic<ULONG> references_{1};
  const std::uint8_t output_id_;
};

}  // namespace

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  for (std::uint8_t output_id = 1; output_id <= 4; ++output_id) {
    if (clsid == minimix::vcam::kVirtualCameraSourceClsids[output_id - 1]) {
      auto* factory = new (std::nothrow) ClassFactory(output_id);
      if (!factory) return E_OUTOFMEMORY;
      const HRESULT hr = factory->QueryInterface(iid, object);
      factory->Release();
      return hr;
    }
  }
  return CLASS_E_CLASSNOTAVAILABLE;
}
extern "C" HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }
