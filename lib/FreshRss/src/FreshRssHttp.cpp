#include "FreshRssHttp.h"

#include <cstdio>
#include <cstring>

#include "FreshRssText.h"

namespace freshrss {
namespace {

constexpr size_t kLineCapacity = 192;
constexpr uint32_t kHeaderTimeoutLoops = 0;  // timeouts are owned by the transport

bool headerContainsToken(const char* value, const char* token) {
  const size_t tokenLength = std::strlen(token);
  const char* cursor = value;
  while (*cursor) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') ++cursor;
    const char* start = cursor;
    while (*cursor && *cursor != ',' && *cursor != ';' && *cursor != ' ' && *cursor != '\t') ++cursor;
    if (static_cast<size_t>(cursor - start) == tokenLength) {
      bool match = true;
      for (size_t i = 0; i < tokenLength; ++i) {
        if (toLowerAscii(start[i]) != toLowerAscii(token[i])) {
          match = false;
          break;
        }
      }
      if (match) return true;
    }
  }
  return false;
}

bool parseChunkSize(const char* line, long long& size) {
  size = 0;
  size_t digits = 0;
  for (const char* p = line; *p; ++p) {
    const int digit = hexValue(*p);
    if (digit < 0) break;
    size = size * 16 + digit;
    if (++digits > 8 || size > 32LL * 1024 * 1024) return false;
  }
  return digits > 0;
}

bool isNoBodyStatus(int status) { return status == 204 || status == 304 || (status >= 100 && status < 200); }

}  // namespace

void HttpSession::setError(const char* reason) { copyBounded(error_, sizeof(error_), reason, std::strlen(reason)); }

void HttpSession::reset() {
  pushbackOffset_ = 0;
  pushbackLength_ = 0;
  identityRemaining_ = -1;
  chunkRemaining_ = 0;
  bodyStarted_ = false;
  bodyComplete_ = false;
  chunked_ = false;
  head_ = HttpResponseHead{};
  error_[0] = '\0';
}

void HttpSession::close() {
  transport_.close();
  reset();
}

int HttpSession::readRaw(char* out, size_t capacity) {
  if (pushbackOffset_ < pushbackLength_) {
    size_t available = pushbackLength_ - pushbackOffset_;
    if (available > capacity) available = capacity;
    std::memcpy(out, pushback_ + pushbackOffset_, available);
    pushbackOffset_ += available;
    if (pushbackOffset_ >= pushbackLength_) {
      pushbackOffset_ = 0;
      pushbackLength_ = 0;
    }
    return static_cast<int>(available);
  }
  return transport_.read(out, capacity);
}

bool HttpSession::readLine(char* out, size_t capacity) {
  size_t written = 0;
  while (true) {
    char c = 0;
    const int got = readRaw(&c, 1);
    if (got <= 0) {
      if (got < 0) setError("connection lost while reading a response");
      return false;
    }
    if (c == '\n') {
      if (written > 0 && out[written - 1] == '\r') --written;
      out[written] = '\0';
      return true;
    }
    if (written + 1 >= capacity) {
      setError("response line too long");
      return false;
    }
    out[written++] = c;
  }
}

