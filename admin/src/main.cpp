// Project Chronicles Server Admin: a WebView2 desktop console for the emulator's /admin/v1 API.
// The web UI posts actions; this host runs them on a small worker pool and posts results back.

#include "console_client.hpp"
#include "encoding.hpp"
#include "http_client.hpp"
#include "prerequisites.hpp"
#include "webview_host.hpp"

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace chronicles::admin_console {
namespace {

using Json = nlohmann::json;

constexpr wchar_t kWindowTitle[] = L"Project Chronicles Server Admin";
constexpr char kDefaultServerUrl[] = "http://127.0.0.1:8080";
// Enough for a log poll, a dashboard refresh and one slow request (such as a password hash)
// to proceed together.
constexpr std::size_t kWorkerCount = 3;

// Runs queued tasks on a fixed set of threads. stop() finishes running tasks, drops queued
// ones and joins every thread.
class TaskPool {
public:
  explicit TaskPool(std::size_t worker_count) {
    for (std::size_t index = 0; index < worker_count; ++index) {
      workers_.emplace_back([this] { run_worker(); });
    }
  }

  TaskPool(const TaskPool &) = delete;

  TaskPool &operator=(const TaskPool &) = delete;

  ~TaskPool() {
    stop();
  }

  void submit(std::function<void()> task) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        return;
      }
      tasks_.push_back(std::move(task));
    }
    condition_.notify_one();
  }

  void stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      tasks_.clear();
    }
    condition_.notify_all();
    for (auto &worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

private:
  void run_worker() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (stopping_) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  bool stopping_{};
};

struct ConsoleOptions {
  std::filesystem::path project_root;
  std::string server_url{kDefaultServerUrl};
  std::filesystem::path token_file;
  // --server or --token-file was given, so it overrides the saved connection.
  bool connection_given{};
  std::string check_server;
};

std::filesystem::path local_app_data() {
  PWSTR raw_path = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw_path))) {
    throw std::runtime_error("Windows did not report the local application data folder");
  }
  std::filesystem::path path(raw_path);
  CoTaskMemFree(raw_path);
  return path;
}

std::filesystem::path executable_directory() {
  std::vector<wchar_t> module_path(32768);
  const auto length =
      GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (length == 0 || length >= module_path.size()) {
    throw std::runtime_error("Cannot resolve the console executable path");
  }
  return std::filesystem::path(module_path.data()).parent_path();
}

// Private operator profiles survive rebuilding or moving the admin executable.
std::filesystem::path server_profiles_file(const ConsoleOptions &options) {
  const std::vector<std::filesystem::path> candidates{
      local_app_data() / "ChroniclesAdmin" / "admin-servers.json",
      executable_directory() / "admin-servers.json",
      options.project_root / "out" / "release" / "current" / "admin" / "admin-servers.json"};
  for (const auto &candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate)) {
      return candidate;
    }
  }
  return {};
}

// Turns an HttpError's JSON body ({"error","message"}) into readable fields.
Json http_failure(const launcher::HttpError &error) {
  Json failure{{"status", error.status()}, {"error", "http_error"}, {"message", error.what()}};
  const auto body = Json::parse(error.body(), nullptr, false);
  if (body.is_object()) {
    if (body.contains("error") && body.at("error").is_string()) {
      failure["error"] = body.at("error");
    }
    if (body.contains("message") && body.at("message").is_string()) {
      failure["message"] = body.at("message");
    }
  }
  if (error.status() == 0) {
    failure["error"] = "server_unreachable";
    failure["message"] = std::string("The server did not answer: ") + error.what();
  }
  return failure;
}

// Runs `work` and posts {type, request_id, ok, result | failure} to the UI.
void run_and_report(launcher::WebViewHost &host, const std::string &type, const Json &request_id,
                    const std::function<Json()> &work) {
  Json event{{"type", type}, {"request_id", request_id}};
  try {
    event["ok"] = true;
    event["result"] = work();
  } catch (const launcher::HttpError &error) {
    event["ok"] = false;
    event["failure"] = http_failure(error);
  } catch (const std::exception &error) {
    event["ok"] = false;
    event["failure"] = {{"status", 0}, {"error", "console_error"}, {"message", error.what()}};
  }
  host.post_event(event.dump());
}

