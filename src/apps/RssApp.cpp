#include "RssApp.h"

#include <Arduino.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>

#include "../../lib/GfxRenderer/src/GfxRenderer.h"
#include "../../lib/InputManager/include/InputManager.h"
#include "../../lib/Logging/src/Logging.h"
#include "../../lib/SDCardManager/include/SDCardManager.h"
#include "../core/BootMode.h"
#include "../core/Core.h"
#include "../network/WifiCredentialStore.h"
#include "../ui/Elements.h"
#include "../ui/views/SettingsViews.h"
#include "MiniAppInput.h"
#include "RssSdStore.h"
#include "RssState.h"
#include "RssTransport.h"
#include "ThemeManager.h"

#define TAG "RSS_APP"

extern GfxRenderer renderer;
extern InputManager inputManager;

namespace papyrix::rss_app {
namespace {

constexpr int kMaxFeedEntries = 24;
constexpr int kMaxArticleEntries = 64;
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kProgressRedrawMs = 400;
constexpr const char* kStateAppsDirectory = "/.papyrix/apps";

enum class Screen : uint8_t {
  Loading,
  NoConfig,
  Feeds,
  Articles,
  Syncing,
  Message,
};

// App menu entries (opened with Center, like the other mini-apps).
enum class MenuItem : uint8_t { SyncNow, ReloadConfig, BrowseFolder, Count };

struct State {
  Screen screen = Screen::Loading;
  bool needsRender = true;
  char message[96] = {};

  freshrss::Config config;
  freshrss::Endpoint endpoint;
  bool configOk = false;
  bool tlsInsecure = false;

  RssState persisted;
  uint32_t lastSyncEpoch = 0;
  bool syncedThisSession = false;
  freshrss::SyncStats stats;

  char feeds[kMaxFeedEntries][64] = {};
  size_t feedCount = 0;
  int feedIndex = 0;

  char articles[kMaxArticleEntries][80] = {};
  size_t articleCount = 0;
  int articleIndex = 0;
  char currentFeed[64] = {};

