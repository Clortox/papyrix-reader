#pragma once

// Test doubles for the FreshRSS core: a scripted byte transport and an
// in-memory article store. Header-only so each test binary can include it
// without a CMake entry.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Relative includes keep this header usable without adding the library include
// directory to every test target.
#include "../../lib/FreshRss/src/FreshRssArticle.h"
#include "../../lib/FreshRss/src/FreshRssHttp.h"
#include "../../lib/FreshRss/src/FreshRssSync.h"

namespace freshrss_test {

// Serves a fixed byte script. `chunkSize` limits how much one read returns so
// tests can exercise framing that spans reads.
class ScriptedTransport : public freshrss::Transport {
 public:
  std::string requests;
  std::string incoming;
  size_t offset = 0;
  size_t chunkSize = 0;
  bool failConnect = false;
  bool failRead = false;
  bool failWrite = false;
  // Fails the next write once, which is how a dropped keep-alive connection
  // shows up on the client side.
  bool failNextRequest = false;
  bool closeAtEnd = false;
  bool connected = false;
  std::vector<std::string> connectLog;

  void queue(const std::string& bytes) { incoming.append(bytes); }

  bool connect(const char* host, uint16_t port, bool tls, bool verify, const char* caPath,
               uint32_t timeoutMs) override {
    (void)timeoutMs;
    std::string entry =
        std::string(host) + ":" + std::to_string(port) + (tls ? ":tls" : ":plain") + (verify ? ":verify" : ":insecure");
    if (caPath) entry += std::string(":") + caPath;
    connectLog.push_back(entry);
    if (failConnect) return false;
    connected = true;
    return true;
  }

  void close() override { connected = false; }

  bool isConnected() override { return connected; }

  int read(char* out, size_t capacity) override {
    if (failRead) return -1;
    if (offset >= incoming.size()) {
      if (closeAtEnd) connected = false;
      return 0;
    }
    size_t count = std::min(capacity, incoming.size() - offset);
    if (chunkSize > 0) count = std::min(count, chunkSize);
    std::memcpy(out, incoming.data() + offset, count);
    offset += count;
    return static_cast<int>(count);
  }

  bool write(const char* data, size_t length) override {
    if (failNextRequest) {
      failNextRequest = false;
      connected = false;
      return false;
    }
    if (failWrite) return false;
    requests.append(data, length);
    return true;
  }

  bool peerClosed() override { return !connected; }
};

// Article store backed by maps. Keeps the same directory naming and prune
// ordering as the SD implementation.
class MemoryArticleStore : public freshrss::ArticleStore {
 public:
  std::map<std::string, std::string> files;  // "dir/name" -> content
  std::vector<std::string> created;
  std::vector<std::string> discarded;
  std::vector<std::string> pruned;
  uint64_t availableBytes = 8ull * 1024 * 1024;
  bool failCreate = false;
  bool failCommit = false;
  bool failDirectory = false;
  size_t pruneCalls = 0;

  class Sink : public freshrss::ArticleFile {
   public:
    Sink(MemoryArticleStore& store, std::string path) : store_(store), path_(std::move(path)) {}

    bool write(const char* data, size_t length) override {
      if (!open_) return false;
      content_.append(data, length);
      return true;
    }

    bool commit() override {
      if (!open_) return false;
      open_ = false;
      if (store_.failCommit) return false;
      store_.files[path_] = content_;
      return true;
    }

    void discard() override {
      open_ = false;
      store_.discarded.push_back(path_);
    }

    const std::string& path() const { return path_; }

   private:
    MemoryArticleStore& store_;
    std::string path_;
    std::string content_;
    bool open_ = true;
  };

  bool ensureFeedDirectory(const char* folder, const char* feedTitle, char* out, size_t capacity) override {
    if (failDirectory) return false;
    const size_t length = freshrss::buildFeedDirectory(out, capacity, folder, feedTitle);
    return length > 0;
  }

