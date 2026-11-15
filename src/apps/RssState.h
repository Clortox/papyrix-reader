#pragma once

#include <cstddef>
#include <cstdint>

// Persisted RSS reader state: when the last sync succeeded and which items were
// opened locally but not yet acknowledged upstream. The format is line-oriented
// text (like the other mini-apps) so a user can inspect and repair it.
//
// This module is free of Arduino and SD types so it can be unit tested.

namespace papyrix::rss_app {

constexpr size_t kMaxPendingItems = 32;
constexpr size_t kMaxItemIdLength = 95;
constexpr size_t kMaxStateBytes = 4096;

struct RssState {
  // When the last sync succeeded. Drives the "is a sync due" decision.
  uint32_t lastSyncEpoch = 0;
  // Start of the incremental window. It only advances after a full pass over the
  // subscriptions, so a feed visited later never misses articles published in
  // between.
  uint32_t sinceEpoch = 0;
  // Subscription index the next sync starts at: feeds beyond the per-run budget
  // are picked up by later runs.
  uint32_t feedCursor = 0;
  // Items opened locally since the last sync; uploaded when the network allows.
  char pending[kMaxPendingItems][kMaxItemIdLength + 1] = {};
  size_t pendingCount = 0;
};

// Serializes the state. Returns the length, or 0 when it does not fit.
size_t encodeState(const RssState& state, char* out, size_t capacity);

// Decodes the state file. Unknown lines are ignored so the file can gain fields
// later. Returns false only when the text is unusable.
bool decodeState(const char* text, size_t length, RssState& out);

// Adds an item to the pending list. Duplicates are ignored. When the list is
// full the oldest entry is dropped: the newest read state matters most.
bool addPendingItem(RssState& state, const char* itemId);

bool removePendingItem(RssState& state, const char* itemId);

bool hasPendingItem(const RssState& state, const char* itemId);

}  // namespace papyrix::rss_app
