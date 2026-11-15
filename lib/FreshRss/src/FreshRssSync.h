#pragma once

#include <cstddef>
#include <cstdint>

#include "FreshRssArticle.h"
#include "FreshRssConfig.h"
#include "FreshRssHttp.h"
#include "FreshRssJson.h"
#include "FreshRssText.h"

// FreshRSS Google Reader API client: login, subscriptions, per-feed item
// downloads with streaming HTML sanitization, upstream mark-as-read, and
// retention pruning.
//
// The sync loop talks to the network through Transport and to the SD card
// through ArticleStore, so the whole flow runs against scripted fakes on the
// host. Nothing here allocates on the heap.

namespace freshrss {

// One subscription from subscription/list.
struct Feed {
  char id[64] = {};  // "feed/12"
  char title[64] = {};
  char categories[96] = {};  // FreshRSS category labels, joined with '|'
};

struct FeedList {
  // Holds every feed maxFeeds can visit; the list is a session member, not a
  // stack object, and this used to truncate a larger subscription list.
  static constexpr size_t kMaxFeeds = 64;
  Feed feeds[kMaxFeeds];
  size_t count = 0;
};

// Metadata of one item from stream/contents.
struct ItemInfo {
  char id[96] = {};
  char title[160] = {};
  char author[64] = {};
  char link[256] = {};
  uint32_t publishedEpoch = 0;
};

// Turns subscription/list JSON into a FeedList.
class SubscriptionCollector {
 public:
  explicit SubscriptionCollector(FeedList& feeds) : feeds_(feeds) {}

  void feed(const char* data, size_t length);
  bool finish();

  bool failed() const { return failed_; }
  const char* error() const { return error_; }
  size_t count() const { return feeds_.count; }

 private:
  void handle(const JsonScanner::Token& token);
  void assignString();

  FeedList& feeds_;
  JsonScanner scanner_;
  int depth_ = 0;
  int subscriptionsDepth_ = -1;
  int feedDepth_ = -1;
  // Depth of the feed's `categories` array, so its labels can be captured.
  int categoriesDepth_ = -1;
  bool pendingSubscriptionsKey_ = false;
  enum class Field : uint8_t { None, Id, Title, CategoryLabel } field_ = Field::None;
  char lastFieldKey_[32] = {};
  Feed current_;
  bool failed_ = false;
  char error_[64] = {};
};

// Receives one item's streamed HTML body.
class ItemSink {
 public:
  virtual ~ItemSink() = default;
  // Called once per item, just before its body arrives. Returning false skips
  // the item: no content chunks follow and endItem reports incomplete.
  virtual bool beginItem(const ItemInfo& info) = 0;
  virtual bool contentChunk(const char* data, size_t length) = 0;
  // Always called once per item, with the complete metadata.
  virtual void endItem(const ItemInfo& info, bool complete) = 0;
};

// Streams one feed's items into an ItemSink.
class StreamCollector {
 public:
  StreamCollector(ItemSink& sink) : sink_(sink) {}

  void feed(const char* data, size_t length);
  bool finish();

  bool failed() const { return failed_; }
  const char* error() const { return error_; }
  // Continuation id for the next page, empty when the server sent none.
  const char* continuation() const { return continuation_; }
  size_t itemCount() const { return itemCount_; }

 private:
  void handle(const JsonScanner::Token& token);
  void beginItemObject();
  void endItemObject();
  void assignString();
  void assignNumber(long long value);

  ItemSink& sink_;
  JsonScanner scanner_;
  int depth_ = 0;
  int itemsDepth_ = -1;
  int itemDepth_ = -1;
  int originDepth_ = -1;
  int summaryDepth_ = -1;
  int contentDepth_ = -1;
  int canonicalDepth_ = -1;
  int alternateDepth_ = -1;
  char lastKey_[32] = {};
  bool pendingItemsKey_ = false;
  bool inContent_ = false;
  bool itemOpen_ = false;
  bool itemHadContent_ = false;
  bool itemSkipped_ = false;
  bool sawCanonical_ = false;
  size_t itemCount_ = 0;

