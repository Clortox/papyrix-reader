#include "FreshRssConfig.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "FreshRssText.h"

namespace freshrss {
namespace {

constexpr size_t kMaxHost = 127;
constexpr size_t kBasePathCapacity = sizeof(Endpoint::basePath);

bool endsWithIgnoreCase(const char* text, const char* suffix) {
  const size_t textLen = std::strlen(text);
  const size_t suffixLen = std::strlen(suffix);
  if (suffixLen > textLen) return false;
  return startsWithIgnoreCase(text + (textLen - suffixLen), suffix);
}

class Parser {
 public:
  Parser(const char* text, size_t length, char* error, size_t errorSize)
      : begin_(text), cur_(text), end_(text + length), errorOut_(error), errorCap_(errorSize) {
    if (errorCap_ > 0) errorOut_[0] = '\0';
    // Editors on Windows happily prepend a UTF-8 BOM; it is not part of JSON.
    if (length >= 3 && static_cast<uint8_t>(text[0]) == 0xEF && static_cast<uint8_t>(text[1]) == 0xBB &&
        static_cast<uint8_t>(text[2]) == 0xBF)
      cur_ += 3;
  }

  bool parseObject(Config& out) {
    skipWs();
    if (!expect('{')) return fail("expected a JSON object");
    skipWs();
    if (peek() == '}') {
      cur_++;
      return fail("config file is empty");
    }
    while (true) {
      skipWs();
      char key[40];
      if (!readString(key, sizeof(key))) return false;
      skipWs();
      if (!expect(':')) return false;
      skipWs();
      if (!readValueForKey(key, out)) return false;
      skipWs();
      const char c = peek();
      if (c == ',') {
        cur_++;
        skipWs();
        // A trailing comma is a common hand-editing mistake; reject it so the
        // user sees the reason instead of a silently ignored rest of file.
        if (peek() == '}') return fail("unexpected trailing comma");
        continue;
      }
      if (c == '}') {
        cur_++;
        return true;
      }
      return fail(c == '\0' ? "unexpected end of file" : "expected ',' or '}'");
    }
  }

 private:
  const char* begin_;
  const char* cur_;
  const char* end_;
  char* errorOut_;
  size_t errorCap_;

  char peek() const { return cur_ < end_ ? *cur_ : '\0'; }
  size_t offset() const { return static_cast<size_t>(cur_ - begin_); }

  bool fail(const char* reason) {
    if (errorCap_ > 0) std::snprintf(errorOut_, errorCap_, "%s at byte %u", reason, static_cast<unsigned>(offset()));
    if (errorCap_ > 0 && reason[0] == '\0') errorOut_[0] = '\0';
    return false;
  }

  void skipWs() {
    while (cur_ < end_) {
      const char c = *cur_;
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
        ++cur_;
      else
        break;
    }
  }

  bool expect(char wanted) {
    if (peek() != wanted) {
      char reason[32];
      std::snprintf(reason, sizeof(reason), "expected '%c'", wanted);
      return fail(reason);
    }
    cur_++;
    return true;
  }

  bool readLiteral(const char* literal) {
    const size_t length = std::strlen(literal);
    if (static_cast<size_t>(end_ - cur_) < length) return false;
    if (std::memcmp(cur_, literal, length) != 0) return false;
    cur_ += length;
    return true;
  }

