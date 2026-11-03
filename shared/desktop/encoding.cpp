#include "encoding.hpp"

#include <windows.h>

#include <array>
#include <charconv>
#include <stdexcept>

namespace chronicles::launcher {
namespace {

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr char kBase64Padding = '=';

constexpr std::uint8_t kInvalidBase64Value = 0xFF;

std::array<std::uint8_t, 256> build_base64_lookup() {
  std::array<std::uint8_t, 256> lookup{};
  lookup.fill(kInvalidBase64Value);
  for (std::size_t index = 0; index < kBase64Alphabet.size(); ++index) {
    const auto character = static_cast<unsigned char>(kBase64Alphabet[index]);
    lookup[character] = static_cast<std::uint8_t>(index);
  }
  return lookup;
}

} // namespace

std::string base64_encode(std::span<const std::uint8_t> bytes) {
  std::string encoded;
  encoded.reserve(((bytes.size() + 2) / 3) * 4);

  std::size_t index = 0;
  while (index + 3 <= bytes.size()) {
    const std::uint32_t group = (static_cast<std::uint32_t>(bytes[index]) << 16) |
                                (static_cast<std::uint32_t>(bytes[index + 1]) << 8) |
                                static_cast<std::uint32_t>(bytes[index + 2]);
    encoded.push_back(kBase64Alphabet[(group >> 18) & 0x3F]);
    encoded.push_back(kBase64Alphabet[(group >> 12) & 0x3F]);
    encoded.push_back(kBase64Alphabet[(group >> 6) & 0x3F]);
    encoded.push_back(kBase64Alphabet[group & 0x3F]);
    index += 3;
  }

  const auto remaining = bytes.size() - index;
  if (remaining == 1) {
    const std::uint32_t group = static_cast<std::uint32_t>(bytes[index]) << 16;
    encoded.push_back(kBase64Alphabet[(group >> 18) & 0x3F]);
    encoded.push_back(kBase64Alphabet[(group >> 12) & 0x3F]);
    encoded.push_back(kBase64Padding);
    encoded.push_back(kBase64Padding);
  } else if (remaining == 2) {
    const std::uint32_t group = (static_cast<std::uint32_t>(bytes[index]) << 16) |
                                (static_cast<std::uint32_t>(bytes[index + 1]) << 8);
    encoded.push_back(kBase64Alphabet[(group >> 18) & 0x3F]);
    encoded.push_back(kBase64Alphabet[(group >> 12) & 0x3F]);
    encoded.push_back(kBase64Alphabet[(group >> 6) & 0x3F]);
    encoded.push_back(kBase64Padding);
  }

  return encoded;
}

std::vector<std::uint8_t> base64_decode(std::string_view text) {
  static const auto lookup = build_base64_lookup();

  std::vector<std::uint8_t> decoded;
  decoded.reserve((text.size() / 4) * 3);

  std::uint32_t accumulator = 0;
  int accumulated_bits = 0;
  for (const char character : text) {
    if (character == kBase64Padding) {
      break;
    }

    const auto value = lookup[static_cast<unsigned char>(character)];
    if (value == kInvalidBase64Value) {
      throw std::invalid_argument("Invalid base64 character");
    }

    accumulator = (accumulator << 6) | value;
    accumulated_bits += 6;
    if (accumulated_bits >= 8) {
      accumulated_bits -= 8;
      decoded.push_back(static_cast<std::uint8_t>((accumulator >> accumulated_bits) & 0xFF));
    }
  }

  return decoded;
}

std::u16string ascii_to_utf16(std::string_view text) {
  std::u16string converted;
  converted.reserve(text.size());
  for (const char character : text) {
    const auto code = static_cast<unsigned char>(character);
    if (code > 0x7F) {
      throw std::invalid_argument("Text must be ASCII");
    }
    converted.push_back(static_cast<char16_t>(code));
  }
  return converted;
}

std::string utf16_to_display_ascii(std::u16string_view text) {
  std::string converted;
  converted.reserve(text.size());
  for (const char16_t code_unit : text) {
    if (code_unit >= 0x20 && code_unit <= 0x7E) {
      converted.push_back(static_cast<char>(code_unit));
    } else {
      converted.push_back('?');
    }
  }
  return converted;
}

std::string utf16_to_utf8(std::u16string_view text) {
  static_assert(sizeof(wchar_t) == sizeof(char16_t));
  const auto *wide = reinterpret_cast<const wchar_t *>(text.data());
  return narrow(std::wstring_view(wide, text.size()));
}

std::vector<std::uint8_t> utf16_to_bytes(std::u16string_view text, bool include_terminator) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve((text.size() + 1) * sizeof(char16_t));
  for (const char16_t code_unit : text) {
    bytes.push_back(static_cast<std::uint8_t>(code_unit & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((code_unit >> 8) & 0xFF));
  }

  if (include_terminator) {
    bytes.push_back(0);
    bytes.push_back(0);
  }
  return bytes;
}

std::u16string utf16_from_bytes(std::span<const std::uint8_t> bytes) {
  if (bytes.size() % sizeof(char16_t) != 0) {
    throw std::invalid_argument("UTF-16 byte count must be even");
  }

  std::u16string text;
  text.reserve(bytes.size() / sizeof(char16_t));
  for (std::size_t index = 0; index < bytes.size(); index += sizeof(char16_t)) {
    const auto low = static_cast<char16_t>(bytes[index]);
    const auto high = static_cast<char16_t>(bytes[index + 1]);
    text.push_back(static_cast<char16_t>(low | (high << 8)));
  }
  return text;
}

std::string format_address(std::uint64_t address) {
  std::array<char, 2 + 16> digits{};
  const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), address, 16);

  std::string formatted = "0x";
  for (const char *cursor = digits.data(); cursor != result.ptr; ++cursor) {
    const char digit = *cursor;
    if (digit >= 'a' && digit <= 'f') {
      formatted.push_back(static_cast<char>(digit - 'a' + 'A'));
    } else {
      formatted.push_back(digit);
    }
  }
  return formatted;
}

