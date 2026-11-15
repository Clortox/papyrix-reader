#pragma once

#include <cstddef>
#include <cstdint>

// Configuration for the Zotero reader. The file is a flat JSON object on the SD
// card, decoded into this fixed-size POD, so nothing is allocated on the heap.
// The streaming JSON scanner is shared with the FreshRSS client (lib/FreshRss).

namespace zotero {

constexpr size_t kMaxApiKey = 63;
constexpr size_t kMaxLibrary = 47;    // "users/1234567" or "groups/12345"
constexpr size_t kMaxFolder = 95;
constexpr size_t kMaxCollection = 31;  // optional collection key filter
constexpr size_t kMaxTag = 63;         // optional tag filter
constexpr size_t kMaxCaPath = 95;

constexpr uint16_t kMinItemsLimit = 1;
constexpr uint16_t kMaxItemsLimit = 100;  // the API caps one page at 100
constexpr uint16_t kMinKeepPerCollection = 1;
constexpr uint16_t kMaxKeepPerCollection = 500;
constexpr uint16_t kMinSyncIntervalMinutes = 5;
constexpr uint16_t kMaxSyncIntervalMinutes = 30 * 24 * 60;
constexpr uint32_t kMinItemBytes = 4096;
constexpr uint32_t kMaxItemBytesLimit = 512 * 1024;
constexpr size_t kMaxConfigBytes = 4096;

struct Config {
  char apiKey[kMaxApiKey + 1] = {0};        // a personal Zotero API key
  // Library prefix as used by the API: "users/<userID>" or "groups/<groupID>".
  // A bare number is accepted and read as a user id.
  char library[kMaxLibrary + 1] = {0};
  char folder[kMaxFolder + 1] = "/Zotero";
  char collection[kMaxCollection + 1] = {0};  // optional: only this collection
  char tag[kMaxTag + 1] = {0};                // optional: only items with this tag
  char caCert[kMaxCaPath + 1] = {0};          // optional PEM for private proxies

  uint16_t itemsLimit = 50;  // newest top-level items per sync
  uint16_t keepPerCollection = 40;
  uint32_t maxItemBytes = 256 * 1024;
  uint16_t syncIntervalMinutes = 60;

  bool includeNotes = true;      // include child notes as content
  bool fetchLinkedPages = true;  // fetch the target URL when no snapshot exists
  bool downloadPdf = false;      // PDFs are not readable on the device
  bool verifyTls = true;         // official roots; ignored when caCert is set

  // True when the library value looks usable.
  bool hasLibrary() const { return library[0] != '\0'; }
};

// Decodes the SD-card JSON file. Unknown keys are skipped, so the file can gain
// fields without breaking older firmware. Returns false with a short reason.
bool parseConfig(const char* text, size_t length, Config& out, char* error, size_t errorSize);

// Clamps ranges and normalizes free-form values.
void normalizeConfig(Config& config);
bool validateConfig(const Config& config, char* error, size_t errorSize);

// Builds the API prefix for a library value: "users/123" stays as it is, "123"
// becomes "users/123", and a group id can be written "groups/456".
void normalizeLibrary(const char* value, char* out, size_t capacity);

// True when a sync is due. A zero `lastSyncEpoch` means "never synced".
bool syncDue(uint32_t lastSyncEpoch, uint32_t nowEpoch, const Config& config, bool neverSynced);

// The annotated example written when no config exists yet.
size_t writeExampleConfig(char* out, size_t capacity);

// Folder name used for items that belong to no collection.
constexpr const char* kUnfiledFolder = "Unfiled";

}  // namespace zotero