bool HttpSession::request(const HttpRequest& request, HttpResponseHead& head, char* error, size_t errorSize) {
  if (errorSize > 0) error[0] = '\0';
  error_[0] = '\0';
  head_ = HttpResponseHead{};
  bodyStarted_ = false;
  bodyComplete_ = false;
  chunked_ = false;
  identityRemaining_ = -1;

  if (!request.path || request.path[0] == '\0') {
    setError("no request path");
    if (errorSize > 0) copyBounded(error, errorSize, error_);
    return false;
  }

  // Large enough for the longest request target the session builds plus the
  // method and version: a truncated request line would leave the server waiting
  // for the rest of it.
  char line[640];
  int written =
      std::snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\n", request.method ? request.method : "GET", request.path);
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(line)) {
    setError("request path too long");
    if (errorSize > 0) copyBounded(error, errorSize, error_);
    return false;
  }
  // The header block is written before the body so a Content-Length body can be
  // a separate buffer without a second copy.
  if (!transport_.write(line, static_cast<size_t>(written))) {
    setError("send failed");
    if (errorSize > 0) copyBounded(error, errorSize, error_);
    return false;
  }

  if (request.host && request.host[0]) {
    written = std::snprintf(line, sizeof(line), "Host: %s\r\n", request.host);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(line) || !transport_.write(line, written)) {
      setError("send failed");
      if (errorSize > 0) copyBounded(error, errorSize, error_);
      return false;
    }
  }
  static const char kCommonHeaders[] =
      "User-Agent: PapyriX-Sync\r\nAccept: application/json\r\nAccept-Encoding: identity\r\n";
  if (!transport_.write(kCommonHeaders, sizeof(kCommonHeaders) - 1)) {
    setError("send failed");
    if (errorSize > 0) copyBounded(error, errorSize, error_);
    return false;
  }
  if (request.authorization && request.authorization[0]) {
    written = std::snprintf(line, sizeof(line), "Authorization: %s\r\n", request.authorization);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(line) || !transport_.write(line, written)) {
      setError("send failed");
      if (errorSize > 0) copyBounded(error, errorSize, error_);
      return false;
    }
  }
  written = std::snprintf(line, sizeof(line), "Connection: %s\r\n", request.keepAlive ? "keep-alive" : "close");
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(line) || !transport_.write(line, written)) {
    setError("send failed");
    if (errorSize > 0) copyBounded(error, errorSize, error_);
    return false;
  }
  if (request.bodyLength > 0) {
    written = std::snprintf(line, sizeof(line), "Content-Type: %s\r\nContent-Length: %u\r\n",
                            request.contentType ? request.contentType : "application/x-www-form-urlencoded",
                            static_cast<unsigned>(request.bodyLength));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(line) || !transport_.write(line, written)) {
      setError("send failed");
      if (errorSize > 0) copyBounded(error, errorSize, error_);
      return false;
    }
  }
  if (!transport_.write("\r\n", 2)) {
    setError("send failed");
    if (errorSize > 0) copyBounded(error, errorSize, error_);
    return false;
  }
  if (request.bodyLength > 0 && request.body) {
    if (!transport_.write(request.body, request.bodyLength)) {
      setError("send failed");
      if (errorSize > 0) copyBounded(error, errorSize, error_);
      return false;
    }
  }

  if (!readLine(line, sizeof(line))) {
    if (errorSize > 0) copyBounded(error, errorSize, error_);
    return false;
  }
  // "HTTP/1.1 200 OK" or "HTTP/1.0 401 Unauthorized"
  const char* space = std::strchr(line, ' ');
  if (!space || std::strncmp(line, "HTTP/", 5) != 0) {
    setError("not an HTTP response");
    if (errorSize > 0) copyBounded(error, errorSize, error_);
    return false;
  }
  int status = 0;
  for (const char* p = space + 1; *p >= '0' && *p <= '9'; ++p) status = status * 10 + (*p - '0');
  if (status < 100 || status > 599) {
    setError("bad HTTP status");
    if (errorSize > 0) copyBounded(error, errorSize, error_);
    return false;
  }
  head_.status = status;

  bool keepAlive = line[7] != '0';  // HTTP/1.0 defaults to close
  size_t headBytes = 0;
  while (true) {
    if (!readLine(line, sizeof(line))) {
      if (errorSize > 0) copyBounded(error, errorSize, error_);
      return false;
    }
    if (line[0] == '\0') break;
    // Bound the whole header block: a broken server must not be able to keep
    // sending headers until the read deadline.
    headBytes += std::strlen(line) + 2;
    if (headBytes > kMaxHeadBytes) {
      setError("response head too large");
      if (errorSize > 0) copyBounded(error, errorSize, error_);
      return false;
    }
    const char* colon = std::strchr(line, ':');
    if (!colon) continue;
    char name[64];
    copyBounded(name, sizeof(name), line, static_cast<size_t>(colon - line));
    trimInPlace(name);
    const char* value = colon + 1;
    while (*value == ' ' || *value == '\t') ++value;
    if (equalsIgnoreCase(name, "content-length")) {
      long long length = 0;
      bool digits = false;
      for (const char* p = value; *p >= '0' && *p <= '9'; ++p) {
        length = length * 10 + (*p - '0');
        digits = true;
        if (length > 64LL * 1024 * 1024) break;
      }
      if (digits) head_.contentLength = length;
    } else if (equalsIgnoreCase(name, "transfer-encoding")) {
      if (headerContainsToken(value, "chunked")) head_.chunked = true;
    } else if (equalsIgnoreCase(name, "connection")) {
      if (headerContainsToken(value, "close")) keepAlive = false;
    } else if (equalsIgnoreCase(name, "content-type")) {
      copyBounded(head_.contentType, sizeof(head_.contentType), value, std::strlen(value));
    } else if (equalsIgnoreCase(name, "location")) {
      copyBounded(head_.location, sizeof(head_.location), value, std::strlen(value));
    } else if (equalsIgnoreCase(name, "last-modified-version")) {
      long long version = 0;
      bool digits = false;
      for (const char* p = value; *p >= '0' && *p <= '9'; ++p) {
        version = version * 10 + (*p - '0');
        digits = true;
        if (version > 100000000000LL) break;
      }
      if (digits) head_.lastModifiedVersion = version;
    }
  }

  head_.keepAlive = keepAlive && request.keepAlive;
  head_.valid = true;
  head = head_;

  if (isNoBodyStatus(status)) {
    bodyComplete_ = true;
  } else if (head_.chunked) {
    chunked_ = true;
    chunkRemaining_ = 0;
  } else if (head_.contentLength >= 0) {
    identityRemaining_ = head_.contentLength;
    if (identityRemaining_ == 0) bodyComplete_ = true;
  } else {
    identityRemaining_ = -1;  // until the peer closes
  }
  return true;
}

