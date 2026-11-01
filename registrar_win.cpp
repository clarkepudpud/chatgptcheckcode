#include "minimix/virtual_camera.h"

#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>

#include <array>
#include <iostream>
#include <string>
#include <vector>

namespace {

using CreateVirtualCamera = HRESULT (WINAPI *)(
    MFVirtualCameraType,
    MFVirtualCameraLifetime,
    MFVirtualCameraAccess,
    LPCWSTR,
    LPCWSTR,
    const GUID*,
    ULONG,
    IMFVirtualCamera**);

struct VirtualCameraApi {
  HMODULE module = nullptr;
  CreateVirtualCamera create = nullptr;
};

VirtualCameraApi load_virtual_camera_api() {
  VirtualCameraApi api;
  api.module = LoadLibraryW(L"mfsensorgroup.dll");
  if (!api.module) return api;
  api.create = reinterpret_cast<CreateVirtualCamera>(
      GetProcAddress(api.module, "MFCreateVirtualCamera"));
  if (!api.create) {
    FreeLibrary(api.module);
    api.module = nullptr;
  }
  return api;
}

std::wstring executable_directory() {
  std::array<wchar_t, 32768> path{};
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) return L"";
  std::wstring result(path.data(), length);
  const auto slash = result.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"" : result.substr(0, slash);
}

std::wstring class_key(std::size_t index) {
  return std::wstring(L"Software\\Classes\\CLSID\\") +
      minimix::vcam::kVirtualCameraSourceIds[index] + L"\\InprocServer32";
}

bool source_is_registered() {
  std::wstring registered_path;
  for (std::size_t index = 0; index < std::size(minimix::vcam::kVirtualCameraSourceIds); ++index) {
    HKEY key = nullptr;
    const LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, class_key(index).c_str(), 0, KEY_READ, &key);
    if (status != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD bytes = 0;
    LONG query = RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &bytes);
    if (query != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)) {
      RegCloseKey(key);
      return false;
    }
    std::wstring path(bytes / sizeof(wchar_t), L'\0');
    query = RegQueryValueExW(
        key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(path.data()), &bytes);
    RegCloseKey(key);
    if (query != ERROR_SUCCESS || path.empty() || path.back() != L'\0') return false;
    path.pop_back();
    if (registered_path.empty()) {
      registered_path = path;
    } else if (_wcsicmp(registered_path.c_str(), path.c_str()) != 0) {
      return false;
    }
  }
  return !registered_path.empty() &&
      GetFileAttributesW(registered_path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

HRESULT probe_registered_source(std::size_t index, const wchar_t** failed_stage) {
  *failed_stage = L"CoCreateInstance(IMFActivate)";
  IMFActivate* activate = nullptr;
  IMFMediaSource* source = nullptr;
  IMFMediaSourceEx* source_ex = nullptr;
  IMFGetService* get_service = nullptr;
  IKsControl* ks_control = nullptr;
  IMFSampleAllocatorControl* allocator_control = nullptr;
  IMFAttributes* stream_attributes = nullptr;
  IMFPresentationDescriptor* presentation = nullptr;
  HRESULT hr = CoCreateInstance(
      minimix::vcam::kVirtualCameraSourceClsids[index], nullptr, CLSCTX_INPROC_SERVER,
      __uuidof(IMFActivate), reinterpret_cast<void**>(&activate));
  if (SUCCEEDED(hr)) {
    *failed_stage = L"IMFActivate::ActivateObject";
    hr = activate->ActivateObject(__uuidof(IMFMediaSource), reinterpret_cast<void**>(&source));
  }
  if (SUCCEEDED(hr)) {
    *failed_stage = L"QueryInterface(IMFMediaSourceEx)";
    hr = source->QueryInterface(__uuidof(IMFMediaSourceEx), reinterpret_cast<void**>(&source_ex));
  }
  if (SUCCEEDED(hr)) {
    *failed_stage = L"QueryInterface(IMFGetService)";
    hr = source->QueryInterface(__uuidof(IMFGetService), reinterpret_cast<void**>(&get_service));
  }
  if (SUCCEEDED(hr)) {
    *failed_stage = L"QueryInterface(IKsControl)";
    hr = source->QueryInterface(__uuidof(IKsControl), reinterpret_cast<void**>(&ks_control));
  }
  if (SUCCEEDED(hr)) {
    *failed_stage = L"QueryInterface(IMFSampleAllocatorControl)";
    hr = source->QueryInterface(
        __uuidof(IMFSampleAllocatorControl), reinterpret_cast<void**>(&allocator_control));
  }
  if (SUCCEEDED(hr)) {
    *failed_stage = L"IMFMediaSourceEx::GetStreamAttributes";
    hr = source_ex->GetStreamAttributes(0, &stream_attributes);
  }
  if (stream_attributes) {
    GUID category = GUID_NULL;
    if (SUCCEEDED(hr)) {
      *failed_stage = L"GetStreamAttributes(MF_DEVICESTREAM_STREAM_CATEGORY)";
      hr = stream_attributes->GetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, &category);
      if (SUCCEEDED(hr) && category != PINNAME_VIDEO_CAPTURE) hr = MF_E_INVALIDMEDIATYPE;
    }
    stream_attributes->Release();
  }
  if (SUCCEEDED(hr)) {
    *failed_stage = L"IMFMediaSource::CreatePresentationDescriptor";
    hr = source->CreatePresentationDescriptor(&presentation);
  }
  if (SUCCEEDED(hr)) {
    *failed_stage = L"IMFMediaSource::Start";
    hr = source->Start(presentation, nullptr, nullptr);
  }
  if (SUCCEEDED(hr)) source->Stop();
  if (presentation) presentation->Release();
  if (allocator_control) allocator_control->Release();
  if (ks_control) ks_control->Release();
  if (get_service) get_service->Release();
  if (source_ex) source_ex->Release();
  if (source) {
    source->Shutdown();
    source->Release();
  }
  if (activate) {
    activate->ShutdownObject();
    activate->Release();
  }
  return hr;
}

