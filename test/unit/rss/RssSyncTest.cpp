// End-to-end sync against a scripted FreshRSS server: login, subscriptions,
// per-feed item downloads, mark-as-read upload, pruning, and the failure paths.

#include <map>
#include <string>
#include <vector>

#include "../../common/freshrss_test_fakes.h"
#include "../../common/test_utils.h"

using freshrss::Config;
using freshrss::Endpoint;
using freshrss::SyncHooks;
using freshrss::SyncOutcome;
using freshrss::SyncRequest;
using freshrss::SyncSession;
using freshrss::SyncStage;
using freshrss::SyncStatus;

namespace {

struct HookLog {
  std::vector<SyncStage> stages;
  std::vector<std::string> stored;
  int abortAtFeed = -1;
  bool failStored = false;
};

HookLog g_hooks;

bool progressHook(void*, const freshrss::SyncProgress& progress) {
  g_hooks.stages.push_back(progress.stage);
  if (g_hooks.abortAtFeed >= 0 && progress.stage == SyncStage::Articles &&
      static_cast<int>(progress.feedIndex) >= g_hooks.abortAtFeed)
    return false;
  return true;
}

bool storedHook(void*, const freshrss::StoredArticle& article) {
  g_hooks.stored.push_back(std::string(article.feedTitle) + "/" + article.fileName);
  return !g_hooks.failStored;
}

Config makeConfig() {
  Config config;
  std::snprintf(config.server, sizeof(config.server), "https://freshrss.example.net/api/greader.php");
  std::snprintf(config.user, sizeof(config.user), "alice");
  std::snprintf(config.apiPassword, sizeof(config.apiPassword), "Abcdef123456");
  std::snprintf(config.folder, sizeof(config.folder), "/RSS");
  config.articlesPerFeed = 5;
  config.keepPerFeed = 2;
  // Favorites are off for the general scripts so each one stays focused; the
  // favorites tests turn them on.
  config.favoritesLimit = 0;
  return config;
}

std::string itemContent(const std::string& id, const std::string& title, const std::string& html,
                        const std::string& extra = "") {
  return freshrss_test::itemsBody(id, title, html, extra);
}

std::string expectedName(const std::string& id, const std::string& title, uint32_t published) {
  char name[128];
  freshrss::buildArticleFileName(name, sizeof(name), title.c_str(), published, published,
                                 freshrss::hashItemId(id.c_str()));
  return name;
}

// Feed folders follow the library's own naming rules, so ask it rather than
// hardcoding a spelling.
std::string feedDirectory(const std::string& feedTitle) {
  char directory[128];
  freshrss::buildFeedDirectory(directory, sizeof(directory), "/RSS", feedTitle.c_str());
  return directory;
}

struct Fixture {
  freshrss_test::ScriptedTransport transport;
  freshrss_test::MemoryArticleStore store;
  Config config = makeConfig();
  Endpoint endpoint;
  SyncHooks hooks;

  Fixture() {
    g_hooks = HookLog{};
    hooks.ctx = nullptr;
    hooks.progress = progressHook;
    hooks.stored = storedHook;
    char error[96] = {};
    if (!freshrss::parseServerUrl(config.server, endpoint, error, sizeof(error))) std::abort();
  }

