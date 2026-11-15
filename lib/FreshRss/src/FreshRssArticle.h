#pragma once

#include <cstddef>
#include <cstdint>

// Offline article files for the FreshRSS mini-app.
//
// Articles are written as ordinary .html files inside a visible library folder,
// so the built-in reader opens them with pagination, fonts, progress and
// bookmarks. File names are deterministic (date + title slug + item id hash):
// re-downloading an item overwrites its own file and no separate index is
// needed. The feed's HTML body streams through a bounded sanitizer, so a large
// article never has to fit in RAM.

namespace freshrss {

// Longest accepted HTML entity, including its '&' and ';'.
constexpr size_t kMaxEntity = 12;

class ByteSink {
 public:
  virtual ~ByteSink() = default;
  virtual bool write(const char* data, size_t length) = 0;
};

struct CivilDate {
  int year = 1970;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
};

// Converts a Unix timestamp to UTC plus a local offset in minutes.
void epochToCivil(uint32_t epoch, int tzOffsetMinutes, CivilDate& out);

// "2026-11-15"
size_t formatIsoDate(const CivilDate& date, char* out, size_t capacity);
// "15 November 2026, 14:05"
size_t formatLongDate(const CivilDate& date, char* out, size_t capacity);

// FNV-1a over the item id. The hash names the file and is stable across
// reboots, so a repeated download lands on the same path.
uint64_t hashItemId(const char* id);

// Makes one path component safe for FAT and for the library browser: control
// characters, separators and characters Windows rejects are replaced, and the
// result is trimmed and length-capped. `fallback` is used when nothing is left.
size_t sanitizeFileComponent(char* out, size_t capacity, const char* text, const char* fallback);

// "<folder>/<feed>" (no trailing slash).
size_t buildFeedDirectory(char* out, size_t capacity, const char* folder, const char* feedTitle);

// "<yyyy-mm-dd>-<slug>-<hash8>.html". `publishedEpoch` 0 falls back to
// `fallbackEpoch` (the fetch time) so files still sort chronologically.
size_t buildArticleFileName(char* out, size_t capacity, const char* title, uint32_t publishedEpoch,
                            uint32_t fallbackEpoch, uint64_t itemHash);

// Extracts the item id that ArticleWriter records in the document head. Used to
// queue an upstream mark-as-read when the user opens a stored article.
bool extractItemId(const char* documentHead, size_t length, char* out, size_t capacity);

// Streams one article document: a small header, the sanitized feed HTML, and a
// footer. `maxBytes` bounds the file so one huge post cannot fill the card.
class ArticleWriter {
 public:
  bool begin(ByteSink& sink, size_t maxBytes, const char* title, const char* feedTitle, const char* author,
             const char* sourceUrl, uint32_t publishedEpoch, const char* itemId);
  // Feed HTML content; tags may be split across calls.
  bool writeContent(const char* data, size_t length);
  // Late byline for feeds that put the author after the body.
  bool writeAuthorLine(const char* author);
  bool end();

  size_t bytesWritten() const { return written_; }
  bool overflowed() const { return overflowed_; }
  bool active() const { return active_; }

 private:
  enum class State : uint8_t { Idle, Text, Tag, Comment, Suppressed, SuppressedTag };

  bool emit(const char* data, size_t length);
  bool emitLiteral(const char* text);
  void resetTagBuffer();
  void handleTag();
  void handleTextChar(char c);

  ByteSink* sink_ = nullptr;
  size_t maxBytes_ = 0;
  size_t written_ = 0;
  bool overflowed_ = false;
  bool active_ = false;
  // Set while the closing markup is written: it is never what overflows.
  bool writingFooter_ = false;

  State state_ = State::Idle;
  char tag_[200] = {};
  size_t tagLength_ = 0;
  bool tagTruncated_ = false;
  bool inQuote_ = false;
  char quoteChar_ = 0;

  // Entity buffering: '&' opens a candidate that is passed through only when it
  // turns out to be well formed.
  bool entityActive_ = false;
  char entity_[kMaxEntity] = {};
  size_t entityLength_ = 0;

  // Comment shedding: comments may contain '>' so they are scanned for "-->".
  int commentDashes_ = 0;

  // Suppression nesting for elements whose content must never reach the reader.
  char suppressedTag_[16] = {};
  int suppressDepth_ = 0;
};

}  // namespace freshrss
