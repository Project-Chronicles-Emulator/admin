#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles::launcher {

class MemoryAccessError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class OperationCancelled : public std::runtime_error {
public:
  OperationCancelled() : std::runtime_error("Operation cancelled") {}
};

// Access to another process's memory. Implementations throw MemoryAccessError on failure and
// OperationCancelled when a stop is requested during a scan. Scans cover committed, private,
// writable memory, which is where the client's heap-allocated strings live.
class ProcessMemory {
public:
  virtual ~ProcessMemory() = default;

  virtual std::vector<std::uint8_t> read(std::uint64_t address, std::size_t size) = 0;

  virtual void write(std::uint64_t address, std::span<const std::uint8_t> bytes) = 0;

  // Addresses where the UTF-16 text starts.
  virtual std::vector<std::uint64_t> find_utf16(std::u16string_view text, std::stop_token stop) = 0;

  // 8-byte aligned addresses holding exactly this pointer value.
  virtual std::vector<std::uint64_t> find_pointer(std::uint64_t value, std::stop_token stop) = 0;
};

} // namespace chronicles::launcher
