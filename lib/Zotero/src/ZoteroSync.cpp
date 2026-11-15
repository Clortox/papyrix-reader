#include "ZoteroSync.h"

#include <cstdio>
#include <cstring>

#include "../../FreshRss/src/FreshRssJson.h"
#include "../../FreshRss/src/FreshRssText.h"

namespace zotero {
namespace {

constexpr const char* kApiHost = "api.zotero.org";
constexpr uint16_t kApiPort = 443;
constexpr uint32_t kConnectTimeoutMs = 20000;
// Room left on the card when starting an article.
constexpr uint64_t kSpaceReserve = 32ull * 1024ull;
constexpr size_t kStreamBuffer = 256;

bool isTextualContent(const char* contentType) {
  if (contentType[0] == '\0') return true;
  return freshrss::startsWithIgnoreCase(contentType, "text/html") ||
         freshrss::startsWithIgnoreCase(contentType, "text/plain") ||
         freshrss::startsWithIgnoreCase(contentType, "application/xhtml");
}

// Splits an absolute URL into what the transport needs plus a request target.
struct UrlParts {
  bool tls = true;
  char host[128] = {};
  uint16_t port = 443;
  char path[384] = {};
};

bool parseUrl(const char* url, UrlParts& out) {
  if (!url) return false;
  const char* rest = nullptr;
  if (freshrss::startsWithIgnoreCase(url, "https://")) {
    out.tls = true;
    out.port = 443;
    rest = url + 8;
  } else if (freshrss::startsWithIgnoreCase(url, "http://")) {
    out.tls = false;
    out.port = 80;
    rest = url + 7;
  } else {
    return false;
  }

  const char* slash = std::strchr(rest, '/');
  size_t authorityLength = slash ? static_cast<size_t>(slash - rest) : std::strlen(rest);
  if (authorityLength == 0 || authorityLength > sizeof(out.host) - 1) return false;

  const char* colon = static_cast<const char*>(std::memchr(rest, ':', authorityLength));
  const size_t hostLength = colon ? static_cast<size_t>(colon - rest) : authorityLength;
  if (hostLength == 0) return false;
  freshrss::copyBounded(out.host, sizeof(out.host), rest, hostLength);
  if (colon) {
    long port = 0;
    for (const char* p = colon + 1; p < rest + authorityLength; ++p) {
      if (*p < '0' || *p > '9') break;
      port = port * 10 + (*p - '0');
    }
    if (port > 0 && port <= 65535) out.port = static_cast<uint16_t>(port);
  }

  if (slash && slash[0] != '\0') {
    freshrss::copyBounded(out.path, sizeof(out.path), slash);
  } else {
    freshrss::copyBounded(out.path, sizeof(out.path), "/");
  }
  return true;
}

}  // namespace

const char* describeStatus(Status status) {
  switch (status) {
    case Status::Ok:
      return "synced";
    case Status::NoConfig:
      return "no config";
    case Status::NetworkError:
      return "network error";
    case Status::AuthFailed:
      return "api key rejected";
    case Status::HttpError:
      return "server error";
    case Status::BadResponse:
      return "bad response";
    case Status::OutOfSpace:
      return "card full";
    case Status::Aborted:
      return "stopped";
  }
  return "error";
}

size_t encodeState(const State& state, char* out, size_t capacity) {
  if (!out || capacity == 0) return 0;
  const int written = std::snprintf(out, capacity, "lastSync=%u\nversion=%u\nstart=%u\n",
                                    static_cast<unsigned>(state.lastSyncEpoch),
                                    static_cast<unsigned>(state.libraryVersion),
                                    static_cast<unsigned>(state.nextStart));
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(written);
}

bool decodeState(const char* text, size_t length, State& out) {
  out = State{};
  if (!text) return true;
  size_t offset = 0;
  while (offset < length) {
    size_t end = offset;
    while (end < length && text[end] != '\n') ++end;
    size_t lineLength = end - offset;
    if (lineLength > 0 && text[offset + lineLength - 1] == '\r') --lineLength;
    const char* line = text + offset;
    const auto parseNumber = [&](const char* field, uint32_t& target) {
      const size_t fieldLength = std::strlen(field);
      if (lineLength <= fieldLength || std::strncmp(line, field, fieldLength) != 0) return;
      uint64_t value = 0;
      for (size_t i = fieldLength; i < lineLength; ++i) {
        if (line[i] < '0' || line[i] > '9') return;
        value = value * 10 + static_cast<uint64_t>(line[i] - '0');
      }
      target = static_cast<uint32_t>(value);
    };
    parseNumber("lastSync=", out.lastSyncEpoch);
    parseNumber("version=", out.libraryVersion);
    parseNumber("start=", out.nextStart);
    offset = end + 1;
  }
  return true;
}

void Session::report(Stage stage, size_t index, size_t count, const char* title) {
  if (!hooks_.progress) return;
  Progress progress;
  progress.stage = stage;
  progress.itemIndex = index;
  progress.itemCount = count;
  progress.title = title ? title : "";
  progress.stored = stats_.articlesStored;
  if (!hooks_.progress(hooks_.ctx, progress)) aborted_ = true;
}

bool Session::ensureConnected(Outcome& outcome) {
  const char* caCert = config_.caCert[0] != '\0' ? config_.caCert : nullptr;
  if (!transport_.connect(kApiHost, kApiPort, true, config_.verifyTls, caCert, kConnectTimeoutMs)) {
    outcome.status = Status::NetworkError;
    freshrss::copyBounded(outcome.message, sizeof(outcome.message), "cannot reach api.zotero.org");
    return false;
  }
  freshrss::copyBounded(host_, sizeof(host_), kApiHost);
  port_ = kApiPort;
  tls_ = true;
  return true;
}

bool Session::ensureApiConnection(Outcome& outcome) {
  if (transport_.isConnected() && std::strcmp(host_, kApiHost) == 0) return true;
  http_.reset();
  transport_.close();
  return ensureConnected(outcome);
}

bool Session::sendRelative(const char* target, bool withAuth, Outcome& outcome) {
  char hostHeader[200];
  char requestHost[160];
  freshrss::copyBounded(requestHost, sizeof(requestHost), host_);
  if (freshrss::buildHostHeader(hostHeader, sizeof(hostHeader), requestHost, port_, tls_) == 0) {
    outcome.status = Status::BadResponse;
    freshrss::copyBounded(outcome.message, sizeof(outcome.message), "bad host");
    return false;
  }

  freshrss::HttpRequest request;
  request.method = "GET";
  request.path = target;
  request.host = hostHeader;
  request.keepAlive = true;
  request.authorization = withAuth ? auth_ : nullptr;

  freshrss::HttpResponseHead head;
  char error[64] = {};
  if (!http_.request(request, head, error, sizeof(error))) {
    // A server that dropped a keep-alive connection is the common case: rebuild
    // the connection and try the same request once more.
    http_.reset();
    transport_.close();
    if (!ensureConnected(outcome) || !http_.request(request, head, error, sizeof(error))) {
      if (outcome.status == Status::Ok) {
        outcome.status = Status::NetworkError;
        freshrss::copyBounded(outcome.message, sizeof(outcome.message), error[0] ? error : "request failed");
      }
      return false;
    }
  }
  if (http_.head().lastModifiedVersion >= 0) {
    libraryVersion_ = static_cast<uint32_t>(http_.head().lastModifiedVersion);
  }
  return true;
}

bool Session::sendApi(const char* path, const char* query, bool withAuth, Outcome& outcome) {
  if (!ensureApiConnection(outcome)) return false;
  if (freshrss::buildRequestPath(path_, sizeof(path_), "", path, query) == 0) {
    outcome.status = Status::BadResponse;
    freshrss::copyBounded(outcome.message, sizeof(outcome.message), "request too long");
    return false;
  }
  return sendRelative(path_, withAuth, outcome);
}

bool Session::checkStatus(Outcome& outcome, const char* what) {
  const int status = http_.head().status;
  if (status == 200) return true;
  // The caller will not read this body.
  http_.drainBody();
  outcome.httpStatus = status;
  if (status == 401 || status == 403) {
    outcome.status = Status::AuthFailed;
    freshrss::copyBounded(outcome.message, sizeof(outcome.message), "check the apiKey and library");
    return false;
  }
  outcome.status = Status::HttpError;
  char message[80];
  if (status == 429 || status == 503) {
    std::snprintf(message, sizeof(message), "%s: rate limited, try again later", what);
  } else {
    std::snprintf(message, sizeof(message), "%s: HTTP %d", what, status);
  }
  freshrss::copyBounded(outcome.message, sizeof(outcome.message), message);
  return false;
}

bool Session::fetchCollections(Outcome& outcome) {
  report(Stage::Collections, 0, 0, "");
  char path[128];
  std::snprintf(path, sizeof(path), "/%s/collections", config_.library);
  if (!sendApi(path, "format=json&limit=24&v=3", true, outcome)) return false;
  if (!checkStatus(outcome, "collections")) {
    // Folders fall back to collection keys, so an unavailable listing is not
    // fatal for the items themselves.
    outcome.status = Status::Ok;
    outcome.httpStatus = 0;
    outcome.message[0] = '\0';
    return true;
  }

  collectionList_.count = 0;
  ItemCollector collector(collectionList_);
  char buffer[kStreamBuffer];
  while (true) {
    const int got = http_.readBody(buffer, sizeof(buffer));
    if (got < 0) {
      outcome.status = Status::NetworkError;
      freshrss::copyBounded(outcome.message, sizeof(outcome.message), http_.error());
      return false;
    }
    if (got == 0) break;
    collector.feed(buffer, static_cast<size_t>(got));
  }
  if (!collector.finish()) {
    outcome.status = Status::BadResponse;
    freshrss::copyBounded(outcome.message, sizeof(outcome.message), collector.error());
    return false;
  }
  for (size_t i = 0; i < collectionList_.count && collectionCount_ < kMaxNamedCollections; ++i) {
    const ItemInfo& entry = collectionList_.items[i];
    if (entry.key[0] == '\0') continue;
    freshrss::copyBounded(collections_[collectionCount_].key, sizeof(collections_[0].key), entry.key);
    freshrss::copyBounded(collections_[collectionCount_].name, sizeof(collections_[0].name),
                          entry.title[0] ? entry.title : entry.key);
    ++collectionCount_;
  }
  return true;
}

const char* Session::collectionName(const char* key) const {
  if (!key || key[0] == '\0') return kUnfiledFolder;
  for (size_t i = 0; i < collectionCount_; ++i) {
    if (std::strcmp(collections_[i].key, key) == 0) return collections_[i].name;
  }
  return key;  // an unknown collection still gets its own folder
}

bool Session::fetchItems(const Request& request, Outcome& outcome) {
  report(Stage::Items, 0, 0, "");
  char path[192];
  if (config_.collection[0] != '\0') {
    std::snprintf(path, sizeof(path), "/%s/collections/%s/items/top", config_.library, config_.collection);
  } else {
    std::snprintf(path, sizeof(path), "/%s/items/top", config_.library);
  }

  size_t written = std::snprintf(query_, sizeof(query_),
                                 "format=json&limit=%u&sort=dateAdded&direction=desc&v=3",
                                 static_cast<unsigned>(config_.itemsLimit));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(query_)) {
    outcome.status = Status::BadResponse;
    freshrss::copyBounded(outcome.message, sizeof(outcome.message), "request too long");
    return false;
  }
  if (config_.tag[0] != '\0') {
    char encoded[160];
    if (freshrss::formEncodeAppend(encoded, sizeof(encoded), 0, config_.tag) == 0) {
      outcome.status = Status::BadResponse;
      freshrss::copyBounded(outcome.message, sizeof(outcome.message), "tag filter too long");
      return false;
    }
    const size_t length = std::strlen(query_);
    const int added = std::snprintf(query_ + length, sizeof(query_) - length, "&tag=%s", encoded);
    if (added <= 0 || static_cast<size_t>(added) >= sizeof(query_) - length) {
      outcome.status = Status::BadResponse;
      freshrss::copyBounded(outcome.message, sizeof(outcome.message), "tag filter too long");
      return false;
    }
  }
  if (request.sinceVersion > 0 && request.startOffset == 0) {
    // Incremental runs only: a backfill pass walks the whole listing with `start`
    // instead, because `since` would skip everything older than the last run.
    char since[32];
    std::snprintf(since, sizeof(since), "&since=%u", static_cast<unsigned>(request.sinceVersion));
    const size_t length = std::strlen(query_);
    if (length + std::strlen(since) + 1 > sizeof(query_)) {
      outcome.status = Status::BadResponse;
      freshrss::copyBounded(outcome.message, sizeof(outcome.message), "request too long");
      return false;
    }
    std::memcpy(query_ + length, since, std::strlen(since) + 1);
  }