int HttpSession::readIdentity(char* out, size_t capacity) {
  if (bodyComplete_) return 0;
  const size_t want = identityRemaining_ >= 0
                          ? static_cast<size_t>(identityRemaining_ < static_cast<long long>(capacity)
                                                    ? identityRemaining_
                                                    : static_cast<long long>(capacity))
                          : capacity;
  if (want == 0) {
    bodyComplete_ = true;
    return 0;
  }
  const int got = readRaw(out, want);
  if (got < 0) {
    setError("connection lost while reading a response");
    return -1;
  }
  if (got == 0) {
    if (identityRemaining_ > 0) {
      setError("response ended early");
      return -1;
    }
    bodyComplete_ = true;
    return 0;
  }
  if (identityRemaining_ > 0) identityRemaining_ -= got;
  if (identityRemaining_ == 0) bodyComplete_ = true;
  return got;
}

bool HttpSession::readChunkSize() {
  char line[kLineCapacity];
  if (!readLine(line, sizeof(line))) return false;
  long long size = 0;
  if (!parseChunkSize(line, size)) {
    setError("malformed chunked response");
    return false;
  }
  if (size == 0) {
    // Trailers end with an empty line.
    while (true) {
      if (!readLine(line, sizeof(line))) return false;
      if (line[0] == '\0') break;
    }
    bodyComplete_ = true;
    return false;  // no data follows
  }
  chunkRemaining_ = size;
  return true;
}

