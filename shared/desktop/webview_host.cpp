#include "webview_host.hpp"

#include "encoding.hpp"

#include <windows.h>
#include <windowsx.h>

#include <objbase.h>
#include <wrl.h>

#include <WebView2.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <vector>

namespace chronicles::launcher {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kDeliverStateMessage = WM_APP + 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 2;
constexpr UINT kTrayIconId = 1;
constexpr UINT kShowLauncherCommand = 1;
constexpr UINT kExitLauncherCommand = 2;

constexpr wchar_t kWindowClass[] = L"ChroniclesLauncherWindow";

struct StateDelivery {
  std::mutex mutex;
  std::string latest;
  std::deque<std::string> events;
  bool pending{};
};

} // namespace

struct WebViewHost::Impl {
  HWND window{};
  ActionHandler handler;
  WebViewWindowOptions options;
  StateDelivery delivery;
  ComPtr<ICoreWebView2> webview;
  ComPtr<ICoreWebView2Controller> controller;
  bool ready{};
  std::atomic<bool> closed{};
  std::wstring trusted_uri;
  UINT taskbar_created_message{};
  bool tray_registered{};
  bool game_running{};
  bool has_game_state{};
  bool restore_maximized{};
  HICON large_icon{};
  HICON small_icon{};

  ~Impl();
  NOTIFYICONDATAW tray_data() const;
  bool ensure_tray_icon();
  void remove_tray_icon();
  void minimize_to_tray();
  void restore_window();
  void show_tray_menu();
  void update_game_state(const std::string &state_json);
};

WebViewHost::Impl::~Impl() {
  remove_tray_icon();
}

NOTIFYICONDATAW WebViewHost::Impl::tray_data() const {
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = window;
  data.uID = kTrayIconId;
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  data.uCallbackMessage = kTrayCallbackMessage;
  data.hIcon = small_icon;
  const auto tooltip = game_running ? options.title + L" - Game running" : options.title;
  wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
  return data;
}

bool WebViewHost::Impl::ensure_tray_icon() {
  if (tray_registered) {
    return true;
  }

  auto data = tray_data();
  if (!Shell_NotifyIconW(NIM_ADD, &data)) {
    return false;
  }

  data.uVersion = NOTIFYICON_VERSION_4;
  if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) {
    Shell_NotifyIconW(NIM_DELETE, &data);
    return false;
  }

  tray_registered = true;
  return true;
}

void WebViewHost::Impl::remove_tray_icon() {
  if (tray_registered) {
    auto data = tray_data();
    Shell_NotifyIconW(NIM_DELETE, &data);
    tray_registered = false;
  }
}

void WebViewHost::Impl::minimize_to_tray() {
  // Keep a taskbar button if Explorer cannot provide a way to restore the launcher.
  if (!ensure_tray_icon()) {
    if (!IsIconic(window)) {
      ShowWindow(window, SW_MINIMIZE);
    }
    return;
  }

  if (!IsIconic(window)) {
    restore_maximized = IsZoomed(window) != FALSE;
  }
  if (controller) {
    controller->put_IsVisible(FALSE);
  }
  ShowWindow(window, SW_HIDE);
}

void WebViewHost::Impl::restore_window() {
  if (controller) {
    controller->put_IsVisible(TRUE);
  }
  ShowWindow(window, restore_maximized ? SW_SHOWMAXIMIZED : SW_RESTORE);
  SetForegroundWindow(window);
}

void WebViewHost::Impl::show_tray_menu() {
  const auto menu = CreatePopupMenu();
  if (menu == nullptr) {
    restore_window();
    return;
  }

  AppendMenuW(menu, MF_STRING, kShowLauncherCommand, options.tray_show_label.c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExitLauncherCommand, options.tray_exit_label.c_str());
  SetMenuDefaultItem(menu, kShowLauncherCommand, FALSE);

  POINT cursor{};
  GetCursorPos(&cursor);
  SetForegroundWindow(window);
  const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                      cursor.x, cursor.y, 0, window, nullptr);
  DestroyMenu(menu);
  // Let the shell dismiss the menu correctly when a click lands outside it.
  PostMessageW(window, WM_NULL, 0, 0);

  if (command == kShowLauncherCommand) {
    restore_window();
  } else if (command == kExitLauncherCommand) {
    PostMessageW(window, WM_CLOSE, 0, 0);
  }
}