  if (request.sinceVersion == 0 && request.startOffset > 0) {
    char start[32];
    std::snprintf(start, sizeof(start), "&start=%u", static_cast<unsigned>(request.startOffset));
    const size_t length = std::strlen(query_);
    if (length + std::strlen(start) + 1 > sizeof(query_)) {
      outcome.status = Status::BadResponse;
      freshrss::copyBounded(outcome.message, sizeof(outcome.message), "request too long");
      return false;
    }
    std::memcpy(query_ + length, start, std::strlen(start) + 1);
  }
  if (!sendApi(path, query_, true, outcome)) return false;
  if (!checkStatus(outcome, "items")) return false;

  items_.count = 0;
  ItemCollector collector(items_);
  char buffer[kStreamBuffer];
  while (true) {
    const int got = http_.readBody(buffer, sizeof(buffer));
    if (got < 0) {
      outcome.status = Status::NetworkError;
      freshrss::copyBounded(outcome.message, sizeof(outcome.message), http_.error());
      return false;
    }
    if (got == 0) break;
    collector.feed(buffer, static_cast<size_t>(got));
  }
  if (!collector.finish()) {
    outcome.status = Status::BadResponse;
    freshrss::copyBounded(outcome.message, sizeof(outcome.message), collector.error());
    return false;
  }

