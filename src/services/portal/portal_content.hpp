#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace chronicles {

// Shared by the operator, HTTP service and native launcher. Returns only recognized fields.
nlohmann::json validate_portal_entry(std::string_view kind, const nlohmann::json &entry);

void validate_portal_kind(std::string_view kind);

void validate_portal_id(const std::string &id);

void validate_portal_url(const std::string &url);

} // namespace chronicles