  MenuItem menuItem = MenuItem::SyncNow;
  // An automatic interval sync must not interrupt browsing with a result screen.
  bool userInitiatedSync = false;
  Screen screenBeforeSync = Screen::Feeds;
  // Set when a menu action replaces the menu with another screen (a sync).
  bool closeMenuAfterAction = false;
  // Subscription index the last run started at, for the menu's progress line.
  uint32_t lastFeedStart = 0;
};

State state;
Core* s_core = nullptr;
// The SD article store lives for the whole app session: the sync writes through
// it and the browser reads the same folder.
freshrss::ArticleStore& sdStore() {
  static SdArticleStore store;
  return store;
}
// Message from the transport of the most recent sync, when it has one.
char lastTransportError[80] = {};
std::unique_ptr<hal::WifiSession> wifiSession;
std::unique_ptr<WifiTransport> transport;
std::unique_ptr<freshrss::SyncSession> syncSession;

// True while the sync loop is running; Back sets it so the run aborts cleanly.
bool syncAborted = false;
uint32_t lastProgressRenderMs = 0;

uint32_t currentEpoch() {
  const std::time_t now = std::time(nullptr);
  if (now <= 0) return 0;
  std::tm utc{};
  if (gmtime_r(&now, &utc) == nullptr || utc.tm_year + 1900 < 2020) return 0;
  return static_cast<uint32_t>(now);
}

void setMessage(const char* text) {
  std::snprintf(state.message, sizeof(state.message), "%s", text ? text : "");
  state.screen = Screen::Message;
  state.needsRender = true;
}

void requestWifiPicker() {
  LOG_INF(TAG, "No usable WiFi credentials; opening the network picker");
  s_core->pendingSync = SyncMode::RssSetup;
  s_core->pendingAppId = APP_RSS;
  // The host leaves for the picker on its next update; meanwhile show something
  // usable rather than a connecting screen.
  state.screen = state.configOk ? Screen::Feeds : Screen::NoConfig;
  state.needsRender = true;
}

int visibleRows(int rowHeight) {
  if (rowHeight <= 0) return 1;
  const int available =
      renderer.getScreenHeight() - ui::SettingsListHit::LIST_START_Y - ui::SettingsListHit::LIST_BOTTOM_MARGIN;
  return std::max(1, available / rowHeight);
}

void refreshFeeds() {
  state.feedCount = listFeeds(state.feeds, kMaxFeedEntries);
  if (state.feedIndex >= static_cast<int>(state.feedCount)) state.feedIndex = 0;
  if (state.feedCount == 0) state.feedIndex = 0;
}

void refreshArticles() {
  char directory[128];
  std::snprintf(directory, sizeof(directory), "%s/%s", kRssFolder, state.currentFeed);
  state.articleCount = listArticles(directory, state.articles, kMaxArticleEntries);
  if (state.articleIndex >= static_cast<int>(state.articleCount)) state.articleIndex = 0;
  if (state.articleCount == 0) state.articleIndex = 0;
}

// "2026-11-15-Why-Rust-Won-1a2b3c4d.html" -> "2026-11-15 Why Rust Won"
void formatArticleLabel(const char* fileName, char* out, size_t capacity) {
  char copy[80];
  std::snprintf(copy, sizeof(copy), "%s", fileName);
  size_t length = std::strlen(copy);
  if (length > 5 && strcasecmp(copy + length - 5, ".html") == 0) {
    copy[length - 5] = '\0';
    length -= 5;
  }
  // The trailing hash keeps names unique; it carries no meaning for the reader.
  const size_t lastDash = [&]() {
    for (size_t i = length; i > 0; --i) {
      if (copy[i - 1] == '-') return i - 1;
    }
    return length;
  }();
  if (length > lastDash && length - lastDash == 9) copy[lastDash] = '\0';
  for (char* p = copy; *p; ++p) {
    if (*p == '-') *p = ' ';
  }
  std::snprintf(out, capacity, "%s", copy);
}

void renderConnecting(const char* text) {
  renderer.clearScreen(THEME.backgroundColor);
  ui::centeredMessage(renderer, THEME, THEME.uiFontId, text);
  renderer.displayBuffer(hal::Display::FAST_REFRESH, true);
}

// Keeps buttons alive while a blocking sync runs. Back aborts the transfer, and
// the idle timer is refreshed so the device cannot sleep mid-download.
void pumpInput() {
  if (!s_core) return;
  s_core->input.resetIdleTimer();
  inputManager.update();
  s_core->input.poll();
  Event event;
  while (s_core->events.pop(event)) {
    if (event.type == EventType::ButtonPress && event.button == Button::Back) syncAborted = true;
    if (event.type == EventType::ButtonLongPress && event.button == Button::Power) syncAborted = true;
    if (event.type == EventType::Tap) {
      const bool frontLrbc = s_core->settings.frontButtonLayout == Settings::FrontLRBC;
      const int index = ui::touch::semanticButtonBarIndex({event.touch.x, event.touch.y}, renderer.getScreenWidth(),
                                                          renderer.getScreenHeight(), frontLrbc);
      if (index == 0) syncAborted = true;
    }
  }
}

// Runs while a transport read waits for data: keeps input alive and reports an
// abort request back to the transport.
bool syncWaitHook(void*) {
  pumpInput();
  return !syncAborted;
}

const char* stageLabel(freshrss::SyncStage stage) {
  switch (stage) {
    case freshrss::SyncStage::Connecting:
      return "Connecting";
    case freshrss::SyncStage::Login:
      return "Signing in";
    case freshrss::SyncStage::Feeds:
      return "Reading feed list";
    case freshrss::SyncStage::Articles:
      return "Downloading articles";
    case freshrss::SyncStage::Favorites:
      return "Downloading favorites";
    case freshrss::SyncStage::MarkRead:
      return "Marking read";
    case freshrss::SyncStage::Prune:
      return "Tidying up";
    case freshrss::SyncStage::Done:
      return "Finishing";
  }
  return "Syncing";
}

void renderSyncProgress(const freshrss::SyncProgress& progress) {
  const uint32_t now = millis();
  if (!state.needsRender && now - lastProgressRenderMs < kProgressRedrawMs) return;
  lastProgressRenderMs = now;
  state.needsRender = false;

  const Theme& theme = THEME;
  renderer.clearScreen(theme.backgroundColor);
  ui::title(renderer, theme, theme.screenMarginTop, "RSS Sync");
  int y = ui::SettingsListHit::LIST_START_Y;
  renderer.drawCenteredText(theme.uiFontId, y, stageLabel(progress.stage), theme.primaryTextBlack);
  y += renderer.getLineHeight(theme.uiFontId) + 6;
  if (progress.stage == freshrss::SyncStage::Articles && progress.feedTitle[0]) {
    char line[80];
    std::snprintf(line, sizeof(line), "%s (%u/%u)", progress.feedTitle, static_cast<unsigned>(progress.feedIndex + 1),
                  static_cast<unsigned>(progress.feedCount));
    ui::text(renderer, theme, y, line);
    y += renderer.getLineHeight(theme.uiFontId);
  }
  char counts[80];
  std::snprintf(counts, sizeof(counts), "%u new, %u already stored", static_cast<unsigned>(progress.articlesStored),
                static_cast<unsigned>(progress.articlesSkipped));
  ui::text(renderer, theme, y, counts);
  ui::ButtonBar buttons("Stop", "", "", "");
  ui::buttonBar(renderer, theme, buttons);
  renderer.displayBuffer(hal::Display::FAST_REFRESH, true);
}

bool syncProgressHook(void*, const freshrss::SyncProgress& progress) {
  pumpInput();
  if (syncAborted) return false;
  renderSyncProgress(progress);
  return true;
}

bool syncStoredHook(void*, const freshrss::StoredArticle& article) {
  LOG_INF(TAG, "Stored %s/%s (%u bytes)", article.feedTitle, article.fileName, static_cast<unsigned>(article.bytes));
  return true;
}

// Runs a full sync. Blocking on purpose: the mini-app host has no worker thread
// for it, and input stays responsive through pumpInput().
freshrss::SyncOutcome runSync() {
  syncAborted = false;
  const uint32_t now = currentEpoch();

  transport.reset(new (std::nothrow) WifiTransport());
  if (!transport) {
    freshrss::SyncOutcome outcome;
    outcome.status = freshrss::SyncStatus::NetworkError;
    std::snprintf(outcome.message, sizeof(outcome.message), "out of memory");
    return outcome;
  }

  transport->setWaitHook(syncWaitHook, nullptr);
  syncSession.reset(new (std::nothrow) freshrss::SyncSession(*transport, sdStore(), state.config, state.endpoint));
  if (!syncSession) {
    freshrss::SyncOutcome outcome;
    outcome.status = freshrss::SyncStatus::NetworkError;
    std::snprintf(outcome.message, sizeof(outcome.message), "out of memory");
    return outcome;
  }

  freshrss::SyncHooks hooks;
  hooks.ctx = nullptr;
  hooks.progress = syncProgressHook;
  hooks.stored = syncStoredHook;
  syncSession->setHooks(hooks);

  // Pending read markers are only dropped for the ids the server accepted.
  const char* ids[kMaxPendingItems];
  for (size_t i = 0; i < state.persisted.pendingCount; ++i) ids[i] = state.persisted.pending[i];

  freshrss::SyncRequest request;
  request.nowEpoch = now;
  request.sinceEpoch = state.persisted.sinceEpoch;
  request.feedOffset = state.persisted.feedCursor;
  request.knownFeedCount = state.persisted.feedCount;
  state.lastFeedStart = state.persisted.feedCursor;
  request.markReadIds = state.persisted.pendingCount > 0 ? ids : nullptr;
  request.markReadCount = state.persisted.pendingCount;

  const freshrss::SyncOutcome outcome = syncSession->run(request);
  std::snprintf(lastTransportError, sizeof(lastTransportError), "%s", transport->error());
  syncSession.reset();
  transport.reset();

  if (outcome.ok() && request.markReadCount > 0) {
    for (size_t i = 0; i < request.markReadCount; ++i) {
      removePendingItem(state.persisted, ids[i]);
    }
  }
  return outcome;
}

// A user-requested sync reports its result on its own screen; an automatic one
// returns to whatever was on screen and keeps the text as a status line.
void finishSyncWithMessage(const char* text, bool success) {
  std::snprintf(state.message, sizeof(state.message), "%s", text);
  if (state.userInitiatedSync || !success) {
    state.screen = Screen::Message;
  } else {
    state.screen = state.screenBeforeSync == Screen::Articles ? Screen::Articles : Screen::Feeds;
  }
  state.needsRender = true;
}

void startSync(bool userInitiated);

// Runs once the link is up: sync now when it is due, otherwise show the feeds.
void afterWifiReady() {
  if (freshrss::syncDue(state.lastSyncEpoch, currentEpoch(), state.config, state.lastSyncEpoch == 0)) {
    startSync(true);
    return;
  }
  state.screen = Screen::Feeds;
  state.needsRender = true;
}

void startSync(bool userInitiated) {
  if (!state.configOk) {
    setMessage("No usable config");
    return;
  }
  if (!s_core->wifi.isConnected()) {
    // Nothing can be fetched without a link: hand the user to the network
    // picker instead of a dead end. The app is relaunched once it connects.
    saveState(state.persisted);
    requestWifiPicker();
    return;
  }
  state.userInitiatedSync = userInitiated;
  state.screenBeforeSync = state.screen;
  state.screen = Screen::Syncing;
  state.needsRender = true;
  lastProgressRenderMs = 0;
}

void finishSync() {
  const freshrss::SyncOutcome outcome = runSync();
  const bool stoppedByUser = syncAborted;
  state.stats = outcome.stats;
  state.syncedThisSession = outcome.ok();
  char text[96];
  if (outcome.ok()) {
    state.lastSyncEpoch = currentEpoch();
    state.persisted.lastSyncEpoch = state.lastSyncEpoch;
    // The item window follows the rotation: it only moves once every subscribed
    // feed has been visited, so later feeds still see the older bound.
    if (outcome.coveredAllFeeds) state.persisted.sinceEpoch = state.lastSyncEpoch;
    state.persisted.feedCursor = outcome.nextFeedOffset;
    state.persisted.feedCount = outcome.stats.feedsSubscribed;
    saveState(state.persisted);
    refreshFeeds();
    if (outcome.stats.articlesStored == 0 && outcome.stats.markedRead == 0) {
      std::snprintf(text, sizeof(text), "Nothing new (%u feeds)", static_cast<unsigned>(outcome.stats.feedsChecked));
    } else {
      std::snprintf(text, sizeof(text), "%u new article%s", static_cast<unsigned>(outcome.stats.articlesStored),
                    outcome.stats.articlesStored == 1 ? "" : "s");
    }
    finishSyncWithMessage(text, true);
    return;
  }

  const char* described = stoppedByUser ? "stopped" : freshrss::describeSyncStatus(outcome.status);
  if (!stoppedByUser && outcome.message[0] != '\0' && outcome.status != freshrss::SyncStatus::Aborted) {
    if ((outcome.status == freshrss::SyncStatus::NetworkError || outcome.status == freshrss::SyncStatus::LoginFailed ||
         outcome.status == freshrss::SyncStatus::OutOfSpace) &&
        lastTransportError[0] != '\0') {
      // The transport message is more specific than the status in these cases.
      std::snprintf(text, sizeof(text), "%s: %s", described, lastTransportError);
    } else {
      std::snprintf(text, sizeof(text), "%s: %s", described, outcome.message);
    }
  } else {
    std::snprintf(text, sizeof(text), "%s", described);
  }
  finishSyncWithMessage(text, false);
}

void openSelectedArticle() {
  if (state.articleCount == 0) return;
  char directory[128];
  char path[192];
  std::snprintf(directory, sizeof(directory), "%s/%s", kRssFolder, state.currentFeed);
  if (buildArticlePath(path, sizeof(path), directory, state.articles[state.articleIndex]) == 0) return;

  if (state.config.markReadOnOpen) {
    char head[320];
    if (readArticleHead(path, head, sizeof(head))) {
      char itemId[96];
      if (freshrss::extractItemId(head, std::strlen(head), itemId, sizeof(itemId))) {
        if (addPendingItem(state.persisted, itemId)) saveState(state.persisted);
      }
    }
  }

  // Return to this folder and row when the reader exits, so the next article is
  // one step away.
  std::snprintf(s_core->settings.fileListDir, sizeof(s_core->settings.fileListDir), "%s", directory);
  std::snprintf(s_core->settings.fileListSelectedName, sizeof(s_core->settings.fileListSelectedName), "%s",
                state.articles[state.articleIndex]);
  s_core->settings.fileListSelectedIndex = 0;
  s_core->settings.saveToFile();

  showTransitionNotification("Opening article");
  saveTransition(BootMode::READER, path, ReturnTo::FILE_MANAGER);
  vTaskDelay(50 / portTICK_PERIOD_MS);
  ESP.restart();
}

void reloadConfig() {
  char error[96] = {};
  const ConfigLoad result = loadConfig(state.config, error, sizeof(error));
  state.configOk = result == ConfigLoad::Loaded;
  state.tlsInsecure = state.configOk && !state.config.verifyTls && state.config.caCert[0] == '\0';
  if (state.configOk) {
    char endpointError[64];
    if (!freshrss::parseServerUrl(state.config.server, state.endpoint, endpointError, sizeof(endpointError))) {
      state.configOk = false;
      std::snprintf(state.message, sizeof(state.message), "%s", endpointError);
    }
  }
  if (!state.configOk && result != ConfigLoad::Loaded) {
    std::snprintf(state.message, sizeof(state.message), "%s", error[0] ? error : "no rss.json on the card");
  }
  state.screen = state.configOk ? Screen::Feeds : Screen::NoConfig;
  state.needsRender = true;
}

void connectSavedWifi() {
  WIFI_STORE.loadFromFile();
  if (WIFI_STORE.getCount() == 0) {
    requestWifiPicker();
    return;
  }
  renderConnecting("Connecting WiFi...");
  const auto& credentials = WIFI_STORE.getCredentials();
  for (int i = 0; i < WIFI_STORE.getCount(); ++i) {
    LOG_INF(TAG, "Trying WiFi: %s", credentials[i].ssid);
    if (s_core->wifi.connect(credentials[i].ssid, credentials[i].password, kWifiConnectTimeoutMs, nullptr).ok()) {
      WIFI_STORE.promoteCredential(credentials[i].ssid);
      return;
    }
    s_core->wifi.shutdown();
  }
  LOG_WRN(TAG, "No stored network accepted the connection");
  char text[96];
  std::snprintf(text, sizeof(text), "Offline: no WiFi (%s)", WIFI_STORE.getCredentials()[0].ssid);
  state.message[0] = '\0';
  state.screen = state.configOk ? Screen::Feeds : Screen::NoConfig;
  state.needsRender = true;
}

void renderFeeds() {
  const Theme& theme = THEME;
  renderer.clearScreen(theme.backgroundColor);
  ui::title(renderer, theme, theme.screenMarginTop, "RSS Feeds");

  char subtitle[96];
  if (state.feedCount == 0) {
    std::snprintf(subtitle, sizeof(subtitle), "No articles yet - press Sync");
  } else if (state.lastSyncEpoch == 0) {
    std::snprintf(subtitle, sizeof(subtitle), "%u feeds, never synced", static_cast<unsigned>(state.feedCount));
  } else {
    std::snprintf(subtitle, sizeof(subtitle), "%u feeds - %u new last sync", static_cast<unsigned>(state.feedCount),
                  static_cast<unsigned>(state.stats.articlesStored));
  }
  ui::text(renderer, theme, ui::SettingsListHit::LIST_START_Y - renderer.getLineHeight(theme.smallFontId), subtitle);
  if (state.message[0] != '\0') {
    ui::text(renderer, theme, ui::SettingsListHit::LIST_START_Y - 2 * renderer.getLineHeight(theme.smallFontId),
             state.message);
  }

  const int rowY = ui::SettingsListHit::LIST_START_Y;
  const int rowHeight = theme.itemHeight + theme.itemSpacing;
  const int visible = visibleRows(rowHeight);
  int first = state.feedIndex - visible / 2;
  if (first < 0) first = 0;
  if (first + visible > static_cast<int>(state.feedCount)) first = static_cast<int>(state.feedCount) - visible;
  if (first < 0) first = 0;
  for (int row = 0; row < visible; ++row) {
    const int index = first + row;
    if (index >= static_cast<int>(state.feedCount)) break;
    ui::menuItem(renderer, theme, rowY + row * rowHeight, state.feeds[index], index == state.feedIndex);
  }
  if (state.feedCount == 0) {
    ui::text(renderer, theme, rowY, "Sync to download articles");
  }
  ui::ButtonBar buttons("Back", "Menu", "Sync", "Open");
  ui::buttonBar(renderer, theme, buttons);
}

void renderArticles() {
  const Theme& theme = THEME;
  renderer.clearScreen(theme.backgroundColor);
  char title[80];
  std::snprintf(title, sizeof(title), "%s", state.currentFeed);
  ui::title(renderer, theme, theme.screenMarginTop, title);
  char subtitle[64];
  std::snprintf(subtitle, sizeof(subtitle), "%u articles", static_cast<unsigned>(state.articleCount));
  ui::text(renderer, theme, ui::SettingsListHit::LIST_START_Y - renderer.getLineHeight(theme.smallFontId), subtitle);

  const int rowY = ui::SettingsListHit::LIST_START_Y;
  const int rowHeight = theme.itemHeight + theme.itemSpacing;
  const int visible = visibleRows(rowHeight);
  int first = state.articleIndex - visible / 2;
  if (first < 0) first = 0;
  if (first + visible > static_cast<int>(state.articleCount)) first = static_cast<int>(state.articleCount) - visible;
  if (first < 0) first = 0;
  for (int row = 0; row < visible; ++row) {
    const int index = first + row;
    if (index >= static_cast<int>(state.articleCount)) break;
    char label[80];
    formatArticleLabel(state.articles[index], label, sizeof(label));
    ui::menuItem(renderer, theme, rowY + row * rowHeight, label, index == state.articleIndex);
  }
  if (state.articleCount == 0) {
    ui::text(renderer, theme, rowY, "No articles stored");
  }
  ui::ButtonBar buttons("Back", "Menu", "Sync", "Read");
  ui::buttonBar(renderer, theme, buttons);
}

void renderMessage() {
  const Theme& theme = THEME;
  renderer.clearScreen(theme.backgroundColor);
  ui::title(renderer, theme, theme.screenMarginTop, "RSS");
  ui::centeredMessage(renderer, theme, theme.uiFontId, state.message);
  ui::ButtonBar buttons("Back", "OK", "", "");
  ui::buttonBar(renderer, theme, buttons);
}

void renderNoConfig() {
  const Theme& theme = THEME;
  renderer.clearScreen(theme.backgroundColor);
  ui::title(renderer, theme, theme.screenMarginTop, "RSS");
  int y = ui::SettingsListHit::LIST_START_Y;
  ui::text(renderer, theme, y, "No rss.json on the card");
  y += renderer.getLineHeight(theme.uiFontId);
  if (state.message[0]) {
    ui::textWrapped(renderer, theme, y, state.message, 2);
    y += 2 * renderer.getLineHeight(theme.smallFontId);
  }
  y += 8;
  ui::textWrapped(renderer, theme, y,
                  "Put an rss.json file on the SD card with server, user and apiPassword. Press Right to create a "
                  "template.",
                  4);
  ui::ButtonBar buttons("Back", "Menu", "Create", "?");
  ui::buttonBar(renderer, theme, buttons);
}

void renderMenu() {
  const Theme& theme = THEME;
  renderer.clearScreen(theme.backgroundColor);
  ui::title(renderer, theme, theme.screenMarginTop, "RSS");

  const int rowY = ui::SettingsListHit::LIST_START_Y;
  const int rowHeight = theme.itemHeight + theme.itemSpacing;
  char line[96];
  std::snprintf(line, sizeof(line), "Sync now");
  ui::menuItem(renderer, theme, rowY, line, state.menuItem == MenuItem::SyncNow);
  ui::menuItem(renderer, theme, rowY + rowHeight, "Reload config", state.menuItem == MenuItem::ReloadConfig);
  ui::menuItem(renderer, theme, rowY + 2 * rowHeight, "Browse article folder",
               state.menuItem == MenuItem::BrowseFolder);

  int y = rowY + 3 * rowHeight + 10;
  char info[96];
  if (state.lastSyncEpoch != 0) {
    const std::time_t last = static_cast<std::time_t>(state.lastSyncEpoch);
    std::tm tm{};
    gmtime_r(&last, &tm);
    std::snprintf(info, sizeof(info), "Last sync: %04d-%02d-%02d %02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min);
  } else {
    std::snprintf(info, sizeof(info), "Last sync: never");
  }
  ui::text(renderer, theme, y, info);
  y += renderer.getLineHeight(theme.uiFontId);
  std::snprintf(info, sizeof(info), "Stored: %u new, %u skipped", static_cast<unsigned>(state.stats.articlesStored),
                static_cast<unsigned>(state.stats.articlesSkipped));
  ui::text(renderer, theme, y, info);
  y += renderer.getLineHeight(theme.uiFontId);
  std::snprintf(info, sizeof(info), "Read markers pending: %u", static_cast<unsigned>(state.persisted.pendingCount));
  ui::text(renderer, theme, y, info);
  y += renderer.getLineHeight(theme.uiFontId);
  if (state.stats.feedsSubscribed > state.config.maxFeeds && state.stats.feedsSubscribed > 0) {
    // Rotation is active: show which slice of the list the last run covered.
    std::snprintf(info, sizeof(info), "Feeds %u-%u of %u", static_cast<unsigned>(state.lastFeedStart) + 1,
                  static_cast<unsigned>(state.lastFeedStart) + state.stats.feedsChecked,
                  static_cast<unsigned>(state.stats.feedsSubscribed));
    ui::text(renderer, theme, y, info);
    y += renderer.getLineHeight(theme.uiFontId);
  }
  std::snprintf(info, sizeof(info), "TLS: %s",
                !state.config.verifyTls ? "not verified" : (state.config.caCert[0] ? "CA file" : "system roots"));
  ui::text(renderer, theme, y, info);
  y += renderer.getLineHeight(theme.uiFontId);
  std::snprintf(info, sizeof(info), "Free heap: %uK", static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  ui::text(renderer, theme, y, info);
  y += renderer.getLineHeight(theme.uiFontId);
  std::snprintf(info, sizeof(info), "Config: %s", state.config.folder);
  ui::text(renderer, theme, y, info);

  ui::ButtonBar buttons("Back", "", "Select", "Activate");
  ui::buttonBar(renderer, theme, buttons);
}

void onButton(Core&, Button button) {
  switch (state.screen) {
    case Screen::Feeds:
      if (button == Button::Up && state.feedCount > 0) {
        state.feedIndex = (state.feedIndex + static_cast<int>(state.feedCount) - 1) % static_cast<int>(state.feedCount);
      } else if (button == Button::Down && state.feedCount > 0) {
        state.feedIndex = (state.feedIndex + 1) % static_cast<int>(state.feedCount);
      } else if (button == Button::Left) {
        startSync(true);
      } else if (button == Button::Right && state.feedCount > 0) {
        std::snprintf(state.currentFeed, sizeof(state.currentFeed), "%s", state.feeds[state.feedIndex]);
        refreshArticles();
        state.screen = Screen::Articles;
      }
      break;
    case Screen::Articles:
      if (button == Button::Up && state.articleCount > 0) {
        state.articleIndex =
            (state.articleIndex + static_cast<int>(state.articleCount) - 1) % static_cast<int>(state.articleCount);
      } else if (button == Button::Down && state.articleCount > 0) {
        state.articleIndex = (state.articleIndex + 1) % static_cast<int>(state.articleCount);
      } else if (button == Button::Left) {
        startSync(true);
      } else if (button == Button::Right) {
        openSelectedArticle();
      }
      break;
    case Screen::NoConfig:
      if (button == Button::Right) {
        char error[96] = {};
        if (writeExampleConfig(error, sizeof(error))) {
          setMessage("rss.json created - edit it on the card");
        } else {
          setMessage(error[0] ? error : "Cannot create rss.json");
        }
      }
      break;
    case Screen::Message:
    case Screen::Loading:
    case Screen::Syncing:
      break;
  }
}

void onMenuButton(Core& core, Button button) {
  if (button == Button::Up) {
    state.menuItem = static_cast<MenuItem>((static_cast<int>(state.menuItem) + static_cast<int>(MenuItem::Count) - 1) %
                                           static_cast<int>(MenuItem::Count));
    state.needsRender = true;
    return;
  }
  if (button == Button::Down) {
    state.menuItem = static_cast<MenuItem>((static_cast<int>(state.menuItem) + 1) % static_cast<int>(MenuItem::Count));
    state.needsRender = true;
    return;
  }
  if (button != Button::Left && button != Button::Right) return;

  switch (state.menuItem) {
    case MenuItem::SyncNow:
      startSync(true);
      state.closeMenuAfterAction = true;
      break;
    case MenuItem::ReloadConfig:
      reloadConfig();
      break;
    case MenuItem::BrowseFolder: {
      char directory[96];
      if (state.currentFeed[0] != '\0') {
        std::snprintf(directory, sizeof(directory), "%s/%s", kRssFolder, state.currentFeed);
      } else {
        std::snprintf(directory, sizeof(directory), "%s", kRssFolder);
      }
      std::snprintf(core.pendingDirectory, sizeof(core.pendingDirectory), "%s", directory);
      break;
    }
    case MenuItem::Count:
      break;
  }
}

}  // namespace

void enter(Core& core) {
  LOG_INF(TAG, "RSS app enter, free heap: %d", static_cast<int>(ESP.getFreeHeap()));
  s_core = &core;
  state = State{};
  syncAborted = false;

  loadState(state.persisted);
  state.lastSyncEpoch = state.persisted.lastSyncEpoch;
  reloadConfig();
  refreshFeeds();

  if (!state.configOk) {
    state.screen = Screen::NoConfig;
    state.needsRender = true;
    return;
  }

  wifiSession.reset(new (std::nothrow) hal::WifiSession(core.wifi, core.cpu));
  if (!wifiSession) {
    core.wifi.shutdown();
    setMessage("Out of memory");
    return;
  }

  if (!core.wifi.isConnected()) {
    state.screen = Screen::Loading;
    connectSavedWifi();
  }
  // Covers both paths: a live connection at entry, and a successful connect from
  // the saved credentials. connectSavedWifi() has already chosen a screen when it
  // could not connect.
  if (core.wifi.isConnected()) afterWifiReady();
}

bool update(Core& core) {
  switch (state.screen) {
    case Screen::Syncing:
      finishSync();
      break;
    case Screen::Loading:
      // Safety net: connecting runs synchronously in enter(), so reaching here
      // means the result was missed. Never stay on the connecting screen.
      if (core.wifi.isConnected()) {
        afterWifiReady();
      } else {
        state.screen = state.configOk ? Screen::Feeds : Screen::NoConfig;
        state.needsRender = true;
      }
      break;
    case Screen::Feeds:
    case Screen::Articles: {
      // Periodic retry while the app is open: the sync runs again once the
      // interval has passed, without leaving the app.
      if (state.configOk && core.wifi.isConnected() &&
          freshrss::syncDue(state.lastSyncEpoch, currentEpoch(), state.config, state.lastSyncEpoch == 0) &&
          !core.input.hasPendingInteraction()) {
        startSync(false);
        break;
      }
      core.cpu.throttle();
      break;
    }
    default:
      break;
  }
  const bool changed = state.needsRender;
  state.needsRender = false;
  return changed;
}

miniapps::Action handleEvent(Core& core, const Event& event, bool menuOpen) {
  if (state.screen == Screen::Syncing) return miniapps::Action::None;

  // Back goes up one level while browsing: articles return to the feed list,
  // the feed list leaves the app.
  if (!menuOpen && state.screen == Screen::Articles) {
    Button button;
    if (miniAppButton(core, event, button) && button == Button::Back) {
      state.screen = Screen::Feeds;
      state.needsRender = true;
      return miniapps::Action::Redraw;
    }
  }

  if (menuOpen) {
    Button button;
    if (!miniAppButton(core, event, button) ||
        (button != Button::Back && button != Button::Left && button != Button::Right && button != Button::Up &&
         button != Button::Down))
      return miniapps::Action::None;
  }

  const miniapps::Action action = handleAppButtons(core, event, menuOpen, onButton, onMenuButton);
  if (state.closeMenuAfterAction) {
    // A menu action replaced the menu with another screen, so leave the menu.
    state.closeMenuAfterAction = false;
    return miniapps::Action::CloseMenu;
  }
  return action;
}

miniapps::Frame render(Core& core, bool menuOpen) {
  (void)core;
  if (menuOpen) {
    renderMenu();
    return {miniapps::Refresh::Fast, true};
  }
  switch (state.screen) {
    case Screen::Loading:
      renderConnecting("Connecting WiFi...");
      return {miniapps::Refresh::None, false};  // the helper already refreshed
    case Screen::NoConfig:
      renderNoConfig();
      break;
    case Screen::Feeds:
      renderFeeds();
      break;
    case Screen::Articles:
      renderArticles();
      break;
    case Screen::Message:
      renderMessage();
      break;
    case Screen::Syncing:
      return {miniapps::Refresh::None, false};
  }
  return {miniapps::Refresh::Fast, true};
}

void exitApp(Core& core) {
  (void)core;
  saveState(state.persisted);
  syncSession.reset();
  transport.reset();
  wifiSession.reset();
}

miniapps::ExitResult prepareExit(Core& core, miniapps::ExitTarget target) {
  (void)core;
  (void)target;
  saveState(state.persisted);
  return miniapps::ExitResult::Ready;
}

}  // namespace papyrix::rss_app