  SyncOutcome run(SyncRequest request = {}) {
    SyncSession session(transport, store, config, endpoint);
    session.setHooks(hooks);
    return session.run(request);
  }
};

// The three requests of a minimal sync: login, feed list, one feed's items.
void queueHappyPath(Fixture& fixture, const std::string& itemId = "tag:google.com,2005:reader/item/1a2b",
                    const std::string& title = "Hello World", const std::string& html = "<p>Body text</p>") {
  fixture.transport.queue(freshrss_test::loginResponse());
  fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
  fixture.transport.queue(freshrss_test::httpResponse(200, itemContent(itemId, title, html)));
  fixture.transport.queue(freshrss_test::httpResponse(200, itemContent(itemId, title, html)));
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("FreshRss sync");

  // --- happy path -----------------------------------------------------------
  {
    Fixture fixture;
    queueHappyPath(fixture);
    const SyncOutcome outcome = fixture.run();

    runner.expectTrue(outcome.ok(), "sync: completes");
    runner.expectEq<uint16_t>(2, outcome.stats.feedsChecked, "sync: both feeds checked");
    runner.expectEq<uint16_t>(2, outcome.stats.articlesStored, "sync: one article per feed stored");
    runner.expectEq<uint16_t>(0, outcome.stats.articlesFailed, "sync: no failures");
    runner.expectEq<size_t>(2, fixture.store.files.size(), "sync: two files on the card");
    runner.expectEq<size_t>(2, fixture.store.pruneCalls, "sync: prune ran per feed");
    runner.expectEq<size_t>(1, fixture.transport.connectLog.size(), "sync: one connection for the whole run");
    runner.expectEqual("freshrss.example.net:443:tls:verify", fixture.transport.connectLog.front(),
                       "sync: TLS with verification by default");

    const std::string name = expectedName("tag:google.com,2005:reader/item/1a2b", "Hello World", 1700000000u);
    const std::string path = feedDirectory("Ars Technica") + "/" + name;
    runner.expectEq<size_t>(1, fixture.store.files.count(path), "sync: article stored under the feed folder");
    if (fixture.store.files.count(path)) {
      const std::string& document = fixture.store.files.at(path);
      runner.expectTrue(document.find("<h1>Hello World</h1>") != std::string::npos, "sync: title in the document");
      runner.expectTrue(document.find("<p>Body text</p>") != std::string::npos, "sync: body in the document");
      runner.expectTrue(document.find("rss-item") != std::string::npos, "sync: item id recorded for mark-read");
    }
    runner.expectTrue(
        fixture.transport.requests.find("POST /api/greader.php/accounts/ClientLogin HTTP/1.1") != std::string::npos,
        "sync: login request sent");
    runner.expectTrue(fixture.transport.requests.find("Email=alice&Passwd=Abcdef123456") != std::string::npos,
                      "sync: login body form-encoded");
    runner.expectTrue(fixture.transport.requests.find("Authorization: GoogleLogin auth=alice/") != std::string::npos,
                      "sync: authorization header sent");
    runner.expectTrue(fixture.transport.requests.find("/reader/api/0/stream/contents/feed/12?") != std::string::npos,
                      "sync: feed stream requested");
    runner.expectTrue(fixture.transport.requests.find("xt=user%2F-%2Fstate%2Fcom.google%2Fread") != std::string::npos,
                      "sync: read items excluded");
    runner.expectTrue(fixture.transport.requests.find("n=5") != std::string::npos, "sync: article limit sent");
    runner.expectTrue(fixture.transport.requests.find("Accept-Encoding: identity") != std::string::npos,
                      "sync: compression not requested");
    runner.expectEq<size_t>(2, g_hooks.stored.size(), "hooks: stored callback fired per article");
  }

  // --- chunked responses and byte-wise reads --------------------------------
  {
    Fixture fixture;
    fixture.transport.chunkSize = 1;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::chunkedResponse(freshrss_test::subscriptionsBody(), 5));
    fixture.transport.queue(freshrss_test::chunkedResponse(
        itemContent("tag:google.com,2005:reader/item/1a2b", "Chunked", "<p>Chunked body</p>"), 3));
    fixture.transport.queue(freshrss_test::chunkedResponse(
        itemContent("tag:google.com,2005:reader/item/1a2b", "Chunked", "<p>Chunked body</p>"), 3));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "chunked: sync completes");
    runner.expectEq<uint16_t>(2, outcome.stats.articlesStored, "chunked: articles stored");
    runner.expectEq<size_t>(2, fixture.store.files.size(), "chunked: files written");
  }

  // --- dedupe ---------------------------------------------------------------
  {
    Fixture fixture;
    const std::string name = expectedName("tag:google.com,2005:reader/item/1a2b", "Hello World", 1700000000u);
    fixture.store.files[feedDirectory("Ars Technica") + "/" + name] = "already here";
    fixture.store.files[feedDirectory("Hacker News") + "/" + name] = "already here";
    queueHappyPath(fixture);
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "dedupe: sync completes");
    runner.expectEq<uint16_t>(2, outcome.stats.articlesSkipped, "dedupe: both items skipped");
    runner.expectEq<uint16_t>(0, outcome.stats.articlesStored, "dedupe: nothing rewritten");
    runner.expectEq<size_t>(0, fixture.store.created.size(), "dedupe: no file opened");
    runner.expectEqual("already here", fixture.store.files[feedDirectory("Ars Technica") + "/" + name],
                       "dedupe: existing kept");
  }

  // --- feed filter ----------------------------------------------------------
  {
    Fixture fixture;
    std::snprintf(fixture.config.feedFilter, sizeof(fixture.config.feedFilter), "feed/13");
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(
        200, itemContent("tag:google.com,2005:reader/item/9z8y", "Filtered", "<p>Only this one</p>")));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "filter: sync completes");
    runner.expectEq<uint16_t>(1, outcome.stats.feedsChecked, "filter: one feed checked");
    runner.expectTrue(fixture.transport.requests.find("stream/contents/feed/12") == std::string::npos,
                      "filter: excluded feed not requested");
    runner.expectTrue(fixture.transport.requests.find("stream/contents/feed/13") != std::string::npos,
                      "filter: included feed requested");
  }

  // --- recent-articles mode -------------------------------------------------
  {
    Fixture fixture;
    fixture.config.includeRead = true;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "Read already", "<p>b</p>")));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/2", "Read already", "<p>b</p>")));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "includeRead: sync completes");
    runner.expectTrue(fixture.transport.requests.find("xt=") == std::string::npos,
                      "includeRead: read state is not filtered");
    runner.expectEq<uint16_t>(2, outcome.stats.articlesStored, "includeRead: already-read items stored");
  }
  {
    Fixture fixture;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "T", "<p>b</p>")));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/2", "T", "<p>b</p>")));
    fixture.run();
    runner.expectTrue(fixture.transport.requests.find("xt=") != std::string::npos,
                      "default: read items are still excluded");
  }

  // --- incremental sync -----------------------------------------------------
  {
    Fixture fixture;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "New", "<p>new</p>")));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/2", "New", "<p>new</p>")));
    SyncRequest request;
    request.nowEpoch = 1794746040u;
    request.sinceEpoch = 1700000000u;
    const SyncOutcome outcome = fixture.run(request);
    runner.expectTrue(outcome.ok(), "incremental: sync completes");
    runner.expectTrue(fixture.transport.requests.find("ot=1700000000") != std::string::npos,
                      "incremental: lower time bound sent");
    runner.expectTrue(fixture.transport.requests.find("ck=1794746040") != std::string::npos,
                      "incremental: cache key sent");
  }

  // --- mark as read ---------------------------------------------------------
  {
    Fixture fixture;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(
        200, itemContent("tag:google.com,2005:reader/item/1a2b", "Hello World", "<p>Body</p>")));
    fixture.transport.queue(freshrss_test::httpResponse(
        200, itemContent("tag:google.com,2005:reader/item/1a2b", "Hello World", "<p>Body</p>")));
    fixture.transport.queue(freshrss_test::httpResponse(200, "8e6845e089457af25303abc6f53356eb60bdb5f8ZZZZZZZZZ\n"));
    fixture.transport.queue(freshrss_test::httpResponse(200, "OK"));

    const char* ids[] = {"tag:google.com,2005:reader/item/1a2b", "tag:google.com,2005:reader/item/3c4d"};
    SyncRequest request;
    request.nowEpoch = 1794746040u;
    request.markReadIds = ids;
    request.markReadCount = 2;
    const SyncOutcome outcome = fixture.run(request);
    runner.expectTrue(outcome.ok(), "mark-read: sync completes");
    runner.expectEq<uint16_t>(2, outcome.stats.markedRead, "mark-read: both ids uploaded");
    runner.expectTrue(
        fixture.transport.requests.find("POST /api/greader.php/reader/api/0/edit-tag HTTP/1.1") != std::string::npos,
        "mark-read: edit-tag posted");
    runner.expectTrue(
        fixture.transport.requests.find("T=8e6845e089457af25303abc6f53356eb60bdb5f8ZZZZZZZZZ") != std::string::npos,
        "mark-read: POST token sent");
    runner.expectTrue(fixture.transport.requests.find("a=user%2F-%2Fstate%2Fcom.google%2Fread") != std::string::npos,
                      "mark-read: read tag added");
    runner.expectTrue(
        fixture.transport.requests.find("i=tag%3Agoogle.com%2C2005%3Areader%2Fitem%2F1a2b") != std::string::npos,
        "mark-read: item id encoded");
  }
  {
    // A failing mark-read must not lose the downloaded articles.
    Fixture fixture;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "T", "<p>b</p>")));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "T", "<p>b</p>")));
    fixture.transport.queue(freshrss_test::httpResponse(200, "token123\n"));
    fixture.transport.queue(freshrss_test::httpResponse(500, "boom"));
    const char* ids[] = {"tag:x/1"};
    SyncRequest request;
    request.nowEpoch = 1794746040u;
    request.markReadIds = ids;
    request.markReadCount = 1;
    const SyncOutcome outcome = fixture.run(request);
    runner.expectTrue(outcome.ok(), "mark-read failure: sync still succeeds");
    runner.expectEq<uint16_t>(2, outcome.stats.articlesStored, "mark-read failure: articles kept");
    runner.expectEq<uint16_t>(0, outcome.stats.markedRead, "mark-read failure: nothing reported as uploaded");
    runner.expectTrue(std::string(outcome.message).find("mark-read") != std::string::npos,
                      "mark-read failure: message mentions it");
  }

  // --- failure paths --------------------------------------------------------
  {
    Fixture fixture;
    fixture.transport.queue(freshrss_test::httpResponse(401, "Unauthorized"));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.status == SyncStatus::LoginFailed, "failure: bad password detected");
    runner.expectEq(401, outcome.httpStatus, "failure: status recorded");
    runner.expectEqual("API password rejected", std::string(outcome.message), "failure: message is actionable");
  }
  {
    Fixture fixture;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, "{\"subscriptions\":[{\"id\":\"feed/1\","));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.status == SyncStatus::BadResponse, "failure: truncated JSON detected");
  }
  {
    Fixture fixture;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, "{}"));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "failure: empty feed list is not an error");
    runner.expectEq<uint16_t>(0, outcome.stats.feedsChecked, "failure: no feeds checked");
  }
  {
    Fixture fixture;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    // Items without a body are counted, not stored.
    fixture.transport.queue(freshrss_test::httpResponse(
        200, "{\"items\":[{\"id\":\"tag:x/1\",\"title\":\"No body\",\"published\":1700000000}]}"));
    fixture.transport.queue(freshrss_test::httpResponse(
        200, "{\"items\":[{\"id\":\"tag:x/1\",\"title\":\"No body\",\"published\":1700000000}]}"));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "failure: bodyless items are tolerated");
    runner.expectEq<uint16_t>(0, outcome.stats.articlesStored, "failure: nothing stored");
    // A headline with no body is nothing to store, not a failure.
    runner.expectEq<uint16_t>(0, outcome.stats.articlesFailed, "failure: bodyless items are not failures");
    runner.expectEq<uint16_t>(2, outcome.stats.articlesSkipped, "failure: bodyless items counted as skipped");
    runner.expectEq<size_t>(0, fixture.store.created.size(), "failure: no empty file created");
  }
  {
    Fixture fixture;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "T", "<p>b</p>")));
    fixture.store.availableBytes = 1000;  // below the reserve
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.status == SyncStatus::OutOfSpace, "failure: full card stops the sync");
    runner.expectEq<uint16_t>(0, outcome.stats.articlesStored, "failure: nothing written on a full card");
  }
  {
    Fixture fixture;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "T", "<p>b</p>")));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "T", "<p>b</p>")));
    fixture.store.failCommit = true;
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "failure: commit failure is per-article");
    runner.expectEq<uint16_t>(0, outcome.stats.articlesStored, "failure: failed article not counted as stored");
    runner.expectEq<uint16_t>(2, outcome.stats.articlesFailed, "failure: failed articles counted");
    runner.expectEq<size_t>(2, fixture.store.discarded.size(), "failure: partial files discarded");
  }
  {
    Fixture fixture;
    queueHappyPath(fixture);
    g_hooks.abortAtFeed = 0;
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.status == SyncStatus::Aborted, "abort: progress hook stops the sync");
    g_hooks.abortAtFeed = -1;
  }
  {
    Fixture fixture;
    queueHappyPath(fixture);
    g_hooks.failStored = true;
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.status == SyncStatus::Aborted, "abort: stored hook stops the sync");
    g_hooks.failStored = false;
  }
  {
    Fixture fixture;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(404, "not found"));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "T", "<p>b</p>")));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "failure: a removed feed does not abort the sync");
    runner.expectEq<uint16_t>(1, outcome.stats.feedsChecked, "failure: only the live feed counted");
    runner.expectEq<uint16_t>(1, outcome.stats.articlesStored, "failure: the live feed still stored its article");
  }
  {
    Fixture fixture;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "T", "<p>b</p>")));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "T", "<p>b</p>")));
    fixture.transport.closeAtEnd = true;
    fixture.transport.failNextRequest = true;  // the server dropped the connection
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "reconnect: sync completes after a dropped connection");
    runner.expectTrue(fixture.transport.connectLog.size() >= 2, "reconnect: connection re-established");
  }

  // --- category selection ---------------------------------------------------
  {
    // Only the feed in the chosen category is requested; the other one is
    // skipped entirely, so a large subscription list costs nothing extra.
    Fixture fixture;
    std::snprintf(fixture.config.categoryFilter, sizeof(fixture.config.categoryFilter), "Tech");
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:c/1", "Kept", "<p>b</p>")));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "categories: sync completes");
    runner.expectEq<uint16_t>(1, outcome.stats.feedsChecked, "categories: only the chosen category synced");
    runner.expectTrue(fixture.transport.requests.find("stream/contents/feed/12") != std::string::npos,
                      "categories: labelled feed requested");
    runner.expectTrue(fixture.transport.requests.find("stream/contents/feed/13") == std::string::npos,
                      "categories: unlabelled feed skipped");
  }

  // --- favorites ------------------------------------------------------------
  {
    Fixture fixture;
    fixture.config.favoritesLimit = 100;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:f/1", "Feed item", "<p>a</p>")));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:f/2", "Feed item", "<p>b</p>")));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:s/1", "Starred item", "<p>star</p>")));
    const SyncOutcome outcome = fixture.run();

    runner.expectTrue(outcome.ok(), "favorites: sync completes");
    const size_t at = fixture.transport.requests.find("state/com.google/starred");
    runner.expectTrue(at != std::string::npos, "favorites: starred stream requested");
    if (at != std::string::npos) {
      const size_t end = fixture.transport.requests.find("\r\n", at);
      const std::string line = fixture.transport.requests.substr(at, end - at);
      runner.expectTrue(line.find("xt=") == std::string::npos, "favorites: read state not filtered");
      runner.expectTrue(line.find("ot=") == std::string::npos, "favorites: sync window not applied");
      runner.expectTrue(line.find("n=100") != std::string::npos, "favorites: configured limit sent");
    }

    const std::string directory = feedDirectory("Favorites");
    const std::string name = expectedName("tag:s/1", "Starred item", 1700000000u);
    runner.expectEq<size_t>(1, fixture.store.files.count(directory + "/" + name),
                            "favorites: stored in their own folder");
    if (fixture.store.files.count(directory + "/" + name)) {
      const std::string& document = fixture.store.files.at(directory + "/" + name);
      runner.expectTrue(document.find("Starred item - Favorites") != std::string::npos,
                        "favorites: byline names the folder");
      runner.expectTrue(document.find("<p>star</p>") != std::string::npos, "favorites: body stored");
    }
    runner.expectEq<size_t>(3, fixture.store.files.size(), "favorites: feeds plus favorite stored");
  }
  {
    // A limit of zero turns the folder off entirely.
    Fixture fixture;
    runner.expectEq<uint16_t>(0, fixture.config.favoritesLimit, "favorites: off in the general fixture");
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    for (int i = 0; i < 2; ++i)
      fixture.transport.queue(
          freshrss_test::httpResponse(200, itemContent("tag:d/" + std::to_string(i), "T", "<p>b</p>")));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "favorites: disabled sync completes");
    runner.expectTrue(fixture.transport.requests.find("starred") == std::string::npos,
                      "favorites: disabled means no request");
    runner.expectEq<size_t>(2, fixture.store.files.size(), "favorites: nothing extra stored");
  }

  // --- round robin over the subscription list -------------------------------
  {
    // A budget smaller than the list stops part way: the caller must not move
    // its incremental window yet.
    Fixture fixture;
    fixture.config.maxFeeds = 2;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBodyN(3)));
    for (int i = 0; i < 2; ++i)
      fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/" + std::to_string(i), "T", "<p>b</p>")));
    SyncRequest request;
    request.nowEpoch = 1794746040u;
    request.feedOffset = 1;
    const SyncOutcome outcome = fixture.run(request);
    runner.expectTrue(outcome.ok(), "rotation: partial pass completes");
    runner.expectEq<uint16_t>(3, outcome.stats.feedsSubscribed, "rotation: subscription count reported");
    runner.expectEq<uint16_t>(2, outcome.stats.feedsChecked, "rotation: budget respected");
    runner.expectFalse(outcome.coveredAllFeeds, "rotation: partial pass does not cover all feeds");
    runner.expectEq<uint32_t>(0, outcome.nextFeedOffset, "rotation: cursor continues after the slice");
    runner.expectTrue(fixture.transport.requests.find("stream/contents/feed/13") != std::string::npos,
                      "rotation: starts at the requested feed");
    runner.expectTrue(fixture.transport.requests.find("stream/contents/feed/12") == std::string::npos,
                      "rotation: feeds before the cursor wait for the next pass");
  }
  {
    // Starting near the end wraps around, which completes a pass.
    Fixture fixture;
    fixture.config.maxFeeds = 2;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBodyN(3)));
    for (int i = 0; i < 2; ++i)
      fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:y/" + std::to_string(i), "T", "<p>b</p>")));
    SyncRequest request;
    request.nowEpoch = 1794746040u;
    request.feedOffset = 2;
    const SyncOutcome outcome = fixture.run(request);
    runner.expectTrue(outcome.ok(), "rotation: wrapping pass completes");
    runner.expectTrue(outcome.coveredAllFeeds, "rotation: wrapping pass covers every feed");
    runner.expectEq<uint32_t>(1, outcome.nextFeedOffset, "rotation: cursor wraps to the remainder");
    runner.expectTrue(fixture.transport.requests.find("stream/contents/feed/14") != std::string::npos,
                      "rotation: visits the last feed");
    runner.expectTrue(fixture.transport.requests.find("stream/contents/feed/12") != std::string::npos,
                      "rotation: wraps to the first feed");
  }
  {
    // A budget larger than the list covers it in one run.
    Fixture fixture;
    fixture.config.maxFeeds = 16;
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBodyN(3)));
    for (int i = 0; i < 3; ++i)
      fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:z/" + std::to_string(i), "T", "<p>b</p>")));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "rotation: full pass completes");
    runner.expectEq<uint16_t>(3, outcome.stats.feedsChecked, "rotation: every feed visited");
    runner.expectTrue(outcome.coveredAllFeeds, "rotation: full pass covers every feed");
    runner.expectEq<uint32_t>(0, outcome.nextFeedOffset, "rotation: cursor resets after a full pass");
  }
  {
    // The filter narrows the pass: skipped feeds consume slots without a request.
    Fixture fixture;
    fixture.config.maxFeeds = 2;
    std::snprintf(fixture.config.feedFilter, sizeof(fixture.config.feedFilter), "feed/13");
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBodyN(3)));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:f/1", "T", "<p>b</p>")));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "rotation: filtered pass completes");
    runner.expectEq<uint16_t>(1, outcome.stats.feedsChecked, "rotation: only the allowed feed counted");
    runner.expectTrue(outcome.coveredAllFeeds, "rotation: filtered pass still covers the list");
    runner.expectTrue(fixture.transport.requests.find("stream/contents/feed/12") == std::string::npos,
                      "rotation: filtered-out feed not requested");
  }

  // --- pruning --------------------------------------------------------------
  {
    Fixture fixture;
    fixture.config.keepPerFeed = 2;
    const std::string directory = feedDirectory("Ars Technica");
    for (int day = 1; day <= 5; ++day) {
      char name[64];
      std::snprintf(name, sizeof(name), "2026-11-%02d-old-%08x.html", day, day);
      fixture.store.files[directory + "/" + name] = "old";
    }
    fixture.transport.queue(freshrss_test::loginResponse());
    fixture.transport.queue(freshrss_test::httpResponse(200, freshrss_test::subscriptionsBody()));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "New", "<p>b</p>")));
    fixture.transport.queue(freshrss_test::httpResponse(200, itemContent("tag:x/1", "New", "<p>b</p>")));
    const SyncOutcome outcome = fixture.run();
    runner.expectTrue(outcome.ok(), "prune: sync completes");
    runner.expectEq<uint16_t>(4, outcome.stats.pruned, "prune: oldest articles removed");
    runner.expectEq<size_t>(3, fixture.store.files.size(), "prune: newest kept plus the new article");
    runner.expectEq<size_t>(1, fixture.store.files.count(directory + "/2026-11-05-old-00000005.html"),
                            "prune: newest old article kept");
    runner.expectEq<size_t>(0, fixture.store.files.count(directory + "/2026-11-01-old-00000001.html"),
                            "prune: oldest article removed");
  }

  return runner.allPassed() ? 0 : 1;
}
