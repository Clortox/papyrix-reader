#pragma once

#include <cstddef>
#include <cstdint>

// The streaming JSON scanner and text helpers are shared with the FreshRSS
// client; both live in lib/FreshRss.
#include "../../FreshRss/src/FreshRssJson.h"

// Parsing for Zotero Web API responses. The API wraps every object as
// { key, version, library, links, meta, data{...} }, and item responses are
// arrays of those wrappers. The streaming JSON scanner is shared with the
// FreshRSS client, so a long response never has to fit in RAM.

namespace zotero {

// One item, flattened into fixed-size fields. Attachments and notes appear here
// too, with their type-specific fields filled in.
struct ItemInfo {
  char key[12] = {};  // 8-character Zotero key
  char itemType[24] = {};
  char title[192] = {};
  char creators[160] = {};  // joined "A; B" (creatorSummary or first/last names)
  char date[32] = {};
  char url[256] = {};
  char abstractNote[400] = {};
  char collection[12] = {};   // first collection key, empty when unfiled
  char tags[120] = {};        // up to a few tags, joined
  char contentType[40] = {};  // attachments: "text/html", "application/pdf", ...
  char filename[128] = {};    // attachments: stored file name
  char linkMode[24] = {};     // attachments: imported_url, linked_url, ...
  char parentItem[12] = {};
  char note[600] = {};  // notes: the HTML body, truncated
  uint32_t version = 0;
  uint16_t numChildren = 0;

  bool isAttachment() const { return textEquals(itemType, "attachment"); }
  bool isNote() const { return textEquals(itemType, "note"); }
  bool isHtmlSnapshot() const { return isAttachment() && textEquals(contentType, "text/html"); }
  bool isPdf() const { return isAttachment() && textEquals(contentType, "application/pdf"); }

  static bool textEquals(const char* a, const char* b);
};

struct ItemList {
  // Bounded so a list can live in the sync session rather than on the stack.
  static constexpr size_t kMaxItems = 24;
  ItemInfo items[kMaxItems];
  size_t count = 0;
};

// Collects an array of item wrappers (items/top, items/<key>/children, or
// collections, whose name lands in `title`).
class ItemCollector {
 public:
  explicit ItemCollector(ItemList& out) : out_(out) {}

  void feed(const char* data, size_t length);
  bool finish();

  bool failed() const { return failed_; }
  const char* error() const { return error_; }
  size_t count() const { return out_.count; }

 private:
  void handle(freshrss::JsonScanner::Token token);
  void beginItem();
  void endItem();
  void assignKey(const char* key);
  void appendText();

  ItemList& out_;
  freshrss::JsonScanner scanner_;
  int depth_ = 0;
  // What each open container is, so a value can be routed by depth.
  enum class Node : uint8_t { Other, Item, Data, Meta, Creators, Creator, Collections, Tags, Tag, Skip };
  Node stack_[12] = {};
  bool skipped_ = false;
  int skipDepth_ = 0;
  ItemInfo* current_ = nullptr;
  char pendingKey_[32] = {};
  char* field_ = nullptr;
  size_t fieldCapacity_ = 0;
  size_t fieldLength_ = 0;
  char nameFirst_[48] = {};
  char nameLast_[48] = {};
  char creatorSummary_[64] = {};
  bool collectionTaken_ = false;
  bool failed_ = false;
  char error_[64] = {};
};

// Zotero dates are free-form; ISO 8601 prefixes ("2017-08-22", with or without
// a time) are the common case. Returns 0 when nothing usable was found.
uint32_t parseDate(const char* text);

// Civil date to Unix seconds (UTC), used by parseDate.
uint32_t epochFromCivil(int year, int month, int day, int hour, int minute, int second);

}  // namespace zotero
