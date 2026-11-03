#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace chronicles::admin_console {

nlohmann::json ssh_admin_request(const nlohmann::json &configuration, const std::string &method,
                                 const std::string &path, const nlohmann::json &body);

} // namespace chronicles::admin_console
