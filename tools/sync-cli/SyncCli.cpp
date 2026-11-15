#include "SyncCli.h"

#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace syncli {
namespace {

constexpr const char* kUserAgent = "PapyriX-Sync-CLI/1.0";

size_t writeCallback(char* data, size_t size, size_t count, void* userdata) {
  static_cast<std::string*>(userdata)->append(data, size * count);
  return size * count;
}

size_t headerCallback(char* data, size_t size, size_t count, void* userdata) {
  auto* block = static_cast<std::string*>(userdata);
  const std::string line(data, size * count);
  // Redirects produce several header blocks; keep the last one.
  if (line.rfind("HTTP/", 0) == 0) block->clear();
  *block += line;
  return size * count;
}

std::string trim(const std::string& text) {
  size_t start = 0;
  size_t end = text.size();
  while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r')) ++start;
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
  return text.substr(start, end - start);
}

bool isHtmlName(const std::string& name) {
  if (name.size() < 6 || name[0] == '.') return false;
  std::string tail = name.substr(name.size() - 5);
  std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return std::tolower(c); });
  return tail == ".html";
}

}  // namespace

// --- HostTransport ---------------------------------------------------------

HostTransport::~HostTransport() { close(); }

bool HostTransport::connect(const char* host, uint16_t port, bool tls, bool verify, const char* caPath,
                            uint32_t timeoutMs) {
  close();
  host_ = host ? host : "";
  port_ = port;
  tls_ = tls;
  verify_ = verify;
  caPath_ = caPath ? caPath : "";
  timeoutMs_ = timeoutMs == 0 ? 30000 : timeoutMs;
  error_.clear();
  if (host_.empty()) {
    error_ = "no host";
    return false;
  }
  // The request is performed on the first read, so nothing to do yet.
  performed_ = false;
  responseServed_ = false;
  return true;
}

void HostTransport::close() {
  if (curl_) {
    curl_easy_cleanup(static_cast<CURL*>(curl_));
    curl_ = nullptr;
  }
  request_.clear();
  response_.clear();
  headerBlock_.clear();
  offset_ = 0;
  performed_ = false;
  responseServed_ = false;
}

bool HostTransport::isConnected() { return !host_.empty(); }

bool HostTransport::write(const char* data, size_t length) {
  if (!data || length == 0) return true;
  if (responseServed_) resetRequest();
  request_.append(data, length);
  return true;
}

int HostTransport::read(char* out, size_t capacity) {
  if (capacity == 0) return 0;
  if (!performed_ && !perform()) return -1;
  if (offset_ >= response_.size()) {
    responseServed_ = true;
    return 0;
  }
  const size_t count = std::min(capacity, response_.size() - offset_);
  std::memcpy(out, response_.data() + offset_, count);
  offset_ += count;
  return static_cast<int>(count);
}

bool HostTransport::peerClosed() { return responseServed_ || offset_ >= response_.size(); }

void HostTransport::resetRequest() {
  request_.clear();
  response_.clear();
  headerBlock_.clear();
  offset_ = 0;
  performed_ = false;
  responseServed_ = false;
}

