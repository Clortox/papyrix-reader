// rss-sync: populate a PapyriX card (or any directory) from a FreshRSS server.
//
// It runs the same sync code as the device, so the files it writes are the ones
// the reader expects and a later on-device sync skips everything by file name.

#include <RssState.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

#include "SyncCli.h"

using papyrix::rss_app::RssState;

namespace {

void usage() {
  std::printf(
      "rss-sync - download FreshRSS articles onto a PapyriX card\n"
      "\n"
      "Usage: rss-sync --root <dir> [options]\n"
      "\n"
      "  --root <dir>     card or directory to write into (required)\n"
      "  --config <file>  defaults to <root>/rss.json\n"
      "  --limit <n>      articles per feed for this run\n"
      "  --dry-run        report what would be written, change nothing\n"
      "  --verbose        list every article as it is stored\n"
      "  --help           this text\n"
      "\n"
      "Auth, server and limits come from the same rss.json the device reads.\n");
}

struct Printer {
  bool verbose = false;
  const char* lastName = nullptr;
};

bool progressHook(void* context, const freshrss::SyncProgress& progress) {
  auto* printer = static_cast<Printer*>(context);
  const char* label = nullptr;
  switch (progress.stage) {
    case freshrss::SyncStage::Connecting:
      label = "connecting";
      break;
    case freshrss::SyncStage::Login:
      label = "signing in";
      break;
    case freshrss::SyncStage::Feeds:
      label = "reading feed list";
      break;
    case freshrss::SyncStage::Articles:
      label = "downloading articles";
      break;
    case freshrss::SyncStage::Favorites:
      label = "downloading favorites";
      break;
    case freshrss::SyncStage::MarkRead:
      label = "marking read";
      break;
    case freshrss::SyncStage::Prune:
      label = "pruning";
      break;
    case freshrss::SyncStage::Done:
      label = "done";
      break;
  }
  if (progress.stage == freshrss::SyncStage::Articles) {
    std::printf("\r  %s (%zu/%zu) %-40.40s", label, progress.feedIndex + 1, progress.feedCount,
                progress.feedTitle ? progress.feedTitle : "");
    std::fflush(stdout);
    return true;
  }
  if (printer->lastName != label) {
    std::printf("\r  %-60s\n", label);
    printer->lastName = label;
  }
  return true;
}

bool storedHook(void* context, const freshrss::StoredArticle& article) {
  auto* printer = static_cast<Printer*>(context);
  if (printer->verbose) {
    std::printf("\r    + %s/%s (%u bytes)%*s\n", article.feedTitle, article.fileName,
                static_cast<unsigned>(article.bytes), 20, "");
  }
  return true;
}

std::string configPath(const std::string& root, const std::string& explicitPath) {
  if (!explicitPath.empty()) return explicitPath;
  const std::string visible = syncli::joinPath(root, "rss.json");
  if (std::filesystem::exists(visible)) return visible;
  return syncli::joinPath(root, ".papyrix/apps/rss.json");
}

}  // namespace