  bool readStringRaw(char* out, size_t capacity) {
    if (peek() != '"') return fail("expected a quoted string");
    cur_++;
    size_t written = 0;
    if (capacity > 0) out[0] = '\0';
    while (true) {
      if (cur_ >= end_) return fail("unterminated string");
      const char c = *cur_++;
      if (c == '"') {
        if (capacity > 0) out[written < capacity ? written : capacity - 1] = '\0';
        return true;
      }
      if (c != '\\') {
        if (written + 1 < capacity) out[written++] = c;
        continue;
      }
      if (cur_ >= end_) return fail("unterminated escape");
      const char esc = *cur_++;
      switch (esc) {
        case '"':
        case '\\':
        case '/':
          if (written + 1 < capacity) out[written++] = esc;
          break;
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't': {
          const char mapped = esc == 'b' ? '\b' : esc == 'f' ? '\f' : esc == 'n' ? '\n' : esc == 'r' ? '\r' : '\t';
          if (written + 1 < capacity) out[written++] = mapped;
          break;
        }
        case 'u': {
          if (static_cast<size_t>(end_ - cur_) < 4) return fail("truncated \\u escape");
          uint32_t code = 0;
          for (int i = 0; i < 4; ++i) {
            const int digit = hexValue(*cur_++);
            if (digit < 0) return fail("bad \\u escape");
            code = (code << 4) | static_cast<uint32_t>(digit);
          }
          // Surrogate pair: fold the low half into one code point.
          if (code >= 0xD800 && code <= 0xDBFF && static_cast<size_t>(end_ - cur_) >= 6 && cur_[0] == '\\' &&
              cur_[1] == 'u') {
            uint32_t low = 0;
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
              const int digit = hexValue(cur_[2 + i]);
              if (digit < 0) {
                ok = false;
                break;
              }
              low = (low << 4) | static_cast<uint32_t>(digit);
            }
            if (ok && low >= 0xDC00 && low <= 0xDFFF) {
              cur_ += 6;
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
          }
          utf8Append(out, capacity, written, code);
          break;
        }
        default:
          return fail("unknown escape");
      }
    }
  }

  bool readString(char* out, size_t capacity) { return readStringRaw(out, capacity); }

  bool readNumber(long& out) {
    const char* start = cur_;
    bool negative = false;
    if (peek() == '-') {
      negative = true;
      cur_++;
    }
    if (peek() < '0' || peek() > '9') return fail("expected a number");
    long value = 0;
    bool overflow = false;
    while (peek() >= '0' && peek() <= '9') {
      // Saturate instead of overflowing; every config value is later clamped to
      // its own range.
      if (value > 1000000000000000000L) {
        overflow = true;
      } else {
        value = value * 10 + (*cur_ - '0');
      }
      cur_++;
    }
    // A fractional or exponent part is accepted and truncated: the config only
    // stores integers, and rejecting "1.0" would be needlessly strict.
    if (peek() == '.') {
      cur_++;
      while (peek() >= '0' && peek() <= '9') cur_++;
    }
    if (peek() == 'e' || peek() == 'E') {
      cur_++;
      if (peek() == '+' || peek() == '-') cur_++;
      while (peek() >= '0' && peek() <= '9') cur_++;
    }
    if (cur_ == start || (negative && cur_ - start == 1)) return fail("expected a number");
    out = overflow ? 100000000L : (negative ? -value : value);
    return true;
  }

  uint16_t clampU16(long value, uint16_t low, uint16_t high) {
    if (value < static_cast<long>(low)) return low;
    if (value > static_cast<long>(high)) return high;
    return static_cast<uint16_t>(value);
  }

  bool skipValue(int depth) {
    if (depth > 6) return fail("value nested too deeply");
    skipWs();
    const char c = peek();
    if (c == '"') {
      char scratch[8];
      return readStringRaw(scratch, sizeof(scratch));
    }
    if (c == '{' || c == '[') {
      const char close = c == '{' ? '}' : ']';
      cur_++;
      skipWs();
      if (peek() == close) {
        cur_++;
        return true;
      }
      while (true) {
        skipWs();
        if (peek() == '"' && c == '{') {
          char scratch[8];
          if (!readStringRaw(scratch, sizeof(scratch))) return false;
          skipWs();
          if (!expect(':')) return false;
        }
        if (!skipValue(depth + 1)) return false;
        skipWs();
        if (peek() == ',') {
          cur_++;
          continue;
        }
        if (peek() == close) {
          cur_++;
          return true;
        }
        return fail("expected ',' or a closing bracket");
      }
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      long scratch = 0;
      return readNumber(scratch);
    }
    if (readLiteral("true") || readLiteral("false") || readLiteral("null")) return true;
    return fail("unrecognized value");
  }

  bool requiredString(const char* key, char* field, size_t capacity) {
    if (peek() != '"') {
      char reason[64];
      std::snprintf(reason, sizeof(reason), "\"%s\" must be a string", key);
      return fail(reason);
    }
    return readString(field, capacity);
  }

