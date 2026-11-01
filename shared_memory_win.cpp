#define NOMINMAX
#include "minimix/frame_protocol.h"
#include "shared_memory_win.h"

#include <windows.h>
#include <wincodec.h>

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

using minimix::vcam::FrameHeader;
using minimix::vcam::FrameSlotHeader;

std::wstring mapping_name(std::uint8_t output_id) {
  return std::wstring(L"Local\\MiniMixVcam.Frame.") + std::to_wstring(output_id) + L".v2";
}

struct SharedMemory {
  HANDLE mapping = nullptr;
  void* view = nullptr;
  std::atomic<int> initialization_state{0};
  std::atomic<std::uint32_t> next_write_slot{0};
  ~SharedMemory() {
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
  }
};

std::array<SharedMemory, 4>& resources() {
  static std::array<SharedMemory, 4> instances;
  return instances;
}

bool valid_header(const FrameHeader& header) {
  return std::memcmp(header.magic, minimix::vcam::kFrameMagic, sizeof(header.magic)) == 0 &&
      header.version == minimix::vcam::kProtocolVersion &&
      header.header_size == minimix::vcam::kHeaderSize &&
      header.slot_count == minimix::vcam::kSlotCount &&
      header.pixel_format == minimix::vcam::kPixelFormatBgra8 &&
      header.slot_header_size == minimix::vcam::kSlotHeaderSize &&
      header.reserved32 == 0 &&
      header.slot_stride == minimix::vcam::kSlotStride &&
      header.capacity_bytes == minimix::vcam::kMaxFrameBytes &&
      header.initialization_state == 2;
}

volatile LONG64* interlocked_state(std::uint64_t* value) {
  return reinterpret_cast<volatile LONG64*>(value);
}

FrameSlotHeader* slot_at(void* view, std::uint32_t index) {
  auto* address = static_cast<std::uint8_t*>(view) + minimix::vcam::kHeaderSize +
      static_cast<std::size_t>(index) * minimix::vcam::kSlotStride;
  return reinterpret_cast<FrameSlotHeader*>(address);
}

std::uint8_t* slot_pixels(FrameSlotHeader* slot) {
  return reinterpret_cast<std::uint8_t*>(slot) + minimix::vcam::kSlotHeaderSize;
}

bool valid_frame(const FrameSlotHeader& slot) {
  return slot.width != 0 && slot.height != 0 &&
      slot.width <= minimix::vcam::kMaxWidth &&
      slot.height <= minimix::vcam::kMaxHeight &&
      slot.width <= (std::numeric_limits<std::uint32_t>::max)() / 4 &&
      slot.stride >= slot.width * 4 &&
      slot.pixel_format == minimix::vcam::kPixelFormatBgra8 &&
      slot.frame_bytes == static_cast<std::uint64_t>(slot.stride) * slot.height &&
      slot.frame_bytes <= minimix::vcam::kMaxFrameBytes;
}

void fill_color_bars(std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t stride) {
  // SMPTE color bars in BGRA format
  const struct { uint8_t r, g, b; } colors[] = {
    {191, 191, 191}, // gray
    {191, 191, 0},   // yellow
    {0, 191, 191},   // cyan
    {0, 191, 0},     // green
    {191, 0, 191},   // magenta
    {191, 0, 0},     // red
    {0, 0, 191},     // blue
  };
  const uint32_t bar_count = 7;
  const uint32_t bar_width = width / bar_count;
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* row = pixels + static_cast<std::size_t>(y) * stride;
    for (uint32_t i = 0; i < bar_count; ++i) {
      uint32_t start_x = i * bar_width;
      uint32_t end_x = (i == bar_count - 1) ? width : (i + 1) * bar_width;
      uint8_t r = colors[i].r;
      uint8_t g = colors[i].g;
      uint8_t b = colors[i].b;
      for (uint32_t x = start_x; x < end_x; ++x) {
        uint8_t* p = row + static_cast<std::size_t>(x) * 4;
        p[0] = b;   // B
        p[1] = g;   // G
        p[2] = r;   // R
        p[3] = 255; // A
      }
    }
  }
}

