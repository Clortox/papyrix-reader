#include "ZoteroItems.h"

#include <cstdio>
#include <cstring>

#include "../../FreshRss/src/FreshRssText.h"

namespace zotero {
namespace {

using freshrss::JsonScanner;

// Appends to a bounded field, keeping it NUL-terminated and truncating long
// values rather than overflowing.
size_t appendBounded(char* field, size_t capacity, size_t length, const char* text, size_t textLength) {
  if (!field || capacity == 0) return 0;
  const size_t used = length < capacity - 1 ? length : capacity - 1;
  const size_t room = capacity - 1 - used;
  const size_t count = textLength < room ? textLength : room;
  if (count > 0) std::memcpy(field + used, text, count);
  const size_t written = used + count;
  field[written] = '\0';
  return written;
}

bool isLeap(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int daysInMonth(int year, int month) {
  static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeap(year)) return 29;
  return kDays[month - 1];
}

}  // namespace

bool ItemInfo::textEquals(const char* a, const char* b) {
  return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

uint32_t epochFromCivil(int year, int month, int day, int hour, int minute, int second) {
  // Howard Hinnant's days_from_civil, shifted so leap days land at the end of a
  // 400-year cycle.
  int64_t y = year;
  y -= month <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
  const int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second;
  return seconds > 0 ? static_cast<uint32_t>(seconds) : 0;
}

uint32_t parseDate(const char* text) {
  if (!text) return 0;
  // Skip to the first digit: Zotero dates occasionally carry a prefix.
  const char* p = text;
  while (*p && (*p < '0' || *p > '9')) ++p;
  if (*p == '\0') return 0;

  auto readNumber = [&p](int digits, int& out) {
    int value = 0;
    int seen = 0;
    while (seen < digits && *p >= '0' && *p <= '9') {
      value = value * 10 + (*p - '0');
      ++p;
      ++seen;
    }
    out = value;
    return seen == digits;
  };

  int year = 0;
  int month = 0;
  int day = 0;
  if (!readNumber(4, year)) return 0;
  if (*p == '-') ++p;
  if (!readNumber(2, month)) return 0;
  if (*p == '-') ++p;
  if (!readNumber(2, day)) return 0;
  if (month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) return 0;

  int hour = 0;
  int minute = 0;
  int second = 0;
  int offsetMinutes = 0;
  if (*p == 'T' || *p == ' ') {
    ++p;
    int localHour = 0;
    int localMinute = 0;
    int localSecond = 0;
    if (readNumber(2, localHour)) {
      hour = localHour;
      if (*p == ':') ++p;
      if (readNumber(2, localMinute)) {
        minute = localMinute;
        if (*p == ':') ++p;
        if (readNumber(2, localSecond)) second = localSecond;
      }
    }
    // A trailing Z is UTC; an explicit offset shifts the result.
    while (*p && *p != 'Z' && *p != 'z' && *p != '+' && *p != '-') ++p;
    if (*p == '+' || *p == '-') {
      const int sign = *p == '+' ? -1 : 1;
      ++p;
      int offsetHour = 0;
      int offsetMinutePart = 0;
      if (readNumber(2, offsetHour)) {
        if (*p == ':') ++p;
        readNumber(2, offsetMinutePart);
        offsetMinutes = sign * (offsetHour * 60 + offsetMinutePart);
      }
    }
  }
  return epochFromCivil(year, month, day, hour, minute, second) + static_cast<uint32_t>(offsetMinutes * 60);
}

void ItemCollector::beginItem() {
  if (out_.count >= ItemList::kMaxItems) {
    current_ = nullptr;
    skipped_ = true;
    return;
  }
  skipped_ = false;
  current_ = &out_.items[out_.count];
  *current_ = ItemInfo{};
}

void ItemCollector::endItem() {
  if (current_) {
    // Creators arrive either as a single name or as first/last parts; the meta
    // summary is the last resort.
    if (current_->creators[0] == '\0') {
      if (nameFirst_[0] != '\0' || nameLast_[0] != '\0') {
        std::snprintf(current_->creators, sizeof(current_->creators), "%s%s%s", nameFirst_,
                      (nameFirst_[0] != '\0' && nameLast_[0] != '\0') ? " " : "", nameLast_);
      } else if (creatorSummary_[0] != '\0') {
        freshrss::copyBounded(current_->creators, sizeof(current_->creators), creatorSummary_);
      }
    }
    out_.count++;
    current_ = nullptr;
  }
  nameFirst_[0] = '\0';
  nameLast_[0] = '\0';
  creatorSummary_[0] = '\0';
}

void ItemCollector::assignKey(const char* key) {
  freshrss::copyBounded(pendingKey_, sizeof(pendingKey_), key);
  field_ = nullptr;
  Node node = depth_ > 0 ? stack_[depth_ - 1] : Node::Other;
  if (!current_) return;
  switch (node) {
    case Node::Item:
      if (std::strcmp(key, "key") == 0) {
        field_ = current_->key;
        fieldCapacity_ = sizeof(current_->key);
      }
      break;
    case Node::Data:
      if (std::strcmp(key, "itemType") == 0) {
        field_ = current_->itemType;
        fieldCapacity_ = sizeof(current_->itemType);
      } else if (std::strcmp(key, "title") == 0 || std::strcmp(key, "name") == 0) {
        // `name` is a collection's display name.
        field_ = current_->title;
        fieldCapacity_ = sizeof(current_->title);
      } else if (std::strcmp(key, "url") == 0) {
        field_ = current_->url;
        fieldCapacity_ = sizeof(current_->url);
      } else if (std::strcmp(key, "abstractNote") == 0) {
        field_ = current_->abstractNote;
        fieldCapacity_ = sizeof(current_->abstractNote);
      } else if (std::strcmp(key, "date") == 0) {
        field_ = current_->date;
        fieldCapacity_ = sizeof(current_->date);
      } else if (std::strcmp(key, "contentType") == 0) {
        field_ = current_->contentType;
        fieldCapacity_ = sizeof(current_->contentType);
      } else if (std::strcmp(key, "filename") == 0) {
        field_ = current_->filename;
        fieldCapacity_ = sizeof(current_->filename);
      } else if (std::strcmp(key, "linkMode") == 0) {
        field_ = current_->linkMode;
        fieldCapacity_ = sizeof(current_->linkMode);
      } else if (std::strcmp(key, "parentItem") == 0) {
        field_ = current_->parentItem;
        fieldCapacity_ = sizeof(current_->parentItem);
      } else if (std::strcmp(key, "note") == 0) {
        field_ = current_->note;
        fieldCapacity_ = sizeof(current_->note);
      }
      break;
    case Node::Creator:
      if (std::strcmp(key, "name") == 0) {
        field_ = current_->creators;
        fieldCapacity_ = sizeof(current_->creators);
      } else if (std::strcmp(key, "firstName") == 0) {
        field_ = nameFirst_;
        fieldCapacity_ = sizeof(nameFirst_);
      } else if (std::strcmp(key, "lastName") == 0) {
        field_ = nameLast_;
        fieldCapacity_ = sizeof(nameLast_);
      }
      break;
    case Node::Meta:
      if (std::strcmp(key, "creatorSummary") == 0) {
        field_ = creatorSummary_;
        fieldCapacity_ = sizeof(creatorSummary_);
      }
      break;
    case Node::Tag:
      if (std::strcmp(key, "tag") == 0) {
        field_ = current_->tags;
        fieldCapacity_ = sizeof(current_->tags);
      }
      break;
    default:
      break;
  }
  if (field_) fieldLength_ = std::strlen(field_);
}

void ItemCollector::appendText() {
  if (!field_) return;
  fieldLength_ = appendBounded(field_, fieldCapacity_, fieldLength_, scanner_.text(), scanner_.length());
}

void ItemCollector::handle(freshrss::JsonScanner::Token token) {
  using Token = freshrss::JsonScanner::Token;
  const Node node = depth_ > 0 ? stack_[depth_ - 1] : Node::Other;

  switch (token) {
    case Token::ArrayBegin: {
      Node kind = Node::Skip;
      if (depth_ == 0 || node == Node::Other) {
        kind = Node::Other;  // the root array
      } else if (node == Node::Item && std::strcmp(pendingKey_, "data") == 0) {
        kind = Node::Data;
      } else if (node == Node::Item && std::strcmp(pendingKey_, "meta") == 0) {
        kind = Node::Meta;
      } else if (node == Node::Data && std::strcmp(pendingKey_, "creators") == 0) {
        kind = Node::Creators;
      } else if (node == Node::Data && std::strcmp(pendingKey_, "collections") == 0) {
        kind = Node::Collections;
        collectionTaken_ = false;
      } else if (node == Node::Data && std::strcmp(pendingKey_, "tags") == 0) {
        kind = Node::Tags;
      }
      if (depth_ < 12) stack_[depth_] = kind;
      ++depth_;
      pendingKey_[0] = '\0';
      break;
    }

    case Token::ObjectBegin: {
      Node kind = Node::Skip;
      if (depth_ == 1) {
        beginItem();
        kind = Node::Item;
      } else if (node == Node::Item && std::strcmp(pendingKey_, "data") == 0) {
        kind = Node::Data;
      } else if (node == Node::Item && std::strcmp(pendingKey_, "meta") == 0) {
        kind = Node::Meta;
      } else if (node == Node::Creators) {
        kind = Node::Creator;
      } else if (node == Node::Tags) {
        kind = Node::Tag;
      }
      if (depth_ < 12) stack_[depth_] = kind;
      ++depth_;
      pendingKey_[0] = '\0';
      field_ = nullptr;
      break;
    }

    case Token::ObjectEnd:
    case Token::ArrayEnd:
      if (depth_ > 0) {
        const Node closed = stack_[depth_ - 1];
        --depth_;
        if (closed == Node::Item) endItem();
      }
      pendingKey_[0] = '\0';
      field_ = nullptr;
      break;

    case Token::Key:
      assignKey(scanner_.text());
      break;

    case Token::StringChunk:
      appendText();
      break;

    case Token::StringEnd:
      appendText();
      if (node == Node::Collections && current_ && !collectionTaken_ && current_->collection[0] == '\0') {
        freshrss::copyBounded(current_->collection, sizeof(current_->collection), scanner_.text());
        collectionTaken_ = true;
      }
      field_ = nullptr;
      break;

    case Token::Number: {
      if (!current_) break;
      if (node == Node::Item && std::strcmp(pendingKey_, "version") == 0) {
        const long long value = scanner_.numberValue();
        current_->version = value > 0 && value <= 4294967295LL ? static_cast<uint32_t>(value) : 0;
      } else if (node == Node::Meta && std::strcmp(pendingKey_, "numChildren") == 0) {
        const long long value = scanner_.numberValue();
        current_->numChildren = value > 0 && value < 65535 ? static_cast<uint16_t>(value) : 0;
      }
      break;
    }

    case Token::Error:
      failed_ = true;
      freshrss::copyBounded(error_, sizeof(error_), scanner_.text());
      break;

    default:
      break;
  }
}

void ItemCollector::feed(const char* data, size_t length) {
  if (failed_ || !data) return;
  for (size_t i = 0; i < length; ++i) {
    const freshrss::JsonScanner::Token token = scanner_.feed(data[i]);
    if (token != freshrss::JsonScanner::Token::None) handle(token);
    if (failed_) return;
  }
}

bool ItemCollector::finish() {
  if (failed_) return false;
  const freshrss::JsonScanner::Token token = scanner_.finish();
  if (token != freshrss::JsonScanner::Token::None) handle(token);
  if (failed_) return false;
  if (scanner_.failed()) {
    failed_ = true;
    freshrss::copyBounded(error_, sizeof(error_), scanner_.text());
    return false;
  }
  if (depth_ != 0) {
    failed_ = true;
    freshrss::copyBounded(error_, sizeof(error_), "response ended inside an item");
    return false;
  }
  return true;
}

}  // namespace zotero