  // Fewer items than asked for means the listing ended, so the next run may
  // start over and the incremental window can move.
  outcome.coveredAll = items_.count < config_.itemsLimit || request.sinceVersion > 0;
  outcome.nextStart = outcome.coveredAll ? 0 : request.startOffset + static_cast<uint32_t>(items_.count);

  for (size_t i = 0; i < items_.count; ++i) {
    if (aborted_) {
      outcome.status = Status::Aborted;
      freshrss::copyBounded(outcome.message, sizeof(outcome.message), "stopped");
      return false;
    }
    const ItemInfo& item = items_.items[i];
    report(Stage::Content, i, items_.count, item.title[0] ? item.title : item.key);
    if (!saveItem(item, request, outcome)) return false;
    stats_.itemsChecked++;
  }
  return true;
}

bool Session::fetchChildren(const ItemInfo& item, ItemList& children, Outcome& outcome) {
  char path[192];
  std::snprintf(path, sizeof(path), "/%s/items/%s/children", config_.library, item.key);
  if (!sendApi(path, "format=json&limit=25&v=3", true, outcome)) return false;
  if (!checkStatus(outcome, "attachments")) {
    // Without the children there is still the item's own URL to fall back on.
    outcome.status = Status::Ok;
    outcome.httpStatus = 0;
    outcome.message[0] = '\0';
    return false;
  }

  ItemCollector collector(children);
  char buffer[kStreamBuffer];
  while (true) {
    const int got = http_.readBody(buffer, sizeof(buffer));
    if (got < 0) {
      outcome.status = Status::NetworkError;
      freshrss::copyBounded(outcome.message, sizeof(outcome.message), http_.error());
      return false;
    }
    if (got == 0) break;
    collector.feed(buffer, static_cast<size_t>(got));
  }
  if (!collector.finish()) {
    outcome.status = Status::BadResponse;
    freshrss::copyBounded(outcome.message, sizeof(outcome.message), collector.error());
    return false;
  }
  return true;
}

