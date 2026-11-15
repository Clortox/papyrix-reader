#include "RssTransport.h"

#include <Arduino.h>
#include <Logging.h>
#include <SDCardManager.h>

#include <cstring>

#define TAG "RSS_NET"

// The ESP-IDF certificate bundle that the Arduino core embeds for
// WiFiClientSecure. Attaching it validates public CAs without configuration.
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace papyrix::rss_app {
namespace {

// This core's NetworkClientSecure::read() is not blocking: it returns -1 when no
// decrypted bytes are buffered yet, without touching the connection. A transport
// read therefore has to wait for data itself, bounded by this deadline.
constexpr uint32_t kReadDeadlineMs = 15000;
constexpr uint32_t kWaitStepMs = 5;
constexpr uint32_t kHookStepMs = 50;

size_t skipUtf8Bom(const char* data, size_t length, const char** out) {
  *out = data;
  if (length >= 3 && static_cast<uint8_t>(data[0]) == 0xEF && static_cast<uint8_t>(data[1]) == 0xBB &&
      static_cast<uint8_t>(data[2]) == 0xBF) {
    *out = data + 3;
    return length - 3;
  }
  return length;
}

}  // namespace

bool WifiTransport::loadCaCertificate(const char* path) {
  FsFile file = SdMan.open(path, O_RDONLY);
  if (!file) {
    std::snprintf(error_, sizeof(error_), "cannot open %s", path);
    return false;
  }
  caPem_.assign(kMaxCaPemBytes + 1, 0);
  const int length = file.read(caPem_.data(), kMaxCaPemBytes);
  file.close();
  if (length <= 0) {
    std::snprintf(error_, sizeof(error_), "empty certificate file");
    caPem_.clear();
    return false;
  }
  caPem_[static_cast<size_t>(length)] = '\0';
  const char* text = nullptr;
  const size_t textLength = skipUtf8Bom(caPem_.data(), static_cast<size_t>(length), &text);
  if (std::strstr(text, "-----BEGIN") == nullptr) {
    std::snprintf(error_, sizeof(error_), "%s is not a PEM file", path);
    caPem_.clear();
    return false;
  }
  // setCACert() keeps the pointer, so the buffer must stay owned here; it does
  // for the lifetime of the session.
  if (text != caPem_.data()) {
    std::memmove(caPem_.data(), text, textLength + 1);
    caPem_.resize(textLength + 1);
  } else {
    caPem_.resize(textLength + 1);
  }
  return true;
}

bool WifiTransport::connect(const char* host, uint16_t port, bool tls, bool verify, const char* caPath,
                            uint32_t timeoutMs) {
  close();
  error_[0] = '\0';
  if (!host || host[0] == '\0') {
    std::snprintf(error_, sizeof(error_), "no host");
    return false;
  }

  if (!tls) {
    plain_.setTimeout(timeoutMs);
    if (!plain_.connect(host, port, static_cast<int32_t>(timeoutMs))) {
      std::snprintf(error_, sizeof(error_), "cannot reach %s", host);
      plain_.stop();
      return false;
    }
    return true;
  }

  // Starting a TLS session with too little heap fails inside mbedTLS with a
  // message the user cannot act on, so check first.
  if (ESP.getFreeHeap() < kMinTlsFreeHeap || ESP.getMaxAllocHeap() < kMinTlsMaxBlock) {
    std::snprintf(error_, sizeof(error_), "not enough memory for TLS (%uK free)",
                  static_cast<unsigned>(ESP.getFreeHeap() / 1024));
    LOG_ERR(TAG, "TLS refused: free=%lu max=%lu", static_cast<unsigned long>(ESP.getFreeHeap()),
            static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    return false;
  }

  secure_.reset(new (std::nothrow) WiFiClientSecure());
  if (!secure_) {
    std::snprintf(error_, sizeof(error_), "not enough memory for TLS");
    return false;
  }
  tls_ = true;

  if (caPath && caPath[0] != '\0') {
    if (!loadCaCertificate(caPath)) {
      close();
      return false;
    }
    secure_->setCACert(caPem_.data());
  } else if (verify) {
    // Built-in root bundle: validates public certificate authorities.
    const size_t bundleSize = static_cast<size_t>(x509_crt_bundle_end - x509_crt_bundle_start);
    secure_->setCACertBundle(x509_crt_bundle_start, bundleSize);
  } else {
    // Self-signed servers: the config asked for no validation.
    secure_->setInsecure();
  }

  secure_->setHandshakeTimeout(15000);
  secure_->setTimeout(timeoutMs);
  LOG_INF(TAG, "TLS connect to %s:%u (verify=%d, ca=%s), free=%lu", host, static_cast<unsigned>(port), verify ? 1 : 0,
          caPath && caPath[0] ? caPath : "-", static_cast<unsigned long>(ESP.getFreeHeap()));
  const uint32_t started = millis();
  if (!secure_->connect(host, port, static_cast<int32_t>(timeoutMs))) {
    std::snprintf(error_, sizeof(error_), "TLS handshake failed");
    LOG_ERR(TAG, "TLS handshake failed after %lums", static_cast<unsigned long>(millis() - started));
    close();
    return false;
  }
  LOG_INF(TAG, "TLS ready in %lums, free=%lu", static_cast<unsigned long>(millis() - started),
          static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}

void WifiTransport::close() {
  plain_.stop();
  if (secure_) {
    secure_->stop();
    secure_.reset();
  }
  caPem_.clear();
  tls_ = false;
}

bool WifiTransport::isConnected() {
  if (tls_) return secure_ && secure_->connected();
  return plain_.connected();
}

int WifiTransport::available() {
  if (tls_) return secure_ ? static_cast<int>(secure_->available()) : 0;
  return static_cast<int>(plain_.available());
}

int WifiTransport::blockingRead(bool tls, char* out, size_t capacity) {
  const uint32_t deadline = millis() + kReadDeadlineMs;
  uint32_t nextHookAt = millis();
  while (true) {
    int got = -1;
    if (tls) {
      if (!secure_) {
        std::snprintf(error_, sizeof(error_), "not connected");
        return -1;
      }
      got = secure_->read(reinterpret_cast<uint8_t*>(out), capacity);
    } else {
      got = plain_.read(reinterpret_cast<uint8_t*>(out), capacity);
    }
    if (got > 0) return got;

    const bool linked = tls ? (secure_ && secure_->connected()) : plain_.connected();
    if (!linked) {
      // A real failure closes the socket in this core; an empty read does not.
      if (got < 0) {
        std::snprintf(error_, sizeof(error_), "connection closed by the server");
        return -1;
      }
      return 0;  // clean end of stream
    }

    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      std::snprintf(error_, sizeof(error_), "read timed out");
      return -1;
    }
    if (waitHook_ && static_cast<int32_t>(millis() - nextHookAt) >= 0) {
      nextHookAt = millis() + kHookStepMs;
      if (!waitHook_(waitContext_)) {
        std::snprintf(error_, sizeof(error_), "stopped");
        return -1;
      }
    }
    delay(kWaitStepMs);
  }
}

int WifiTransport::read(char* out, size_t capacity) {
  if (capacity == 0) return 0;
  return blockingRead(tls_, out, capacity);
}

bool WifiTransport::write(const char* data, size_t length) {
  if (length == 0) return true;
  if (tls_) {
    if (!secure_) return false;
    return secure_->write(reinterpret_cast<const uint8_t*>(data), length) == length;
  }
  return plain_.write(reinterpret_cast<const uint8_t*>(data), length) == length;
}

bool WifiTransport::peerClosed() { return !isConnected(); }

}  // namespace papyrix::rss_app