  bool articleExists(const char* directory, const char* fileName) override {
    return files.count(std::string(directory) + "/" + fileName) != 0;
  }

  freshrss::ArticleFile* createArticle(const char* directory, const char* fileName) override {
    if (failCreate) return nullptr;
    const std::string path = std::string(directory) + "/" + fileName;
    created.push_back(path);
    sink_ = std::make_unique<Sink>(*this, path);
    return sink_.get();
  }

  bool pruneFeed(const char* directory, uint16_t keep, uint16_t& removed) override {
    ++pruneCalls;
    removed = 0;
    const std::string prefix = std::string(directory) + "/";
    std::vector<std::string> names;
    for (const auto& entry : files) {
      if (entry.first.compare(0, prefix.size(), prefix) == 0) names.push_back(entry.first);
    }
    // File names start with an ISO date, so lexicographic order is chronological.
    std::sort(names.begin(), names.end());
    while (names.size() > keep) {
      const std::string victim = names.front();
      names.erase(names.begin());
      files.erase(victim);
      pruned.push_back(victim);
      ++removed;
    }
    return true;
  }

  uint64_t freeBytes() override { return availableBytes; }

 private:
  std::unique_ptr<Sink> sink_;
};

// A minimal HTTP/1.1 response with a Content-Length body.
inline std::string httpResponse(int status, const std::string& body, const std::string& extraHeaders = "") {
  std::string response = "HTTP/1.1 " + std::to_string(status) + " OK\r\n";
  response += "Content-Type: application/json\r\n";
  response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  response += extraHeaders;
  response += "\r\n";
  response += body;
  return response;
}

// Splits a body into chunked transfer-encoding.
inline std::string chunkedResponse(const std::string& body, size_t chunkSize = 16) {
  std::string response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
  size_t offset = 0;
  while (offset < body.size()) {
    const size_t count = std::min(chunkSize, body.size() - offset);
    char header[24];
    std::snprintf(header, sizeof(header), "%zx\r\n", count);
    response += header;
    response += body.substr(offset, count);
    response += "\r\n";
    offset += count;
  }
  response += "0\r\n\r\n";
  return response;
}

inline std::string loginResponse(const std::string& auth = "alice/8e6845e089457af25303abc6f53356eb60bdb5f8") {
  return httpResponse(200, "SID=" + auth + "\nLSID=null\nAuth=" + auth + "\n");
}

inline std::string subscriptionsBody() {
  return R"({"subscriptions":[
    {"id":"feed/12","title":"Ars Technica","categories":[{"id":"user/-/label/Tech","label":"Tech"}],"url":"https://a/feed","htmlUrl":"https://a/"},
    {"id":"feed/13","title":"Hacker News","categories":[],"url":"https://b/feed","htmlUrl":"https://b/"}
  ]})";
}

// A subscription list of `count` feeds, for rotation tests.
inline std::string subscriptionsBodyN(size_t count) {
  std::string body = "{\"subscriptions\":[";
  for (size_t i = 0; i < count; ++i) {
    if (i) body += ",";
    body += "{\"id\":\"feed/" + std::to_string(12 + i) + "\",\"title\":\"Feed " + std::to_string(i) +
            "\",\"categories\":[]}";
  }
  body += "]}";
  return body;
}

// One item with a body. `id` and `title` are injected for readability.
inline std::string itemsBody(const std::string& id, const std::string& title, const std::string& content,
                             const std::string& extra = "") {
  return "{\"id\":\"user/-/state/com.google/reading-list\",\"updated\":1700000000,\"items\":[{"
         "\"id\":\"" +
         id + "\",\"crawlTimeMsec\":\"1700000000000\",\"published\":1700000000,\"title\":\"" + title +
         "\",\"canonical\":[{\"href\":\"https://example.com/post\"}],"
         "\"origin\":{\"streamId\":\"feed/12\",\"title\":\"Ars Technica\"},"
         "\"summary\":{\"content\":\"" +
         content + "\"}" + extra + "}]}";
}

}  // namespace freshrss_test