bool Session::streamBodyInto(freshrss::ArticleWriter& writer, Outcome& outcome) {
  // A binary attachment (a PDF, say) is not readable here: leave it to the
  // metadata line instead of writing junk into the article.
  if (!isTextualContent(http_.head().contentType)) return false;
  char buffer[kStreamBuffer];
  while (true) {
    const int got = http_.readBody(buffer, sizeof(buffer));
    if (got < 0) {
      outcome.status = Status::NetworkError;
      freshrss::copyBounded(outcome.message, sizeof(outcome.message), http_.error());
      return false;
    }
    if (got == 0) break;
    if (!writer.writeContent(buffer, static_cast<size_t>(got))) return false;
  }
  return true;
}

bool Session::streamAttachment(const char* path, freshrss::ArticleWriter& writer, Outcome& outcome) {
  if (!sendRelative(path, true, outcome)) return false;
  const int status = http_.head().status;
  if (status == 302 || status == 301 || status == 307 || status == 308) {
    // Zotero file storage answers with a redirect to a pre-signed URL on another
    // host; the API key must not travel there.
    char location[sizeof(http_.head().location)];
    freshrss::copyBounded(location, sizeof(location), http_.head().location);
    http_.drainBody();
    if (location[0] == '\0') return false;
    char target[384];
    if (!openAbsoluteUrl(location, target, sizeof(target), outcome)) return false;
    if (!sendRelative(target, false, outcome)) return false;
    if (http_.head().status != 200) {
      http_.drainBody();
      return false;
    }
    return streamBodyInto(writer, outcome);
  }
  if (status != 200) {
    http_.drainBody();
    return false;
  }
  return streamBodyInto(writer, outcome);
}