  enum class Field : uint8_t {
    None,
    Id,
    Title,
    Author,
    Published,
    CrawlTime,
    Link,
    OriginTitle,
    Continuation
  } field_ = Field::None;
  ItemInfo current_;
  char continuation_[24] = {};
  bool failed_ = false;
  char error_[64] = {};
};

// Destination for downloaded articles. The app implements this over SdFat; the
// host tests over memory.
class ArticleFile : public ByteSink {
 public:
  ~ArticleFile() override = default;
  // Publishes the file (flush + close). Returns false when the write failed.
  virtual bool commit() = 0;
  // Closes and removes a partial file.
  virtual void discard() = 0;
};

class ArticleStore {
 public:
  virtual ~ArticleStore() = default;
  // Creates the feed's directory when missing and writes its path to `out`.
  virtual bool ensureFeedDirectory(const char* folder, const char* feedTitle, char* out, size_t capacity) = 0;
  // True when the article is already on the card: it is skipped, not rewritten.
  virtual bool articleExists(const char* directory, const char* fileName) = 0;
  virtual ArticleFile* createArticle(const char* directory, const char* fileName) = 0;
  // Deletes the oldest articles until at most `keep` remain.
  virtual bool pruneFeed(const char* directory, uint16_t keep, uint16_t& removed) = 0;
  virtual uint64_t freeBytes() = 0;
};

enum class SyncStage { Connecting, Login, Feeds, Articles, Favorites, MarkRead, Prune, Done };

// Starred items are always downloaded, into a folder of their own.
constexpr const char* kFavoritesFolderName = "Favorites";
constexpr const char* kFavoritesApiPath = "/reader/api/0/stream/contents/user/-/state/com.google/starred";

struct SyncProgress {
  SyncStage stage = SyncStage::Connecting;
  size_t feedIndex = 0;
  size_t feedCount = 0;
  const char* feedTitle = "";
  uint16_t articlesStored = 0;
  uint16_t articlesSkipped = 0;
};

struct StoredArticle {
  const char* feedTitle = "";
  const char* fileName = "";
  const char* title = "";
  uint32_t bytes = 0;
};

struct SyncHooks {
  void* ctx = nullptr;
  // Return false to abort the sync (Back button, low battery, ...).
  bool (*progress)(void* ctx, const SyncProgress& progress) = nullptr;
  bool (*stored)(void* ctx, const StoredArticle& article) = nullptr;
};

enum class SyncStatus {
  Ok,
  NoConfig,
  NetworkError,
  LoginFailed,
  HttpError,
  BadResponse,
  OutOfSpace,
  Aborted,
};

struct SyncStats {
  uint16_t feedsChecked = 0;
  // Subscriptions the server reported, before the per-run budget is applied.
  uint16_t feedsSubscribed = 0;
  uint16_t articlesStored = 0;
  uint16_t articlesSkipped = 0;
  uint16_t articlesFailed = 0;
  uint16_t pruned = 0;
  uint16_t markedRead = 0;
  uint32_t bytesStored = 0;
};

struct SyncOutcome {
  SyncStatus status = SyncStatus::Ok;
  int httpStatus = 0;
  SyncStats stats;
  char message[80] = {};
  // True when this run completed a full pass over the subscriptions. Feeds
  // rotate across runs, so the caller must keep its incremental window until a
  // pass completes; otherwise a feed visited later would never see the articles
  // published since the window moved.
  bool coveredAllFeeds = false;
  // Where the next run should start, so successive runs cover every feed.
  uint32_t nextFeedOffset = 0;
  bool ok() const { return status == SyncStatus::Ok; }
};

// One sync run's inputs.
struct SyncRequest {
  const char* const* markReadIds = nullptr;  // items opened locally since the last sync
  size_t markReadCount = 0;
  uint32_t nowEpoch = 0;
  // Only items published after this are fetched. 0 fetches the newest N items.
  uint32_t sinceEpoch = 0;
  // Subscription index this run starts at. Feeds beyond the per-run budget wait
  // for a later run, so a large subscription list is covered over several syncs.
  size_t feedOffset = 0;
  // How many subscriptions the caller saw last time. When the server now reports
  // more, those feeds have nothing on the card yet, so this run ignores the
  // incremental window for them instead of only fetching brand-new items.
  uint16_t knownFeedCount = 0;
};

// Streams one feed's items into article files. Owns the file currently being
// written, the sanitizer, and the accounting for that feed.
class ArticleDownloader : public ItemSink {
 public:
  ArticleDownloader(ArticleStore& store, const Config& config, const Feed& feed, SyncHooks hooks, SyncStats& stats)
      : store_(store), config_(config), feed_(feed), hooks_(hooks), stats_(stats) {}

