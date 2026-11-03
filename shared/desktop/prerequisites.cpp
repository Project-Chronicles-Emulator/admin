#include "prerequisites.hpp"

#include "distribution/distribution.hpp"
#include "http_client.hpp"
#include "patch_installer.hpp"
#include "windows_command_line.hpp"
#include "xorstr.hpp"

#include <windows.h>

#include <objbase.h>
#include <wrl.h>

#include <WebView2.h>
#include <commctrl.h>
#include <shellapi.h>

#include <memory>
#include <stdexcept>
#include <string_view>

namespace chronicles::launcher {
namespace {

bool runtime_available() {
  // The SDK can also discover preview browsers. Production requires Evergreen's
  // registration, rather than treating a developer's Canary install as a runtime.
  bool registered = false;
  for (const auto hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    const auto subkey =
        hive == HKEY_CURRENT_USER
            ? L"Software\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
            : L"Software\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-"
              L"00C3A9A7E4C5}";
    wchar_t registered_version[256]{};
    DWORD bytes = sizeof(registered_version);
    if (RegGetValueW(hive, subkey, L"pv", RRF_RT_REG_SZ, nullptr, registered_version, &bytes) ==
            ERROR_SUCCESS &&
        registered_version[0] != L'\0' && std::wstring_view(registered_version) != L"0.0.0.0") {
      registered = true;
    }
  }
  if (!registered) {
    return false;
  }
  LPWSTR version = nullptr;
  const auto result = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
  const bool available = SUCCEEDED(result) && version != nullptr;
  CoTaskMemFree(version);
  return available;
}

struct SetupProgress {
  HWND window{};

  explicit SetupProgress(bool visible) {
    if (!visible) {
      return;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    window = CreateWindowExW(WS_EX_APPWINDOW, L"STATIC", L"Project Chronicles · Preparing your launcher",
                             WS_OVERLAPPED | WS_CAPTION | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             470, 155, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr) {
      return;
    }
    CreateWindowExW(0, L"STATIC",
                    L"Preparing Microsoft WebView2…\nChronicles will open automatically when ready.",
                    WS_CHILD | WS_VISIBLE, 24, 22, 415, 45, window, nullptr,
                    GetModuleHandleW(nullptr), nullptr);
    const auto progress =
        CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 24, 80,
                        415, 14, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(progress, PBM_SETMARQUEE, TRUE, 40);
    UpdateWindow(window);
  }

  ~SetupProgress() {
    if (window != nullptr) {
      DestroyWindow(window);
    }
  }
};

} // namespace

void ensure_visual_cpp_runtime(const std::filesystem::path &directory,
                               const std::filesystem::path &cache_directory) {
  const auto installed = [] {
    wchar_t system_directory[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
      return false;
    }
    for (const auto name : {L"msvcp140.dll", L"vcruntime140.dll", L"vcruntime140_1.dll"}) {
      const auto path = std::filesystem::path(system_directory) / name;
      if (!std::filesystem::is_regular_file(path)) {
        return false;
      }
    }
    return true;
  };
  if (installed()) {
    return;
  }

  constexpr auto expected_hash = "843068991daaa1f73ad9f6239bce4d0f6a07a51f18c37ea2a867e9beca71295c";
  auto installer = directory / "prerequisites/VC_redist.x64.exe";
  require_regular_patch_path(installer);
  if (!std::filesystem::exists(installer)) {
    installer = cache_directory / "VC_redist-84306899-x64.exe";
    require_regular_patch_path(installer);
    if (!std::filesystem::exists(installer)) {
      std::filesystem::create_directories(cache_directory);
      const auto partial = cache_directory / "VC_redist-84306899-x64.part";
      require_regular_patch_path(partial);
      const std::string url = xorstr_(
          "https://download.visualstudio.microsoft.com/download/pr/"
          "ebdab8e5-1d7b-4d9f-a11b-cbb1720c3b12/"
          "843068991DAAA1F73AD9F6239BCE4D0F6A07A51F18C37EA2A867E9BECA71295C/VC_redist.x64.exe");
      constexpr std::uint64_t kVisualCppInstallerBytes = 18731856;
      http_download_file(url, partial, kVisualCppInstallerBytes, {});
      if (distribution::sha256_file(partial) != expected_hash) {
        throw std::runtime_error(
            "Microsoft Visual C++ setup failed verification. Please retry Chronicles.");
      }
      std::filesystem::rename(partial, installer);
    }
  }
  if (!std::filesystem::is_regular_file(installer) ||
      distribution::sha256_file(installer) != expected_hash) {
    throw std::runtime_error(
        "Microsoft Visual C++ setup is damaged. Reinstall Project Chronicles or contact support.");
  }

  SHELLEXECUTEINFOW setup{};
  setup.cbSize = sizeof(setup);
  setup.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  setup.lpVerb = L"runas";
  setup.lpFile = installer.c_str();
  setup.lpParameters = L"/install /quiet /norestart";
  setup.nShow = SW_HIDE;
  if (!ShellExecuteExW(&setup) || setup.hProcess == nullptr) {
    throw std::runtime_error("Approve Microsoft's Visual C++ setup prompt to prepare the game.");
  }
  const std::unique_ptr<void, decltype(&CloseHandle)> process(setup.hProcess, &CloseHandle);
  const auto result = WaitForSingleObject(process.get(), 5 * 60 * 1000);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.get(), &exit_code);
  const bool success = exit_code == 0 || exit_code == 3010 || exit_code == 1638;
  if (result != WAIT_OBJECT_0 || !success || !installed()) {
    throw std::runtime_error(
        "Microsoft Visual C++ setup did not finish. Restart Windows and retry Chronicles.");
  }
}

void ensure_webview_runtime(const std::filesystem::path &directory, bool show_progress) {
  if (runtime_available()) {
    return;
  }

  const auto bootstrapper = directory / "prerequisites/MicrosoftEdgeWebview2Setup.exe";
  require_regular_patch_path(bootstrapper);
  const std::string expected_hash =
      xorstr_("ca4c9016c70e2184c5e231662e0e98abf4826647180ef3faab32fcb1ca2ec15e");
  if (!std::filesystem::is_regular_file(bootstrapper) ||
      distribution::sha256_file(bootstrapper) != expected_hash) {
    throw std::runtime_error("The Microsoft setup component is missing or damaged. "
                             "Extract the complete Project Chronicles package and try again.");
  }

  SetupProgress progress(show_progress);
  auto command = join_windows_command_line({bootstrapper.wstring(), L"/silent", L"/install"});
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(bootstrapper.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process)) {
    throw std::runtime_error("Microsoft WebView2 setup could not start (Windows error " +
                             std::to_string(GetLastError()) + ").");
  }
  CloseHandle(process.hThread);

  // Keep the preparation window responsive while Microsoft's per-user installer runs.
  const auto deadline = GetTickCount64() + 5 * 60 * 1000;
  DWORD wait_result = WAIT_TIMEOUT;
  while (GetTickCount64() < deadline) {
    wait_result = MsgWaitForMultipleObjects(1, &process.hProcess, FALSE, 500, QS_ALLINPUT);
    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_FAILED) {
      break;
    }
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  if (wait_result != WAIT_OBJECT_0 || (exit_code != 0 && exit_code != 3010) ||
      !runtime_available()) {
    throw std::runtime_error("Microsoft WebView2 is not ready yet. Check your internet "
                             "connection and reopen Project Chronicles to retry preparation.");
  }
}

} // namespace chronicles::launcher