void WebViewHost::Impl::update_game_state(const std::string &state_json) {
  const auto state = nlohmann::json::parse(state_json, nullptr, false);
  if (!state.is_object() || !state.contains("client") || !state.at("client").is_object()) {
    return;
  }
  const auto &client = state.at("client");
  if (!client.contains("running") || !client.at("running").is_boolean()) {
    return;
  }

  const bool now_running = client.at("running").get<bool>();
  const auto &launch = state.value("launch", nlohmann::json::object());
  const bool launched_by_this_window = launch.is_object() && launch.contains("attempt") &&
                                       launch.at("attempt").is_number_unsigned() &&
                                       launch.at("attempt").get<std::uint64_t>() > 0;
  const bool game_started =
      launched_by_this_window && has_game_state && now_running && !game_running;
  const bool game_exited = !now_running && game_running;
  game_running = now_running;
  has_game_state = true;
  if (tray_registered) {
    auto data = tray_data();
    Shell_NotifyIconW(NIM_MODIFY, &data);
  }
  // A late status update can report an already running client after the first snapshot.
  // Only a launch requested in this window should send it to the tray.
  if (game_started) {
    minimize_to_tray();
  } else if (game_exited) {
    restore_window();
  }
}

namespace {

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto *impl = reinterpret_cast<WebViewHost::Impl *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(window, message, wparam, lparam);
  }
  if (impl == nullptr) {
    return DefWindowProcW(window, message, wparam, lparam);
  }

  if (impl->taskbar_created_message != 0 && message == impl->taskbar_created_message) {
    if (impl->tray_registered) {
      impl->tray_registered = false;
      if (!impl->ensure_tray_icon() && !IsWindowVisible(window)) {
        impl->restore_window();
      }
    }
    return 0;
  }

  switch (message) {
  case WM_SETFOCUS:
    if (!impl->options.forward_keyboard_focus || !impl->controller) {
      return DefWindowProcW(window, message, wparam, lparam);
    }
    // The native frame has no editors. Return keyboard input to the WebView's
    // previously focused control when the admin window is activated again.
    impl->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    return 0;

  case WM_SYSCOMMAND:
    if ((wparam & 0xFFF0) == SC_MINIMIZE) {
      impl->minimize_to_tray();
      return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);

  case kTrayCallbackMessage:
    if (HIWORD(lparam) != kTrayIconId) {
      return 0;
    }
    if (LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT ||
        LOWORD(lparam) == WM_LBUTTONDBLCLK) {
      impl->restore_window();
    } else if (LOWORD(lparam) == WM_CONTEXTMENU) {
      impl->show_tray_menu();
    }
    return 0;

  case WM_NCCALCSIZE:
    // Keep the resizable Win32 frame while the WebView renders our title bar.
    if (wparam != 0) {
      return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);

  case WM_NCHITTEST: {
    const auto default_hit = DefWindowProcW(window, message, wparam, lparam);
    if (default_hit != HTCLIENT || IsZoomed(window)) {
      return default_hit;
    }
    RECT bounds{};
    GetWindowRect(window, &bounds);
    const int border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, GetDpiForWindow(window)) +
                       GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(window));
    const int cursor_x = GET_X_LPARAM(lparam);
    const int cursor_y = GET_Y_LPARAM(lparam);
    const bool left = cursor_x < bounds.left + border;
    const bool right = cursor_x >= bounds.right - border;
    const bool top = cursor_y < bounds.top + border;
    const bool bottom = cursor_y >= bounds.bottom - border;
    if (top && left) {
      return HTTOPLEFT;
    }
    if (top && right) {
      return HTTOPRIGHT;
    }
    if (bottom && left) {
      return HTBOTTOMLEFT;
    }
    if (bottom && right) {
      return HTBOTTOMRIGHT;
    }
    if (left) {
      return HTLEFT;
    }
    if (right) {
      return HTRIGHT;
    }
    if (top) {
      return HTTOP;
    }
    if (bottom) {
      return HTBOTTOM;
    }
    return HTCLIENT;
  }

  case WM_GETMINMAXINFO: {
    auto *limits = reinterpret_cast<MINMAXINFO *>(lparam);
    const auto dpi = GetDpiForWindow(window);
    limits->ptMinTrackSize = {MulDiv(960, dpi, 96), MulDiv(640, dpi, 96)};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
      limits->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left,
                               monitor.rcWork.top - monitor.rcMonitor.top};
      limits->ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left,
                           monitor.rcWork.bottom - monitor.rcWork.top};
    }
    return 0;
  }

  case WM_DPICHANGED: {
    const auto *bounds = reinterpret_cast<RECT *>(lparam);
    SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                 bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
  }

  case WM_SIZE:
    if (wparam == SIZE_MINIMIZED) {
      impl->minimize_to_tray();
      return 0;
    }
    if (impl->controller) {
      RECT bounds{};
      GetClientRect(window, &bounds);
      impl->controller->put_Bounds(bounds);
    }
    return 0;

  case kDeliverStateMessage: {
    if (!impl->ready || !impl->webview) {
      return 0;
    }
    std::string state;
    std::deque<std::string> events;
    {
      std::scoped_lock lock(impl->delivery.mutex);
      if (impl->delivery.pending) {
        state = impl->delivery.latest;
        impl->delivery.pending = false;
      }
      events.swap(impl->delivery.events);
    }
    if (!state.empty()) {
      impl->webview->PostWebMessageAsJson(widen(state).c_str());
      impl->update_game_state(state);
    }
    for (const auto &event : events) {
      impl->webview->PostWebMessageAsJson(widen(event).c_str());
    }
    return 0;
  }

  case WM_CLOSE:
    impl->closed = true;
    DestroyWindow(window);
    return 0;

  case WM_DESTROY:
    impl->closed = true;
    impl->remove_tray_icon();
    PostQuitMessage(0);
    return 0;

  default:
    return DefWindowProcW(window, message, wparam, lparam);
  }
}

