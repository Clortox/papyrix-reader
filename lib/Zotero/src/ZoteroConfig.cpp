#include "ZoteroConfig.h"

#include <cstdio>
#include <cstring>

#include "../../FreshRss/src/FreshRssJson.h"
#include "../../FreshRss/src/FreshRssText.h"

namespace zotero {
namespace {

using freshrss::JsonScanner;
// Text helpers shared with the FreshRSS client.
using freshrss::copyBounded;
using freshrss::trimInPlace;

constexpr size_t kMaxKeyLength = 31;
constexpr size_t kMaxValueLength = 255;

// The parser walks the token stream one byte at a time: the config is a single
// flat object, but nested values are skipped so a richer file still loads.
class Parser {
 public:
  Parser(Config& out, char* error, size_t errorSize) : out_(out), errorOut_(error), errorCap_(errorSize) {
    if (errorCap_ > 0) errorOut_[0] = '\0';
  }

  bool feed(char c) {
    const JsonScanner::Token token = scanner_.feed(c);
    if (token == JsonScanner::Token::None) return true;
    return handle(token);
  }

  bool finish() {
    const JsonScanner::Token token = scanner_.finish();
    if (token != JsonScanner::Token::None && !handle(token)) return false;
    if (scanner_.failed()) return fail(scanner_.text());
    if (!sawObject_) return fail("expected a JSON object");
    if (skipDepth_ != 0) return fail("config ended inside a container");
    if (state_ != State::ExpectKey && state_ != State::Done) return fail("config ended mid-value");
    return true;
  }

 private:
  enum class State : uint8_t { Start, ExpectKey, ExpectValue, InValue, Done };

  bool fail(const char* reason) {
    if (errorCap_ > 0) std::snprintf(errorOut_, errorCap_, "%s", reason);
    failed_ = true;
    return false;
  }

  bool handle(const JsonScanner::Token token) {
    using Token = JsonScanner::Token;
    switch (token) {
      case Token::ObjectBegin:
        if (state_ == State::Start) {
          sawObject_ = true;
          state_ = State::ExpectKey;
        } else {
          ++skipDepth_;  // a nested object is not part of this flat config
        }
        break;

      case Token::ArrayBegin:
        ++skipDepth_;
        break;

      case Token::ArrayEnd:
        if (skipDepth_ > 0) --skipDepth_;
        break;

      case Token::ObjectEnd:
        if (skipDepth_ > 0) {
          --skipDepth_;
        } else {
          state_ = State::Done;
        }
        break;

      case Token::Key:
        if (skipDepth_ > 0) break;
        copyBounded(key_, sizeof(key_), scanner_.text(), scanner_.length());
        valueLength_ = 0;
        value_[0] = '\0';
        state_ = State::ExpectValue;
        break;

      case Token::StringChunk:
        if (skipDepth_ > 0) break;
        appendChunk();
        break;

      case Token::StringEnd:
        if (skipDepth_ > 0) break;
        appendChunk();
        if (state_ == State::ExpectValue || state_ == State::InValue) {
          if (!assignString()) return false;
        }
        state_ = State::ExpectKey;
        break;

      case Token::Number:
        if (skipDepth_ > 0) break;
        if (state_ != State::ExpectValue && state_ != State::InValue) break;
        if (!assignNumber(scanner_.numberValue())) return false;
        state_ = State::ExpectKey;
        break;

      case Token::Bool:
        if (skipDepth_ > 0) break;
        if (state_ != State::ExpectValue && state_ != State::InValue) break;
        if (!assignBool(scanner_.boolValue())) return false;
        state_ = State::ExpectKey;
        break;

      case Token::Null:
        state_ = State::ExpectKey;
        break;

      case Token::Error:
        return fail(scanner_.text());

      case Token::None:
        break;
    }
    return !failed_;
  }

  void appendChunk() {
    const size_t length = scanner_.length();
    const size_t room = sizeof(value_) - 1 - valueLength_;
    const size_t count = length < room ? length : room;
    std::memcpy(value_ + valueLength_, scanner_.text(), count);
    valueLength_ += count;
    value_[valueLength_] = '\0';
  }

  bool assignString() {
    trimInPlace(value_);
    if (std::strcmp(key_, "apiKey") == 0) return copyField("apiKey", out_.apiKey, sizeof(out_.apiKey));
    if (std::strcmp(key_, "library") == 0) return copyField("library", out_.library, sizeof(out_.library));
    if (std::strcmp(key_, "folder") == 0) return copyField("folder", out_.folder, sizeof(out_.folder));
    if (std::strcmp(key_, "collection") == 0) return copyField("collection", out_.collection, sizeof(out_.collection));
    if (std::strcmp(key_, "tag") == 0) return copyField("tag", out_.tag, sizeof(out_.tag));
    if (std::strcmp(key_, "caCert") == 0) return copyField("caCert", out_.caCert, sizeof(out_.caCert));
    if (isNumericKey()) return typeError("a number");
    if (isBooleanKey()) return typeError("true or false");
    return true;  // unknown key: ignored on purpose
  }

