#pragma once

// The reader library is included by path so this header is self-contained and
// does not depend on the build system's library include paths.
#include <cstddef>
#include <cstdint>

#include "../../lib/FreshRss/src/FreshRssArticle.h"
#include "../../lib/FreshRss/src/FreshRssConfig.h"
#include "../../lib/FreshRss/src/FreshRssSync.h"
#include "RssState.h"

// The store owns the open file through a pointer, so the card library's header
// stays out of this interface.
class FsFile;

// SD-card access for the RSS mini-app: the JSON config, the sync state file,
// the article store, and the directory listings used by the browser.

namespace papyrix::rss_app {

constexpr const char* kRssFolder = "/RSS";

enum class ConfigLoad : uint8_t {
  Loaded,
  Missing,  // no config file on the card yet
  Invalid,  // a file exists but could not be used
};

// Reads the JSON config. Both the visible path and the hidden fallback are
// accepted so the file can be dropped on the card or uploaded with the web UI.
ConfigLoad loadConfig(freshrss::Config& out, char* error, size_t errorSize);

// Writes the annotated example config to the visible path. Refuses to overwrite
// an existing file so a real credential is never lost.
bool writeExampleConfig(char* error, size_t errorSize);

bool loadState(RssState& out);
bool saveState(const RssState& state);

// Article store on the card. The sync writes one article at a time, so a single
// file handle is reused and the store doubles as the article sink.
class SdArticleStore : public freshrss::ArticleStore, public freshrss::ArticleFile {
 public:
  bool ensureFeedDirectory(const char* folder, const char* feedTitle, char* out, size_t capacity) override;
  bool articleExists(const char* directory, const char* fileName) override;
  freshrss::ArticleFile* createArticle(const char* directory, const char* fileName) override;
  bool pruneFeed(const char* directory, uint16_t keep, uint16_t& removed) override;
  uint64_t freeBytes() override;

  // freshrss::ArticleFile for the file currently being written.
  bool write(const char* data, size_t length) override;
  bool commit() override;
  void discard() override;

 private:
  bool removePath(const char* path);
  char path_[192] = {};
  FsFile* file_ = nullptr;
  bool open_ = false;
};

// Directory names under the article folder, newest first.
size_t listFeeds(char entries[][64], size_t capacity);
// File names inside one feed folder, newest first.
size_t listArticles(const char* directory, char names[][80], size_t capacity);
// Reads the head of a stored article (used to recover its item id).
bool readArticleHead(const char* path, char* out, size_t capacity);
// Builds "<folder>/<feed>/<name>" into `out`.
size_t buildArticlePath(char* out, size_t capacity, const char* directory, const char* fileName);
bool deleteArticle(const char* directory, const char* fileName);

}  // namespace papyrix::rss_app
