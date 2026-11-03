#include "windows_command_line.hpp"

namespace chronicles::launcher {
namespace {

std::wstring quote_argument(const std::wstring &argument) {
  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }

    if (character == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
    } else {
      quoted.append(backslashes, L'\\');
      quoted.push_back(character);
    }
    backslashes = 0;
  }

  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

} // namespace

std::wstring join_windows_command_line(const std::vector<std::wstring> &arguments) {
  std::wstring command;
  for (const auto &argument : arguments) {
    if (!command.empty()) {
      command.push_back(L' ');
    }
    command += quote_argument(argument);
  }
  return command;
}

} // namespace chronicles::launcher
