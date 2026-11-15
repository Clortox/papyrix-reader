// Persisted RSS state: the incremental window, the rotation cursor, and the
// queued read markers. A bug here would silently miss articles, so the encoding
// round-trip and the queue rules are checked directly.

#include <cstring>
#include <string>

#include "../../../src/apps/RssState.h"
#include "../../common/test_utils.h"

using namespace papyrix::rss_app;

int main() {
  TestUtils::TestRunner runner("FreshRss state");

  // --- round trip -----------------------------------------------------------
  {
    RssState state;
    state.lastSyncEpoch = 1794746040u;
    state.sinceEpoch = 1794000000u;
    state.feedCursor = 17;
    addPendingItem(state, "tag:google.com,2005:reader/item/1a2b");
    addPendingItem(state, "tag:google.com,2005:reader/item/3c4d");

    char text[kMaxStateBytes];
    const size_t length = encodeState(state, text, sizeof(text));
    runner.expectTrue(length > 0, "state: encodes");

    RssState decoded;
    runner.expectTrue(decodeState(text, length, decoded), "state: decodes");
    runner.expectEq<uint32_t>(1794746040u, decoded.lastSyncEpoch, "state: last sync round-trips");
    runner.expectEq<uint32_t>(1794000000u, decoded.sinceEpoch, "state: window round-trips");
    runner.expectEq<uint32_t>(17, decoded.feedCursor, "state: rotation cursor round-trips");
    runner.expectEq<size_t>(2, decoded.pendingCount, "state: markers round-trip");
    runner.expectTrue(hasPendingItem(decoded, "tag:google.com,2005:reader/item/3c4d"), "state: marker content kept");
  }
  {
    // A file written by an older build has no window or cursor fields.
    const char legacy[] = "lastSync=1700000000\npending=tag:x/1\n";
    RssState decoded;
    runner.expectTrue(decodeState(legacy, sizeof(legacy) - 1, decoded), "legacy: decodes");
    runner.expectEq<uint32_t>(1700000000u, decoded.lastSyncEpoch, "legacy: last sync read");
    runner.expectEq<uint32_t>(0, decoded.sinceEpoch, "legacy: window starts unset");
    runner.expectEq<uint32_t>(0, decoded.feedCursor, "legacy: cursor starts at zero");
    runner.expectEq<size_t>(1, decoded.pendingCount, "legacy: marker read");
  }
  {
    const char junk[] = "bogus\nsince=notanumber\ncursor=99\n\n";
    RssState decoded;
    decodeState(junk, sizeof(junk) - 1, decoded);
    runner.expectEq<uint32_t>(0, decoded.sinceEpoch, "junk: unparsable window ignored");
    runner.expectEq<uint32_t>(99, decoded.feedCursor, "junk: cursor still read");
  }

  // --- marker queue ---------------------------------------------------------
  {
    RssState state;
    runner.expectTrue(addPendingItem(state, "tag:x/1"), "queue: first add accepted");
    runner.expectFalse(addPendingItem(state, "tag:x/1"), "queue: duplicate ignored");
    runner.expectEq<size_t>(1, state.pendingCount, "queue: duplicate not stored");
    addPendingItem(state, "tag:x/2");
    runner.expectTrue(removePendingItem(state, "tag:x/1"), "queue: removal works");
    runner.expectFalse(hasPendingItem(state, "tag:x/1"), "queue: removed marker gone");
    runner.expectTrue(hasPendingItem(state, "tag:x/2"), "queue: other marker kept");
    runner.expectFalse(removePendingItem(state, "tag:x/9"), "queue: removing an unknown marker is a no-op");
  }
  {
    // The queue is bounded; the newest markers are the ones worth keeping.
    RssState state;
    for (size_t i = 0; i < kMaxPendingItems + 5; ++i) {
      char id[32];
      std::snprintf(id, sizeof(id), "tag:x/%zu", i);
      addPendingItem(state, id);
    }
    runner.expectEq<size_t>(kMaxPendingItems, state.pendingCount, "queue: bounded");
    runner.expectFalse(hasPendingItem(state, "tag:x/0"), "queue: oldest marker dropped");
    runner.expectTrue(hasPendingItem(state, "tag:x/36"), "queue: newest marker kept");
  }
  {
    RssState state;
    runner.expectFalse(addPendingItem(state, ""), "queue: empty id rejected");
    runner.expectFalse(addPendingItem(state, nullptr), "queue: null id rejected");
  }

  // --- bounds ---------------------------------------------------------------
  {
    RssState state;
    char tiny[8];
    runner.expectEq<size_t>(0, encodeState(state, tiny, sizeof(tiny)), "encode: refusing to truncate");
  }

  return runner.allPassed() ? 0 : 1;
}
