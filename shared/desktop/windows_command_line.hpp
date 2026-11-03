#pragma once

#include <string>
#include <vector>

namespace chronicles::launcher {

// Quotes one argument using the Windows CreateProcess command-line escaping rules.
std::wstring join_windows_command_line(const std::vector<std::wstring> &arguments);

} // namespace chronicles::launcher
