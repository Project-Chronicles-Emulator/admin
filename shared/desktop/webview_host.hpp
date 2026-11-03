#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace chronicles::launcher {

// Window text for a host. The defaults are the player launcher's; other tools built on this
// host (such as the server admin console) pass their own.
struct WebViewWindowOptions {
  std::wstring title = L"Project Chronicles Launcher";
  std::wstring tray_show_label = L"Show launcher";
  std::wstring tray_exit_label = L"Exit launcher";
  int icon_resource_id{};
  bool forward_keyboard_focus{};
};

// Native Win32 window hosting a WebView2 (Edge/Chromium) view. The UI is plain HTML/CSS/JS; all
// privileged work stays in the C++ controller on its worker thread.
class WebViewHost {
public:
  using ActionHandler = std::function<void(const nlohmann::json &action)>;

  WebViewHost();
  ~WebViewHost();

  void create(const std::filesystem::path &ui_directory,
              const std::filesystem::path &user_data_directory, ActionHandler handler,
              WebViewWindowOptions options = {});

  // Thread-safe. Only the newest state is delivered, so a busy controller cannot flood the view.
  void post_state(const std::string &state_json);

  // Action results are queued separately so periodic state updates cannot overwrite them.
  void post_event(const std::string &event_json);

  int run_message_loop();

  void close();

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace chronicles::launcher
