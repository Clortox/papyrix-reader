#include "RssState.h"

#include <cstdio>
#include <cstring>

namespace papyrix::rss_app {
namespace {

void copyBounded(char* out, size_t capacity, const char* text, size_t length) {
  if (!out || capacity == 0) return;
  const size_t count = length < capacity - 1 ? length : capacity - 1;
  if (count > 0 && text) std::memcpy(out, text, count);
  out[count] = '\0';
}

bool parseUnsigned(const char* text, size_t length, uint32_t& out) {
  if (length == 0 || length > 10) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + static_cast<uint64_t>(text[i] - '0');
  }
  out = static_cast<uint32_t>(value);
  return true;
}

}  // namespace

size_t encodeState(const RssState& state, char* out, size_t capacity) {
  if (!out || capacity == 0) return 0;
  int written = std::snprintf(out, capacity, "lastSync=%u\nsince=%u\ncursor=%u\nfeeds=%u\n",
                              static_cast<unsigned>(state.lastSyncEpoch),
                              static_cast<unsigned>(state.sinceEpoch),
                              static_cast<unsigned>(state.feedCursor),
                              static_cast<unsigned>(state.feedCount));
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    out[0] = '\0';
    return 0;
  }
  size_t length = static_cast<size_t>(written);
  for (size_t i = 0; i < state.pendingCount; ++i) {
    if (state.pending[i][0] == '\0') continue;
    written = std::snprintf(out + length, capacity - length, "pending=%s\n", state.pending[i]);
    if (written <= 0 || static_cast<size_t>(written) >= capacity - length) {
      out[0] = '\0';
      return 0;
    }
    length += static_cast<size_t>(written);
  }
  return length;
}

bool decodeState(const char* text, size_t length, RssState& out) {
  out = RssState{};
  if (!text) return true;
  size_t offset = 0;
  while (offset < length) {
    size_t end = offset;
    while (end < length && text[end] != '\n') ++end;
    size_t lineLength = end - offset;
    if (lineLength > 0 && text[offset + lineLength - 1] == '\r') --lineLength;
    const char* line = text + offset;
    if (lineLength > 8 && std::strncmp(line, "lastSync=", 9) == 0) {
      uint32_t epoch = 0;
      if (parseUnsigned(line + 9, lineLength - 9, epoch)) out.lastSyncEpoch = epoch;
    } else if (lineLength > 6 && std::strncmp(line, "since=", 6) == 0) {
      uint32_t epoch = 0;
      if (parseUnsigned(line + 6, lineLength - 6, epoch)) out.sinceEpoch = epoch;
    } else if (lineLength > 7 && std::strncmp(line, "cursor=", 7) == 0) {
      uint32_t cursor = 0;
      if (parseUnsigned(line + 7, lineLength - 7, cursor)) out.feedCursor = cursor;
    } else if (lineLength > 6 && std::strncmp(line, "feeds=", 6) == 0) {
      uint32_t count = 0;
      if (parseUnsigned(line + 6, lineLength - 6, count) && count <= 65535) out.feedCount = static_cast<uint16_t>(count);
    } else if (lineLength > 8 && std::strncmp(line, "pending=", 8) == 0) {
      char id[kMaxItemIdLength + 1];
      copyBounded(id, sizeof(id), line + 8, lineLength - 8);
      if (id[0] != '\0') addPendingItem(out, id);
    }
    offset = end + 1;
  }
  return true;
}

bool addPendingItem(RssState& state, const char* itemId) {
  if (!itemId || itemId[0] == '\0') return false;
  if (hasPendingItem(state, itemId)) return false;
  if (state.pendingCount >= kMaxPendingItems) {
    // Drop the oldest entry so the newest read state is remembered.
    for (size_t i = 1; i < kMaxPendingItems; ++i) {
      std::memcpy(state.pending[i - 1], state.pending[i], sizeof(state.pending[0]));
    }
    state.pendingCount = kMaxPendingItems - 1;
  }
  copyBounded(state.pending[state.pendingCount], sizeof(state.pending[0]), itemId, std::strlen(itemId));
  state.pendingCount++;
  return true;
}

bool removePendingItem(RssState& state, const char* itemId) {
  if (!itemId) return false;
  for (size_t i = 0; i < state.pendingCount; ++i) {
    if (std::strcmp(state.pending[i], itemId) != 0) continue;
    for (size_t j = i + 1; j < state.pendingCount; ++j) {
      std::memcpy(state.pending[j - 1], state.pending[j], sizeof(state.pending[0]));
    }
    state.pendingCount--;
    state.pending[state.pendingCount][0] = '\0';
    return true;
  }
  return false;
}

bool hasPendingItem(const RssState& state, const char* itemId) {
  if (!itemId) return false;
  for (size_t i = 0; i < state.pendingCount; ++i) {
    if (std::strcmp(state.pending[i], itemId) == 0) return true;
  }
  return false;
}

}  // namespace papyrix::rss_app
