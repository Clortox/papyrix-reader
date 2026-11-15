#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Small text helpers shared by the FreshRSS core modules. Everything is inline
// and allocation-free so the module stays usable from the host test harness and
// from the firmware.

namespace freshrss {

inline char toLowerAscii(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

inline bool equalsIgnoreCase(const char* a, const char* b) {
  if (!a || !b) return a == b;
  while (*a && *b) {
    if (toLowerAscii(*a) != toLowerAscii(*b)) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

inline bool startsWithIgnoreCase(const char* text, const char* prefix) {
  if (!text || !prefix) return false;
  while (*prefix) {
    if (toLowerAscii(*text) != toLowerAscii(*prefix)) return false;
    ++text;
    ++prefix;
  }
  return true;
}

// Copies at most capacity-1 bytes and always terminates.
inline size_t copyBounded(char* out, size_t capacity, const char* text, size_t length) {
  if (!out || capacity == 0) return 0;
  size_t count = length < capacity - 1 ? length : capacity - 1;
  if (count > 0 && text) std::memcpy(out, text, count);
  out[count] = '\0';
  return count;
}

inline size_t copyBounded(char* out, size_t capacity, const char* text) {
  return copyBounded(out, capacity, text, text ? std::strlen(text) : 0);
}

// Trims leading and trailing ASCII spaces and tabs in place.
inline void trimInPlace(char* text) {
  if (!text) return;
  const size_t length = std::strlen(text);
  size_t start = 0;
  while (start < length && (text[start] == ' ' || text[start] == '\t')) ++start;
  size_t end = length;
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
  if (start > 0) std::memmove(text, text + start, end - start);
  text[end - start] = '\0';
}

// Appends one code point as UTF-8. Surrogates and out-of-range values are
// replaced with '?' rather than producing invalid UTF-8.
inline void utf8Append(char* out, size_t capacity, size_t& written, uint32_t code) {
  uint8_t bytes[4];
  size_t count = 0;
  if (code <= 0x7F) {
    bytes[count++] = static_cast<uint8_t>(code);
  } else if (code <= 0x7FF) {
    bytes[count++] = static_cast<uint8_t>(0xC0 | (code >> 6));
    bytes[count++] = static_cast<uint8_t>(0x80 | (code & 0x3F));
  } else if (code <= 0xFFFF && !(code >= 0xD800 && code <= 0xDFFF)) {
    bytes[count++] = static_cast<uint8_t>(0xE0 | (code >> 12));
    bytes[count++] = static_cast<uint8_t>(0x80 | ((code >> 6) & 0x3F));
    bytes[count++] = static_cast<uint8_t>(0x80 | (code & 0x3F));
  } else if (code > 0xFFFF && code <= 0x10FFFF) {
    bytes[count++] = static_cast<uint8_t>(0xF0 | (code >> 18));
    bytes[count++] = static_cast<uint8_t>(0x80 | ((code >> 12) & 0x3F));
    bytes[count++] = static_cast<uint8_t>(0x80 | ((code >> 6) & 0x3F));
    bytes[count++] = static_cast<uint8_t>(0x80 | (code & 0x3F));
  } else {
    bytes[count++] = '?';
  }
  for (size_t i = 0; i < count; ++i) {
    if (out && written + 1 < capacity) out[written++] = static_cast<char>(bytes[i]);
  }
}

inline int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline bool isJsonSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

}  // namespace freshrss
