#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles::launcher {

std::string base64_encode(std::span<const std::uint8_t> bytes);

// Throws std::invalid_argument for characters outside the standard base64 alphabet.
std::vector<std::uint8_t> base64_decode(std::string_view text);

// Throws std::invalid_argument for non-ASCII input; endpoint URLs written into the client are
// ASCII.
std::u16string ascii_to_utf16(std::string_view text);

// Replaces non-ASCII code units with '?'. Intended for logs and UI display only.
std::string utf16_to_display_ascii(std::u16string_view text);

// Lossless UTF-16 to UTF-8 conversion, used for bridge requests.
std::string utf16_to_utf8(std::u16string_view text);

// Little-endian UTF-16 bytes, as stored by the Windows client.
std::vector<std::uint8_t> utf16_to_bytes(std::u16string_view text, bool include_terminator);

// Throws std::invalid_argument when the byte count is odd.
std::u16string utf16_from_bytes(std::span<const std::uint8_t> bytes);

std::string format_address(std::uint64_t address);

// Accepts hexadecimal text with an optional 0x prefix. Throws std::invalid_argument.
std::uint64_t parse_address(std::string_view text);

std::uint32_t read_u32_le(std::span<const std::uint8_t> bytes, std::size_t offset);

std::uint64_t read_u64_le(std::span<const std::uint8_t> bytes, std::size_t offset);

std::vector<std::uint8_t> u32_to_bytes_le(std::uint32_t value);

std::wstring widen(std::string_view utf8);

std::string narrow(std::wstring_view wide);

} // namespace chronicles::launcher