void initialize_mapping(FrameHeader* header, void* view) {
  std::memcpy(header->magic, minimix::vcam::kFrameMagic, sizeof(header->magic));
  header->version = minimix::vcam::kProtocolVersion;
  header->header_size = minimix::vcam::kHeaderSize;
  header->slot_count = minimix::vcam::kSlotCount;
  header->pixel_format = minimix::vcam::kPixelFormatBgra8;
  header->slot_header_size = minimix::vcam::kSlotHeaderSize;
  header->reserved32 = 0;
  header->slot_stride = minimix::vcam::kSlotStride;
  header->next_sequence = 1;
  header->capacity_bytes = minimix::vcam::kMaxFrameBytes;

  auto* initial = slot_at(view, 0);
  initial->width = 1280;
  initial->height = 720;
  initial->stride = 1280 * 4;
  initial->pixel_format = minimix::vcam::kPixelFormatBgra8;
  initial->frame_bytes = static_cast<std::uint64_t>(initial->stride) * initial->height;
  fill_color_bars(slot_pixels(initial), initial->width, initial->height, initial->stride);
  InterlockedExchange64(interlocked_state(&initial->state), 1LL << 8);
  InterlockedExchange64(interlocked_state(&header->initialization_state), 2);
}

int initialize(std::uint8_t output_id) {
  if (!minimix::vcam::valid_output_id(output_id)) return 1;
  auto& state = resources()[output_id - 1];
  int observed = state.initialization_state.load(std::memory_order_acquire);
  if (observed == 2) return 0;
  if (observed < 0) return -observed;

  int uninitialized = 0;
  if (!state.initialization_state.compare_exchange_strong(
          uninitialized, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
    for (int attempt = 0; attempt < 100; ++attempt) {
      observed = state.initialization_state.load(std::memory_order_acquire);
      if (observed == 2) return 0;
      if (observed < 0) return -observed;
      Sleep(1);
    }
    return 5;
  }

  const auto finish = [&state](int error) {
    state.initialization_state.store(error == 0 ? 2 : -error, std::memory_order_release);
    return error;
  };

  const auto map_name = mapping_name(output_id);
  SetLastError(ERROR_SUCCESS);
  state.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
      static_cast<DWORD>(minimix::vcam::kMappingSize >> 32),
      static_cast<DWORD>(minimix::vcam::kMappingSize & 0xffffffff), map_name.c_str());
  if (!state.mapping) return finish(2);
  state.view = MapViewOfFile(state.mapping, FILE_MAP_ALL_ACCESS, 0, 0,
      static_cast<SIZE_T>(minimix::vcam::kMappingSize));
  if (!state.view) return finish(2);

  auto* header = static_cast<FrameHeader*>(state.view);
  auto* init_state = interlocked_state(&header->initialization_state);
  LONG64 mapping_state = InterlockedCompareExchange64(init_state, 0, 0);
  if (mapping_state != 2 && mapping_state == 0 &&
      InterlockedCompareExchange64(init_state, 1, 0) == 0) {
    initialize_mapping(header, state.view);
    mapping_state = 2;
  } else if (mapping_state != 2) {
    for (int attempt = 0; attempt < 100 && mapping_state != 2; ++attempt) {
      Sleep(1);
      mapping_state = InterlockedCompareExchange64(init_state, 0, 0);
    }
  }

  if (mapping_state != 2) return finish(5);
  if (!valid_header(*header)) return finish(4);
  return finish(0);
}

int publish_frame(std::uint8_t output_id, const std::uint8_t* pixels,
                  std::uint32_t width, std::uint32_t height,
                  std::uint32_t stride, bool black) {
  if (!minimix::vcam::valid_output_id(output_id) || (!pixels && !black) || !width || !height ||
      width > minimix::vcam::kMaxWidth || height > minimix::vcam::kMaxHeight ||
      width > (std::numeric_limits<std::uint32_t>::max)() / 4 || stride < width * 4) return 1;
  const std::uint64_t frame_bytes = static_cast<std::uint64_t>(stride) * height;
  if (frame_bytes > minimix::vcam::kMaxFrameBytes) return 1;
  const int initialized = initialize(output_id);
  if (initialized) return initialized;

  auto& resources_for_output = resources()[output_id - 1];
  auto* header = static_cast<FrameHeader*>(resources_for_output.view);
  const auto start = resources_for_output.next_write_slot.fetch_add(
      1, std::memory_order_relaxed) % minimix::vcam::kSlotCount;
  for (std::uint32_t offset = 0; offset < minimix::vcam::kSlotCount; ++offset) {
    const auto index = (start + offset) % minimix::vcam::kSlotCount;
    auto* slot = slot_at(resources_for_output.view, index);
    auto* slot_state = interlocked_state(&slot->state);
    const LONG64 current = InterlockedCompareExchange64(slot_state, 0, 0);
    const auto current_bits = static_cast<std::uint64_t>(current);
    if ((current_bits & (minimix::vcam::kSlotWriting | minimix::vcam::kSlotReaderMask)) != 0) {
      continue;
    }
    if (InterlockedCompareExchange64(slot_state, minimix::vcam::kSlotWriting, current) != current) {
      continue;
    }

    slot->width = width;
    slot->height = height;
    slot->stride = stride;
    slot->pixel_format = minimix::vcam::kPixelFormatBgra8;
    slot->frame_bytes = frame_bytes;
    if (black) {
      std::memset(slot_pixels(slot), 0, static_cast<std::size_t>(frame_bytes));
    } else {
      std::memcpy(slot_pixels(slot), pixels, static_cast<std::size_t>(frame_bytes));
    }

    auto sequence = static_cast<std::uint64_t>(
        InterlockedIncrement64(interlocked_state(&header->next_sequence)));
    sequence &= minimix::vcam::kSlotSequenceMask;
    if (sequence == 0) sequence = 1;
    InterlockedExchange64(
        slot_state, static_cast<LONG64>(sequence << 8));
    return 0;
  }
  return 5;
}

