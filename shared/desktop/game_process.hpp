#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chronicles::launcher {

struct ClientProcess {
  std::uint32_t pid{};
  std::int64_t create_time{};
  std::filesystem::path executable;
};

// The real game process, not the same-named Steam bootstrap executable at the install root.
std::filesystem::path expected_client_executable(const std::filesystem::path &game_directory);

std::optional<ClientProcess> find_client_process(const std::filesystem::path &game_directory);

std::vector<ClientProcess> find_client_processes(const std::filesystem::path &game_directory);

std::optional<ClientProcess> find_client_process_by_pid(const std::filesystem::path &game_directory,
                                                        std::uint32_t pid);

// Launches through Steam so platform initialization and Steam ticket issuance stay intact.
void launch_through_steam(const std::string &app_id,
                          const std::filesystem::path &bootstrap_archive);

// Starts the verified managed executable with a child-only Steam environment and bootstrap.
std::uint32_t launch_managed_client(const std::filesystem::path &game_directory,
                                    const std::string &app_id,
                                    const std::filesystem::path &bootstrap_archive);

// Starts a guarded private copy through the local launch helper. The helper checks its
// replacement module and outbound firewall rules before each launch.
void launch_private_client(const std::filesystem::path &project_root,
                           const std::filesystem::path &game_directory, const std::string &app_id,
                           const std::filesystem::path &bootstrap_archive);

} // namespace chronicles::launcher
