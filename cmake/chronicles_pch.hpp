#pragma once

// Precompiled for emulator targets that link nlohmann/json and spdlog. Keep this list to
// standard and third-party headers that many sources include; never add project headers,
// because editing one would rebuild every precompiled header. Boost.Asio stays out: it brings
// <windows.h> macros such as min, max and ERROR into files that never included it.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
