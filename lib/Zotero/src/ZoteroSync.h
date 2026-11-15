#pragma once

#include <cstddef>
#include <cstdint>

// Zotero library sync: collections, top-level items, their attachments, and the
// saved web page each one carries. Articles are written through the same store
// and sanitizing writer the reader uses, so the result is a normal offline book.
//
// The transport, HTTP framing, article store and writer are shared with the
// FreshRSS client (lib/FreshRss).

#include "../../FreshRss/src/FreshRssArticle.h"  // ArticleWriter, ByteSink
#include "../../FreshRss/src/FreshRssHttp.h"     // Transport, HttpSession
#include "../../FreshRss/src/FreshRssSync.h"     // ArticleStore, ArticleFile
#include "ZoteroConfig.h"
#include "ZoteroItems.h"

namespace zotero {

enum class Stage { Connecting, Collections, Items, Content, Prune, Done };
enum class Status { Ok, NoConfig, NetworkError, AuthFailed, HttpError, BadResponse, OutOfSpace, Aborted };

struct Progress {
  Stage stage = Stage::Connecting;
  size_t itemIndex = 0;
  size_t itemCount = 0;
  const char* title = "";
  uint16_t stored = 0;
};

struct StoredArticle {
  const char* collection = "";
  const char* fileName = "";
  const char* title = "";
  uint32_t bytes = 0;
};

struct Hooks {
  void* ctx = nullptr;
  // Return false to stop the sync (a back button, a closed laptop lid).
  bool (*progress)(void* ctx, const Progress& progress) = nullptr;
  bool (*stored)(void* ctx, const StoredArticle& article) = nullptr;
};

// Persisted between runs: when the sync last finished, and the library version
// the server reported, so the next run can ask for `since=<version>`.
struct State {
  uint32_t lastSyncEpoch = 0;
  uint32_t libraryVersion = 0;
  uint32_t nextStart = 0;  // listing offset for the next run
};

size_t encodeState(const State& state, char* out, size_t capacity);
bool decodeState(const char* text, size_t length, State& out);

struct Stats {
  uint16_t itemsChecked = 0;
  uint16_t articlesStored = 0;
  uint16_t articlesSkipped = 0;
  uint16_t articlesFailed = 0;
  uint16_t snapshots = 0;  // saved web pages taken from Zotero storage
  uint16_t pages = 0;      // pages fetched from their original URL
  uint16_t pruned = 0;
  uint32_t bytesStored = 0;
};

struct Outcome {
  Status status = Status::Ok;
  // Where the next run should continue, and whether this run reached the end of
  // the listing (in which case the incremental window may move).
  uint32_t nextStart = 0;
  bool coveredAll = false;
  int httpStatus = 0;
  Stats stats;
  char message[80] = {};
  uint32_t libraryVersion = 0;
  bool ok() const { return status == Status::Ok; }
};

struct Request {
  uint32_t nowEpoch = 0;
  // Library version the client already has; 0 asks for a full listing.
  uint32_t sinceVersion = 0;
  // Offset to start the listing at. The library is backfilled one page per run,
  // so a big library needs several runs instead of one enormous request.
  uint32_t startOffset = 0;
};

const char* describeStatus(Status status);

class Session {
 public:
  static constexpr size_t kMaxNamedCollections = 48;

  Session(freshrss::Transport& transport, freshrss::ArticleStore& store, const Config& config)
      : transport_(transport), store_(store), config_(config), http_(transport) {}

  void setHooks(const Hooks& hooks) { hooks_ = hooks; }

  Outcome run(const Request& request);

 private:
  struct NamedCollection {
    char key[12] = {};
    char name[64] = {};
  };

  bool ensureConnected(Outcome& outcome);
  // Builds and sends one API request on the current connection.
  bool sendApi(const char* path, const char* query, bool withAuth, Outcome& outcome);
  // Sends a request target on whatever connection is open.
  bool sendRelative(const char* target, bool withAuth, Outcome& outcome);
  bool checkStatus(Outcome& outcome, const char* what);
  bool fetchCollections(Outcome& outcome);
  bool fetchItems(const Request& request, Outcome& outcome);
  bool fetchChildren(const ItemInfo& item, ItemList& children, Outcome& outcome);
  bool saveItem(const ItemInfo& item, const Request& request, Outcome& outcome);
  // Streams an attachment file (or the page at `url`) into the writer.
  bool streamAttachment(const char* path, freshrss::ArticleWriter& writer, Outcome& outcome);
  // Streams the current response body through the sanitizer.
  bool streamBodyInto(freshrss::ArticleWriter& writer, Outcome& outcome);
  bool streamPage(const char* url, freshrss::ArticleWriter& writer, Outcome& outcome);
  // Returns to the API host after fetching from somewhere else.
  bool ensureApiConnection(Outcome& outcome);
  // Reconnects to the host named by an absolute URL. Keeps `pathOut` for the
  // request target.
  bool openAbsoluteUrl(const char* url, char* pathOut, size_t pathCapacity, Outcome& outcome);
  const char* collectionName(const char* key) const;
  void report(Stage stage, size_t index, size_t count, const char* title);

  freshrss::Transport& transport_;
  freshrss::ArticleStore& store_;
  const Config& config_;
  freshrss::HttpSession http_;
  Hooks hooks_;
  Stats stats_;
  NamedCollection collections_[kMaxNamedCollections];
  // Held here rather than on the stack: a list is tens of kilobytes.
  ItemList items_;
  ItemList children_;
  ItemList collectionList_;
  size_t collectionCount_ = 0;
  char query_[320] = {};
  char path_[512] = {};
  char host_[160] = {};
  char auth_[96] = {};
  uint16_t port_ = 443;
  bool tls_ = true;
  bool aborted_ = false;
  uint32_t libraryVersion_ = 0;
};

}  // namespace zotero