std::uint64_t parse_address(std::string_view text) {
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
  }
  if (text.empty()) {
    throw std::invalid_argument("Address is empty");
  }

  std::uint64_t address = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), address, 16);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    throw std::invalid_argument("Address is not hexadecimal");
  }
  return address;
}

std::uint32_t read_u32_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
    throw std::out_of_range("u32 read exceeds buffer");
  }

  std::uint32_t value = 0;
  for (std::size_t index = 0; index < sizeof(std::uint32_t); ++index) {
    value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8);
  }
  return value;
}

std::uint64_t read_u64_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint64_t)) {
    throw std::out_of_range("u64 read exceeds buffer");
  }

  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8);
  }
  return value;
}

std::vector<std::uint8_t> u32_to_bytes_le(std::uint32_t value) {
  std::vector<std::uint8_t> bytes(sizeof(std::uint32_t));
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF);
  }
  return bytes;
}

std::wstring widen(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }

  const auto source_length = static_cast<int>(utf8.size());
  const int required =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), source_length, nullptr, 0);
  if (required <= 0) {
    throw std::invalid_argument("Text is not valid UTF-8");
  }

  std::wstring wide(static_cast<std::size_t>(required), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), source_length, wide.data(),
                      required);
  return wide;
}

std::string narrow(std::wstring_view wide) {
  if (wide.empty()) {
    return {};
  }

  const auto source_length = static_cast<int>(wide.size());
  const int required =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), source_length, nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    throw std::invalid_argument("Text cannot be converted to UTF-8");
  }

  std::string utf8(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), source_length, utf8.data(), required, nullptr,
                      nullptr);
  return utf8;
}

} // namespace chronicles::launcher
