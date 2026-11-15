#pragma once

#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// The reader library is included by path so this header resolves without the
// build system's library include paths.
#include "../../lib/FreshRss/src/FreshRssHttp.h"

// Transport implementation for the FreshRSS client: a plain WiFiClient for
// http:// servers and a WiFiClientSecure session for https://.

namespace papyrix::rss_app {

// Headroom required before starting a TLS session. This core build fixes the
// mbedTLS session buffers at 16 KiB per direction, so a session needs roughly
// 40 KiB plus handshake state; a failed check reports a usable message instead
// of failing inside the handshake.
constexpr uint32_t kMinTlsFreeHeap = 48 * 1024;
constexpr uint32_t kMinTlsMaxBlock = 24 * 1024;
constexpr size_t kMaxCaPemBytes = 4096;

class WifiTransport : public freshrss::Transport {
 public:
  // Called while a read is waiting for data. Returning false aborts the wait,
  // which is how Back stops a stalled transfer.
  using WaitHook = bool (*)(void* context);

  bool connect(const char* host, uint16_t port, bool tls, bool verify, const char* caPath, uint32_t timeoutMs) override;
  void close() override;
  bool isConnected() override;
  // Bytes available on the current connection, for input pumping while waiting.
  int available();
  // Blocks until data arrives, the peer closes, or the read deadline expires.
  int read(char* out, size_t capacity) override;
  bool write(const char* data, size_t length) override;
  bool peerClosed() override;

  void setWaitHook(WaitHook hook, void* context) {
    waitHook_ = hook;
    waitContext_ = context;
  }

  const char* error() const { return error_; }

 private:
  int blockingRead(bool tls, char* out, size_t capacity);
  bool loadCaCertificate(const char* path);

  WiFiClient plain_;
  std::unique_ptr<WiFiClientSecure> secure_;
  std::vector<char> caPem_;
  bool tls_ = false;
  WaitHook waitHook_ = nullptr;
  void* waitContext_ = nullptr;
  char error_[80] = {};
};

}  // namespace papyrix::rss_app