bool unregister_source_machine();

int register_source_machine() {
  const std::wstring directory = executable_directory();
  if (directory.empty()) {
    std::cerr << "Could not locate the MiniMix camera runtime folder.\n";
    return 2;
  }
  const std::wstring dll_path = directory + L"\\MiniMixVcam.dll";
  if (GetFileAttributesW(dll_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::wcerr << L"MiniMixVcam.dll is missing beside the registrar.\n";
    return 2;
  }
  for (std::size_t index = 0; index < std::size(minimix::vcam::kVirtualCameraSourceIds); ++index) {
    HKEY key = nullptr;
    const LONG status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, class_key(index).c_str(), 0, nullptr,
      REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
      std::wcerr << L"Could not register the MiniMix source for all users (" << status << L").\n";
      unregister_source_machine();
      return 3;
    }
    const DWORD path_bytes = static_cast<DWORD>((dll_path.size() + 1) * sizeof(wchar_t));
    LONG write = RegSetValueExW(key, nullptr, 0, REG_SZ,
      reinterpret_cast<const BYTE*>(dll_path.c_str()), path_bytes);
    if (write == ERROR_SUCCESS) {
      constexpr wchar_t threading_model[] = L"Both";
      write = RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(threading_model), sizeof(threading_model));
    }
    RegCloseKey(key);
    if (write != ERROR_SUCCESS) {
      std::wcerr << L"Could not write the MiniMix COM registration (" << write << L").\n";
      unregister_source_machine();
      return 3;
    }
    const LONG user_cleanup = RegDeleteTreeW(HKEY_CURRENT_USER, class_key(index).c_str());
    if (user_cleanup != ERROR_SUCCESS && user_cleanup != ERROR_FILE_NOT_FOUND) {
      std::wcerr << L"Could not remove the old per-user MiniMix registration ("
                 << user_cleanup << L").\n";
      unregister_source_machine();
      return 3;
    }
  }
  std::wcout << L"MiniMix virtual camera source registered for all users.\n";
  return 0;
}

bool unregister_source_machine() {
  bool success = true;
  for (const auto& source_id : minimix::vcam::kVirtualCameraSourceIds) {
    const LONG status = RegDeleteTreeW(
        HKEY_LOCAL_MACHINE,
        (std::wstring(L"Software\\Classes\\CLSID\\") + source_id).c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
      std::wcerr << L"Could not remove the MiniMix COM registration (" << status << L").\n";
      success = false;
    }
    RegDeleteTreeW(
        HKEY_CURRENT_USER,
        (std::wstring(L"Software\\Classes\\CLSID\\") + source_id).c_str());
  }
  return success;
}