  bool requiredNumber(const char* key, long& value) {
    if (peek() != '-' && (peek() < '0' || peek() > '9')) {
      char reason[64];
      std::snprintf(reason, sizeof(reason), "\"%s\" must be a number", key);
      return fail(reason);
    }
    return readNumber(value);
  }

  bool requiredBool(const char* key, bool& value) {
    if (readLiteral("true")) {
      value = true;
      return true;
    }
    if (readLiteral("false")) {
      value = false;
      return true;
    }
    char reason[64];
    std::snprintf(reason, sizeof(reason), "\"%s\" must be true or false", key);
    return fail(reason);
  }

  bool readValueForKey(const char* key, Config& out) {
    long number = 0;
    if (std::strcmp(key, "server") == 0) return requiredString(key, out.server, sizeof(out.server));
    if (std::strcmp(key, "user") == 0) return requiredString(key, out.user, sizeof(out.user));
    if (std::strcmp(key, "apiPassword") == 0) return requiredString(key, out.apiPassword, sizeof(out.apiPassword));
    if (std::strcmp(key, "caCert") == 0) return requiredString(key, out.caCert, sizeof(out.caCert));
    if (std::strcmp(key, "folder") == 0) return requiredString(key, out.folder, sizeof(out.folder));
    if (std::strcmp(key, "feeds") == 0) return requiredString(key, out.feedFilter, sizeof(out.feedFilter));
    if (std::strcmp(key, "categories") == 0)
      return requiredString(key, out.categoryFilter, sizeof(out.categoryFilter));
    if (std::strcmp(key, "articlesPerFeed") == 0) {
      if (!requiredNumber(key, number)) return false;
      out.articlesPerFeed = clampU16(number, kMinArticlesPerFeed, kMaxArticlesPerFeed);
      return true;
    }
    if (std::strcmp(key, "maxFeeds") == 0) {
      if (!requiredNumber(key, number)) return false;
      out.maxFeeds = static_cast<uint8_t>(clampU16(number, kMinMaxFeeds, kMaxMaxFeeds));
      return true;
    }
    if (std::strcmp(key, "syncIntervalMinutes") == 0) {
      if (!requiredNumber(key, number)) return false;
      out.syncIntervalMinutes = clampU16(number, kMinSyncIntervalMinutes, kMaxSyncIntervalMinutes);
      return true;
    }
    if (std::strcmp(key, "favoritesLimit") == 0) {
      if (!requiredNumber(key, number)) return false;
      out.favoritesLimit = clampU16(number, 0, kMaxFavoritesLimit);
      return true;
    }
    if (std::strcmp(key, "keepPerFeed") == 0) {
      if (!requiredNumber(key, number)) return false;
      out.keepPerFeed = clampU16(number, kMinKeepPerFeed, kMaxKeepPerFeed);
      return true;
    }
    if (std::strcmp(key, "maxArticleBytes") == 0) {
      if (!requiredNumber(key, number)) return false;
      if (number < static_cast<long>(kMinArticleBytes))
        out.maxArticleBytes = kMinArticleBytes;
      else if (number > static_cast<long>(kMaxArticleBytes))
        out.maxArticleBytes = kMaxArticleBytes;
      else
        out.maxArticleBytes = static_cast<uint32_t>(number);
      return true;
    }
    if (std::strcmp(key, "verifyTls") == 0) return requiredBool(key, out.verifyTls);
    if (std::strcmp(key, "includeRead") == 0) return requiredBool(key, out.includeRead);
    if (std::strcmp(key, "markReadOnOpen") == 0) return requiredBool(key, out.markReadOnOpen);
    return skipValue(0);
  }
};

}  // namespace

bool parseServerUrl(const char* server, Endpoint& out, char* error, size_t errorSize) {
  out = Endpoint{};
  if (errorSize > 0) error[0] = '\0';
  if (!server || server[0] == '\0') {
    if (errorSize > 0) std::snprintf(error, errorSize, "server URL is empty");
    return false;
  }

  const char* rest = nullptr;
  if (startsWithIgnoreCase(server, "https://")) {
    out.tls = true;
    out.port = 443;
    rest = server + 8;
  } else if (startsWithIgnoreCase(server, "http://")) {
    out.tls = false;
    out.port = 80;
    rest = server + 7;
  } else {
    if (errorSize > 0) std::snprintf(error, errorSize, "server must start with http:// or https://");
    return false;
  }

  // Split authority from path.
  const char* slash = std::strchr(rest, '/');
  size_t authorityLength = slash ? static_cast<size_t>(slash - rest) : std::strlen(rest);
  if (authorityLength == 0) {
    if (errorSize > 0) std::snprintf(error, errorSize, "server has no host");
    return false;
  }
  if (authorityLength > kMaxHost) {
    if (errorSize > 0) std::snprintf(error, errorSize, "server host is too long");
    return false;
  }

  const char* colon = nullptr;
  if (rest[0] == '[') {
    // IPv6 literal: [::1]:8080
    const char* closing = static_cast<const char*>(std::memchr(rest, ']', authorityLength));
    if (!closing) {
      if (errorSize > 0) std::snprintf(error, errorSize, "server IPv6 address is malformed");
      return false;
    }
    const size_t hostLength = static_cast<size_t>(closing - rest - 1);
    copyBounded(out.host, sizeof(out.host), rest + 1, hostLength);
    const char* after = closing + 1;
    if (after < rest + authorityLength && *after == ':') colon = after;
  } else {
    colon = static_cast<const char*>(std::memchr(rest, ':', authorityLength));
    // A second colon means an unbracketed IPv6 literal, which cannot carry a
    // port here.
    if (colon && std::memchr(colon + 1, ':', authorityLength - static_cast<size_t>(colon - rest) - 1)) colon = nullptr;
    const size_t hostLength = colon ? static_cast<size_t>(colon - rest) : authorityLength;
    if (hostLength == 0) {
      if (errorSize > 0) std::snprintf(error, errorSize, "server has no host");
      return false;
    }
    copyBounded(out.host, sizeof(out.host), rest, hostLength);
  }

  if (colon && colon + 1 < rest + authorityLength) {
    long port = 0;
    for (const char* p = colon + 1; p < rest + authorityLength; ++p) {
      if (*p < '0' || *p > '9') {
        if (errorSize > 0) std::snprintf(error, errorSize, "server port is not a number");
        return false;
      }
      port = port * 10 + (*p - '0');
      if (port > 65535) break;
    }
    if (port < 1 || port > 65535) {
      if (errorSize > 0) std::snprintf(error, errorSize, "server port is out of range");
      return false;
    }
    out.port = static_cast<uint16_t>(port);
  }

  char path[128];
  copyBounded(path, sizeof(path), slash ? slash : "", slash ? std::strlen(slash) : 0);
  size_t pathLength = std::strlen(path);
  while (pathLength > 1 && path[pathLength - 1] == '/') path[--pathLength] = '\0';
  if (pathLength >= sizeof(path) - 1) {
    if (errorSize > 0) std::snprintf(error, errorSize, "server path is too long");
    return false;
  }

  if (pathLength == 0) {
    std::snprintf(out.basePath, kBasePathCapacity, "/api/greader.php");
  } else if (endsWithIgnoreCase(path, "/api/greader.php")) {
    copyBounded(out.basePath, kBasePathCapacity, path, pathLength);
  } else {
    const int written = std::snprintf(out.basePath, kBasePathCapacity, "%s/api/greader.php", path);
    if (written <= 0 || static_cast<size_t>(written) >= kBasePathCapacity) {
      if (errorSize > 0) std::snprintf(error, errorSize, "server path is too long");
      return false;
    }
  }
  return true;
}

bool parseConfigJson(const char* text, size_t length, Config& out, char* error, size_t errorSize) {
  out = Config{};
  if (!text) {
    if (errorSize > 0) std::snprintf(error, errorSize, "no config text");
    return false;
  }
  Parser parser(text, length, error, errorSize);
  if (!parser.parseObject(out)) return false;
  normalizeConfig(out);
  return true;
}

void normalizeConfig(Config& config) {
  trimInPlace(config.server);
  trimInPlace(config.user);
  trimInPlace(config.caCert);
  trimInPlace(config.folder);
  trimInPlace(config.feedFilter);
  trimInPlace(config.categoryFilter);

  size_t length = std::strlen(config.server);
  while (length > 0 && config.server[length - 1] == '/') config.server[--length] = '\0';

  // The article folder must be an absolute path, and it must not be the card
  // root: the sync deletes files it no longer wants, and a wrong folder value
  // must never endanger unrelated files.
  if (config.folder[0] != '/') {
    char rebuilt[sizeof(config.folder)];
    std::snprintf(rebuilt, sizeof(rebuilt), "/%s", config.folder[0] ? config.folder : "RSS");
    std::memcpy(config.folder, rebuilt, sizeof(config.folder));
  }
  length = std::strlen(config.folder);
  while (length > 1 && config.folder[length - 1] == '/') config.folder[--length] = '\0';
  if (std::strcmp(config.folder, "/") == 0 || config.folder[0] == '\0')
    std::snprintf(config.folder, sizeof(config.folder), "/RSS");

  if (config.articlesPerFeed < kMinArticlesPerFeed) config.articlesPerFeed = kMinArticlesPerFeed;
  if (config.articlesPerFeed > kMaxArticlesPerFeed) config.articlesPerFeed = kMaxArticlesPerFeed;
  if (config.maxFeeds < kMinMaxFeeds) config.maxFeeds = kMinMaxFeeds;
  if (config.maxFeeds > kMaxMaxFeeds) config.maxFeeds = kMaxMaxFeeds;
  if (config.syncIntervalMinutes < kMinSyncIntervalMinutes) config.syncIntervalMinutes = kMinSyncIntervalMinutes;
  if (config.syncIntervalMinutes > kMaxSyncIntervalMinutes) config.syncIntervalMinutes = kMaxSyncIntervalMinutes;
  if (config.favoritesLimit > kMaxFavoritesLimit) config.favoritesLimit = kMaxFavoritesLimit;
  if (config.keepPerFeed < kMinKeepPerFeed) config.keepPerFeed = kMinKeepPerFeed;
  if (config.keepPerFeed > kMaxKeepPerFeed) config.keepPerFeed = kMaxKeepPerFeed;
  if (config.maxArticleBytes < kMinArticleBytes) config.maxArticleBytes = kMinArticleBytes;
  if (config.maxArticleBytes > kMaxArticleBytes) config.maxArticleBytes = kMaxArticleBytes;
}

bool validateConfig(const Config& config, char* error, size_t errorSize) {
  if (config.server[0] == '\0') {
    if (errorSize > 0) std::snprintf(error, errorSize, "server is not set");
    return false;
  }
  Endpoint endpoint;
  char endpointError[64];
  if (!parseServerUrl(config.server, endpoint, endpointError, sizeof(endpointError))) {
    if (errorSize > 0) std::snprintf(error, errorSize, "%s", endpointError);
    return false;
  }
  if (config.user[0] == '\0') {
    if (errorSize > 0) std::snprintf(error, errorSize, "user is not set");
    return false;
  }
  if (config.apiPassword[0] == '\0') {
    if (errorSize > 0) std::snprintf(error, errorSize, "apiPassword is not set");
    return false;
  }
  if (config.folder[0] != '/') {
    if (errorSize > 0) std::snprintf(error, errorSize, "folder must be an absolute path");
    return false;
  }
  return true;
}

bool syncDue(uint32_t lastSyncEpoch, uint32_t nowEpoch, const Config& config, bool neverSynced) {
  if (neverSynced || lastSyncEpoch == 0) return true;
  // Without a plausible clock (no NTP or RTC) staleness cannot be measured, so
  // an explicit sync request is safer than assuming freshness.
  if (nowEpoch == 0) return true;
  if (nowEpoch < lastSyncEpoch) return true;  // clock moved backwards: resync
  const uint32_t intervalSeconds = static_cast<uint32_t>(config.syncIntervalMinutes) * 60u;
  return nowEpoch - lastSyncEpoch >= intervalSeconds;
}

namespace {

// Case-insensitive comparison of a filter entry against a feed id or title.
bool entryMatches(const char* entry, size_t entryLength, const char* feedId, const char* feedTitle) {
  if (entryLength == 0) return false;
  if (entryLength == 1 && entry[0] == '*') return true;
  const auto matches = [&](const char* candidate) {
    if (!candidate) return false;
    const size_t length = std::strlen(candidate);
    if (length != entryLength) return false;
    for (size_t i = 0; i < entryLength; ++i) {
      if (std::tolower(static_cast<unsigned char>(entry[i])) != std::tolower(static_cast<unsigned char>(candidate[i])))
        return false;
    }
    return true;
  };
  if (matches(feedId) || matches(feedTitle)) return true;
  // "feed/12" is also accepted as just "12".
  if (feedId && std::strncmp(feedId, "feed/", 5) == 0 && matches(feedId + 5)) return true;
  return false;
}

}  // namespace

namespace {

// True when one '|'-separated label matches the filter entry exactly.
bool labelMatches(const char* entry, size_t entryLength, const char* labels) {
  if (!labels) return false;
  const char* cursor = labels;
  while (*cursor) {
    const char* start = cursor;
    while (*cursor && *cursor != '|') ++cursor;
    size_t length = static_cast<size_t>(cursor - start);
    if (length == entryLength) {
      bool same = true;
      for (size_t i = 0; i < length; ++i) {
        if (std::tolower(static_cast<unsigned char>(start[i])) != std::tolower(static_cast<unsigned char>(entry[i]))) {
          same = false;
          break;
        }
      }
      if (same) return true;
    }
    if (*cursor == '|') ++cursor;
  }
  return false;
}

}  // namespace

bool feedAllowed(const Config& config, const char* feedId, const char* feedTitle, const char* feedCategories) {
  if (config.hasFeedFilter()) {
    bool allowed = false;
    const char* cursor = config.feedFilter;
    while (*cursor) {
      while (*cursor == ' ' || *cursor == ',') ++cursor;
      const char* start = cursor;
      while (*cursor && *cursor != ',') ++cursor;
      size_t length = static_cast<size_t>(cursor - start);
      while (length > 0 && (start[length - 1] == ' ' || start[length - 1] == '\t')) --length;
      if (entryMatches(start, length, feedId, feedTitle)) {
        allowed = true;
        break;
      }
      if (*cursor == ',') ++cursor;
    }
    if (!allowed) return false;
  }

  if (!config.hasCategoryFilter()) return true;
  const bool uncategorized = !feedCategories || feedCategories[0] == '\0';
  const char* cursor = config.categoryFilter;
  while (*cursor) {
    while (*cursor == ' ' || *cursor == ',') ++cursor;
    const char* start = cursor;
    while (*cursor && *cursor != ',') ++cursor;
    size_t length = static_cast<size_t>(cursor - start);
    while (length > 0 && (start[length - 1] == ' ' || start[length - 1] == '\t')) --length;
    if (length > 0) {
      if (uncategorized && length == 13 && strncasecmp(start, "Uncategorized", 13) == 0) return true;
      if (labelMatches(start, length, feedCategories)) return true;
    }
    if (*cursor == ',') ++cursor;
  }
  return false;
}

size_t writeExampleConfig(char* out, size_t capacity) {
  if (!out || capacity == 0) return 0;
  const char* text =
      "{\n"
      "  \"_readme\": \"FreshRSS reader config. Find your API address in FreshRSS: Settings > Account > API. Use the "
      "API password, not your login password. Reboot or reopen the app after editing.\",\n"
      "  \"_server\": \"Full API address, e.g. https://freshrss.example.net/api/greader.php\",\n"
      "  \"server\": \"\",\n"
      "  \"user\": \"\",\n"
      "  \"apiPassword\": \"\",\n"
      "  \"verifyTls\": true,\n"
      "  \"caCert\": \"\",\n"
      "  \"folder\": \"/RSS\",\n"
      "  \"articlesPerFeed\": 15,\n"
      "  \"maxFeeds\": 8,\n"
      "  \"syncIntervalMinutes\": 60,\n"
      "  \"keepPerFeed\": 40,\n"
      "  \"favoritesLimit\": 100,\n"
      "  \"maxArticleBytes\": 98304,\n"
      "  \"markReadOnOpen\": true,\n"
      "  \"includeRead\": false,\n"
      "  \"feeds\": \"\",\n"
      "  \"categories\": \"\"\n"
      "}\n";
  const size_t length = std::strlen(text);
  const size_t count = length < capacity - 1 ? length : capacity - 1;
  std::memcpy(out, text, count);
  out[count] = '\0';
  return count;
}

}  // namespace freshrss
