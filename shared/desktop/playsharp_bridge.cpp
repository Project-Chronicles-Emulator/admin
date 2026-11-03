#include "playsharp_bridge.hpp"

#include "encoding.hpp"

#include <windows.h>

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace chronicles::launcher {
namespace {

using json = nlohmann::json;

constexpr auto kReadPollInterval = std::chrono::milliseconds(10);

constexpr auto kScanPollInterval = std::chrono::milliseconds(250);
constexpr auto kBridgeResponseTimeout = std::chrono::seconds(30);

constexpr std::uint64_t kMaxScanBytes = 64ull * 1024 * 1024 * 1024;

constexpr int kMaxScanDurationMs = 15 * 60 * 1000;

void close_handle(HANDLE &handle) {
  if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
    CloseHandle(handle);
    handle = nullptr;
  }
}

std::string windows_error_message(const std::string &context) {
  const auto code = GetLastError();
  std::ostringstream message;
  message << context << " (Windows error " << code << ")";
  return message.str();
}

} // namespace

struct PlaySharpBridge::Impl {
  HANDLE child_stdin_read{};
  HANDLE child_stdin_write{};
  HANDLE child_stdout_read{};
  HANDLE child_stdout_write{};
  PROCESS_INFORMATION process{};
  std::int64_t next_id{1};
  std::string pending;

  ~Impl() {
    close_handle(child_stdin_write);
    close_handle(child_stdout_read);

    if (process.hProcess != nullptr) {
      // Ask for a bounded graceful exit, then terminate only our own bridge child.
      if (WaitForSingleObject(process.hProcess, 1500) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 0);
        WaitForSingleObject(process.hProcess, 2000);
      }
      close_handle(process.hProcess);
      close_handle(process.hThread);
    }

    close_handle(child_stdin_read);
    close_handle(child_stdout_write);
  }
};

PlaySharpBridge::PlaySharpBridge() : impl_(std::make_unique<Impl>()) {}

PlaySharpBridge::~PlaySharpBridge() = default;