int install_cameras() {
  const int registration = register_source_machine();
  if (registration != 0) return registration;
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
    std::cerr << "CoInitializeEx failed: 0x" << std::hex
              << static_cast<unsigned long>(apartment) << "\n";
    unregister_source_machine();
    return 2;
  }
  const HRESULT startup = MFStartup(MF_VERSION);
  if (FAILED(startup)) {
    if (SUCCEEDED(apartment)) CoUninitialize();
    std::cerr << "MFStartup failed: 0x" << std::hex
              << static_cast<unsigned long>(startup) << "\n";
    unregister_source_machine();
    return 2;
  }
  // This API is exported by mfsensorgroup.dll on supported Windows 11 releases.
  const VirtualCameraApi api = load_virtual_camera_api();
  if (!api.create) {
    MFShutdown();
    if (SUCCEEDED(apartment)) CoUninitialize();
    unregister_source_machine();
    std::cerr << "unsupported-windows-version\n";
    return 6;
  }
  int result = 0;
  bool rollback_succeeded = true;
  std::vector<IMFVirtualCamera*> cameras;
  for (std::size_t index = 0; index < std::size(minimix::vcam::kVirtualCameraSourceIds); ++index) {
    const wchar_t* failed_stage = nullptr;
    const HRESULT probe = probe_registered_source(index, &failed_stage);
    if (FAILED(probe)) {
      std::wcerr << L"MiniMix COM source preflight failed for MiniMix " << index + 1
                 << L" at " << failed_stage << L": 0x" << std::hex
                 << static_cast<unsigned long>(probe) << L"\n";
      result = 5;
      break;
    }

    IMFVirtualCamera* camera = nullptr;
    const HRESULT create = api.create(MFVirtualCameraType_SoftwareCameraSource,
      MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers,
      minimix::vcam::kVirtualCameraNames[index], minimix::vcam::kVirtualCameraSourceIds[index],
      nullptr, 0, &camera);
    if (FAILED(create) || !camera) {
      std::cerr << "MFCreateVirtualCamera failed for MiniMix " << index + 1
                << ": 0x" << std::hex << static_cast<unsigned long>(create) << "\n";
      if (camera) camera->Release();
      result = 5;
      break;
    }
    const HRESULT start = camera->Start(nullptr);
    if (FAILED(start)) {
      std::cerr << "IMFVirtualCamera::Start failed for MiniMix " << index + 1
                << ": 0x" << std::hex << static_cast<unsigned long>(start) << "\n";
      const HRESULT remove = camera->Remove();
      if (FAILED(remove)) {
        std::cerr << "Could not roll back MiniMix " << index + 1 << ": 0x"
                  << std::hex << static_cast<unsigned long>(remove) << "\n";
        rollback_succeeded = false;
      }
      camera->Release();
      result = 5;
      break;
    }
    cameras.push_back(camera);
  }
  if (result != 0) {
    for (auto* camera : cameras) {
      const HRESULT remove = camera->Remove();
      if (FAILED(remove)) {
        std::cerr << "Could not roll back an installed MiniMix camera: 0x"
                  << std::hex << static_cast<unsigned long>(remove) << "\n";
        rollback_succeeded = false;
      }
    }
  } else {
    std::cout << "MiniMix virtual cameras installed for all users.\n";
  }
  for (auto* camera : cameras) {
    camera->Release();
  }
  FreeLibrary(api.module);
  MFShutdown();
  if (SUCCEEDED(apartment)) CoUninitialize();
  if (result != 0 && rollback_succeeded && !unregister_source_machine()) {
    std::cerr << "Could not fully roll back the MiniMix COM registration.\n";
  }
  return result;
}

int uninstall_cameras() {
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
    std::cerr << "CoInitializeEx failed: 0x" << std::hex
              << static_cast<unsigned long>(apartment) << "\n";
    return 2;
  }
  const HRESULT startup = MFStartup(MF_VERSION);
  if (FAILED(startup)) {
    if (SUCCEEDED(apartment)) CoUninitialize();
    std::cerr << "MFStartup failed: 0x" << std::hex
              << static_cast<unsigned long>(startup) << "\n";
    return 2;
  }
  const VirtualCameraApi api = load_virtual_camera_api();
  if (!api.create) {
    MFShutdown();
    if (SUCCEEDED(apartment)) CoUninitialize();
    std::cerr << "Could not load the Windows virtual-camera API to remove MiniMix.\n";
    return 6;
  }
  bool success = true;
  for (std::size_t index = 0; index < std::size(minimix::vcam::kVirtualCameraSourceIds); ++index) {
    IMFVirtualCamera* camera = nullptr;
    const HRESULT create = api.create(MFVirtualCameraType_SoftwareCameraSource,
      MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers,
      minimix::vcam::kVirtualCameraNames[index], minimix::vcam::kVirtualCameraSourceIds[index],
      nullptr, 0, &camera);
    if (FAILED(create) || !camera) {
      std::wcerr << L"Could not open MiniMix " << index + 1 << L" for removal: 0x"
                 << std::hex << static_cast<unsigned long>(create) << L"\n";
      if (camera) camera->Release();
      success = false;
      continue;
    }
    const HRESULT remove = camera->Remove();
    if (FAILED(remove)) {
      std::wcerr << L"Could not remove MiniMix " << index + 1 << L": 0x"
                 << std::hex << static_cast<unsigned long>(remove) << L"\n";
      success = false;
    }
    camera->Release();
  }
  FreeLibrary(api.module);
  MFShutdown();
  if (SUCCEEDED(apartment)) CoUninitialize();
  if (!success || !unregister_source_machine()) return 5;
  std::cout << "MiniMix virtual cameras removed.\n";
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: MiniMixVcamRegistrar --install|--uninstall|--status\n";
    return 2;
  }
  const std::wstring command = argv[1];
  if (command == L"--install") return install_cameras();
  if (command == L"--uninstall") return uninstall_cameras();
  if (command == L"--status") {
    const VirtualCameraApi api = load_virtual_camera_api();
    if (!api.create) {
      std::cout << "unsupported-windows-version\n";
      return 6;
    }
    FreeLibrary(api.module);
    const bool registered = source_is_registered();
    std::cout << (registered ? "installed\n" : "not-installed\n");
    return registered ? 0 : 4;
  }
  return 2;
}