  void setDirectory(const char* directory) { copyBounded(directory_, sizeof(directory_), directory); }
  void setFallbackEpoch(uint32_t epoch) { fallbackEpoch_ = epoch; }

  bool beginItem(const ItemInfo& info) override;
  bool contentChunk(const char* data, size_t length) override;
  void endItem(const ItemInfo& info, bool complete) override;

  // Set when the store or the card failed: the sync must stop, not continue.
  bool fatal() const { return fatal_; }
  bool abortRequested() const { return abort_; }

 private:
  ArticleStore& store_;
  const Config& config_;
  const Feed& feed_;
  SyncHooks hooks_;
  SyncStats& stats_;

  char directory_[160] = {};
  char fileName_[96] = {};
  char authorAtBegin_[64] = {};
  uint32_t fallbackEpoch_ = 0;
  const ItemInfo* current_ = nullptr;
  ArticleFile* file_ = nullptr;
  ArticleWriter writer_;
  bool open_ = false;
  bool skipped_ = false;
  bool fatal_ = false;
  bool abort_ = false;
};

// A single sync run over one connection. The session keeps the connection alive
// between requests; a server that closes it is reconnected transparently.
class SyncSession {
 public:
  static constexpr size_t kMaxMarkReadPerRequest = 20;

  SyncSession(Transport& transport, ArticleStore& store, const Config& config, const Endpoint& endpoint)
      : transport_(transport), store_(store), config_(config), endpoint_(endpoint), http_(transport) {}

  void setHooks(const SyncHooks& hooks) { hooks_ = hooks; }

  SyncOutcome run(const SyncRequest& request);

  const FeedList& feeds() const { return feeds_; }
  // Logs in and reads the subscription list without downloading anything: the
  // listing modes of the CLIs use it to show what could be synced.
  bool fetchFeedList(SyncOutcome& outcome);
  const Endpoint& endpoint() const { return endpoint_; }

 private:
  bool ensureConnected(SyncOutcome& outcome);
  bool login(SyncOutcome& outcome);
  bool fetchSubscriptions(SyncOutcome& outcome);
  bool fetchFeed(const Feed& feed, size_t index, size_t total, const SyncRequest& request, SyncOutcome& outcome);
  // Downloads the newest starred items, ignoring read state and the sync window.
  bool fetchFavorites(const SyncRequest& request, SyncOutcome& outcome);
  // Reads one feed's (or the favorites') response into the downloader.
  bool streamDownload(ArticleDownloader& downloader, SyncOutcome& outcome);
  bool uploadMarkRead(const char* const* ids, size_t count, SyncOutcome& outcome);
  bool fetchToken(SyncOutcome& outcome);
  bool send(const char* method, const char* apiPath, const char* query, const char* authorization, const char* body,
            size_t bodyLength, SyncOutcome& outcome);
  bool checkStatus(SyncOutcome& outcome, const char* what);
  void report(SyncStage stage, size_t feedIndex, size_t feedCount, const char* feedTitle);

  Transport& transport_;
  ArticleStore& store_;
  const Config& config_;
  Endpoint endpoint_;
  HttpSession http_;
  SyncHooks hooks_;
  FeedList feeds_;
  SyncStats stats_;
  char auth_[160] = {};  // full Authorization header value
  char token_[64] = {};  // POST token for edit-tag
  char path_[512] = {};
  char body_[1024] = {};
  char host_[160] = {};
  char query_[256] = {};
  uint32_t nowEpoch_ = 0;
  bool aborted_ = false;
};

// User-facing one-liner for a failed sync.
const char* describeSyncStatus(SyncStatus status);

}  // namespace freshrss
