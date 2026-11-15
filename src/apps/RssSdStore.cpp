#include "RssSdStore.h"

// Library headers by path, real implementation first: this file then resolves
// without the build system's library include paths.
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../../lib/Logging/src/Logging.h"
#include "../../lib/SDCardManager/include/SDCardManager.h"
#include "../../lib/SdFat/src/SdFat.h"

#define TAG "RSS_SD"

namespace papyrix::rss_app {
namespace {

// The visible config location: easy to drop on the card or upload with the web
// UI. The hidden path is also accepted so the file can live with the other app
// data.
constexpr const char* kConfigPaths[] = {"/rss.json", "/.papyrix/apps/rss.json"};
constexpr const char* kStatePath = "/.papyrix/apps/rss.txt";
constexpr const char* kAppsDirectory = "/.papyrix/apps";

// Directory listings are bounded: the prune pass and the browser both keep only
// what they can show or act on in one pass.
constexpr size_t kMaxListedNames = 64;
constexpr size_t kPruneBatch = 16;

char configText[freshrss::kMaxConfigBytes];
char nameBuffer[kMaxListedNames][80];
// The sync writes one article at a time, so one handle is enough; the store
// exposes it through a pointer to keep the header free of the card library.
FsFile articleFile;

bool isHtmlFile(const char* name) {
  const size_t length = std::strlen(name);
  return length > 5 && name[0] != '.' && strcasecmp(name + length - 5, ".html") == 0;
}

size_t buildPath(char* out, size_t capacity, const char* directory, const char* name) {
  const int written = std::snprintf(out, capacity, "%s/%s", directory, name);
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(written);
}

// Collects up to `kMaxListedNames` names from a directory into the shared
// buffer. Directories are filtered by `wantDirectories`; `limit` bounds the
// caller's needs (a browser needs fewer rows than a prune pass).
size_t collectNames(const char* path, bool wantDirectories, size_t limit) {
  FsFile directory = SdMan.open(path);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return 0;
  }
  const size_t capacity = limit < kMaxListedNames ? limit : kMaxListedNames;
  size_t count = 0;
  for (FsFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    char name[80];
    entry.getName(name, sizeof(name));
    const bool isDirectory = entry.isDirectory();
    entry.close();
    if (name[0] == '\0') continue;
    if (wantDirectories) {
      if (!isDirectory || name[0] == '.') continue;
    } else if (!isHtmlFile(name)) {
      continue;
    }
    if (count < capacity) {
      std::memcpy(nameBuffer[count], name, sizeof(nameBuffer[0]));
      nameBuffer[count][sizeof(nameBuffer[0]) - 1] = '\0';
      ++count;
    }
  }
  directory.close();
  return count;
}

}  // namespace

ConfigLoad loadConfig(freshrss::Config& out, char* error, size_t errorSize) {
  if (errorSize > 0) error[0] = '\0';
  const char* path = nullptr;
  for (const char* candidate : kConfigPaths) {
    if (SdMan.exists(candidate)) {
      path = candidate;
      break;
    }
  }
  if (!path) return ConfigLoad::Missing;

  const size_t length = SdMan.readFileToBuffer(path, configText, sizeof(configText), freshrss::kMaxConfigBytes);
  if (length == 0) {
    if (errorSize > 0) std::snprintf(error, errorSize, "cannot read %s", path);
    return ConfigLoad::Invalid;
  }
  char parseError[96];
  if (!freshrss::parseConfigJson(configText, length, out, parseError, sizeof(parseError))) {
    if (errorSize > 0) std::snprintf(error, errorSize, "%s", parseError);
    LOG_ERR(TAG, "Config parse failed: %s", parseError);
    return ConfigLoad::Invalid;
  }
  if (!freshrss::validateConfig(out, parseError, sizeof(parseError))) {
    if (errorSize > 0) std::snprintf(error, errorSize, "%s", parseError);
    return ConfigLoad::Invalid;
  }
  LOG_INF(TAG, "Config loaded from %s", path);
  return ConfigLoad::Loaded;
}

bool writeExampleConfig(char* error, size_t errorSize) {
  if (errorSize > 0) error[0] = '\0';
  if (SdMan.exists(kConfigPaths[0])) {
    if (errorSize > 0) std::snprintf(error, errorSize, "rss.json already exists");
    return false;
  }
  char text[freshrss::kMaxConfigBytes];
  const size_t length = freshrss::writeExampleConfig(text, sizeof(text));
  if (length == 0) {
    if (errorSize > 0) std::snprintf(error, errorSize, "cannot build the example");
    return false;
  }
  FsFile file = SdMan.open(kConfigPaths[0], O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    if (errorSize > 0) std::snprintf(error, errorSize, "cannot create rss.json");
    return false;
  }
  const bool written = file.write(text, length) == length;
  const bool synced = written && file.sync();
  file.close();
  if (!synced) {
    SdMan.remove(kConfigPaths[0]);
    if (errorSize > 0) std::snprintf(error, errorSize, "cannot write rss.json");
    return false;
  }
  return true;
}

bool loadState(RssState& out) {
  out = RssState{};
  if (!SdMan.exists(kStatePath)) return true;
  char text[4096];
  const size_t length = SdMan.readFileToBuffer(kStatePath, text, sizeof(text), sizeof(text) - 1);
  if (length == 0) return false;
  return decodeState(text, length, out);
}

