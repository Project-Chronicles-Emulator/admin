#pragma once

#include <filesystem>

namespace chronicles::launcher {

// Uses the bundled, hash-pinned Microsoft bootstrapper only when WebView2 is absent.
void ensure_webview_runtime(const std::filesystem::path &directory, bool show_progress = true);

void ensure_visual_cpp_runtime(const std::filesystem::path &directory,
                               const std::filesystem::path &cache_directory);

} // namespace chronicles::launcher