std::wstring file_uri(const std::filesystem::path &path) {
  constexpr DWORD kMaximumUrlLength = 2083;
  std::vector<wchar_t> buffer(kMaximumUrlLength);
  DWORD length = static_cast<DWORD>(buffer.size());
  if (FAILED(UrlCreateFromPathW(path.c_str(), buffer.data(), &length, 0))) {
    throw std::runtime_error("Could not build the launcher UI URL");
  }
  return std::wstring(buffer.data());
}

void report_webview_failure(HWND window, const std::wstring &title, HRESULT result) {
  const auto message = L"Could not initialize the interface. Install or repair the "
                       L"Microsoft Edge WebView2 Runtime. HRESULT: " +
                       std::to_wstring(result);
  MessageBoxW(window, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
  PostMessageW(window, WM_CLOSE, 0, 0);
}

} // namespace

WebViewHost::WebViewHost() : impl_(std::make_unique<Impl>()) {}

WebViewHost::~WebViewHost() = default;

void WebViewHost::create(const std::filesystem::path &ui_directory,
                         const std::filesystem::path &user_data_directory, ActionHandler handler,
                         WebViewWindowOptions options) {
  impl_->handler = std::move(handler);
  impl_->options = std::move(options);
  impl_->taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");

  const auto instance = GetModuleHandleW(nullptr);
  const auto load_icon = [&](int width, int height) {
    HICON icon = nullptr;
    if (impl_->options.icon_resource_id != 0) {
      icon =
          static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(impl_->options.icon_resource_id),
                                        IMAGE_ICON, width, height, LR_SHARED));
    }
    return icon != nullptr ? icon : LoadIconW(nullptr, IDI_APPLICATION);
  };
  impl_->large_icon = load_icon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
  impl_->small_icon = load_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.hInstance = instance;
  window_class.lpszClassName = kWindowClass;
  window_class.lpfnWndProc = &window_procedure;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  window_class.hIcon = impl_->large_icon;
  window_class.hIconSm = impl_->small_icon;
  RegisterClassExW(&window_class);

  const auto dpi = GetDpiForSystem();
  const int width = std::min(MulDiv(1440, dpi, 96), GetSystemMetrics(SM_CXSCREEN) - 40);
  const int height = std::min(MulDiv(900, dpi, 96), GetSystemMetrics(SM_CYSCREEN) - 60);
  const int left = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
  const int top = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

  impl_->window =
      CreateWindowExW(0, kWindowClass, impl_->options.title.c_str(), WS_OVERLAPPEDWINDOW, left, top,
                      width, height, nullptr, nullptr, instance, impl_.get());
  if (impl_->window == nullptr) {
    throw std::runtime_error("Could not create the launcher window");
  }

  const BOOL dark = TRUE;
  DwmSetWindowAttribute(impl_->window, 20, &dark, sizeof(dark));
  SetWindowPos(impl_->window, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  ShowWindow(impl_->window, SW_SHOW);
  UpdateWindow(impl_->window);

  const auto index = ui_directory / "index.html";
  if (!std::filesystem::is_regular_file(index)) {
    throw std::runtime_error("Launcher UI not found: " + index.string());
  }

  std::filesystem::create_directories(user_data_directory);
  const auto uri = file_uri(index);
  impl_->trusted_uri = uri;

  const auto environment_callback =
      Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this, uri](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
            if (FAILED(result) || environment == nullptr) {
              report_webview_failure(impl_->window, impl_->options.title,
                                     FAILED(result) ? result : E_POINTER);
              return result;
            }

            ComPtr<ICoreWebView2Environment> env = environment;
            env->CreateCoreWebView2Controller(
                impl_->window,
                Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this, uri](HRESULT controller_result,
                                ICoreWebView2Controller *controller) -> HRESULT {
                      if (FAILED(controller_result) || controller == nullptr) {
                        report_webview_failure(impl_->window, impl_->options.title,
                                               FAILED(controller_result) ? controller_result
                                                                         : E_POINTER);
                        return controller_result;
                      }

                      impl_->controller = controller;
                      impl_->controller->get_CoreWebView2(&impl_->webview);

                      ComPtr<ICoreWebView2Settings> settings;
                      impl_->webview->get_Settings(&settings);
                      settings->put_IsScriptEnabled(TRUE);
                      settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                      settings->put_IsWebMessageEnabled(TRUE);
                      settings->put_AreDevToolsEnabled(FALSE);
                      settings->put_AreDefaultContextMenusEnabled(FALSE);
                      settings->put_IsZoomControlEnabled(FALSE);
                      settings->put_IsStatusBarEnabled(FALSE);

                      RECT client{};
                      GetClientRect(impl_->window, &client);
                      impl_->controller->put_Bounds(client);
                      impl_->controller->put_IsVisible(TRUE);
                      if (impl_->options.forward_keyboard_focus &&
                          GetFocus() == impl_->window) {
                        impl_->controller->MoveFocus(
                            COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                      }

                      EventRegistrationToken message_token{};
                      impl_->webview->add_WebMessageReceived(
                          Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [this](ICoreWebView2 *,
                                     ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                                LPWSTR raw = nullptr;
                                LPWSTR source = nullptr;
                                args->get_Source(&source);
                                const bool trusted =
                                    source != nullptr && impl_->trusted_uri == source;
                                CoTaskMemFree(source);
                                if (!trusted) {
                                  return S_OK;
                                }
                                args->get_WebMessageAsJson(&raw);
                                if (raw == nullptr) {
                                  return S_OK;
                                }

                                try {
                                  const auto action = nlohmann::json::parse(narrow(raw));
                                  if (!action.is_object() || !action.contains("action") ||
                                      !action.at("action").is_string()) {
                                    CoTaskMemFree(raw);
                                    return S_OK;
                                  }
                                  const auto name = action.at("action").get<std::string>();
                                  if (name == "ui_ready") {
                                    // Navigation can finish after the worker's first state or
                                    // error. Replay it without waiting for a network request.
                                    {
                                      std::scoped_lock lock(impl_->delivery.mutex);
                                      impl_->delivery.pending = true;
                                    }
                                    impl_->ready = true;
                                    PostMessageW(impl_->window, kDeliverStateMessage, 0, 0);
                                    if (impl_->handler) {
                                      impl_->handler(action);
                                    }
                                  } else if (name == "window_minimize") {
                                    impl_->minimize_to_tray();
                                  } else if (name == "window_maximize") {
                                    ShowWindow(impl_->window,
                                               IsZoomed(impl_->window) ? SW_RESTORE : SW_MAXIMIZE);
                                  } else if (name == "window_close") {
                                    PostMessageW(impl_->window, WM_CLOSE, 0, 0);
                                  } else if (name == "window_drag") {
                                    ReleaseCapture();
                                    PostMessageW(impl_->window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                                  } else if (impl_->handler) {
                                    impl_->handler(action);
                                  }
                                } catch (const std::exception &) {
                                  // Malformed UI messages are ignored rather than crashing the
                                  // host.
                                }
                                CoTaskMemFree(raw);
                                return S_OK;
                              })
                              .Get(),
                          &message_token);

                      EventRegistrationToken navigation_token{};
                      impl_->webview->add_NavigationStarting(
                          Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                              [this](ICoreWebView2 *,
                                     ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                LPWSTR destination = nullptr;
                                args->get_Uri(&destination);
                                const bool trusted =
                                    destination != nullptr && impl_->trusted_uri == destination;
                                CoTaskMemFree(destination);
                                args->put_Cancel(!trusted);
                                if (trusted) {
                                  impl_->ready = false;
                                }
                                return S_OK;
                              })
                              .Get(),
                          &navigation_token);

                      EventRegistrationToken popup_token{};
                      impl_->webview->add_NewWindowRequested(
                          Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [](ICoreWebView2 *,
                                 ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                                args->put_Handled(TRUE);
                                return S_OK;
                              })
                              .Get(),
                          &popup_token);

                      impl_->webview->Navigate(uri.c_str());
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          });

  const HRESULT creation = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data_directory.wstring().c_str(), nullptr, environment_callback.Get());
  if (FAILED(creation)) {
    throw std::runtime_error(
        "WebView2 is unavailable. Install the Microsoft Edge WebView2 Runtime (HRESULT " +
        std::to_string(creation) + ")");
  }
}

void WebViewHost::post_state(const std::string &state_json) {
  {
    std::scoped_lock lock(impl_->delivery.mutex);
    impl_->delivery.latest = state_json;
    impl_->delivery.pending = true;
  }
  if (impl_->window != nullptr && !impl_->closed) {
    PostMessageW(impl_->window, kDeliverStateMessage, 0, 0);
  }
}

void WebViewHost::post_event(const std::string &event_json) {
  {
    std::scoped_lock lock(impl_->delivery.mutex);
    impl_->delivery.events.push_back(event_json);
  }
  if (impl_->window != nullptr && !impl_->closed) {
    PostMessageW(impl_->window, kDeliverStateMessage, 0, 0);
  }
}

int WebViewHost::run_message_loop() {
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}

void WebViewHost::close() {
  if (impl_->window != nullptr) {
    PostMessageW(impl_->window, WM_CLOSE, 0, 0);
  }
}

} // namespace chronicles::launcher