bool saveState(const RssState& state) {
  char text[kMaxStateBytes];
  const size_t length = encodeState(state, text, sizeof(text));
  if (length == 0) {
    LOG_ERR(TAG, "State too large to save");
    return false;
  }
  SdMan.ensureDirectoryExists(kAppsDirectory);
  FsFile file = SdMan.open(kStatePath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return false;
  const bool written = file.write(text, length) == length;
  const bool synced = written && file.sync();
  file.close();
  if (!synced) {
    LOG_ERR(TAG, "State write failed");
    return false;
  }
  return true;
}

size_t buildArticlePath(char* out, size_t capacity, const char* directory, const char* fileName) {
  return buildPath(out, capacity, directory, fileName);
}

bool SdArticleStore::ensureFeedDirectory(const char* folder, const char* feedTitle, char* out, size_t capacity) {
  if (freshrss::buildFeedDirectory(out, capacity, folder, feedTitle) == 0) return false;
  if (!SdMan.ensureDirectoryExists(out)) return false;
  return SdMan.exists(out);
}

bool SdArticleStore::articleExists(const char* directory, const char* fileName) {
  char path[192];
  if (buildPath(path, sizeof(path), directory, fileName) == 0) return true;  // refuse to write on overflow
  return SdMan.exists(path);
}

freshrss::ArticleFile* SdArticleStore::createArticle(const char* directory, const char* fileName) {
  if (open_) return nullptr;
  if (buildPath(path_, sizeof(path_), directory, fileName) == 0) return nullptr;
  articleFile = SdMan.open(path_, O_WRONLY | O_CREAT | O_TRUNC);
  if (!articleFile) {
    LOG_ERR(TAG, "Cannot create %s", path_);
    path_[0] = '\0';
    return nullptr;
  }
  file_ = &articleFile;
  open_ = true;
  return static_cast<freshrss::ArticleFile*>(this);
}

bool SdArticleStore::removePath(const char* path) {
  if (!SdMan.exists(path)) return true;
  return SdMan.remove(path);
}

bool SdArticleStore::write(const char* data, size_t length) {
  if (!open_ || !file_ || length == 0) return true;
  return file_->write(data, length) == length;
}

bool SdArticleStore::commit() {
  if (!open_ || !file_) return false;
  // SdFat defers the last sector and the directory entry until sync().
  const bool synced = file_->sync();
  const bool closed = file_->close();
  open_ = false;
  file_ = nullptr;
  if (!synced || !closed) {
    LOG_ERR(TAG, "Cannot publish %s", path_);
    removePath(path_);
    return false;
  }
  return true;
}

void SdArticleStore::discard() {
  // The handle may already be closed by a failed sync; removing the path keeps
  // a half-written article off the card either way.
  if (open_ && file_) {
    file_->close();
    open_ = false;
    file_ = nullptr;
  }
  if (path_[0] != '\0') removePath(path_);
  path_[0] = '\0';
}

bool SdArticleStore::pruneFeed(const char* directory, uint16_t keep, uint16_t& removed) {
  removed = 0;
  // Count first so the batch loop knows how many files must go. A listing is
  // capped, so the loop repeats until the directory is inside the limit.
  size_t total = collectNames(directory, false, kMaxListedNames);
  while (total > keep) {
    const size_t batch = std::min(kPruneBatch, total - keep);
    // Names are sorted through pointers: arrays are not assignable, and this
    // keeps the shared buffer in place.
    const char* ordered[kMaxListedNames];
    for (size_t i = 0; i < total; ++i) ordered[i] = nameBuffer[i];
    std::sort(ordered, ordered + total, [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
    for (size_t i = 0; i < batch; ++i) {
      char path[192];
      if (buildPath(path, sizeof(path), directory, ordered[i]) == 0) break;
      if (removePath(path)) ++removed;
    }
    // A capped listing cannot tell whether more files remain, so recount until
    // the directory reports a size inside the limit.
    const size_t previous = total;
    total = collectNames(directory, false, kMaxListedNames);
    if (total >= previous && previous < kMaxListedNames) break;
  }
  return true;
}

uint64_t SdArticleStore::freeBytes() { return SdMan.freeBytes(); }

size_t listFeeds(char entries[][64], size_t capacity) {
  size_t count = collectNames(kRssFolder, true, capacity);
  if (count > capacity) count = capacity;
  for (size_t i = 0; i < count; ++i) {
    std::snprintf(entries[i], 64, "%s", nameBuffer[i]);
  }
  return count;
}

size_t listArticles(const char* directory, char names[][80], size_t capacity) {
  size_t count = collectNames(directory, false, capacity);
  if (count > capacity) count = capacity;
  // File names begin with an ISO date, so sorting descending puts the newest
  // article first. Sorting through pointers keeps the shared buffer in place
  // (array elements are not assignable).
  const char* ordered[kMaxListedNames];
  for (size_t i = 0; i < count; ++i) ordered[i] = nameBuffer[i];
  std::sort(ordered, ordered + count, [](const char* a, const char* b) { return std::strcmp(a, b) > 0; });
  for (size_t i = 0; i < count; ++i) {
    std::snprintf(names[i], 80, "%s", ordered[i]);
  }
  return count;
}

bool readArticleHead(const char* path, char* out, size_t capacity) {
  if (!out || capacity < 2) return false;
  out[0] = '\0';
  FsFile file = SdMan.open(path, O_RDONLY);
  if (!file) return false;
  const int got = file.read(out, capacity - 1);
  file.close();
  if (got <= 0) return false;
  out[got] = '\0';
  return true;
}

bool deleteArticle(const char* directory, const char* fileName) {
  char path[192];
  if (buildPath(path, sizeof(path), directory, fileName) == 0) return false;
  return SdMan.remove(path);
}

}  // namespace papyrix::rss_app
