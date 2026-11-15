// Shared host-side pieces for the sync CLIs: a libcurl transport that speaks the
// same Transport/HttpSession interface as the firmware, a filesystem article
// store that writes the same layout the device uses, and small helpers.
//
// Because both the transport and the store implement the firmware's interfaces,
// the CLIs run the very same sync code as the device.

#pragma once

#include <FreshRssArticle.h>
#include <FreshRssConfig.h>
#include <FreshRssHttp.h>
#include <FreshRssSync.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace syncli {

// Transport over libcurl. The firmware's HTTP layer writes a whole request and
// then reads the response, so this buffers the request, performs it with curl on
// the first read, and serves a synthesized HTTP response the shared parser can
// read. Redirects are followed by curl.
class HostTransport : public freshrss::Transport {
 public:
  HostTransport() = default;
  ~HostTransport() override;

  bool connect(const char* host, uint16_t port, bool tls, bool verify, const char* caPath,
               uint32_t timeoutMs) override;
  void close() override;
  bool isConnected() override;
  int read(char* out, size_t capacity) override;
  bool write(const char* data, size_t length) override;
  bool peerClosed() override;

  const char* error() const { return error_.c_str(); }
  // Status of the response curl last delivered (after redirects).
  int lastStatus() const { return lastStatus_; }

 private:
  bool perform();
  void resetRequest();

  std::string host_;
  uint16_t port_ = 443;
  bool tls_ = true;
  bool verify_ = true;
  uint32_t timeoutMs_ = 30000;
  std::string caPath_;

  std::string request_;
  std::string response_;
  std::string headerBlock_;
  size_t offset_ = 0;
  bool performed_ = false;
  bool responseServed_ = false;
  int lastStatus_ = 0;
  std::string error_;
  void* curl_ = nullptr;  // CURL*, kept out of the header
};

// Article store writing to a directory tree, so the card layout matches what the
// device writes and re-syncs skip existing files.
class FsArticleStore : public freshrss::ArticleStore {
 public:
  explicit FsArticleStore(std::string root);
  ~FsArticleStore() override;

  bool ensureFeedDirectory(const char* folder, const char* feedTitle, char* out, size_t capacity) override;
  bool articleExists(const char* directory, const char* fileName) override;
  freshrss::ArticleFile* createArticle(const char* directory, const char* fileName) override;
  bool pruneFeed(const char* directory, uint16_t keep, uint16_t& removed) override;
  uint64_t freeBytes() override;

  void setDryRun(bool dryRun) { dryRun_ = dryRun; }
  bool dryRun() const { return dryRun_; }
  // Files that would be written or were written, for the summary.
  std::vector<std::string>& created() { return created_; }
  std::vector<std::string>& pruned() { return pruned_; }

 private:
  class File;

  std::string root_;
  std::unique_ptr<File> sink_;
  bool dryRun_ = false;
  std::vector<std::string> created_;
  std::vector<std::string> pruned_;
};

// --- small helpers ---------------------------------------------------------

// Reads a whole file. Returns false when it cannot be read.
bool readFile(const std::string& path, std::string& out);

// Reads the device state file (last sync, window, rotation cursor, read markers).
// Returns true when the file is absent or decoded.
bool loadState(const std::string& path, std::string& text);

// Writes a file atomically enough for a card: write, flush, close.
bool writeFile(const std::string& path, const std::string& text, std::string& error);

// Creates a directory and its parents.
bool makeDirectories(const std::string& path, std::string& error);

// Command line: --flag, --key=value and --key value are all accepted.
struct Args {
  std::vector<std::string> items;
};

Args parseArgs(int argc, char** argv);
bool hasOption(const Args& args, const std::string& name);
std::string optionValue(const Args& args, const std::string& name, const std::string& fallback = "");

// Current time as Unix seconds, or 0 when the clock is unusable.
uint32_t nowEpoch();

std::string joinPath(const std::string& a, const std::string& b);

}  // namespace syncli