bool Session::openAbsoluteUrl(const char* url, char* pathOut, size_t pathCapacity, Outcome& outcome) {
  UrlParts parts;
  if (!parseUrl(url, parts)) return false;
  freshrss::copyBounded(pathOut, pathCapacity, parts.path);
  if (transport_.isConnected() && std::strcmp(host_, parts.host) == 0 && port_ == parts.port && tls_ == parts.tls)
    return true;

  http_.reset();
  transport_.close();
  const char* caCert = config_.caCert[0] != '\0' ? config_.caCert : nullptr;
  if (!transport_.connect(parts.host, parts.port, parts.tls, config_.verifyTls, caCert, kConnectTimeoutMs)) {
    outcome.status = Status::NetworkError;
    freshrss::copyBounded(outcome.message, sizeof(outcome.message), "cannot reach the page host");
    return false;
  }
  freshrss::copyBounded(host_, sizeof(host_), parts.host);
  port_ = parts.port;
  tls_ = parts.tls;
  return true;
}

bool Session::streamPage(const char* url, freshrss::ArticleWriter& writer, Outcome& outcome) {
  char target[384];
  if (!openAbsoluteUrl(url, target, sizeof(target), outcome)) {
    // A page we cannot reach is not a sync failure: the item still gets whatever
    // else it has.
    outcome.status = Status::Ok;
    outcome.httpStatus = 0;
    outcome.message[0] = '\0';
    return false;
  }
  if (!sendRelative(target, false, outcome)) {
    outcome.status = Status::Ok;
    outcome.httpStatus = 0;
    outcome.message[0] = '\0';
    return false;
  }
  if (http_.head().status != 200) {
    http_.drainBody();
    return false;
  }
  return streamBodyInto(writer, outcome);
}

bool Session::saveItem(const ItemInfo& item, const Request& request, Outcome& outcome) {
  const char* folder = collectionName(item.collection);
  char directory[256];
  if (!store_.ensureFeedDirectory(config_.folder, folder, directory, sizeof(directory))) {
    outcome.status = Status::OutOfSpace;
    freshrss::copyBounded(outcome.message, sizeof(outcome.message), "cannot create folder");
    return false;
  }

  const uint32_t publishedEpoch = parseDate(item.date);
  char name[160];
  if (freshrss::buildArticleFileName(name, sizeof(name), item.title, publishedEpoch, request.nowEpoch,
                                     freshrss::hashItemId(item.key)) == 0) {
    stats_.articlesFailed++;
    return true;
  }
  if (store_.articleExists(directory, name)) {
    // The same item always maps to the same file, so nothing is refetched.
    stats_.articlesSkipped++;
    return true;
  }
  if (store_.freeBytes() < static_cast<uint64_t>(config_.maxItemBytes) + kSpaceReserve) {
    outcome.status = Status::OutOfSpace;
    freshrss::copyBounded(outcome.message, sizeof(outcome.message), "card full");
    return false;
  }

  freshrss::ArticleFile* file = store_.createArticle(directory, name);
  if (!file) {
    stats_.articlesFailed++;
    return true;
  }

  const uint32_t fallbackEpoch = request.nowEpoch != 0 ? request.nowEpoch : 1794746040u;
  freshrss::ArticleWriter writer;
  if (!writer.begin(*file, config_.maxItemBytes, item.title, folder, item.creators, item.url,
                    publishedEpoch != 0 ? publishedEpoch : fallbackEpoch, item.key)) {
    file->discard();
    stats_.articlesFailed++;
    return true;
  }

  // What to read, best first: the page Zotero saved, the item's own note, the
  // page at its original URL, then the metadata alone.
  bool hasContent = false;
  if (item.numChildren > 0) {
    ItemList children;
    if (fetchChildren(item, children, outcome)) {
      for (size_t i = 0; i < children.count; ++i) {
        const ItemInfo& child = children.items[i];
        if (!child.isHtmlSnapshot()) continue;
        char childPath[192];
        std::snprintf(childPath, sizeof(childPath), "/%s/items/%s/file", config_.library, child.key);
        if (streamAttachment(childPath, writer, outcome)) {
          stats_.snapshots++;
          hasContent = true;
        }
        break;
      }
      if (config_.includeNotes) {
        for (size_t i = 0; i < children.count; ++i) {
          const ItemInfo& child = children.items[i];
          if (!child.isNote() || child.note[0] == '\0') continue;
          if (writer.writeContent(child.note, std::strlen(child.note))) hasContent = true;
        }
      }
    } else if (outcome.status != Status::Ok) {
      file->discard();
      return false;
    }
  }

  if (config_.includeNotes && item.isNote() && item.note[0] != '\0') {
    if (writer.writeContent(item.note, std::strlen(item.note))) hasContent = true;
  }
  if (config_.fetchLinkedPages && item.url[0] != '\0') {
    if (streamPage(item.url, writer, outcome)) {
      stats_.pages++;
      hasContent = true;
    } else if (outcome.status != Status::Ok) {
      file->discard();
      return false;
    }
  }
  if (!hasContent && item.abstractNote[0] != '\0') {
    const char* open = "<p><em>Abstract</em></p><p>";
    const char* close = "</p>";
    if (writer.writeContent(open, std::strlen(open)) &&
        writer.writeContent(item.abstractNote, std::strlen(item.abstractNote)) &&
        writer.writeContent(close, std::strlen(close)))
      hasContent = true;
  }
  if (item.numChildren > 0) {
    ItemList children;
    // Already fetched for a snapshot; fetching again is cheap and keeps this
    // path independent.
    if (fetchChildren(item, children, outcome)) {
      for (size_t i = 0; i < children.count; ++i) {
        const ItemInfo& child = children.items[i];
        if (!child.isAttachment() || child.isHtmlSnapshot() || child.filename[0] == '\0') continue;
        char line[220];
        std::snprintf(line, sizeof(line), "<p><em>Attachment on the Zotero server: %s (%s)</em></p>", child.filename,
                      child.contentType[0] ? child.contentType : "file");
        writer.writeContent(line, std::strlen(line));
      }
    } else if (outcome.status != Status::Ok) {
      file->discard();
      return false;
    }
  }

  const bool written = writer.end();
  if (!written || !file->commit()) {
    file->discard();
    stats_.articlesFailed++;
    return true;
  }

  stats_.articlesStored++;
  stats_.bytesStored += static_cast<uint32_t>(writer.bytesWritten());
  if (hooks_.stored) {
    StoredArticle stored;
    stored.collection = folder;
    stored.fileName = name;
    stored.title = item.title;
    stored.bytes = static_cast<uint32_t>(writer.bytesWritten());
    if (!hooks_.stored(hooks_.ctx, stored)) aborted_ = true;
  }
  return true;
}

