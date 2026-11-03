#include "services/portal/portal_content.hpp"
#include "distribution/distribution.hpp"

#include <algorithm>
#include <stdexcept>

namespace chronicles {
namespace {

std::string text_field(const nlohmann::json &entry, const char *name, std::size_t maximum,
                       bool allow_empty = false) {
  const auto value = entry.at(name).get<std::string>();
  const bool has_text =
      std::ranges::any_of(value, [](unsigned char character) { return character > 32; });
  if ((!allow_empty && !has_text) || value.size() > maximum ||
      std::ranges::any_of(value, [](unsigned char character) { return character < 32; })) {
    throw std::invalid_argument(std::string(name) +
                                " is empty, too long or contains control characters");
  }
  return value;
}

} // namespace

void validate_portal_kind(std::string_view kind) {
  if (kind != "store" && kind != "community") {
    throw std::invalid_argument("Content kind must be store or community");
  }
}

void validate_portal_id(const std::string &id) {
  if (id.empty() || id.size() > 80 || !std::ranges::all_of(id, [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '-';
      })) {
    throw std::invalid_argument(
        "Content id must contain 1-80 lowercase letters, digits or hyphens");
  }
}

void validate_portal_url(const std::string &url) {
  if (url.size() > 2048 || !url.starts_with("https://") && !url.starts_with("http://") ||
      std::ranges::any_of(url, [](unsigned char character) {
        return character <= 32 || character >= 127 || character == '\\' || character == '"' ||
               character == '<' || character == '>';
      })) {
    throw std::invalid_argument("Links must be ASCII HTTPS URLs (HTTP is loopback-only)");
  }
  const auto authority_start = url.starts_with("https://") ? 8 : 7;
  const auto path_start = url.find_first_of("/?#", authority_start);
  distribution::parse_service_origin(url.substr(0, path_start));
}

nlohmann::json validate_portal_entry(std::string_view kind, const nlohmann::json &entry) {
  validate_portal_kind(kind);
  const auto id = text_field(entry, "id", 80);
  validate_portal_id(id);
  const auto url = text_field(entry, "url", 2048);
  validate_portal_url(url);
  const auto &order = entry.at("sort_order");
  if (!order.is_number_integer() || order < -1000 || order > 1000 ||
      !entry.at("published").is_boolean()) {
    throw std::invalid_argument("sort_order must be -1000..1000 and published must be a boolean");
  }
  nlohmann::json result{{"id", id},
                        {"url", url},
                        {"sort_order", order},
                        {"published", entry.at("published")},
                        {"title", text_field(entry, "title", 160)},
                        {"summary", text_field(entry, "summary", 400)}};
  if (kind == "store") {
    const auto art = text_field(entry, "art", 16);
    if (art != "citadel" && art != "voyage" && art != "company" && art != "discover") {
      throw std::invalid_argument("Store art must be citadel, voyage, company or discover");
    }
    result["art"] = art;
    result["category"] = text_field(entry, "category", 40);
    result["price_label"] = text_field(entry, "price_label", 60, true);
  } else {
    const auto type = text_field(entry, "type", 16);
    if (type != "discord" && type != "forum" && type != "support" && type != "website") {
      throw std::invalid_argument("Community type must be discord, forum, support or website");
    }
    result["type"] = type;
  }
  return result;
}

} // namespace chronicles