bool HostTransport::perform() {
  performed_ = true;
  response_.clear();
  headerBlock_.clear();
  offset_ = 0;

  // Split the buffered request into its head and body.
  const size_t split = request_.find("\r\n\r\n");
  if (split == std::string::npos) {
    error_ = "malformed request";
    return false;
  }
  const std::string head = request_.substr(0, split);
  const std::string body = request_.substr(split + 4);

  std::istringstream lines(head);
  std::string requestLine;
  std::getline(lines, requestLine);
  requestLine = trim(requestLine);
  const size_t firstSpace = requestLine.find(' ');
  const size_t secondSpace = requestLine.find(' ', firstSpace == std::string::npos ? 0 : firstSpace + 1);
  if (firstSpace == std::string::npos) {
    error_ = "malformed request line";
    return false;
  }
  const std::string method = requestLine.substr(0, firstSpace);
  const std::string target = secondSpace == std::string::npos
                                 ? requestLine.substr(firstSpace + 1)
                                 : requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);

  struct curl_slist* headers = nullptr;
  std::string line;
  while (std::getline(lines, line)) {
    line = trim(line);
    if (line.empty()) continue;
    const std::string lower = [&line] {
      std::string copy = line;
      std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char c) { return std::tolower(c); });
      return copy;
    }();
    // curl owns the connection and the body length.
    if (lower.rfind("host:", 0) == 0 || lower.rfind("connection:", 0) == 0 || lower.rfind("content-length:", 0) == 0)
      continue;
    headers = curl_slist_append(headers, line.c_str());
  }

  std::string url = tls_ ? "https://" : "http://";
  if (host_.find(':') != std::string::npos)
    url += "[" + host_ + "]";
  else
    url += host_;
  const bool defaultPort = (tls_ && port_ == 443) || (!tls_ && port_ == 80);
  if (!defaultPort) url += ":" + std::to_string(port_);
  url += target;

  CURL* curl = static_cast<CURL*>(curl_);
  if (!curl) {
    curl = curl_easy_init();
    if (!curl) {
      error_ = "cannot initialize libcurl";
      if (headers) curl_slist_free_all(headers);
      return false;
    }
    curl_ = curl;
  } else {
    curl_easy_reset(curl);
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs_));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headerBlock_);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify_ ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_ ? 2L : 0L);
  if (!caPath_.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, caPath_.c_str());
  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    if (method == "HEAD") curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  } else if (method == "GET") {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  }

  const CURLcode result = curl_easy_perform(curl);
  if (headers) curl_slist_free_all(headers);
  if (result != CURLE_OK) {
    error_ = curl_easy_strerror(result);
    return false;
  }
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  lastStatus_ = static_cast<int>(status);

  // Rebuild a plain HTTP/1.1 response for the shared parser: the body curl gave
  // us is already de-chunked, so its length is authoritative.
  std::string rebuilt;
  const size_t headerEnd = headerBlock_.find("\r\n");
  if (headerBlock_.rfind("HTTP/", 0) == 0 && headerEnd != std::string::npos) {
    rebuilt = headerBlock_.substr(0, headerEnd + 2);
    std::istringstream headerLines(headerBlock_.substr(headerEnd + 2));
    std::string headerLine;
    while (std::getline(headerLines, headerLine)) {
      headerLine = trim(headerLine);
      if (headerLine.empty()) continue;
      const std::string lower = [&headerLine] {
        std::string copy = headerLine;
        std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char c) { return std::tolower(c); });
        return copy;
      }();
      if (lower.rfind("transfer-encoding:", 0) == 0 || lower.rfind("content-length:", 0) == 0 ||
          lower.rfind("connection:", 0) == 0)
        continue;
      rebuilt += headerLine + "\r\n";
    }
  } else {
    rebuilt = "HTTP/1.1 " + std::to_string(status) + " OK\r\nContent-Type: application/octet-stream\r\n";
  }
  rebuilt += "Connection: close\r\nContent-Length: " + std::to_string(response_.size()) + "\r\n\r\n";
  response_ = rebuilt + response_;
  return true;
}

// --- FsArticleStore --------------------------------------------------------

class FsArticleStore::File : public freshrss::ArticleFile {
 public:
  File(FsArticleStore& store, std::string path) : store_(store), path_(std::move(path)) {}

  bool write(const char* data, size_t length) override {
    if (!open_) return false;
    content_.append(data, length);
    return true;
  }

  bool commit() override {
    open_ = false;
    if (store_.dryRun()) {
      store_.created().push_back(path_);
      return true;
    }
    std::string error;
    if (!writeFile(path_, content_, error)) {
      std::fprintf(stderr, "  ! %s\n", error.c_str());
      return false;
    }
    store_.created().push_back(path_);
    return true;
  }

  void discard() override { open_ = false; }

 private:
  FsArticleStore& store_;
  std::string path_;
  std::string content_;
  bool open_ = true;
};

FsArticleStore::FsArticleStore(std::string root) : root_(std::move(root)) {}

FsArticleStore::~FsArticleStore() = default;

