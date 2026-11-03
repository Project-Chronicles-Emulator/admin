#include "game_process.hpp"

#include "encoding.hpp"
#include "patch_installer.hpp"
#include "playsharp_bridge.hpp"
#include "windows_command_line.hpp"

#include <windows.h>

#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <vector>

namespace chronicles::launcher {
namespace {

struct HandleCloser {
  using pointer = HANDLE;
  void operator()(HANDLE handle) const {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using ScopedHandle = std::unique_ptr<HANDLE, HandleCloser>;

std::filesystem::path process_image_path(std::uint32_t pid) {
  ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (process.get() == nullptr) {
    return {};
  }

  std::vector<wchar_t> buffer(32768);
  DWORD length = static_cast<DWORD>(buffer.size());
  if (!QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &length)) {
    return {};
  }
  return std::filesystem::path(std::wstring(buffer.data(), length));
}

bool same_path_case_insensitive(const std::filesystem::path &left,
                                const std::filesystem::path &right) {
  const auto left_text = left.lexically_normal().wstring();
  const auto right_text = right.lexically_normal().wstring();
  if (left_text.size() != right_text.size()) {
    return false;
  }

  return std::equal(left_text.begin(), left_text.end(), right_text.begin(),
                    [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
}

std::filesystem::path find_steam_executable() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) ==
      ERROR_SUCCESS) {
    std::vector<wchar_t> buffer(4096);
    DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    DWORD type = 0;
    const auto status = RegQueryValueExW(key, L"SteamPath", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(buffer.data()), &size);
    RegCloseKey(key);
    if (status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
      const std::filesystem::path steam(std::wstring(buffer.data()));
      const auto executable = steam / L"steam.exe";
      if (std::filesystem::is_regular_file(executable)) {
        return executable;
      }
    }
  }

  const std::filesystem::path fallback{L"C:\\Program Files (x86)\\Steam\\steam.exe"};
  if (std::filesystem::is_regular_file(fallback)) {
    return fallback;
  }
  throw std::runtime_error("Steam was not found. Start the game through Steam instead.");
}

} // namespace

std::filesystem::path expected_client_executable(const std::filesystem::path &game_directory) {
  return game_directory / L"X5" / L"Binaries" / L"Win64" / L"ArcheAgeChronicles.exe";
}

std::optional<ClientProcess> find_client_process(const std::filesystem::path &game_directory) {
  auto clients = find_client_processes(game_directory);
  if (clients.empty()) {
    return std::nullopt;
  }

  return clients.front();
}

std::vector<ClientProcess> find_client_processes(const std::filesystem::path &game_directory) {
  const auto expected = expected_client_executable(game_directory);
  ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (snapshot.get() == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Could not enumerate processes");
  }

  std::vector<ClientProcess> clients;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.get(), &entry)) {
    return clients;
  }

  do {
    if (_wcsicmp(entry.szExeFile, L"ArcheAgeChronicles.exe") != 0) {
      continue;
    }

    const auto image = process_image_path(entry.th32ProcessID);
    if (image.empty() || !same_path_case_insensitive(image, expected)) {
      continue;
    }

    ClientProcess client;
    client.pid = entry.th32ProcessID;
    client.create_time = query_process_create_time(client.pid);
    client.executable = image;
    if (client.create_time != 0) {
      clients.push_back(std::move(client));
    }
  } while (Process32NextW(snapshot.get(), &entry));

  std::ranges::sort(clients, [](const ClientProcess &left, const ClientProcess &right) {
    if (left.create_time != right.create_time) {
      return left.create_time < right.create_time;
    }
    return left.pid < right.pid;
  });
  return clients;
}

std::optional<ClientProcess> find_client_process_by_pid(const std::filesystem::path &game_directory,
                                                        std::uint32_t pid) {
  if (pid == 0) {
    return std::nullopt;
  }

  const auto image = process_image_path(pid);
  const auto expected = expected_client_executable(game_directory);
  if (image.empty() || !same_path_case_insensitive(image, expected)) {
    return std::nullopt;
  }

  ClientProcess client;
  client.pid = pid;
  client.create_time = query_process_create_time(pid);
  client.executable = image;
  if (client.create_time == 0) {
    return std::nullopt;
  }
  return client;
}

void launch_through_steam(const std::string &app_id,
                          const std::filesystem::path &bootstrap_archive) {
  if (app_id.empty() ||
      !std::ranges::all_of(app_id, [](char digit) { return digit >= '0' && digit <= '9'; })) {
    throw std::invalid_argument("The Steam application ID must contain only digits");
  }

  const auto steam = find_steam_executable();
  if (!std::filesystem::is_regular_file(bootstrap_archive)) {
    throw std::runtime_error("The local bootstrap archive is missing");
  }

  const auto arguments = L"-applaunch " + widen(app_id) + L" -textconfig \"-IniBootstrap=" +
                         bootstrap_archive.wstring() + L"\"";
  const auto result = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"open", steam.c_str(), arguments.c_str(), nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    throw std::runtime_error("Steam could not launch the game (ShellExecute code " +
                             std::to_string(result) + ")");
  }
}

