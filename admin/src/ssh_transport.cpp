#include "ssh_transport.hpp"

#include "encoding.hpp"
#include "http_client.hpp"
#include "windows_command_line.hpp"
#include "xorstr.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace chronicles::admin_console {
namespace {

struct HandleCloser {
  using pointer = HANDLE;

  void operator()(HANDLE handle) const {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using Handle = std::unique_ptr<void, HandleCloser>;

} // namespace

nlohmann::json ssh_admin_request(const nlohmann::json &configuration, const std::string &method,
                                 const std::string &path, const nlohmann::json &body) {
  using nlohmann::json;
  const auto key =
      std::filesystem::path(launcher::widen(configuration.at("key_file").get<std::string>()));
  const auto known_hosts =
      std::filesystem::path(launcher::widen(configuration.at("known_hosts").get<std::string>()));
  if (!key.is_absolute() || key.parent_path() != known_hosts.parent_path() ||
      !std::filesystem::is_regular_file(key) || !std::filesystem::is_regular_file(known_hosts)) {
    throw std::invalid_argument("Choose the protected SSH key and matching known_hosts file.");
  }
  const auto host = configuration.at("host").get<std::string>();
  if (host.empty() || host.front() == '-' || host.size() > 255 ||
      host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@.-_") !=
          std::string::npos) {
    throw std::invalid_argument("Configure a valid user@host for the admin SSH connection.");
  }
  const auto input =
      json{{"method", method}, {"path", path}, {"body", body.dump()}}.dump(-1, ' ', true);
  if (input.size() > 262144) {
    throw std::invalid_argument("The admin request exceeds the SSH transport limit.");
  }

  std::array<wchar_t, MAX_PATH> system_directory{};
  if (GetSystemDirectoryW(system_directory.data(), MAX_PATH) == 0) {
    throw std::runtime_error("Windows did not report its system directory.");
  }
  const auto executable = std::filesystem::path(system_directory.data()) / "OpenSSH/ssh.exe";
  auto command = launcher::join_windows_command_line(
      {executable.wstring(),
       L"-F",
       L"NUL",
       L"-o",
       L"BatchMode=yes",
       L"-o",
       L"StrictHostKeyChecking=yes",
       L"-o",
       L"ConnectTimeout=10",
       L"-o",
       L"ServerAliveInterval=10",
       L"-o",
       L"ServerAliveCountMax=2",
       L"-o",
       L"UserKnownHostsFile=" + known_hosts.filename().wstring(),
       L"-i",
       key.filename().wstring(),
       launcher::widen(host),
       L"powershell.exe",
       L"-NoLogo",
       L"-NoProfile",
       L"-NonInteractive",
       L"-File",
       L"C:/Chronicles/ops/admin_remote_proxy.ps1"});

  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE input_read_raw{};
  HANDLE input_write_raw{};
  HANDLE output_read_raw{};
  HANDLE output_write_raw{};
  if (!CreatePipe(&input_read_raw, &input_write_raw, &security, 0)) {
    throw std::runtime_error("Cannot create the admin SSH input pipe.");
  }
  Handle input_read(input_read_raw);
  Handle input_write(input_write_raw);
  if (!CreatePipe(&output_read_raw, &output_write_raw, &security, 0)) {
    throw std::runtime_error("Cannot create the admin SSH output pipe.");
  }
  Handle output_read(output_read_raw);
  Handle output_write(output_write_raw);
  SetHandleInformation(input_write.get(), HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input_read.get();
  startup.hStdOutput = output_write.get();
  startup.hStdError = output_write.get();
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                      nullptr, key.parent_path().c_str(), &startup, &process)) {
    throw std::runtime_error("Cannot start Windows OpenSSH. Install its optional client.");
  }
  Handle child(process.hProcess);
  Handle thread(process.hThread);
  input_read.reset();
  output_write.reset();
  DWORD written{};
  const bool sent = WriteFile(input_write.get(), input.data(), static_cast<DWORD>(input.size()),
                              &written, nullptr) &&
                    written == input.size();
  input_write.reset();
  if (!sent) {
    TerminateProcess(child.get(), 1);
    throw std::runtime_error("The admin SSH request could not be sent.");
  }

  std::string response;
  const auto deadline = GetTickCount64() + 45000;
  for (;;) {
    DWORD available{};
    if (PeekNamedPipe(output_read.get(), nullptr, 0, nullptr, &available, nullptr) && available) {
      std::array<char, 8192> buffer{};
      DWORD received{};
      if (ReadFile(output_read.get(), buffer.data(),
                   (std::min)(available, static_cast<DWORD>(buffer.size())), &received, nullptr)) {
        response.append(buffer.data(), received);
      }
    } else if (WaitForSingleObject(child.get(), 0) == WAIT_OBJECT_0) {
      break;
    } else {
      Sleep(20);
    }
    if (GetTickCount64() >= deadline || response.size() > 4 * 1024 * 1024) {
      TerminateProcess(child.get(), 1);
      throw std::runtime_error("The admin SSH request timed out or returned too much data.");
    }
  }

  DWORD exit_code = 1;
  GetExitCodeProcess(child.get(), &exit_code);
  const auto envelope = json::parse(response, nullptr, false);
  if (exit_code != 0 || !envelope.is_object() || !envelope.contains("status")) {
    throw std::runtime_error("Live admin connection failed. Verify SSH access and the VPS proxy.");
  }
  const auto status = envelope.at("status").get<int>();
  const auto response_body = envelope.at("body").get<std::string>();
  if (status < 200 || status >= 300) {
    throw launcher::HttpError("Live admin request was refused", status, response_body);
  }
  return json::parse(response_body);
}

} // namespace chronicles::admin_console