void dispatch(launcher::WebViewHost &host, TaskPool &pool, ConsoleClient &client,
              const Json &message) {
  const auto action = message.value("action", std::string{});
  const auto payload = message.value("payload", Json::object());
  const auto request_id = payload.value("request_id", Json());

  if (action == "ui_ready") {
    host.post_event(Json{{"type", "settings"}, {"settings", client.describe()}}.dump());
    return;
  }

  if (action == "connect") {
    pool.submit([&host, &client, payload, request_id] {
      run_and_report(host, "connect_result", request_id, [&] { return client.connect(payload); });
    });
    return;
  }

  if (action == "api") {
    pool.submit([&host, &client, payload, request_id] {
      run_and_report(host, "api_result", request_id, [&] {
        return client.request(payload.value("method", std::string("GET")),
                              payload.value("path", std::string{}),
                              payload.value("body", Json::object()));
      });
    });
  }
}

int run_console(const ConsoleOptions &options) {
  const auto data_directory = local_app_data() / "ChroniclesAdmin";
  ConsoleConnection defaults{options.server_url, options.token_file};
  ConsoleClient client(data_directory / "settings.json", defaults, options.connection_given,
                       server_profiles_file(options));

  launcher::WebViewHost host;
  TaskPool pool(kWorkerCount);

  launcher::WebViewWindowOptions window_options;
  window_options.title = kWindowTitle;
  window_options.icon_resource_id = 101;
  window_options.forward_keyboard_focus = true;
  window_options.tray_show_label = L"Show admin console";
  window_options.tray_exit_label = L"Exit admin console";

  host.create(
      executable_directory() / "web", data_directory / "webview2",
      [&](const Json &message) { dispatch(host, pool, client, message); }, window_options);

  const int result = host.run_message_loop();
  // Join workers before the host and client they reference are destroyed.
  pool.stop();
  return result;
}

ConsoleOptions parse_arguments(int argc, wchar_t **argv) {
  ConsoleOptions options;
  options.project_root = CHRONICLES_ADMIN_PROJECT_ROOT;

  for (int index = 1; index < argc; ++index) {
    std::wstring argument = argv[index];
    std::wstring value;

    // Accept both "--server URL" and "--server=URL".
    const auto equals = argument.find(L'=');
    if (argument.starts_with(L"--") && equals != std::wstring::npos) {
      value = argument.substr(equals + 1);
      argument = argument.substr(0, equals);
    } else if (index + 1 < argc) {
      value = argv[++index];
    } else {
      // Paths containing spaces must be quoted, or the shell splits them into two arguments.
      throw std::invalid_argument("Option " + launcher::narrow(argument) +
                                  " needs a value. Quote paths that contain spaces.");
    }

    if (argument == L"--check-server") {
      options.check_server = launcher::narrow(value);
    } else if (argument == L"--server") {
      options.server_url = launcher::narrow(value);
      options.connection_given = true;
    } else if (argument == L"--token-file") {
      options.token_file = std::filesystem::absolute(value);
      options.connection_given = true;
    } else if (argument == L"--project-root") {
      options.project_root = std::filesystem::absolute(value);
    } else {
      throw std::invalid_argument("Unknown option " + launcher::narrow(argument) +
                                  ". Use --server URL, --token-file PATH or --project-root PATH.");
    }
  }

  // tools/start_persistent.ps1 writes the token here for the local server.
  if (options.token_file.empty()) {
    options.token_file = options.project_root / "out" / "admin" / "admin.token";
  }
  return options;
}

} // namespace
} // namespace chronicles::admin_console

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  using namespace chronicles::admin_console;

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  int argc = 0;
  const auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) {
    return 1;
  }

  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    LocalFree(argv);
    return 1;
  }

  int result = 1;
  bool cli_check = false;
  try {
    const auto options = parse_arguments(argc, argv);
    cli_check = !options.check_server.empty();
    if (cli_check) {
      ConsoleClient client({}, {options.server_url, options.token_file}, true,
                           server_profiles_file(options));
      const auto overview = client.connect({{"server_id", options.check_server}});
      std::cout << Json{{"connected", true},
                        {"server_id", options.check_server},
                        {"realm", overview.at("realm").at("world_name")}}
                       .dump()
                << "\n";
      result = 0;
    } else {
      chronicles::launcher::ensure_webview_runtime(executable_directory());
      result = run_console(options);
    }
  } catch (const std::exception &error) {
    if (cli_check) {
      std::cerr << error.what() << "\n";
    } else {
      MessageBoxW(nullptr, chronicles::launcher::widen(error.what()).c_str(), kWindowTitle,
                  MB_OK | MB_ICONERROR);
    }
  }

  CoUninitialize();
  LocalFree(argv);
  return result;
}