  bool assignNumber(long long value) {
    if (std::strcmp(key_, "itemsLimit") == 0) {
      out_.itemsLimit = clampU16(value, kMinItemsLimit, kMaxItemsLimit);
      return true;
    }
    if (std::strcmp(key_, "keepPerCollection") == 0) {
      out_.keepPerCollection = clampU16(value, kMinKeepPerCollection, kMaxKeepPerCollection);
      return true;
    }
    if (std::strcmp(key_, "syncIntervalMinutes") == 0) {
      out_.syncIntervalMinutes = clampU16(value, kMinSyncIntervalMinutes, kMaxSyncIntervalMinutes);
      return true;
    }
    if (std::strcmp(key_, "maxItemBytes") == 0) {
      if (value < static_cast<long long>(kMinItemBytes))
        out_.maxItemBytes = kMinItemBytes;
      else if (value > static_cast<long long>(kMaxItemBytesLimit))
        out_.maxItemBytes = kMaxItemBytesLimit;
      else
        out_.maxItemBytes = static_cast<uint32_t>(value);
      return true;
    }
    if (std::strcmp(key_, "apiKey") == 0 || std::strcmp(key_, "library") == 0 ||
        std::strcmp(key_, "folder") == 0 || std::strcmp(key_, "collection") == 0 || std::strcmp(key_, "tag") == 0 ||
        std::strcmp(key_, "caCert") == 0)
      return typeError("a string");
    if (isBooleanKey()) return typeError("true or false");
    return true;
  }

  bool assignBool(bool value) {
    if (std::strcmp(key_, "includeNotes") == 0) {
      out_.includeNotes = value;
      return true;
    }
    if (std::strcmp(key_, "fetchLinkedPages") == 0) {
      out_.fetchLinkedPages = value;
      return true;
    }
    if (std::strcmp(key_, "downloadPdf") == 0) {
      out_.downloadPdf = value;
      return true;
    }
    if (std::strcmp(key_, "verifyTls") == 0) {
      out_.verifyTls = value;
      return true;
    }
    if (isNumericKey()) return typeError("a number");
    return true;
  }

  bool isNumericKey() const {
    return std::strcmp(key_, "itemsLimit") == 0 || std::strcmp(key_, "keepPerCollection") == 0 ||
           std::strcmp(key_, "syncIntervalMinutes") == 0 || std::strcmp(key_, "maxItemBytes") == 0;
  }

  bool isBooleanKey() const {
    return std::strcmp(key_, "includeNotes") == 0 || std::strcmp(key_, "fetchLinkedPages") == 0 ||
           std::strcmp(key_, "downloadPdf") == 0 || std::strcmp(key_, "verifyTls") == 0;
  }

  bool copyField(const char* name, char* field, size_t capacity) {
    if (std::strlen(value_) >= capacity) {
      if (errorCap_ > 0) std::snprintf(errorOut_, errorCap_, "\"%s\" is too long", name);
      failed_ = true;
      return false;
    }
    std::memcpy(field, value_, std::strlen(value_) + 1);
    return true;
  }

  bool typeError(const char* expected) {
    if (errorCap_ > 0) std::snprintf(errorOut_, errorCap_, "\"%s\" must be %s", key_, expected);
    failed_ = true;
    return false;
  }

  static uint16_t clampU16(long long value, uint16_t low, uint16_t high) {
    if (value < static_cast<long long>(low)) return low;
    if (value > static_cast<long long>(high)) return high;
    return static_cast<uint16_t>(value);
  }