int publish_bgra(std::uint8_t output_id, const std::uint8_t* pixels,
                 std::uint32_t width, std::uint32_t height, std::uint32_t stride) {
  return publish_frame(output_id, pixels, width, height, stride, false);
}

class SlotReadGuard {
 public:
  explicit SlotReadGuard(volatile LONG64* state) : state_(state) {}
  ~SlotReadGuard() { InterlockedExchangeAdd64(state_, -1); }
  SlotReadGuard(const SlotReadGuard&) = delete;
  SlotReadGuard& operator=(const SlotReadGuard&) = delete;

 private:
  volatile LONG64* state_;
};

FrameSlotHeader* pin_latest_slot(void* view) {
  for (std::uint32_t attempt = 0; attempt < minimix::vcam::kSlotCount * 2; ++attempt) {
    FrameSlotHeader* latest = nullptr;
    std::uint64_t latest_sequence = 0;
    for (std::uint32_t index = 0; index < minimix::vcam::kSlotCount; ++index) {
      auto* slot = slot_at(view, index);
      const auto bits = static_cast<std::uint64_t>(
          InterlockedCompareExchange64(interlocked_state(&slot->state), 0, 0));
      const auto sequence = minimix::vcam::slot_sequence(bits);
      if ((bits & minimix::vcam::kSlotWriting) == 0 && sequence > latest_sequence) {
        latest = slot;
        latest_sequence = sequence;
      }
    }
    if (!latest) return nullptr;

    auto* slot_state = interlocked_state(&latest->state);
    const LONG64 observed = InterlockedCompareExchange64(slot_state, 0, 0);
    const auto bits = static_cast<std::uint64_t>(observed);
    if ((bits & minimix::vcam::kSlotWriting) != 0 ||
        minimix::vcam::slot_sequence(bits) != latest_sequence ||
        (bits & minimix::vcam::kSlotReaderMask) == minimix::vcam::kSlotReaderMask) {
      continue;
    }
    if (InterlockedCompareExchange64(slot_state, observed + 1, observed) == observed) {
      return latest;
    }
  }
  return nullptr;
}

bool copy_frame(std::uint8_t output_id, std::uint8_t* destination,
                std::uint32_t target_width, std::uint32_t target_height) {
  if (!destination || !target_width || !target_height ||
      target_width > minimix::vcam::kMaxWidth ||
      target_height > minimix::vcam::kMaxHeight) {
    return false;
  }
  const std::size_t target_stride = static_cast<std::size_t>(target_width) * 4;
  std::memset(destination, 0, target_stride * target_height);
  if (initialize(output_id) != 0) return false;

  auto* slot = pin_latest_slot(resources()[output_id - 1].view);
  if (!slot) return false;
  SlotReadGuard guard(interlocked_state(&slot->state));
  if (!valid_frame(*slot)) return false;

  const auto* source = slot_pixels(slot);
  if (slot->width == target_width && slot->height == target_height) {
    std::memcpy(destination, source, static_cast<std::size_t>(slot->frame_bytes));
    return true;
  }

  for (std::uint32_t y = 0; y < target_height; ++y) {
    const std::uint32_t source_y =
        static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * slot->height / target_height);
    auto* row = destination + static_cast<std::size_t>(y) * target_stride;
    const auto* source_row = source + static_cast<std::size_t>(source_y) * slot->stride;
    for (std::uint32_t x = 0; x < target_width; ++x) {
      const std::uint32_t source_x =
          static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * slot->width / target_width);
      std::memcpy(row + static_cast<std::size_t>(x) * 4,
                  source_row + static_cast<std::size_t>(source_x) * 4, 4);
    }
  }
  return true;
}