Outcome Session::run(const Request& request) {
  Outcome outcome;
  stats_ = Stats{};
  collectionCount_ = 0;
  libraryVersion_ = 0;
  aborted_ = false;
  if (config_.apiKey[0] != '\0') {
    std::snprintf(auth_, sizeof(auth_), "Bearer %s", config_.apiKey);
  } else {
    auth_[0] = '\0';  // a public library needs no key
  }

  report(Stage::Connecting, 0, 0, "");
  if (aborted_) {
    outcome.status = Status::Aborted;
    outcome.stats = stats_;
    return outcome;
  }
  if (!ensureConnected(outcome) || !fetchCollections(outcome) || !fetchItems(request, outcome)) {
    outcome.stats = stats_;
    outcome.libraryVersion = libraryVersion_;
    return outcome;
  }
  // Only a completed pass may move the incremental window; until then the next
  // run resumes the listing where this one stopped.
  if (!outcome.coveredAll) outcome.libraryVersion = request.sinceVersion;

  report(Stage::Prune, 0, 0, "");
  // Pruning runs here for collections that received articles; each folder is
  // trimmed to keepPerCollection so a long library cannot fill the card.
  for (size_t i = 0; i < collectionCount_; ++i) {
    char directory[256];
    char name[128];
    freshrss::copyBounded(name, sizeof(name), collections_[i].name);
    if (!store_.ensureFeedDirectory(config_.folder, name, directory, sizeof(directory))) continue;
    uint16_t removed = 0;
    if (store_.pruneFeed(directory, config_.keepPerCollection, removed)) stats_.pruned += removed;
  }
  {
    char directory[256];
    if (store_.ensureFeedDirectory(config_.folder, kUnfiledFolder, directory, sizeof(directory))) {
      uint16_t removed = 0;
      if (store_.pruneFeed(directory, config_.keepPerCollection, removed)) stats_.pruned += removed;
    }
  }

  report(Stage::Done, 0, 0, "");
  outcome.status = Status::Ok;
  outcome.stats = stats_;
  outcome.libraryVersion = libraryVersion_;
  freshrss::copyBounded(outcome.message, sizeof(outcome.message), "synced");
  return outcome;
}

}  // namespace zotero