int HttpSession::readChunked(char* out, size_t capacity) {
  while (true) {
    if (bodyComplete_) return 0;
    if (chunkRemaining_ == 0) {
      if (!readChunkSize()) {
        if (bodyComplete_) return 0;
        return -1;
      }
      continue;
    }
    const size_t want = static_cast<size_t>(
        chunkRemaining_ < static_cast<long long>(capacity) ? chunkRemaining_ : static_cast<long long>(capacity));
    const int got = readRaw(out, want);
    if (got < 0) {
      setError("connection lost while reading a response");
      return -1;
    }
    if (got == 0) {
      setError("chunked response ended early");
      return -1;
    }
    chunkRemaining_ -= got;
    if (chunkRemaining_ == 0) {
      // Consume the CRLF that terminates the chunk data.
      char line[kLineCapacity];
      if (!readLine(line, sizeof(line))) return -1;
      if (line[0] != '\0') {
        setError("malformed chunked response");
        return -1;
      }
    }
    return got;
  }
}

int HttpSession::readBody(char* out, size_t capacity) {
  if (hasError()) return -1;
  if (bodyComplete_) return 0;
  if (capacity == 0) return 0;
  bodyStarted_ = true;
  return chunked_ ? readChunked(out, capacity) : readIdentity(out, capacity);
}

int HttpSession::readBodyAll(char* out, size_t capacity) {
  if (!out || capacity == 0) return -1;
  size_t total = 0;
  while (true) {
    if (total + 1 >= capacity) {
      // The body does not fit in the caller's buffer: report it instead of
      // returning a silently truncated copy.
      setError("response too large");
      return -1;
    }
    const int got = readBody(out + total, capacity - 1 - total);
    if (got < 0) return -1;
    if (got == 0) break;
    total += static_cast<size_t>(got);
  }
  out[total] = '\0';
  return static_cast<int>(total);
}

void HttpSession::drainBody() {
  char scratch[64];
  size_t total = 0;
  while (total < 8192) {
    const int got = readBody(scratch, sizeof(scratch));
    if (got <= 0) break;
    total += static_cast<size_t>(got);
  }
}

size_t formEncodeAppend(char* out, size_t capacity, size_t written, const char* text) {
  if (!out || !text) return 0;
  static const char* kHex = "0123456789ABCDEF";
  for (const char* p = text; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '_' || c == '.' || c == '~';
    if (unreserved) {
      if (written + 2 > capacity) return 0;
      out[written++] = static_cast<char>(c);
    } else {
      if (written + 4 > capacity) return 0;
      out[written++] = '%';
      out[written++] = kHex[c >> 4];
      out[written++] = kHex[c & 0x0F];
    }
  }
  if (written + 1 > capacity) return 0;
  out[written] = '\0';
  return written;
}

size_t buildRequestPath(char* out, size_t capacity, const char* basePath, const char* apiPath, const char* query) {
  if (!out || capacity == 0) return 0;
  const char* base = basePath ? basePath : "";
  const char* api = apiPath ? apiPath : "";
  const int written = query && query[0] ? std::snprintf(out, capacity, "%s%s?%s", base, api, query)
                                        : std::snprintf(out, capacity, "%s%s", base, api);
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(written);
}

size_t buildHostHeader(char* out, size_t capacity, const char* host, uint16_t port, bool tls) {
  if (!out || capacity == 0 || !host) return 0;
  const uint16_t defaultPort = tls ? 443 : 80;
  const int written = port == defaultPort ? std::snprintf(out, capacity, "%s", host)
                                          : std::snprintf(out, capacity, "%s:%u", host, static_cast<unsigned>(port));
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(written);
}

const char* describeHttpStatus(int status) {
  switch (status) {
    case 400:
      return "server rejected the request";
    case 401:
      return "API password rejected";
    case 403:
      return "access denied";
    case 404:
      return "API address not found";
    case 413:
      return "request too large";
    case 429:
      return "too many requests";
    case 500:
    case 502:
    case 503:
    case 504:
      return "server error";
    default:
      return status >= 400 ? "server refused the request" : "";
  }
}

}  // namespace freshrss