bool FsArticleStore::ensureFeedDirectory(const char* folder, const char* feedTitle, char* out, size_t capacity) {
  char directory[512];
  if (freshrss::buildFeedDirectory(directory, sizeof(directory), folder, feedTitle) == 0) return false;
  // Directories are reported the way the device reports them (card absolute,
  // "/RSS/<feed>"); the host root is added only when touching the filesystem.
  const std::string path = joinPath(root_, directory);
  std::string error;
  if (!dryRun_ && !makeDirectories(path, error)) {
    std::fprintf(stderr, "  ! %s\n", error.c_str());
    return false;
  }
  if (std::strlen(directory) >= capacity) return false;
  std::snprintf(out, capacity, "%s", directory);
  return true;
}

bool FsArticleStore::articleExists(const char* directory, const char* fileName) {
  std::error_code code;
  return fs::exists(joinPath(root_, std::string(directory) + "/" + fileName), code);
}

freshrss::ArticleFile* FsArticleStore::createArticle(const char* directory, const char* fileName) {
  const std::string path = joinPath(root_, std::string(directory) + "/" + fileName);
  if (!dryRun_) {
    std::string error;
    if (!makeDirectories(fs::path(path).parent_path().string(), error)) {
      std::fprintf(stderr, "  ! %s\n", error.c_str());
      return nullptr;
    }
  }
  sink_ = std::make_unique<File>(*this, path);
  return sink_.get();
}

bool FsArticleStore::pruneFeed(const char* directory, uint16_t keep, uint16_t& removed) {
  removed = 0;
  std::error_code code;
  const std::string path = joinPath(root_, directory);
  if (!fs::is_directory(path, code)) return true;

  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(path, code)) {
    if (!entry.is_regular_file(code)) continue;
    const std::string name = entry.path().filename().string();
    if (isHtmlName(name)) names.push_back(name);
  }
  // File names start with an ISO date, so lexical order is chronological.
  std::sort(names.begin(), names.end());
  while (names.size() > keep) {
    const std::string victim = joinPath(path, names.front());
    names.erase(names.begin());
    if (dryRun_) {
      pruned_.push_back(victim);
      ++removed;
      continue;
    }
    if (fs::remove(victim, code)) {
      pruned_.push_back(victim);
      ++removed;
    }
  }
  return true;
}

uint64_t FsArticleStore::freeBytes() {
  std::error_code code;
  const fs::space_info info = fs::space(root_, code);
  if (code) return 0;
  return info.available;
}

// --- helpers ---------------------------------------------------------------

bool readFile(const std::string& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

bool loadState(const std::string& path, std::string& text) {
  if (!fs::exists(path)) {
    text.clear();
    return true;
  }
  return readFile(path, text);
}

bool writeFile(const std::string& path, const std::string& text, std::string& error) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    error = "cannot open " + path;
    return false;
  }
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  file.flush();
  if (!file) {
    error = "cannot write " + path;
    return false;
  }
  return true;
}

bool makeDirectories(const std::string& path, std::string& error) {
  if (path.empty()) return true;
  std::error_code code;
  fs::create_directories(path, code);
  if (code && !fs::is_directory(path)) {
    error = "cannot create " + path + ": " + code.message();
    return false;
  }
  return true;
}

Args parseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) args.items.emplace_back(argv[i]);
  return args;
}

bool hasOption(const Args& args, const std::string& name) {
  for (const std::string& item : args.items) {
    if (item == name) return true;
    if (item.rfind(name + "=", 0) == 0) return true;
  }
  return false;
}

std::string optionValue(const Args& args, const std::string& name, const std::string& fallback) {
  for (size_t i = 0; i < args.items.size(); ++i) {
    const std::string& item = args.items[i];
    if (item.rfind(name + "=", 0) == 0) return item.substr(name.size() + 1);
    if (item == name && i + 1 < args.items.size()) return args.items[i + 1];
  }
  return fallback;
}

uint32_t nowEpoch() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  return seconds > 0 ? static_cast<uint32_t>(seconds) : 0;
}

std::string joinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (a.back() == '/' && b.front() == '/') return a + b.substr(1);
  if (a.back() != '/' && b.front() != '/') return a + "/" + b;
  return a + b;
}

}  // namespace syncli