void PlaySharpBridge::start(const std::filesystem::path &bridge_executable) {
  if (!std::filesystem::is_regular_file(bridge_executable)) {
    throw MemoryAccessError("PlaySharp bridge not found: " + bridge_executable.string());
  }

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  if (!CreatePipe(&impl_->child_stdin_read, &impl_->child_stdin_write, &security, 0) ||
      !CreatePipe(&impl_->child_stdout_read, &impl_->child_stdout_write, &security, 0)) {
    throw MemoryAccessError(windows_error_message("Could not create bridge pipes"));
  }
  SetHandleInformation(impl_->child_stdin_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(impl_->child_stdout_read, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = impl_->child_stdin_read;
  startup.hStdOutput = impl_->child_stdout_write;
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  std::wstring command = L"\"" + bridge_executable.wstring() + L"\"";
  const auto working_directory = bridge_executable.parent_path().wstring();
  const BOOL created =
      CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                     working_directory.c_str(), &startup, &impl_->process);
  if (!created) {
    throw MemoryAccessError(windows_error_message("Could not start the PlaySharp bridge"));
  }

  // Only the child needs these ends.
  close_handle(impl_->child_stdin_read);
  close_handle(impl_->child_stdout_write);

  const auto setup = call("setup", json::object(), {});
  if (!setup.value("success", false)) {
    throw MemoryAccessError(
        "The PlaySharp kernel driver is not available. Start the PlaySharp driver first, or "
        "check the backend setting.");
  }
}

bool PlaySharpBridge::is_running() const {
  if (impl_->process.hProcess == nullptr) {
    return false;
  }
  return WaitForSingleObject(impl_->process.hProcess, 0) == WAIT_TIMEOUT;
}

json PlaySharpBridge::call(const std::string &method, const json &params, std::stop_token stop) {
  if (!is_running()) {
    throw MemoryAccessError("The PlaySharp bridge is not running");
  }

  const auto id = impl_->next_id++;
  const auto deadline = std::chrono::steady_clock::now() + kBridgeResponseTimeout;
  json request{{"id", id}, {"method", method}, {"params", params}};
  const auto payload = request.dump() + "\n";

  DWORD written = 0;
  if (!WriteFile(impl_->child_stdin_write, payload.data(), static_cast<DWORD>(payload.size()),
                 &written, nullptr) ||
      written != payload.size()) {
    throw MemoryAccessError(windows_error_message("Could not write to the PlaySharp bridge"));
  }

  for (;;) {
    const auto newline = impl_->pending.find('\n');
    if (newline != std::string::npos) {
      const auto line = impl_->pending.substr(0, newline);
      impl_->pending.erase(0, newline + 1);
      if (line.empty()) {
        continue;
      }

      const auto response = json::parse(line);
      if (response.value("id", std::int64_t{-1}) != id) {
        continue;
      }
      if (response.contains("error")) {
        throw MemoryAccessError(method + ": " + response.at("error").value("message", "error"));
      }

      const auto &result = response.at("result");
      if (!result.value("success", false)) {
        std::string detail = result.value("message", std::string{});
        if (detail.empty()) {
          detail = result.value("error", std::string{"operation failed"});
        }
        throw MemoryAccessError(method + ": " + detail);
      }
      return result;
    }

    if (stop.stop_requested()) {
      throw OperationCancelled();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      throw MemoryAccessError("The PlaySharp bridge did not answer " + method + " within " +
                              std::to_string(kBridgeResponseTimeout.count()) + " seconds");
    }

    DWORD available = 0;
    if (!PeekNamedPipe(impl_->child_stdout_read, nullptr, 0, nullptr, &available, nullptr)) {
      throw MemoryAccessError(windows_error_message("Lost the PlaySharp bridge connection"));
    }
    if (available == 0) {
      if (!is_running()) {
        throw MemoryAccessError("The PlaySharp bridge exited unexpectedly");
      }
      std::this_thread::sleep_for(kReadPollInterval);
      continue;
    }

    char buffer[8192];
    DWORD read_count = 0;
    const auto chunk = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
    if (!ReadFile(impl_->child_stdout_read, buffer, chunk, &read_count, nullptr)) {
      throw MemoryAccessError(windows_error_message("Could not read from the PlaySharp bridge"));
    }
    impl_->pending.append(buffer, read_count);
  }
}

BridgeProcessMemory::BridgeProcessMemory(PlaySharpBridge &bridge, std::uint32_t pid,
                                         std::string backend_mode)
    : bridge_(bridge), pid_(pid), mode_(std::move(backend_mode)),
      create_time_(query_process_create_time(pid)) {
  if (create_time_ == 0) {
    throw MemoryAccessError("Could not query the client's process identity");
  }
}

std::vector<std::uint8_t> BridgeProcessMemory::read(std::uint64_t address, std::size_t size) {
  const auto result = bridge_.call(
      "read_memory",
      {{"pid", pid_}, {"address", format_address(address)}, {"size", size}, {"mode", mode_}}, {});
  return base64_decode(result.at("data").get<std::string>());
}

void BridgeProcessMemory::write(std::uint64_t address, std::span<const std::uint8_t> bytes) {
  verify_process_identity();
  // The mode is pinned in the request; the bridge never falls back to another backend for writes.
  bridge_.call("write_memory",
               {{"pid", pid_},
                {"address", format_address(address)},
                {"data", base64_encode(bytes)},
                {"mode", mode_}},
               {});
}

std::vector<std::uint64_t> BridgeProcessMemory::find_utf16(std::u16string_view text,
                                                           std::stop_token stop) {
  return run_scan({{"kind", "string"}, {"encoding", "utf16"}, {"value", utf16_to_utf8(text)}},
                  stop);
}

std::vector<std::uint64_t> BridgeProcessMemory::find_pointer(std::uint64_t value,
                                                             std::stop_token stop) {
  return run_scan({{"kind", "value"},
                   {"value_type", "uint64"},
                   {"value", format_address(value)},
                   {"alignment", 8}},
                  stop);
}

void BridgeProcessMemory::verify_process_identity() const {
  const auto current = query_process_create_time(pid_);
  if (current == 0 || current != create_time_) {
    throw MemoryAccessError("The client process changed; refusing to write to a stale session");
  }
}

std::vector<std::uint64_t> BridgeProcessMemory::run_scan(json params, std::stop_token stop) {
  verify_process_identity();
  params["pid"] = pid_;
  params["mode"] = mode_;
  params["executable"] = false;
  params["writable"] = true;
  params["type"] = MEM_PRIVATE;
  params["max_scan_bytes"] = kMaxScanBytes;
  params["max_duration_ms"] = kMaxScanDurationMs;
  params["max_results"] = 1000;

  const auto started = bridge_.call("scan_start", params, stop);
  const auto session_id = started.at("session_id").get<std::string>();

  try {
    for (;;) {
      if (stop.stop_requested()) {
        bridge_.call("scan_cancel", {{"session_id", session_id}}, {});
        throw OperationCancelled();
      }

      const auto status = bridge_.call("scan_status", {{"session_id", session_id}}, stop);
      const auto state = status.at("state").get<std::string>();
      if (state == "failed") {
        throw MemoryAccessError("Memory scan failed: " + status.value("error", std::string{}));
      }
      if (state == "cancelled") {
        throw OperationCancelled();
      }
      if (state == "completed") {
        if (status.value("truncated", false)) {
          throw MemoryAccessError("Memory scan was truncated: " +
                                  status.value("stop_reason", std::string{}));
        }
        break;
      }

      std::this_thread::sleep_for(kScanPollInterval);
    }

    std::vector<std::uint64_t> addresses;
    std::size_t offset = 0;
    for (;;) {
      const auto page = bridge_.call(
          "scan_results", {{"session_id", session_id}, {"offset", offset}, {"limit", 500}}, stop);
      for (const auto &match : page.at("results")) {
        addresses.push_back(parse_address(match.at("address").get<std::string>()));
      }
      offset += page.at("count").get<std::size_t>();
      if (offset >= page.at("total").get<std::size_t>()) {
        break;
      }
    }

    bridge_.call("scan_close", {{"session_id", session_id}}, {});
    return addresses;
  } catch (...) {
    try {
      bridge_.call("scan_close", {{"session_id", session_id}}, {});
    } catch (const std::exception &) {
      // Preserve the original failure; the bridge session also expires on process exit.
    }
    throw;
  }
}

std::int64_t query_process_create_time(std::uint32_t pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
  if (process == nullptr) {
    return 0;
  }

  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  const BOOL queried = GetProcessTimes(process, &created, &exited, &kernel, &user);
  // A terminated process can still have a valid creation time while another handle keeps it alive.
  const DWORD wait_result = WaitForSingleObject(process, 0);
  CloseHandle(process);
  if (!queried || wait_result != WAIT_TIMEOUT) {
    return 0;
  }

  ULARGE_INTEGER value{};
  value.LowPart = created.dwLowDateTime;
  value.HighPart = created.dwHighDateTime;
  return static_cast<std::int64_t>(value.QuadPart);
}

} // namespace chronicles::launcher
