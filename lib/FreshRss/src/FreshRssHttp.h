#pragma once

#include <cstddef>
#include <cstdint>

// HTTP/1.1 request writing and response framing for the FreshRSS client.
//
// The transport is the only port point to the network: the firmware implements
// it over WiFiClient/WiFiClientSecure, the host tests over a scripted byte
// stream. Everything in this module is allocation-free and Arduino-free.

namespace freshrss {

class Transport {
 public:
  virtual ~Transport() = default;

  // Opens a connection. `tls` selects a secure transport, `verify` requests
  // certificate validation (against a built-in root bundle unless `caPath`
  // names a PEM file on the SD card). Returns false and may write `error`.
  virtual bool connect(const char* host, uint16_t port, bool tls, bool verify, const char* caPath,
                       uint32_t timeoutMs) = 0;
  virtual void close() = 0;
  virtual bool isConnected() = 0;
  // Reads at most `capacity` bytes. Returns >0, 0 at end of stream, <0 on error.
  virtual int read(char* out, size_t capacity) = 0;
  virtual bool write(const char* data, size_t length) = 0;
  // True when the peer closed the connection since the last read.
  virtual bool peerClosed() = 0;
};

struct HttpRequest {
  const char* method = "GET";
  const char* path = nullptr;           // request target, e.g. "/api/greader.php/reader/api/0/token"
  const char* authorization = nullptr;  // value for the Authorization header, or nullptr
  const char* body = nullptr;           // request body (form-encoded)
  size_t bodyLength = 0;
  const char* contentType = nullptr;  // defaults to form-urlencoded when a body is present
  const char* host = nullptr;         // value for the Host header
  bool keepAlive = true;
};

struct HttpResponseHead {
  int status = 0;
  long long contentLength = -1;  // -1 when absent
  bool chunked = false;
  bool keepAlive = false;
  bool valid = false;
};

// Writes requests and decodes responses on one connection. A response is read in
// two phases: request() consumes the status line and headers, then readBody()
// streams the decoded body. Connection reuse requires the caller to keep using
// the same session; a failed request leaves the session unusable until close().
class HttpSession {
 public:
  static constexpr size_t kMaxHeadBytes = 2048;
  static constexpr size_t kPushbackCapacity = 320;

  explicit HttpSession(Transport& transport) : transport_(transport) {}

  // Sends one request and parses its response head.
  bool request(const HttpRequest& request, HttpResponseHead& head, char* error, size_t errorSize);

  // Decoded body bytes: >0 read, 0 at end of the body, -1 on a protocol error.
  int readBody(char* out, size_t capacity);

  // Reads the whole (bounded) body into `out`. Returns the length, or -1 when
  // the body does not fit or the stream fails.
  int readBodyAll(char* out, size_t capacity);

  // Discards a bounded body. A response the caller does not read must still be
  // drained, or its bytes would be parsed as the next response's status line.
  void drainBody();

  const char* error() const { return error_; }
  bool hasError() const { return error_[0] != '\0'; }
  const HttpResponseHead& head() const { return head_; }
  void close();
  // Drops the response state without touching the transport (used after a
  // reconnect).
  void reset();

 private:
  Transport& transport_;
  HttpResponseHead head_;
  char error_[96] = {0};

  // Bytes read past the header terminator belong to the body.
  char pushback_[kPushbackCapacity];
  size_t pushbackOffset_ = 0;
  size_t pushbackLength_ = 0;

  long long identityRemaining_ = -1;
  long long chunkRemaining_ = 0;
  bool bodyStarted_ = false;
  bool bodyComplete_ = false;
  bool chunked_ = false;
  bool trailerPending_ = false;

  void setError(const char* reason);
  int readRaw(char* out, size_t capacity);
  int readIdentity(char* out, size_t capacity);
  int readChunked(char* out, size_t capacity);
  bool readChunkSize();
  bool readLine(char* out, size_t capacity);
};

// Percent-encodes `text` for an application/x-www-form-urlencoded body and
// appends it to `out`. Returns the new length, or 0 when it does not fit.
size_t formEncodeAppend(char* out, size_t capacity, size_t written, const char* text);

// Builds "basePath + apiPath" plus an optional query string. Returns the length
// or 0 when the result does not fit.
size_t buildRequestPath(char* out, size_t capacity, const char* basePath, const char* apiPath, const char* query);

// Joins host and port for a Host header ("host" or "host:port").
size_t buildHostHeader(char* out, size_t capacity, const char* host, uint16_t port, bool tls);

// Explains an HTTP status in one short line for the app's status screen.
const char* describeHttpStatus(int status);

}  // namespace freshrss