  Config& out_;
  char* errorOut_;
  size_t errorCap_;
  JsonScanner scanner_;
  State state_ = State::Start;
  bool sawObject_ = false;
  bool failed_ = false;
  char key_[kMaxKeyLength + 1] = {};
  char value_[kMaxValueLength + 1] = {};
  size_t valueLength_ = 0;
  int skipDepth_ = 0;
};

}  // namespace

bool parseConfig(const char* text, size_t length, Config& out, char* error, size_t errorSize) {
  out = Config{};
  if (!text) {
    if (errorSize > 0) std::snprintf(error, errorSize, "no config text");
    return false;
  }
  Parser parser(out, error, errorSize);
  for (size_t i = 0; i < length; ++i) {
    if (!parser.feed(text[i])) return false;
  }
  if (!parser.finish()) return false;
  normalizeConfig(out);
  return true;
}

void normalizeLibrary(const char* value, char* out, size_t capacity) {
  if (!out || capacity == 0) return;
  out[0] = '\0';
  if (!value || value[0] == '\0') return;
  if (freshrss::startsWithIgnoreCase(value, "users/") || freshrss::startsWithIgnoreCase(value, "groups/")) {
    copyBounded(out, capacity, value);
    return;
  }
  // A bare number is a user id; a name is assumed to be one too and left for the
  // server to reject with a clear status.
  std::snprintf(out, capacity, "users/%s", value);
}

void normalizeConfig(Config& config) {
  freshrss::trimInPlace(config.apiKey);
  freshrss::trimInPlace(config.library);
  freshrss::trimInPlace(config.collection);
  freshrss::trimInPlace(config.tag);
  freshrss::trimInPlace(config.caCert);
  freshrss::trimInPlace(config.folder);

  char library[sizeof(config.library)];
  normalizeLibrary(config.library, library, sizeof(library));
  std::memcpy(config.library, library, sizeof(config.library));

  if (config.folder[0] != '/') {
    char rebuilt[sizeof(config.folder)];
    std::snprintf(rebuilt, sizeof(rebuilt), "/%s", config.folder[0] ? config.folder : "Zotero");
    std::memcpy(config.folder, rebuilt, sizeof(config.folder));
  }
  size_t length = std::strlen(config.folder);
  while (length > 1 && config.folder[length - 1] == '/') config.folder[--length] = '\0';
  if (std::strcmp(config.folder, "/") == 0 || config.folder[0] == '\0')
    std::snprintf(config.folder, sizeof(config.folder), "/Zotero");

  if (config.itemsLimit < kMinItemsLimit) config.itemsLimit = kMinItemsLimit;
  if (config.itemsLimit > kMaxItemsLimit) config.itemsLimit = kMaxItemsLimit;
  if (config.keepPerCollection < kMinKeepPerCollection) config.keepPerCollection = kMinKeepPerCollection;
  if (config.keepPerCollection > kMaxKeepPerCollection) config.keepPerCollection = kMaxKeepPerCollection;
  if (config.syncIntervalMinutes < kMinSyncIntervalMinutes)
    config.syncIntervalMinutes = kMinSyncIntervalMinutes;
  if (config.syncIntervalMinutes > kMaxSyncIntervalMinutes)
    config.syncIntervalMinutes = kMaxSyncIntervalMinutes;
  if (config.maxItemBytes < kMinItemBytes) config.maxItemBytes = kMinItemBytes;
  if (config.maxItemBytes > kMaxItemBytesLimit) config.maxItemBytes = kMaxItemBytesLimit;
}

bool validateConfig(const Config& config, char* error, size_t errorSize) {
  if (config.apiKey[0] == '\0') {
    if (errorSize > 0) std::snprintf(error, errorSize, "apiKey is not set");
    return false;
  }
  if (config.library[0] == '\0') {
    if (errorSize > 0) std::snprintf(error, errorSize, "library is not set");
    return false;
  }
  const bool userLibrary = freshrss::startsWithIgnoreCase(config.library, "users/");
  const bool groupLibrary = freshrss::startsWithIgnoreCase(config.library, "groups/");
  if (!userLibrary && !groupLibrary) {
    if (errorSize > 0) std::snprintf(error, errorSize, "library must be users/<id> or groups/<id>");
    return false;
  }
  const char* id = config.library + (userLibrary ? 6 : 7);
  if (id[0] == '\0') {
    if (errorSize > 0) std::snprintf(error, errorSize, "library id is missing");
    return false;
  }
  for (const char* p = id; *p; ++p) {
    if (*p < '0' || *p > '9') {
      if (errorSize > 0) std::snprintf(error, errorSize, "library id must be numeric");
      return false;
    }
  }
  if (config.folder[0] != '/') {
    if (errorSize > 0) std::snprintf(error, errorSize, "folder must be an absolute path");
    return false;
  }
  return true;
}

bool syncDue(uint32_t lastSyncEpoch, uint32_t nowEpoch, const Config& config, bool neverSynced) {
  if (neverSynced || lastSyncEpoch == 0) return true;
  if (nowEpoch == 0) return true;
  if (nowEpoch < lastSyncEpoch) return true;
  const uint32_t intervalSeconds = static_cast<uint32_t>(config.syncIntervalMinutes) * 60u;
  return nowEpoch - lastSyncEpoch >= intervalSeconds;
}

size_t writeExampleConfig(char* out, size_t capacity) {
  if (!out || capacity == 0) return 0;
  const char* text =
      "{\n"
      "  \"_readme\": \"Zotero reader config. Create an API key at zotero.org/settings/keys (read access is "
      "enough), then set library to users/<your numeric user id>, or groups/<group id> for a group library. The user "
      "id is shown on the same page.\",\n"
      "  \"apiKey\": \"\",\n"
      "  \"library\": \"\",\n"
      "  \"folder\": \"/Zotero\",\n"
      "  \"collection\": \"\",\n"
      "  \"tag\": \"\",\n"
      "  \"itemsLimit\": 50,\n"
      "  \"keepPerCollection\": 40,\n"
      "  \"syncIntervalMinutes\": 60,\n"
      "  \"maxItemBytes\": 262144,\n"
      "  \"includeNotes\": true,\n"
      "  \"fetchLinkedPages\": true,\n"
      "  \"downloadPdf\": false,\n"
      "  \"verifyTls\": true,\n"
      "  \"caCert\": \"\"\n"
      "}\n";
  const size_t length = std::strlen(text);
  const size_t count = length < capacity - 1 ? length : capacity - 1;
  std::memcpy(out, text, count);
  out[count] = '\0';
  return count;
}

}  // namespace zotero