int main(int argc, char** argv) {
  const syncli::Args args = syncli::parseArgs(argc, argv);
  if (syncli::hasOption(args, "--help") || argc == 1) {
    usage();
    return argc == 1 ? 2 : 0;
  }

  const bool dryRun = syncli::hasOption(args, "--dry-run");
  const bool verbose = syncli::hasOption(args, "--verbose");
  const std::string root = syncli::optionValue(args, "--root");
  if (root.empty()) {
    std::fprintf(stderr, "error: --root is required (the mounted card, e.g. /run/media/you/CARD)\n");
    return 2;
  }

  const std::string path = configPath(root, syncli::optionValue(args, "--config"));
  std::string text;
  if (!syncli::readFile(path, text)) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return 2;
  }

  freshrss::Config config;
  char error[128] = {};
  if (!freshrss::parseConfigJson(text.data(), text.size(), config, error, sizeof(error))) {
    std::fprintf(stderr, "error: %s: %s\n", path.c_str(), error);
    return 2;
  }
  if (!freshrss::validateConfig(config, error, sizeof(error))) {
    std::fprintf(stderr, "error: %s: %s\n", path.c_str(), error);
    return 2;
  }
  const std::string limitText = syncli::optionValue(args, "--limit");
  if (!limitText.empty()) {
    const long limit = std::strtol(limitText.c_str(), nullptr, 10);
    if (limit > 0 && limit <= freshrss::kMaxArticlesPerFeed) config.articlesPerFeed = static_cast<uint16_t>(limit);
  }

  freshrss::Endpoint endpoint;
  if (!freshrss::parseServerUrl(config.server, endpoint, error, sizeof(error))) {
    std::fprintf(stderr, "error: %s\n", error);
    return 2;
  }

  std::printf("rss-sync: %s@%s%s -> %s%s\n", config.user, endpoint.host, endpoint.basePath, root.c_str(),
              config.folder);
  std::printf("  %u feeds max, %u articles per feed, keeping %u\n", config.maxFeeds, config.articlesPerFeed,
              config.keepPerFeed);
  if (dryRun) std::printf("  dry run: nothing will be written\n");

  // The CLI shares the device's state file, so a sync here also counts as one
  // there and read markers queued on the device are uploaded.
  const std::string statePath = syncli::joinPath(root, ".papyrix/apps/rss.txt");
  std::string stateText;
  RssState state;
  if (!syncli::loadState(statePath, stateText)) {
    std::fprintf(stderr, "error: cannot read %s\n", statePath.c_str());
    return 2;
  }
  papyrix::rss_app::decodeState(stateText.data(), stateText.size(), state);

  const char* ids[papyrix::rss_app::kMaxPendingItems];
  for (size_t i = 0; i < state.pendingCount; ++i) ids[i] = state.pending[i];

  syncli::HostTransport transport;
  syncli::FsArticleStore store(root);
  store.setDryRun(dryRun);

  freshrss::SyncSession session(transport, store, config, endpoint);
  Printer printer;
  printer.verbose = verbose;
  freshrss::SyncHooks hooks;
  hooks.ctx = &printer;
  hooks.progress = progressHook;
  hooks.stored = storedHook;
  session.setHooks(hooks);

  freshrss::SyncRequest request;
  request.nowEpoch = syncli::nowEpoch();
  request.sinceEpoch = state.sinceEpoch;
  request.feedOffset = state.feedCursor;
  request.markReadIds = state.pendingCount > 0 ? ids : nullptr;
  request.markReadCount = state.pendingCount;

  const freshrss::SyncOutcome outcome = session.run(request);
  std::printf("\r  %-60s\n", outcome.ok() ? "sync finished" : "sync failed");

  if (outcome.ok() && request.markReadCount > 0) {
    for (size_t i = 0; i < request.markReadCount; ++i) papyrix::rss_app::removePendingItem(state, ids[i]);
  }

  std::printf("  feeds: %u of %u%s\n", outcome.stats.feedsChecked, outcome.stats.feedsSubscribed,
              outcome.coveredAllFeeds ? " (full pass)" : "");
  std::printf("  articles: %u new, %u already present, %u failed\n", outcome.stats.articlesStored,
              outcome.stats.articlesSkipped, outcome.stats.articlesFailed);
  if (outcome.stats.markedRead) std::printf("  read markers uploaded: %u\n", outcome.stats.markedRead);
  if (outcome.stats.pruned) std::printf("  pruned: %u\n", outcome.stats.pruned);
  if (!outcome.ok()) {
    const char* described = freshrss::describeSyncStatus(outcome.status);
    std::fprintf(stderr, "error: %s%s%s\n", described, outcome.message[0] ? ": " : "", outcome.message);
    return 1;
  }

  // Persist the same state the device keeps, so the next run continues here.
  if (!dryRun) {
    state.lastSyncEpoch = request.nowEpoch;
    if (outcome.coveredAllFeeds) state.sinceEpoch = request.nowEpoch;
    state.feedCursor = outcome.nextFeedOffset;
    char encoded[papyrix::rss_app::kMaxStateBytes];
    const size_t length = papyrix::rss_app::encodeState(state, encoded, sizeof(encoded));
    std::string writeError;
    if (length == 0 || !syncli::makeDirectories(syncli::joinPath(root, ".papyrix/apps"), writeError) ||
        !syncli::writeFile(statePath, std::string(encoded, length), writeError)) {
      std::fprintf(stderr, "warning: could not save state: %s\n", writeError.c_str());
      return 1;
    }
  }
  return 0;
}
