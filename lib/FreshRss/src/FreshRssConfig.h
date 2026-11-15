#pragma once

#include <cstddef>
#include <cstdint>

// Configuration and endpoint handling for the FreshRSS mini-app. Everything
// here is a fixed-size POD: the config is decoded once from a JSON file on the
// SD card and never allocated on the heap. No Arduino types are used so the
// whole module runs in the host test harness.

namespace freshrss {

// Storage limits. Field sizes are chosen so one Config is a few hundred bytes.
constexpr size_t kMaxUrl = 255;  // scheme + host + optional prefix + /api/greader.php
constexpr size_t kMaxUser = 63;
constexpr size_t kMaxSecret = 95;  // FreshRSS API passwords are short; this is generous
constexpr size_t kMaxCaPath = 95;  // SD path to a PEM bundle
constexpr size_t kMaxFolder = 95;  // visible library folder that holds articles
constexpr size_t kMaxFeedFilter = 255;

// Bounds for numeric keys. A malformed or hostile file cannot make the sync
// loop unbounded.
constexpr uint16_t kMinArticlesPerFeed = 1;
constexpr uint16_t kMaxArticlesPerFeed = 50;
constexpr uint8_t kMinMaxFeeds = 1;
constexpr uint8_t kMaxMaxFeeds = 64;
constexpr uint16_t kMinSyncIntervalMinutes = 5;
constexpr uint16_t kMaxSyncIntervalMinutes = 30 * 24 * 60;
constexpr uint16_t kMinKeepPerFeed = 1;
constexpr uint16_t kMaxKeepPerFeed = 200;
constexpr uint32_t kMinArticleBytes = 4096;
constexpr uint32_t kMaxArticleBytes = 512 * 1024;
constexpr size_t kMaxConfigBytes = 4096;

struct Config {
  char server[kMaxUrl + 1] = {0};  // e.g. "https://freshrss.example.net/api/greader.php"
  char user[kMaxUser + 1] = {0};   // FreshRSS login name (not the API password)
  char apiPassword[kMaxSecret + 1] = {0};
  char caCert[kMaxCaPath + 1] = {0};  // optional: SD path to a PEM to validate against
  char folder[kMaxFolder + 1] = "/RSS";
  char feedFilter[kMaxFeedFilter + 1] = {0};  // optional comma-separated feed ids/titles

  uint16_t articlesPerFeed = 15;
  uint8_t maxFeeds = 8;
  uint16_t syncIntervalMinutes = 60;
  uint16_t keepPerFeed = 40;
  uint32_t maxArticleBytes = 96 * 1024;
  bool verifyTls = true;       // official roots; ignored when caCert is set
  bool markReadOnOpen = true;  // queue an upstream mark-as-read when an article opens
  // Fetch the newest items per feed even when the server already reports them as
  // read: an offline "recent articles" shelf instead of an unread mirror.
  bool includeRead = false;

  bool hasFeedFilter() const { return feedFilter[0] != '\0'; }
};

// Parsed server URL. Requests are sent to basePath + apiPath on host:port.
struct Endpoint {
  bool tls = true;
  char host[128] = {0};
  uint16_t port = 443;
  char basePath[160] = {0};  // ends with "/api/greader.php", never a trailing slash
};

// Splits a configured server URL into connection parameters. `server` may omit
// the trailing "/api/greader.php": it is appended when the path does not
// already end with it. Returns false and writes `error` on a malformed value.
bool parseServerUrl(const char* server, Endpoint& out, char* error, size_t errorSize);

// Decodes the SD-card JSON file into `out`. Unknown keys are skipped so the
// file can gain fields without breaking older firmware. Returns false with a
// short reason in `error` for a syntax error or an unusable value.
bool parseConfigJson(const char* text, size_t length, Config& out, char* error, size_t errorSize);

// Clamps ranges and normalizes free-form values (folder, server, filter).
// `validateConfig` reports what is still missing or unusable afterwards.
void normalizeConfig(Config& config);
bool validateConfig(const Config& config, char* error, size_t errorSize);

// True when the last sync is old enough (or the clock moved backwards). A zero
// `lastSyncEpoch` means "never synced" and always syncs.
bool syncDue(uint32_t lastSyncEpoch, uint32_t nowEpoch, const Config& config, bool neverSynced);

// True when a feed id ("feed/12") or title matches the optional filter list.
bool feedAllowed(const Config& config, const char* feedId, const char* feedTitle);

// Appends a JSON key/value pair to `out`. Used to write the annotated example
// file when no config exists yet.
size_t writeExampleConfig(char* out, size_t capacity);

}  // namespace freshrss