std::uint32_t launch_managed_client(const std::filesystem::path &game_directory,
                                    const std::string &app_id,
                                    const std::filesystem::path &bootstrap_archive) {
  if (app_id.empty() ||
      !std::ranges::all_of(app_id, [](char digit) { return digit >= '0' && digit <= '9'; })) {
    throw std::invalid_argument("The Steam application ID must contain only digits");
  }
  const auto executable = std::filesystem::absolute(expected_client_executable(game_directory));
  require_regular_patch_path(executable);
  require_regular_patch_path(bootstrap_archive);
  if (!std::filesystem::is_regular_file(executable) ||
      !std::filesystem::is_regular_file(bootstrap_archive)) {
    throw std::runtime_error("The managed executable or bootstrap archive is missing");
  }

  // Keep inherited platform variables scoped to the child; the launcher worker and UI run
  // concurrently, so changing the process environment around CreateProcess would race.
  const std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> inherited(
      GetEnvironmentStringsW(), &FreeEnvironmentStringsW);
  if (inherited == nullptr) {
    throw std::runtime_error("Cannot read the launch environment");
  }
  std::vector<std::wstring> variables;
  for (const wchar_t *entry = inherited.get(); *entry != L'\0'; entry += wcslen(entry) + 1) {
    if (_wcsnicmp(entry, L"SteamAppId=", 11) != 0 && _wcsnicmp(entry, L"SteamGameId=", 12) != 0) {
      variables.emplace_back(entry);
    }
  }
  variables.push_back(L"SteamAppId=" + widen(app_id));
  variables.push_back(L"SteamGameId=" + widen(app_id));
  std::ranges::sort(variables, [](const auto &left, const auto &right) {
    return _wcsicmp(left.c_str(), right.c_str()) < 0;
  });
  std::vector<wchar_t> environment;
  for (const auto &variable : variables) {
    environment.insert(environment.end(), variable.begin(), variable.end());
    environment.push_back(L'\0');
  }
  environment.push_back(L'\0');

  auto command = join_windows_command_line(
      {executable.wstring(), L"-textconfig", L"-IniBootstrap=" + bootstrap_archive.wstring()});
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const auto working_directory = executable.parent_path().wstring();
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT, environment.data(), working_directory.c_str(),
                      &startup, &process)) {
    throw std::runtime_error("Cannot start the managed client (Windows error " +
                             std::to_string(GetLastError()) + ")");
  }
  ScopedHandle process_handle(process.hProcess);
  ScopedHandle thread_handle(process.hThread);
  return process.dwProcessId;
}

void launch_private_client(const std::filesystem::path &project_root,
                           const std::filesystem::path &game_directory, const std::string &app_id,
                           const std::filesystem::path &bootstrap_archive) {
  if (app_id.empty() ||
      !std::ranges::all_of(app_id, [](char digit) { return digit >= '0' && digit <= '9'; })) {
    throw std::invalid_argument("The Steam application ID must contain only digits");
  }

  const auto helper = project_root / L"tools" / L"launch_private_client.ps1";
  if (!std::filesystem::is_regular_file(helper) ||
      !std::filesystem::is_regular_file(bootstrap_archive)) {
    throw std::runtime_error("The private launch helper or bootstrap archive is missing");
  }

  const std::vector<std::wstring> arguments{L"pwsh.exe",
                                            L"-NoProfile",
                                            L"-File",
                                            helper.wstring(),
                                            L"-PrivateRoot",
                                            game_directory.wstring(),
                                            L"-BootstrapArchive",
                                            bootstrap_archive.wstring(),
                                            L"-SteamAppId",
                                            widen(app_id)};
  auto command = join_windows_command_line(arguments);

  const auto log_path = project_root / L"out" / L"launcher" / L"private-launch.log";
  std::filesystem::create_directories(log_path.parent_path());
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  ScopedHandle log_file(CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &security,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (log_file.get() == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Could not open the private launch log");
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = log_file.get();
  startup.hStdError = log_file.get();

  PROCESS_INFORMATION process{};
  const auto working_directory = project_root.wstring();
  const BOOL created =
      CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                     working_directory.c_str(), &startup, &process);
  if (!created) {
    throw std::runtime_error("Could not start the guarded private launch helper (Windows error " +
                             std::to_string(GetLastError()) + ")");
  }

  ScopedHandle process_handle(process.hProcess);
  ScopedHandle thread_handle(process.hThread);
  const DWORD wait_result = WaitForSingleObject(process_handle.get(), 30000);
  if (wait_result != WAIT_OBJECT_0) {
    throw std::runtime_error("The guarded private launch helper did not finish within 30 seconds");
  }

  DWORD exit_code = 1;
  if (!GetExitCodeProcess(process_handle.get(), &exit_code) || exit_code != 0) {
    throw std::runtime_error("The guarded private launch helper rejected the launch; inspect " +
                             log_path.string());
  }
}

} // namespace chronicles::launcher