int decode_and_publish_jpeg(std::uint8_t output_id, const std::uint8_t* jpeg,
                            std::uint32_t jpeg_length) {
  if (!minimix::vcam::valid_output_id(output_id) || !jpeg || !jpeg_length) return 1;
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitialize = SUCCEEDED(apartment);
  IWICImagingFactory* factory = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* converter = nullptr;

  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
  if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(const_cast<BYTE*>(jpeg), jpeg_length);
  if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(stream, nullptr,
      WICDecodeMetadataCacheOnDemand, &decoder);
  if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
  if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
  if (SUCCEEDED(hr)) hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
      WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
  UINT width = 0, height = 0;
  if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);
  if (SUCCEEDED(hr) && (!width || !height || width > minimix::vcam::kMaxWidth ||
      height > minimix::vcam::kMaxHeight)) hr = E_INVALIDARG;
  const std::uint32_t stride = width * 4;
  std::vector<std::uint8_t> pixels;
  if (SUCCEEDED(hr)) {
    pixels.resize(static_cast<std::size_t>(stride) * height);
    hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
  }
  const int result = SUCCEEDED(hr) ? publish_bgra(output_id, pixels.data(), width, height, stride) : 6;
  if (converter) converter->Release();
  if (frame) frame->Release();
  if (decoder) decoder->Release();
  if (stream) stream->Release();
  if (factory) factory->Release();
  if (uninitialize) CoUninitialize();
  return result;
}

}  // namespace

namespace minimix::vcam::internal {

int initialize_frame_channel(std::uint8_t output_id) {
  return initialize(output_id);
}

bool copy_latest_frame(std::uint8_t output_id, std::uint8_t* destination,
                       std::uint32_t target_width, std::uint32_t target_height) {
  return copy_frame(output_id, destination, target_width, target_height);
}

}  // namespace minimix::vcam::internal

extern "C" int minimix_vcam_publish_bgra(const std::uint8_t* pixels, std::uint32_t width,
                                          std::uint32_t height, std::uint32_t stride) {
  return publish_bgra(1, pixels, width, height, stride);
}
extern "C" int minimix_vcam_publish_jpeg(std::uint8_t output_id, const std::uint8_t* jpeg,
                                          std::uint32_t jpeg_length) {
  return decode_and_publish_jpeg(output_id, jpeg, jpeg_length);
}
extern "C" int minimix_vcam_clear(std::uint8_t output_id) {
  constexpr std::uint32_t width = 1280;
  constexpr std::uint32_t height = 720;
  if (!minimix::vcam::valid_output_id(output_id)) return 1;
  const int initialized = initialize(output_id);
  if (initialized) return initialized;

  auto& resources_for_output = resources()[output_id - 1];
  auto* header = static_cast<FrameHeader*>(resources_for_output.view);
  const auto start = resources_for_output.next_write_slot.fetch_add(
      1, std::memory_order_relaxed) % minimix::vcam::kSlotCount;
  for (std::uint32_t offset = 0; offset < minimix::vcam::kSlotCount; ++offset) {
    const auto index = (start + offset) % minimix::vcam::kSlotCount;
    auto* slot = slot_at(resources_for_output.view, index);
    auto* slot_state = interlocked_state(&slot->state);
    const LONG64 current = InterlockedCompareExchange64(slot_state, 0, 0);
    const auto current_bits = static_cast<std::uint64_t>(current);
    if ((current_bits & (minimix::vcam::kSlotWriting | minimix::vcam::kSlotReaderMask)) != 0) {
      continue;
    }
    if (InterlockedCompareExchange64(slot_state, minimix::vcam::kSlotWriting, current) != current) {
      continue;
    }

    slot->width = width;
    slot->height = height;
    slot->stride = width * 4;
    slot->pixel_format = minimix::vcam::kPixelFormatBgra8;
    slot->frame_bytes = static_cast<std::uint64_t>(slot->stride) * height;
    fill_color_bars(slot_pixels(slot), width, height, slot->stride);

    auto sequence = static_cast<std::uint64_t>(
        InterlockedIncrement64(interlocked_state(&header->next_sequence)));
    sequence &= minimix::vcam::kSlotSequenceMask;
    if (sequence == 0) sequence = 1;
    InterlockedExchange64(slot_state, static_cast<LONG64>(sequence << 8));
    return 0;
  }
  return 5;
}
