#pragma once

#include "process_memory.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace chronicles::launcher {

// Owns the launcher-started playsharp-bridge.exe child. The bridge speaks one JSON object per
// line on stdin/stdout and never unloads the kernel driver when it exits.
class PlaySharpBridge {
public:
  PlaySharpBridge();
  ~PlaySharpBridge();

  PlaySharpBridge(const PlaySharpBridge &) = delete;
  PlaySharpBridge &operator=(const PlaySharpBridge &) = delete;

  void start(const std::filesystem::path &bridge_executable);

  bool is_running() const;

  // Returns the "result" object of the response. Throws MemoryAccessError for transport failures
  // or when the bridge reports success:false.
  nlohmann::json call(const std::string &method, const nlohmann::json &params,
                      std::stop_token stop);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ProcessMemory over a PlaySharpBridge, pinned to one backend for every operation. Writes never
// fall back to another backend.
class BridgeProcessMemory : public ProcessMemory {
public:
  BridgeProcessMemory(PlaySharpBridge &bridge, std::uint32_t pid, std::string backend_mode);

  std::vector<std::uint8_t> read(std::uint64_t address, std::size_t size) override;

  void write(std::uint64_t address, std::span<const std::uint8_t> bytes) override;

  std::vector<std::uint64_t> find_utf16(std::u16string_view text, std::stop_token stop) override;

  std::vector<std::uint64_t> find_pointer(std::uint64_t value, std::stop_token stop) override;

  // Process identity guard: a stale session must not read or write a new process reusing the PID.
  void verify_process_identity() const;

private:
  std::vector<std::uint64_t> run_scan(nlohmann::json params, std::stop_token stop);

  PlaySharpBridge &bridge_;
  std::uint32_t pid_;
  std::string mode_;
  std::int64_t create_time_;
};

// Creation time of a live process in 100 ns intervals since 1601, or 0 when it has exited or
// cannot be queried.
std::int64_t query_process_create_time(std::uint32_t pid);

} // namespace chronicles::launcher
