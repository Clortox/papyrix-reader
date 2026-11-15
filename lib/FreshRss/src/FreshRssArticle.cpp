#include "FreshRssArticle.h"

#include <cstdio>
#include <cstring>

#include "FreshRssText.h"

namespace freshrss {
namespace {

constexpr size_t kMaxSlug = 40;

const char* kMonthNames[12] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

// Elements whose text content must never reach the reader.
bool dropWithContent(const char* name) {
  static const char* kDropped[] = {"script", "style",  "iframe",   "object", "embed", "noscript", "form",
                                   "button", "select", "textarea", "video",  "audio", "canvas",   "svg",
                                   "math",   "head",   "template", "map",    "applet"};
  for (const char* candidate : kDropped) {
    if (equalsIgnoreCase(name, candidate)) return true;
  }
  return false;
}

// Elements that are passed through as-is. Tags outside this list are unwrapped
// (dropped) while their text is kept, which keeps the reader's XML pipeline away
// from tags it cannot style.
bool keepTag(const char* name) {
  static const char* kKept[] = {
      "p",      "div",        "br",    "span", "h1",    "h2",    "h3",      "h4", "h5",  "h6", "ul",  "ol",
      "li",     "blockquote", "pre",   "code", "em",    "i",     "strong",  "b",  "u",   "s",  "sub", "sup",
      "table",  "tr",         "td",    "th",   "thead", "tbody", "caption", "dl", "dt",  "dd", "a",   "hr",
      "figure", "figcaption", "small", "mark", "cite",  "time",  "abbr",    "q",  "ins", "del"};
  for (const char* candidate : kKept) {
    if (equalsIgnoreCase(name, candidate)) return true;
  }
  return false;
}

// Attributes worth keeping: a link needs its target. Anything else (event
// handlers, inline styles, tracking pixels) is dropped with its value.
bool keepAttribute(const char* tagName, const char* attributeName) {
  if (!equalsIgnoreCase(tagName, "a")) return false;
  return equalsIgnoreCase(attributeName, "href");
}

size_t appendEscaped(char* out, size_t capacity, size_t written, const char* text, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    const char* replacement = nullptr;
    switch (c) {
      case '&':
        replacement = "&amp;";
        break;
      case '<':
        replacement = "&lt;";
        break;
      case '>':
        replacement = "&gt;";
        break;
      case '"':
        replacement = "&quot;";
        break;
      default:
        break;
    }
    if (replacement) {
      const size_t replacementLength = std::strlen(replacement);
      if (written + replacementLength + 1 > capacity) return 0;
      std::memcpy(out + written, replacement, replacementLength);
      written += replacementLength;
    } else {
      if (written + 2 > capacity) return 0;
      out[written++] = c;
    }
  }
  if (written + 1 > capacity) return 0;
  out[written] = '\0';
  return written;
}

bool knownEntity(const char* entity, size_t length) {
  static const char* kKnown[] = {"amp", "lt", "gt", "quot", "apos", "nbsp"};
  for (const char* candidate : kKnown) {
    const size_t candidateLength = std::strlen(candidate);
    if (candidateLength != length) continue;
    bool match = true;
    for (size_t i = 0; i < length; ++i) {
      if (entity[i] != candidate[i]) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

bool validEntity(const char* entity, size_t length) {
  if (length < 2 || length > kMaxEntity - 1) return false;
  size_t index = 0;
  if (entity[0] == '#') {
    ++index;
    if (index < length && (entity[index] == 'x' || entity[index] == 'X')) ++index;
    if (index >= length) return false;
    for (; index < length; ++index) {
      const char c = entity[index];
      const bool digit =
          entity[index - 1] == 'x' || entity[index - 1] == 'X' ? hexValue(c) >= 0 : (c >= '0' && c <= '9');
      if (!digit) return false;
    }
    return true;
  }
  return knownEntity(entity, length);
}

}  // namespace

void epochToCivil(uint32_t epoch, int tzOffsetMinutes, CivilDate& out) {
  int64_t seconds = static_cast<int64_t>(epoch) + static_cast<int64_t>(tzOffsetMinutes) * 60;
  int64_t days = seconds / 86400;
  int64_t remainder = seconds % 86400;
  if (remainder < 0) {
    remainder += 86400;
    --days;
  }
  out.hour = static_cast<int>(remainder / 3600);
  out.minute = static_cast<int>((remainder % 3600) / 60);

  // Howard Hinnant's civil_from_days: shift the epoch to 0000-03-01 so leap days
  // land at the end of the 400-year cycle.
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t year = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned dayOfMonth = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = mp < 10 ? mp + 3 : mp - 9;
  out.year = static_cast<int>(year + (month <= 2 ? 1 : 0));
  out.month = static_cast<int>(month);
  out.day = static_cast<int>(dayOfMonth);
}

size_t formatIsoDate(const CivilDate& date, char* out, size_t capacity) {
  const int written = std::snprintf(out, capacity, "%04d-%02d-%02d", date.year, date.month, date.day);
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    if (capacity > 0) out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(written);
}

size_t formatLongDate(const CivilDate& date, char* out, size_t capacity) {
  const char* month = (date.month >= 1 && date.month <= 12) ? kMonthNames[date.month - 1] : "";
  const int written =
      std::snprintf(out, capacity, "%d %s %d, %02d:%02d", date.day, month, date.year, date.hour, date.minute);
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    if (capacity > 0) out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(written);
}

uint64_t hashItemId(const char* id) {
  uint64_t hash = 1469598103934665603ull;  // FNV-1a 64-bit offset basis
  if (id) {
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(id); *p; ++p) {
      hash ^= static_cast<uint64_t>(*p);
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

namespace {

// Length of the UTF-8 sequence starting at `c`, or 0 when it is not valid.
size_t utf8SequenceLength(const unsigned char* p, const unsigned char* end) {
  const unsigned char first = *p;
  size_t length = 0;
  uint32_t value = 0;
  if (first < 0x80) return 1;
  if (first >= 0xC2 && first <= 0xDF) {
    length = 2;
    value = first & 0x1F;
  } else if (first >= 0xE0 && first <= 0xEF) {
    length = 3;
    value = first & 0x0F;
  } else if (first >= 0xF0 && first <= 0xF4) {
    length = 4;
    value = first & 0x07;
  } else {
    return 0;
  }
  if (p + length > end) return 0;
  for (size_t i = 1; i < length; ++i) {
    if ((p[i] & 0xC0) != 0x80) return 0;
    value = (value << 6) | (p[i] & 0x3F);
  }
  // Reject overlongs, surrogates and out-of-range code points.
  if (length == 3 && ((first == 0xE0 && p[1] < 0xA0) || (first == 0xED && p[1] >= 0xA0))) return 0;
  if (length == 4 && ((first == 0xF0 && p[1] < 0x90) || (first == 0xF4 && p[1] >= 0x90))) return 0;
  (void)value;
  return length;
}

}  // namespace

size_t sanitizeFileComponent(char* out, size_t capacity, const char* text, const char* fallback) {
  if (!out || capacity < 2) return 0;
  size_t written = 0;
  // A leading separator is skipped, which also keeps a name from starting with
  // a dot (hidden) or a dash.
  bool lastWasSeparator = true;
  if (text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text);
    const auto* end = bytes + std::strlen(text);
    for (const unsigned char* p = bytes; p < end && written + 1 < capacity;) {
      const unsigned char c = *p;
      if (c >= 0x80) {
        // Multi-byte sequences are kept only when they are valid UTF-8: a card
        // mounted with a utf8 charset refuses to create any other name.
        const size_t length = utf8SequenceLength(p, end);
        if (length == 0 || written + length >= capacity) {
          if (!lastWasSeparator) {
            out[written++] = '-';
            lastWasSeparator = true;
          }
          ++p;
          continue;
        }
        for (size_t i = 0; i < length; ++i) out[written++] = static_cast<char>(p[i]);
        p += length;
        lastWasSeparator = false;
        continue;
      }
      ++p;
      const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        (c == '.' && !lastWasSeparator);
      if (safe) {
        out[written++] = static_cast<char>(c);
        lastWasSeparator = false;
      } else if (!lastWasSeparator) {
        out[written++] = '-';
        lastWasSeparator = true;
      }
    }
  }
  while (written > 0 && (out[written - 1] == '-' || out[written - 1] == '.')) --written;
  if (written == 0) {
    copyBounded(out, capacity, fallback && fallback[0] ? fallback : "item");
    return std::strlen(out);
  }
  out[written] = '\0';
  return written;
}

size_t buildFeedDirectory(char* out, size_t capacity, const char* folder, const char* feedTitle) {
  char feed[64];
  sanitizeFileComponent(feed, sizeof(feed), feedTitle, "feed");
  const char* base = (folder && folder[0]) ? folder : "/RSS";
  const int written = std::snprintf(out, capacity, "%s/%s", base, feed);
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    if (capacity > 0) out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(written);
}

size_t buildArticleFileName(char* out, size_t capacity, const char* title, uint32_t publishedEpoch,
                            uint32_t fallbackEpoch, uint64_t itemHash) {
  const uint32_t epoch = publishedEpoch != 0 ? publishedEpoch : fallbackEpoch;
  CivilDate date{};
  epochToCivil(epoch, 0, date);
  char iso[16];
  if (formatIsoDate(date, iso, sizeof(iso)) == 0) copyBounded(iso, sizeof(iso), "0000-00-00");

  char slug[kMaxSlug + 1];
  sanitizeFileComponent(slug, sizeof(slug), title, "article");
  if (std::strlen(slug) > kMaxSlug) slug[kMaxSlug] = '\0';

  const int written =
      std::snprintf(out, capacity, "%s-%s-%08x.html", iso, slug, static_cast<unsigned>(itemHash & 0xFFFFFFFFu));
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    if (capacity > 0) out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(written);
}

bool extractItemId(const char* documentHead, size_t length, char* out, size_t capacity) {
  if (!documentHead || !out || capacity == 0) return false;
  out[0] = '\0';
  static const char* kMarker = "name=\"rss-item\" content=\"";
  const size_t markerLength = std::strlen(kMarker);
  if (length < markerLength) return false;
  for (size_t i = 0; i + markerLength < length; ++i) {
    if (std::memcmp(documentHead + i, kMarker, markerLength) != 0) continue;
    size_t written = 0;
    size_t cursor = i + markerLength;
    while (cursor < length && documentHead[cursor] != '"' && written + 1 < capacity) {
      char c = documentHead[cursor++];
      // Undo the attribute escaping applied when the file was written.
      if (c == '&') {
        if (cursor + 4 < length && std::memcmp(documentHead + cursor, "amp;", 4) == 0) {
          cursor += 4;
          c = '&';
        } else if (cursor + 3 < length && std::memcmp(documentHead + cursor, "lt;", 3) == 0) {
          cursor += 3;
          c = '<';
        } else if (cursor + 3 < length && std::memcmp(documentHead + cursor, "gt;", 3) == 0) {
          cursor += 3;
          c = '>';
        }
      }
      out[written++] = c;
    }
    out[written] = '\0';
    return written > 0;
  }
  return false;
}

bool ArticleWriter::emit(const char* data, size_t length) {
  if (!active_ || length == 0) return true;
  if (maxBytes_ > 0 && written_ + length > maxBytes_) {
    overflowed_ = true;
    return false;
  }
  if (!sink_ || !sink_->write(data, length)) {
    overflowed_ = true;
    return false;
  }
  written_ += length;
  return true;
}

bool ArticleWriter::emitLiteral(const char* text) { return emit(text, std::strlen(text)); }

bool ArticleWriter::begin(ByteSink& sink, size_t maxBytes, const char* title, const char* feedTitle, const char* author,
                          const char* sourceUrl, uint32_t publishedEpoch, const char* itemId) {
  *this = ArticleWriter{};
  sink_ = &sink;
  maxBytes_ = maxBytes;
  active_ = true;
  state_ = State::Text;

  const char* safeTitle = (title && title[0]) ? title : "Untitled";

  char escaped[512];
  if (appendEscaped(escaped, sizeof(escaped), 0, safeTitle, std::strlen(safeTitle)) == 0)
    copyBounded(escaped, sizeof(escaped), "Untitled");

  char escapedItem[192];
  escapedItem[0] = '\0';
  if (itemId && itemId[0]) appendEscaped(escapedItem, sizeof(escapedItem), 0, itemId, std::strlen(itemId));

  char escapedFeed[128];
  escapedFeed[0] = '\0';
  if (feedTitle && feedTitle[0]) appendEscaped(escapedFeed, sizeof(escapedFeed), 0, feedTitle, std::strlen(feedTitle));

  char head[768];
  const int headWritten = std::snprintf(head, sizeof(head),
                                        "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"/>"
                                        "<meta name=\"rss-item\" content=\"%s\"/>"
                                        "<title>%s%s%s</title></head><body>\n<h1>%s</h1>\n",
                                        escapedItem, escaped, escapedFeed[0] ? " - " : "", escapedFeed, escaped);
  if (headWritten <= 0 || static_cast<size_t>(headWritten) >= sizeof(head)) return false;
  if (!emit(head, static_cast<size_t>(headWritten))) return false;

  // The byline is always present, so the reader always has a date line even for
  // feeds that omit or mis-format their timestamps.
  CivilDate date{};
  epochToCivil(publishedEpoch, 0, date);
  char dateText[48];
  if (formatLongDate(date, dateText, sizeof(dateText)) == 0) copyBounded(dateText, sizeof(dateText), "date unknown");
  const int bylineWritten = std::snprintf(head, sizeof(head), "<p><em>%s%s%s</em></p>\n",
                                          escapedFeed[0] ? escapedFeed : "", escapedFeed[0] ? " - " : "", dateText);
  if (bylineWritten <= 0 || static_cast<size_t>(bylineWritten) >= sizeof(head)) return false;
  if (!emit(head, static_cast<size_t>(bylineWritten))) return false;

  if (author && author[0]) {
    char escapedAuthor[160];
    if (appendEscaped(escapedAuthor, sizeof(escapedAuthor), 0, author, std::strlen(author)) > 0) {
      if (!emitLiteral("<p><em>")) return false;
      if (!emitLiteral(escapedAuthor)) return false;
      if (!emitLiteral("</em></p>\n")) return false;
    }
  }
  if (sourceUrl && sourceUrl[0]) {
    char escapedUrl[320];
    if (appendEscaped(escapedUrl, sizeof(escapedUrl), 0, sourceUrl, std::strlen(sourceUrl)) > 0) {
      if (!emitLiteral("<p><em>Source: ")) return false;
      if (!emitLiteral(escapedUrl)) return false;
      if (!emitLiteral("</em></p>\n")) return false;
    }
  }
  if (!emitLiteral("<hr/>\n")) return false;
  return true;
}

void ArticleWriter::resetTagBuffer() {
  tagLength_ = 0;
  tag_[0] = '\0';
  tagTruncated_ = false;
  inQuote_ = false;
  quoteChar_ = 0;
}

void ArticleWriter::handleTextChar(char c) {
  if (state_ != State::Text) return;
  if (c == '<') {
    state_ = State::Tag;
    resetTagBuffer();
    return;
  }
  if (c == '&') {
    entityActive_ = true;
    entityLength_ = 0;
    return;
  }
  if (entityActive_) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#';
    if (c == ';') {
      // A terminated entity is passed through only when it is well formed;
      // otherwise it would break the reader's XML parser.
      if (validEntity(entity_, entityLength_)) {
        char out[kMaxEntity + 2];
        out[0] = '&';
        std::memcpy(out + 1, entity_, entityLength_);
        out[entityLength_ + 1] = ';';
        out[entityLength_ + 2] = '\0';
        emit(out, entityLength_ + 2);
      } else {
        // Keep the literal text visible: escape the '&' and the ';' with it.
        emitLiteral("&amp;");
        emit(entity_, entityLength_);
        emitLiteral(";");
      }
      entityActive_ = false;
      entityLength_ = 0;
      return;
    }
    if (allowed && entityLength_ + 1 < kMaxEntity) {
      entity_[entityLength_++] = c;
      entity_[entityLength_] = '\0';
      return;
    }
    emitLiteral("&amp;");
    emit(entity_, entityLength_);
    entityActive_ = false;
    entityLength_ = 0;
    // The character that broke the candidate still belongs to the text; it may
    // even open a tag or another entity.
    handleTextChar(c);
    return;
  }
  if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t' && c != '\r') return;  // not valid XML
  emit(&c, 1);
}

void ArticleWriter::handleTag() {
  // The buffer holds everything after '<', including the closing '>'.
  const char* buffer = tag_;
  size_t length = tagLength_;
  const bool closing = length > 0 && buffer[0] == '/';
  size_t index = closing ? 1 : 0;
  const size_t nameStart = index;
  while (index < length) {
    const char c = buffer[index];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')
      ++index;
    else
      break;
  }
  char name[32];
  const size_t nameLength = index - nameStart;
  copyBounded(name, sizeof(name), buffer + nameStart, nameLength);

  // A '<' that does not start a plausible tag is text: keep it as such instead
  // of silently deleting an article's punctuation.
  if (tagTruncated_ || nameLength == 0) {
    emitLiteral("&lt;");
    char escaped[256];
    if (appendEscaped(escaped, sizeof(escaped), 0, buffer, length) > 0) emitLiteral(escaped);
    return;
  }

  if (closing) {
    if (keepTag(name)) {
      char out[40];
      const int written = std::snprintf(out, sizeof(out), "</%s>", name);
      if (written > 0) emit(out, static_cast<size_t>(written));
    }
    return;
  }

  const bool selfClosing = length >= 2 && buffer[length - 2] == '/';
  if (dropWithContent(name)) {
    if (!selfClosing) {
      state_ = State::Suppressed;
      copyBounded(suppressedTag_, sizeof(suppressedTag_), name);
      suppressDepth_ = 1;
    }
    return;
  }
  if (!keepTag(name)) return;

  char out[256];
  size_t written = 0;
  const int header = std::snprintf(out, sizeof(out), "<%s", name);
  if (header <= 0 || static_cast<size_t>(header) >= sizeof(out)) return;
  written = static_cast<size_t>(header);

  // Only a small whitelist of attributes survives, so an attribute value can
  // never smuggle markup into the reader.
  size_t cursor = index;
  while (cursor < length) {
    while (cursor < length && (buffer[cursor] == ' ' || buffer[cursor] == '\t' || buffer[cursor] == '\n' ||
                               buffer[cursor] == '\r' || buffer[cursor] == '/'))
      ++cursor;
    if (cursor >= length || buffer[cursor] == '>') break;
    const size_t attributeStart = cursor;
    while (cursor < length && buffer[cursor] != '=' && buffer[cursor] != ' ' && buffer[cursor] != '\t' &&
           buffer[cursor] != '>' && buffer[cursor] != '/')
      ++cursor;
    char attribute[40];
    copyBounded(attribute, sizeof(attribute), buffer + attributeStart, cursor - attributeStart);
    const size_t valueStart = cursor;
    // Skip to the end of the value so the next loop iteration starts after it.
    while (cursor < length && buffer[cursor] != '=' && buffer[cursor] != ' ' && buffer[cursor] != '\t' &&
           buffer[cursor] != '>')
      ++cursor;
    if (cursor < length && buffer[cursor] == '=') {
      ++cursor;
      while (cursor < length && (buffer[cursor] == ' ' || buffer[cursor] == '\t')) ++cursor;
      const char quote = cursor < length ? buffer[cursor] : '\0';
      if (quote == '"' || quote == '\'') {
        ++cursor;
        while (cursor < length && buffer[cursor] != quote) ++cursor;
        if (cursor < length) ++cursor;
      } else {
        while (cursor < length && buffer[cursor] != ' ' && buffer[cursor] != '\t' && buffer[cursor] != '>') ++cursor;
      }
    }
    if (attribute[0] == '\0' || !keepAttribute(name, attribute)) continue;
    // Re-emit the attribute with its raw source text so quoting is preserved.
    const size_t attributeLength = std::strlen(attribute);
    const size_t valueLength = cursor > valueStart ? cursor - valueStart : 0;
    if (written + attributeLength + valueLength + 3 >= sizeof(out)) continue;
    out[written++] = ' ';
    std::memcpy(out + written, attribute, attributeLength);
    written += attributeLength;
    if (valueLength > 0) {
      std::memcpy(out + written, buffer + valueStart, valueLength);
      written += valueLength;
    }
  }
  if (selfClosing) {
    if (written + 3 >= sizeof(out)) return;
    out[written++] = '/';
    out[written++] = '>';
  } else {
    if (written + 2 >= sizeof(out)) return;
    out[written++] = '>';
  }
  out[written] = '\0';
  emit(out, written);
}

bool ArticleWriter::writeContent(const char* data, size_t length) {
  if (!active_) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = data[i];
    switch (state_) {
      case State::Idle:
        return false;
      case State::Text:
        handleTextChar(c);
        break;
      case State::Tag:
        if (tagLength_ == 2 && tag_[0] == '!' && tag_[1] == '-') {
          // "<!--": the rest is a comment and must be dropped whole.
          state_ = State::Comment;
          commentDashes_ = 1;
          resetTagBuffer();
          break;
        }
        if (c == '>' && !inQuote_) {
          if (tagLength_ + 1 < sizeof(tag_)) {
            tag_[tagLength_++] = c;
            tag_[tagLength_] = '\0';
          } else {
            tagTruncated_ = true;
          }
          state_ = State::Text;
          handleTag();
        } else {
          if (c == '"' || c == '\'') {
            if (inQuote_ && c == quoteChar_)
              inQuote_ = false;
            else if (!inQuote_) {
              inQuote_ = true;
              quoteChar_ = c;
            }
          }
          if (tagLength_ + 1 < sizeof(tag_)) {
            tag_[tagLength_++] = c;
            tag_[tagLength_] = '\0';
          } else {
            tagTruncated_ = true;
          }
        }
        break;
      case State::Comment:
        // Comments are dropped entirely; dash counting finds the "-->".
        if (c == '-') {
          if (++commentDashes_ >= 3) {
            state_ = State::Text;
            commentDashes_ = 0;
          }
        } else if (c == '>') {
          if (commentDashes_ >= 2) {
            state_ = State::Text;
            commentDashes_ = 0;
          }
        } else {
          commentDashes_ = 0;
        }
        break;
      case State::Suppressed:
        if (c == '<') {
          state_ = State::SuppressedTag;
          resetTagBuffer();
        }
        break;
      case State::SuppressedTag:
        if (c == '>' && !inQuote_) {
          if (tagLength_ + 1 < sizeof(tag_)) {
            tag_[tagLength_++] = c;
            tag_[tagLength_] = '\0';
          }
          state_ = State::Suppressed;
          // Look for the matching end tag while inside a dropped element.
          const bool closing = tag_[0] == '/';
          char name[32];
          size_t index = closing ? 1 : 0;
          const size_t nameStart = index;
          while (index < tagLength_) {
            const char ch = tag_[index];
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-')
              ++index;
            else
              break;
          }
          copyBounded(name, sizeof(name), tag_ + nameStart, index - nameStart);
          const bool selfClosing = tagLength_ >= 2 && tag_[tagLength_ - 2] == '/';
          if (equalsIgnoreCase(name, suppressedTag_)) {
            if (closing) {
              if (--suppressDepth_ <= 0) {
                state_ = State::Text;
                suppressedTag_[0] = '\0';
                suppressDepth_ = 0;
              }
            } else if (!selfClosing) {
              ++suppressDepth_;
            }
          } else if (!closing && dropWithContent(name) && !selfClosing) {
            // Nested dropped element: keep counting so the outer one still ends
            // at the right place.
            ++suppressDepth_;
          }
        } else {
          if (tagLength_ + 1 < sizeof(tag_)) {
            tag_[tagLength_++] = c;
            tag_[tagLength_] = '\0';
          }
        }
        break;
    }
    if (!active_ || overflowed_) return false;
  }
  return true;
}

bool ArticleWriter::writeAuthorLine(const char* author) {
  if (!active_ || !author || !author[0]) return true;
  char escaped[160];
  if (appendEscaped(escaped, sizeof(escaped), 0, author, std::strlen(author)) == 0) return true;
  return emitLiteral("<p><em>") && emitLiteral(escaped) && emitLiteral("</em></p>\n");
}

bool ArticleWriter::end() {
  if (!active_) return false;
  // Any half-open suppression or tag must not leak into the closing markup.
  if (state_ == State::Suppressed || state_ == State::SuppressedTag) {
    emitLiteral("\n");
  }
  const bool ok = emitLiteral("\n<hr/>\n</body></html>\n");
  active_ = false;
  return ok && !overflowed_;
}

}  // namespace freshrss
