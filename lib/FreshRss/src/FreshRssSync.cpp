#include "FreshRssSync.h"

#include <cstdio>
#include <cstring>

#include "FreshRssText.h"

namespace freshrss {
namespace {

// FreshRSS sends feed titles with HTML entities kept for compatibility.
void decodeBasicEntities(char* text) {
  struct Entity {
    const char* encoded;
    char decoded;
  };
  static const Entity kEntities[] = {{"&amp;", '&'},  {"&lt;", '<'},   {"&gt;", '>'},
                                     {"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''}};
  for (const Entity& entity : kEntities) {
    const size_t encodedLength = std::strlen(entity.encoded);
    char* cursor = text;
    while ((cursor = std::strstr(cursor, entity.encoded)) != nullptr) {
      *cursor = entity.decoded;
      std::memmove(cursor + 1, cursor + encodedLength, std::strlen(cursor + encodedLength) + 1);
      ++cursor;
    }
  }
}

// Percent-encodes one path segment, keeping the characters FreshRSS uses in
// stream ids ("feed/12") and item ids intact.
size_t pathEncodeAppend(char* out, size_t capacity, size_t written, const char* text) {
  static const char* kHex = "0123456789ABCDEF";
  for (const char* p = text; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                      c == '_' || c == '.' || c == '~' || c == '/' || c == ',' || c == ':' || c == '@' || c == '+';
    if (safe) {
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

}  // namespace

void SubscriptionCollector::assignString() {
  if (feedDepth_ < 0 || depth_ != feedDepth_) return;
  switch (field_) {
    case Field::Id:
      copyBounded(current_.id, sizeof(current_.id), scanner_.text(), scanner_.length());
      break;
    case Field::Title:
      copyBounded(current_.title, sizeof(current_.title), scanner_.text(), scanner_.length());
      decodeBasicEntities(current_.title);
      break;
    default:
      break;
  }
}

void SubscriptionCollector::handle(const JsonScanner::Token& token) {
  using Token = JsonScanner::Token;
  switch (token) {
    case Token::ObjectBegin:
      ++depth_;
      if (subscriptionsDepth_ >= 0 && depth_ == subscriptionsDepth_ + 1) {
        current_ = Feed{};
        feedDepth_ = depth_;
        field_ = Field::None;
      }
      break;
    case Token::ObjectEnd:
      if (feedDepth_ >= 0 && depth_ == feedDepth_) {
        if (current_.id[0] != '\0' && feeds_.count < FeedList::kMaxFeeds) {
          if (current_.title[0] == '\0') copyBounded(current_.title, sizeof(current_.title), current_.id);
          feeds_.feeds[feeds_.count++] = current_;
        }
        feedDepth_ = -1;
      }
      if (depth_ > 0) --depth_;
      break;
    case Token::ArrayBegin:
      ++depth_;
      if (pendingSubscriptionsKey_) {
        subscriptionsDepth_ = depth_;
        pendingSubscriptionsKey_ = false;
      }
      break;
    case Token::ArrayEnd:
      if (subscriptionsDepth_ >= 0 && depth_ == subscriptionsDepth_) subscriptionsDepth_ = -1;
      if (depth_ > 0) --depth_;
      break;
    case Token::Key:
      field_ = Field::None;
      if (depth_ == subscriptionsDepth_ + 1) {
        if (equalsIgnoreCase(scanner_.text(), "id"))
          field_ = Field::Id;
        else if (equalsIgnoreCase(scanner_.text(), "title"))
          field_ = Field::Title;
      }
      pendingSubscriptionsKey_ = equalsIgnoreCase(scanner_.text(), "subscriptions");
      break;
    case Token::StringEnd:
      assignString();
      break;
    case Token::Error:
      failed_ = true;
      copyBounded(error_, sizeof(error_), scanner_.text());
      break;
    default:
      break;
  }
}

void SubscriptionCollector::feed(const char* data, size_t length) {
  if (failed_ || !data) return;
  for (size_t i = 0; i < length; ++i) {
    const JsonScanner::Token token = scanner_.feed(data[i]);
    if (token != JsonScanner::Token::None) handle(token);
    if (failed_) return;
  }
}

bool SubscriptionCollector::finish() {
  if (failed_) return false;
  const JsonScanner::Token token = scanner_.finish();
  if (token != JsonScanner::Token::None) handle(token);
  if (failed_) return false;
  if (scanner_.failed()) {
    failed_ = true;
    copyBounded(error_, sizeof(error_), scanner_.text());
    return false;
  }
  return true;
}

void StreamCollector::beginItemObject() {
  current_ = ItemInfo{};
  itemDepth_ = depth_;
  originDepth_ = -1;
  summaryDepth_ = -1;
  contentDepth_ = -1;
  canonicalDepth_ = -1;
  alternateDepth_ = -1;
  itemOpen_ = false;
  itemHadContent_ = false;
  itemSkipped_ = false;
  sawCanonical_ = false;
  inContent_ = false;
  field_ = Field::None;
  lastKey_[0] = '\0';
}

void StreamCollector::endItemObject() {
  if (!itemOpen_) {
    // No usable body arrived: report the item so the caller can count it as
    // skipped without creating an empty file.
    sink_.endItem(current_, false);
  } else {
    sink_.endItem(current_, itemHadContent_ && !itemSkipped_);
  }
  itemDepth_ = -1;
  inContent_ = false;
  itemCount_++;
}

void StreamCollector::assignString() {
  if (inContent_) {
    // A string value inside summary/content is the article body.
    if (itemOpen_ && !itemSkipped_ && scanner_.length() > 0) {
      if (!sink_.contentChunk(scanner_.text(), scanner_.length()))
        itemSkipped_ = true;
      else
        itemHadContent_ = true;
    }
    return;
  }
  if (itemDepth_ < 0) {
    if (field_ == Field::Continuation) {
      copyBounded(continuation_, sizeof(continuation_), scanner_.text(), scanner_.length());
    }
    return;
  }
  if (depth_ == itemDepth_) {
    switch (field_) {
      case Field::Id:
        copyBounded(current_.id, sizeof(current_.id), scanner_.text(), scanner_.length());
        break;
      case Field::Title:
        copyBounded(current_.title, sizeof(current_.title), scanner_.text(), scanner_.length());
        decodeBasicEntities(current_.title);
        break;
      case Field::Author:
        copyBounded(current_.author, sizeof(current_.author), scanner_.text(), scanner_.length());
        decodeBasicEntities(current_.author);
        break;
      case Field::CrawlTime:
        if (current_.publishedEpoch == 0) {
          // Milliseconds since the epoch; only the seconds are stored.
          uint64_t millis = 0;
          for (const char* p = scanner_.text(); *p >= '0' && *p <= '9'; ++p) millis = millis * 10 + (*p - '0');
          if (millis > 0) current_.publishedEpoch = static_cast<uint32_t>(millis / 1000ull);
        }
        break;
      default:
        break;
    }
    return;
  }
  if (field_ != Field::Link) return;
  const bool canonical = canonicalDepth_ >= 0 && depth_ == canonicalDepth_ + 1;
  const bool alternate = alternateDepth_ >= 0 && depth_ == alternateDepth_ + 1;
  if (!canonical && !alternate) return;
  if (alternate && sawCanonical_) return;
  copyBounded(current_.link, sizeof(current_.link), scanner_.text(), scanner_.length());
  if (canonical) sawCanonical_ = true;
}

void StreamCollector::assignNumber(long long value) {
  if (itemDepth_ >= 0 && depth_ == itemDepth_ && field_ == Field::Published && value > 0 && value <= 4294967295LL) {
    current_.publishedEpoch = static_cast<uint32_t>(value);
  }
}

void StreamCollector::handle(const JsonScanner::Token& token) {
  using Token = JsonScanner::Token;
  switch (token) {
    case Token::ObjectBegin:
      ++depth_;
      if (itemsDepth_ >= 0 && depth_ == itemsDepth_ + 1) {
        beginItemObject();
      } else if (itemDepth_ >= 0 && depth_ == itemDepth_ + 1) {
        if (equalsIgnoreCase(lastKey_, "origin"))
          originDepth_ = depth_;
        else if (equalsIgnoreCase(lastKey_, "summary"))
          summaryDepth_ = depth_;
        else if (equalsIgnoreCase(lastKey_, "content"))
          contentDepth_ = depth_;
      } else if (canonicalDepth_ >= 0 && depth_ == canonicalDepth_ + 1) {
        // object inside the canonical array
      } else if (alternateDepth_ >= 0 && depth_ == alternateDepth_ + 1) {
        // object inside the alternate array
      }
      break;
    case Token::ObjectEnd:
      if (itemDepth_ >= 0 && depth_ == itemDepth_) {
        endItemObject();
      } else if (originDepth_ >= 0 && depth_ == originDepth_) {
        originDepth_ = -1;
      } else if (summaryDepth_ >= 0 && depth_ == summaryDepth_) {
        summaryDepth_ = -1;
        inContent_ = false;
      } else if (contentDepth_ >= 0 && depth_ == contentDepth_) {
        contentDepth_ = -1;
        inContent_ = false;
      }
      if (depth_ > 0) --depth_;
      break;
    case Token::ArrayBegin:
      ++depth_;
      if (pendingItemsKey_) {
        itemsDepth_ = depth_;
        pendingItemsKey_ = false;
      } else if (itemDepth_ >= 0 && depth_ == itemDepth_ + 1) {
        if (equalsIgnoreCase(lastKey_, "canonical"))
          canonicalDepth_ = depth_;
        else if (equalsIgnoreCase(lastKey_, "alternate"))
          alternateDepth_ = depth_;
      }
      break;
    case Token::ArrayEnd:
      if (itemsDepth_ >= 0 && depth_ == itemsDepth_) itemsDepth_ = -1;
      if (canonicalDepth_ >= 0 && depth_ == canonicalDepth_) canonicalDepth_ = -1;
      if (alternateDepth_ >= 0 && depth_ == alternateDepth_) alternateDepth_ = -1;
      if (depth_ > 0) --depth_;
      break;
    case Token::Key:
      field_ = Field::None;
      copyBounded(lastKey_, sizeof(lastKey_), scanner_.text(), scanner_.length());
      if (itemDepth_ >= 0 && depth_ == itemDepth_) {
        if (equalsIgnoreCase(lastKey_, "id"))
          field_ = Field::Id;
        else if (equalsIgnoreCase(lastKey_, "title"))
          field_ = Field::Title;
        else if (equalsIgnoreCase(lastKey_, "author"))
          field_ = Field::Author;
        else if (equalsIgnoreCase(lastKey_, "published"))
          field_ = Field::Published;
        else if (equalsIgnoreCase(lastKey_, "crawlTimeMsec"))
          field_ = Field::CrawlTime;
      } else if (itemDepth_ >= 0 && (depth_ == summaryDepth_ || depth_ == contentDepth_) &&
                 equalsIgnoreCase(lastKey_, "content")) {
        inContent_ = true;
        // Metadata that precedes the body is now known; open the destination.
        if (!itemOpen_) {
          itemOpen_ = true;
          itemSkipped_ = !sink_.beginItem(current_);
        }
      } else if (itemDepth_ >= 0 && depth_ == canonicalDepth_ + 1 && equalsIgnoreCase(lastKey_, "href")) {
        field_ = Field::Link;
      } else if (itemDepth_ >= 0 && depth_ == alternateDepth_ + 1 && equalsIgnoreCase(lastKey_, "href")) {
        field_ = Field::Link;
      } else if (itemDepth_ < 0 && depth_ == 1 && equalsIgnoreCase(lastKey_, "continuation")) {
        field_ = Field::Continuation;
      }
      pendingItemsKey_ = equalsIgnoreCase(lastKey_, "items");
      break;
    case Token::StringChunk:
      if (inContent_ && itemOpen_ && !itemSkipped_) {
        if (!sink_.contentChunk(scanner_.text(), scanner_.length()))
          itemSkipped_ = true;
        else
          itemHadContent_ = true;
      }
      break;
    case Token::StringEnd:
      assignString();
      break;
    case Token::Number:
      assignNumber(scanner_.numberValue());
      break;
    case Token::Error:
      failed_ = true;
      copyBounded(error_, sizeof(error_), scanner_.text());
      break;
    default:
      break;
  }
}

void StreamCollector::feed(const char* data, size_t length) {
  if (failed_ || !data) return;
  for (size_t i = 0; i < length; ++i) {
    const JsonScanner::Token token = scanner_.feed(data[i]);
    if (token != JsonScanner::Token::None) handle(token);
    if (failed_) return;
  }
}

bool StreamCollector::finish() {
  if (failed_) return false;
  const JsonScanner::Token token = scanner_.finish();
  if (token != JsonScanner::Token::None) handle(token);
  if (failed_) return false;
  if (scanner_.failed()) {
    failed_ = true;
    copyBounded(error_, sizeof(error_), scanner_.text());
    return false;
  }
  return true;
}

namespace {

// Appends literal (already encoded) text and keeps the NUL terminator.
// Returns the new length, or 0 when it does not fit.
size_t appendLiteral(char* out, size_t capacity, size_t written, const char* text) {
  const size_t length = std::strlen(text);
  if (written + length + 1 > capacity) return 0;
  std::memcpy(out + written, text, length);
  out[written + length] = '\0';
  return written + length;
}

size_t appendUnsigned(char* out, size_t capacity, size_t written, uint32_t value) {
  char number[16];
  const int length = std::snprintf(number, sizeof(number), "%u", static_cast<unsigned>(value));
  if (length <= 0) return 0;
  return appendLiteral(out, capacity, written, number);
}

// Leaves room for the largest single article plus filesystem slack.
constexpr uint64_t kSpaceReserve = 32ull * 1024ull;
constexpr size_t kBodyStreamBuffer = 256;

}  // namespace

bool ArticleDownloader::beginItem(const ItemInfo& info) {
  if (fatal_ || open_) return false;
  current_ = &info;
  skipped_ = false;
  authorAtBegin_[0] = '\0';
  if (info.id[0] == '\0') {
    // Without an item id there is no stable file name and no way to avoid
    // re-downloading the same article on every sync.
    skipped_ = true;
    return false;
  }
  const uint32_t epoch = info.publishedEpoch != 0 ? info.publishedEpoch : fallbackEpoch_;
  const uint64_t hash = hashItemId(info.id);
  if (buildArticleFileName(fileName_, sizeof(fileName_), info.title, epoch, fallbackEpoch_, hash) == 0) {
    fatal_ = true;
    return false;
  }
  if (store_.articleExists(directory_, fileName_)) {
    stats_.articlesSkipped++;
    skipped_ = true;
    return false;
  }
  if (store_.freeBytes() < static_cast<uint64_t>(config_.maxArticleBytes) + kSpaceReserve) {
    fatal_ = true;
    return false;
  }
  file_ = store_.createArticle(directory_, fileName_);
  if (!file_) {
    fatal_ = true;
    return false;
  }
  if (!writer_.begin(*file_, config_.maxArticleBytes, info.title, feed_.title, info.author, info.link, epoch,
                     info.id)) {
    file_->discard();
    file_ = nullptr;
    stats_.articlesFailed++;
    return false;
  }
  copyBounded(authorAtBegin_, sizeof(authorAtBegin_), info.author);
  open_ = true;
  return true;
}

bool ArticleDownloader::contentChunk(const char* data, size_t length) {
  if (!open_) return false;
  if (!writer_.writeContent(data, length)) {
    // The article exceeded the per-file budget: keep the card clean and count it
    // as failed rather than storing half a post.
    file_->discard();
    file_ = nullptr;
    open_ = false;
    stats_.articlesFailed++;
    return false;
  }
  return true;
}

void ArticleDownloader::endItem(const ItemInfo& info, bool complete) {
  if (!open_) {
    if (!skipped_ && !fatal_) stats_.articlesFailed++;
    current_ = nullptr;
    return;
  }
  const bool authorArrivedLate = info.author[0] != '\0' && std::strcmp(info.author, authorAtBegin_) != 0;
  if (authorArrivedLate) writer_.writeAuthorLine(info.author);
  const bool stored = complete && writer_.end();
  if (!stored || !file_->commit()) {
    file_->discard();
    stats_.articlesFailed++;
  } else {
    stats_.articlesStored++;
    stats_.bytesStored += static_cast<uint32_t>(writer_.bytesWritten());
    if (hooks_.stored) {
      StoredArticle article;
      article.feedTitle = feed_.title;
      article.fileName = fileName_;
      article.title = info.title;
      article.bytes = static_cast<uint32_t>(writer_.bytesWritten());
      if (!hooks_.stored(hooks_.ctx, article)) abort_ = true;
    }
  }
  file_ = nullptr;
  open_ = false;
  current_ = nullptr;
}

void SyncSession::report(SyncStage stage, size_t feedIndex, size_t feedCount, const char* feedTitle) {
  if (!hooks_.progress) return;
  SyncProgress progress;
  progress.stage = stage;
  progress.feedIndex = feedIndex;
  progress.feedCount = feedCount;
  progress.feedTitle = feedTitle ? feedTitle : "";
  progress.articlesStored = stats_.articlesStored;
  progress.articlesSkipped = stats_.articlesSkipped;
  if (!hooks_.progress(hooks_.ctx, progress)) aborted_ = true;
}

bool SyncSession::ensureConnected(SyncOutcome& outcome) {
  if (transport_.isConnected()) return true;
  http_.reset();
  const char* caCert = config_.caCert[0] != '\0' ? config_.caCert : nullptr;
  if (!transport_.connect(endpoint_.host, endpoint_.port, endpoint_.tls, config_.verifyTls, caCert, 20000)) {
    outcome.status = SyncStatus::NetworkError;
    copyBounded(outcome.message, sizeof(outcome.message), "cannot reach server");
    return false;
  }
  return true;
}

bool SyncSession::send(const char* method, const char* apiPath, const char* query, const char* authorization,
                       const char* body, size_t bodyLength, SyncOutcome& outcome) {
  if (!buildRequestPath(path_, sizeof(path_), endpoint_.basePath, apiPath, query)) {
    outcome.status = SyncStatus::BadResponse;
    copyBounded(outcome.message, sizeof(outcome.message), "request too long");
    return false;
  }
  if (!buildHostHeader(host_, sizeof(host_), endpoint_.host, endpoint_.port, endpoint_.tls)) {
    outcome.status = SyncStatus::BadResponse;
    copyBounded(outcome.message, sizeof(outcome.message), "bad host");
    return false;
  }

  HttpRequest request;
  request.method = method;
  request.path = path_;
  request.authorization = authorization;
  request.body = body;
  request.bodyLength = bodyLength;
  request.host = host_;
  request.keepAlive = true;

  HttpResponseHead head;
  char error[64];
  if (!http_.request(request, head, error, sizeof(error))) {
    // A server that dropped the keep-alive connection is the common case here:
    // reconnect once and repeat the request before reporting a network error.
    http_.reset();
    transport_.close();
    if (ensureConnected(outcome) && http_.request(request, head, error, sizeof(error))) return true;
    if (outcome.status == SyncStatus::Ok) {
      outcome.status = SyncStatus::NetworkError;
      copyBounded(outcome.message, sizeof(outcome.message), error[0] ? error : "request failed");
    }
    return false;
  }
  return true;
}

bool SyncSession::checkStatus(SyncOutcome& outcome, const char* what) {
  const int status = http_.head().status;
  if (status == 200) return true;
  // The caller will not read this body; leaving it would desynchronise the next
  // request on the same connection.
  http_.drainBody();
  outcome.httpStatus = status;
  if (status == 401 || status == 403) {
    outcome.status = SyncStatus::LoginFailed;
    copyBounded(outcome.message, sizeof(outcome.message), "API password rejected");
    return false;
  }
  outcome.status = SyncStatus::HttpError;
  char message[80];
  std::snprintf(message, sizeof(message), "%s: %s (%d)", what, describeHttpStatus(status), status);
  copyBounded(outcome.message, sizeof(outcome.message), message);
  return false;
}

bool SyncSession::login(SyncOutcome& outcome) {
  report(SyncStage::Login, 0, 0, "");
  size_t written = appendLiteral(body_, sizeof(body_), 0, "Email=");
  written = written ? formEncodeAppend(body_, sizeof(body_), written, config_.user) : 0;
  written = written ? appendLiteral(body_, sizeof(body_), written, "&Passwd=") : 0;
  written = written ? formEncodeAppend(body_, sizeof(body_), written, config_.apiPassword) : 0;
  if (written == 0) {
    outcome.status = SyncStatus::NoConfig;
    copyBounded(outcome.message, sizeof(outcome.message), "credentials too long");
    return false;
  }
  if (!send("POST", "/accounts/ClientLogin", nullptr, nullptr, body_, written, outcome)) return false;
  if (!checkStatus(outcome, "login")) return false;

  char response[512];
  const int length = http_.readBodyAll(response, sizeof(response));
  if (length <= 0) {
    outcome.status = SyncStatus::BadResponse;
    copyBounded(outcome.message, sizeof(outcome.message), "empty login response");
    return false;
  }
  const char* found = nullptr;
  for (const char* p = response; *p; ++p) {
    if ((p == response || p[-1] == '\n') && std::strncmp(p, "Auth=", 5) == 0) {
      found = p + 5;
      break;
    }
  }
  if (!found || found[0] == '\0' || found[0] == '\n' || found[0] == '\r') {
    outcome.status = SyncStatus::LoginFailed;
    copyBounded(outcome.message, sizeof(outcome.message), "API password rejected");
    return false;
  }
  char value[128];
  size_t valueLength = 0;
  while (found[valueLength] && found[valueLength] != '\n' && found[valueLength] != '\r' &&
         valueLength + 1 < sizeof(value))
    value[valueLength] = found[valueLength], ++valueLength;
  value[valueLength] = '\0';
  std::snprintf(auth_, sizeof(auth_), "GoogleLogin auth=%s", value);
  return true;
}

bool SyncSession::fetchSubscriptions(SyncOutcome& outcome) {
  report(SyncStage::Feeds, 0, 0, "");
  size_t written = appendLiteral(query_, sizeof(query_), 0, "output=json&ck=");
  written = written ? appendUnsigned(query_, sizeof(query_), written, nowEpoch_) : 0;
  if (written == 0) {
    outcome.status = SyncStatus::BadResponse;
    copyBounded(outcome.message, sizeof(outcome.message), "request too long");
    return false;
  }
  if (!send("GET", "/reader/api/0/subscription/list", query_, auth_, nullptr, 0, outcome)) return false;
  if (!checkStatus(outcome, "feed list")) return false;

  feeds_.count = 0;
  SubscriptionCollector collector(feeds_);
  char buffer[kBodyStreamBuffer];
  while (true) {
    const int got = http_.readBody(buffer, sizeof(buffer));
    if (got < 0) {
      outcome.status = SyncStatus::NetworkError;
      copyBounded(outcome.message, sizeof(outcome.message), http_.error());
      return false;
    }
    if (got == 0) break;
    collector.feed(buffer, static_cast<size_t>(got));
  }
  if (!collector.finish()) {
    outcome.status = SyncStatus::BadResponse;
    copyBounded(outcome.message, sizeof(outcome.message), collector.error());
    return false;
  }
  return true;
}

bool SyncSession::fetchFeed(const Feed& feed, size_t index, size_t total, const SyncRequest& request,
                            SyncOutcome& outcome) {
  char directory[160];
  if (!store_.ensureFeedDirectory(config_.folder, feed.title, directory, sizeof(directory))) {
    outcome.status = SyncStatus::OutOfSpace;
    copyBounded(outcome.message, sizeof(outcome.message), "cannot create folder");
    return false;
  }

  // Unread items only, newest first. `ot` is FreshRSS's lower date bound, so an
  // incremental sync asks for items published after the last successful run.
  size_t written = appendLiteral(query_, sizeof(query_), 0, "output=json&r=d&n=");
  written = written ? appendUnsigned(query_, sizeof(query_), written, config_.articlesPerFeed) : 0;
  if (!config_.includeRead) {
    // Unread mirror: exclude what the server already reports as read.
    written = written ? appendLiteral(query_, sizeof(query_), written, "&xt=") : 0;
    written = written ? formEncodeAppend(query_, sizeof(query_), written, "user/-/state/com.google/read") : 0;
  }
  written = written ? appendLiteral(query_, sizeof(query_), written, "&ck=") : 0;
  written = written ? appendUnsigned(query_, sizeof(query_), written, request.nowEpoch) : 0;
  if (written && request.sinceEpoch != 0) {
    written = appendLiteral(query_, sizeof(query_), written, "&ot=");
    written = written ? appendUnsigned(query_, sizeof(query_), written, request.sinceEpoch) : 0;
  }
  if (written == 0) {
    outcome.status = SyncStatus::BadResponse;
    copyBounded(outcome.message, sizeof(outcome.message), "request too long");
    return false;
  }

  char apiPath[200];
  size_t pathLength = appendLiteral(apiPath, sizeof(apiPath), 0, "/reader/api/0/stream/contents/");
  pathLength = pathLength ? pathEncodeAppend(apiPath, sizeof(apiPath), pathLength, feed.id) : 0;
  if (pathLength == 0) {
    outcome.status = SyncStatus::BadResponse;
    copyBounded(outcome.message, sizeof(outcome.message), "feed id too long");
    return false;
  }

  if (!send("GET", apiPath, query_, auth_, nullptr, 0, outcome)) return false;
  if (!checkStatus(outcome, "articles")) {
    // A feed that disappeared on the server must not abort the whole sync.
    if (outcome.httpStatus == 400 || outcome.httpStatus == 404) {
      stats_.articlesFailed++;
      outcome.status = SyncStatus::Ok;
      outcome.httpStatus = 0;
      outcome.message[0] = '\0';
      return true;
    }
    return false;
  }

  ArticleDownloader downloader(store_, config_, feed, hooks_, stats_);
  downloader.setDirectory(directory);
  downloader.setFallbackEpoch(request.nowEpoch);
  StreamCollector collector(downloader);
  char buffer[kBodyStreamBuffer];
  while (true) {
    const int got = http_.readBody(buffer, sizeof(buffer));
    if (got < 0) {
      outcome.status = SyncStatus::NetworkError;
      copyBounded(outcome.message, sizeof(outcome.message), http_.error());
      return false;
    }
    if (got == 0) break;
    collector.feed(buffer, static_cast<size_t>(got));
    if (downloader.fatal() || downloader.abortRequested() || aborted_) break;
  }

  if (aborted_ || downloader.abortRequested()) {
    outcome.status = SyncStatus::Aborted;
    copyBounded(outcome.message, sizeof(outcome.message), "stopped");
    return false;
  }
  if (downloader.fatal()) {
    outcome.status = SyncStatus::OutOfSpace;
    copyBounded(outcome.message, sizeof(outcome.message), "card full");
    return false;
  }
  if (!collector.finish()) {
    outcome.status = SyncStatus::BadResponse;
    copyBounded(outcome.message, sizeof(outcome.message), collector.error());
    return false;
  }

  stats_.feedsChecked++;
  uint16_t removed = 0;
  if (store_.pruneFeed(directory, config_.keepPerFeed, removed)) stats_.pruned += removed;
  (void)index;
  (void)total;
  return true;
}

bool SyncSession::fetchToken(SyncOutcome& outcome) {
  if (!send("GET", "/reader/api/0/token", nullptr, auth_, nullptr, 0, outcome)) return false;
  if (!checkStatus(outcome, "token")) return false;
  char response[128];
  const int length = http_.readBodyAll(response, sizeof(response));
  if (length <= 0) {
    outcome.status = SyncStatus::BadResponse;
    copyBounded(outcome.message, sizeof(outcome.message), "empty token response");
    return false;
  }
  size_t written = 0;
  for (const char* p = response; *p && *p != '\r' && *p != '\n' && written + 1 < sizeof(token_); ++p)
    token_[written++] = *p;
  token_[written] = '\0';
  if (token_[0] == '\0') {
    outcome.status = SyncStatus::BadResponse;
    copyBounded(outcome.message, sizeof(outcome.message), "empty token response");
    return false;
  }
  return true;
}

bool SyncSession::uploadMarkRead(const char* const* ids, size_t count, SyncOutcome& outcome) {
  if (count == 0) return true;
  if (token_[0] == '\0' && !fetchToken(outcome)) return false;

  size_t index = 0;
  while (index < count) {
    if (aborted_) {
      outcome.status = SyncStatus::Aborted;
      copyBounded(outcome.message, sizeof(outcome.message), "stopped");
      return false;
    }
    size_t written = appendLiteral(body_, sizeof(body_), 0, "T=");
    written = written ? formEncodeAppend(body_, sizeof(body_), written, token_) : 0;
    written = written ? appendLiteral(body_, sizeof(body_), written, "&a=") : 0;
    written = written ? formEncodeAppend(body_, sizeof(body_), written, "user/-/state/com.google/read") : 0;
    if (written == 0) {
      outcome.status = SyncStatus::BadResponse;
      copyBounded(outcome.message, sizeof(outcome.message), "mark-read request too long");
      return false;
    }

    size_t batch = 0;
    while (index < count && batch < kMaxMarkReadPerRequest) {
      const char* id = ids[index];
      if (!id || id[0] == '\0') {
        ++index;
        continue;
      }
      const size_t before = written;
      written = appendLiteral(body_, sizeof(body_), written, "&i=");
      written = written ? formEncodeAppend(body_, sizeof(body_), written, id) : 0;
      if (written == 0) {
        // This id does not fit: send what has been collected and start over.
        body_[before] = '\0';
        written = before;
        break;
      }
      ++index;
      ++batch;
    }
    if (batch == 0) {
      outcome.status = SyncStatus::BadResponse;
      copyBounded(outcome.message, sizeof(outcome.message), "item id too long");
      return false;
    }

    if (!send("POST", "/reader/api/0/edit-tag", nullptr, auth_, body_, written, outcome)) return false;
    if (!checkStatus(outcome, "mark read")) return false;
    // The body is a short acknowledgement; draining it keeps the connection in
    // a reusable state.
    char drain[64];
    while (http_.readBody(drain, sizeof(drain)) > 0) {
    }
    stats_.markedRead = static_cast<uint16_t>(stats_.markedRead + batch);
  }
  return true;
}

SyncOutcome SyncSession::run(const SyncRequest& request) {
  SyncOutcome outcome;
  stats_ = SyncStats{};
  feeds_.count = 0;
  auth_[0] = '\0';
  token_[0] = '\0';
  aborted_ = false;
  nowEpoch_ = request.nowEpoch;

  report(SyncStage::Connecting, 0, 0, "");
  if (aborted_) {
    outcome.status = SyncStatus::Aborted;
    outcome.stats = stats_;
    return outcome;
  }
  if (!ensureConnected(outcome) || !login(outcome) || !fetchSubscriptions(outcome)) {
    outcome.stats = stats_;
    return outcome;
  }

  stats_.feedsSubscribed = static_cast<uint16_t>(feeds_.count < UINT16_MAX ? feeds_.count : UINT16_MAX);

  // A run visits at most maxFeeds feeds, starting at the caller's cursor and
  // wrapping around, so a long subscription list is covered over several syncs.
  size_t checked = 0;
  size_t steps = 0;
  for (; steps < feeds_.count && checked < config_.maxFeeds; ++steps) {
    const size_t index = (static_cast<size_t>(request.feedOffset) + steps) % feeds_.count;
    const Feed& feed = feeds_.feeds[index];
    if (!feedAllowed(config_, feed.id, feed.title)) continue;
    ++checked;
    report(SyncStage::Articles, index, feeds_.count, feed.title);
    if (aborted_) {
      outcome.status = SyncStatus::Aborted;
      outcome.stats = stats_;
      return outcome;
    }
    if (!fetchFeed(feed, index, feeds_.count, request, outcome)) {
      outcome.stats = stats_;
      return outcome;
    }
  }

  // The pass is complete in two cases: the run consumed the whole list (so every
  // allowed feed was visited), or the rotation crossed past the end of the list
  // (so the sweep that started after the previous window advance now covers every
  // index). Stopping exactly at the last index is not a full pass: the feeds
  // before the starting cursor were not visited.
  const size_t scanned = static_cast<size_t>(request.feedOffset) + steps;
  outcome.coveredAllFeeds = feeds_.count == 0 || steps >= feeds_.count || scanned > feeds_.count;
  outcome.nextFeedOffset =
      feeds_.count == 0 ? 0 : static_cast<uint32_t>((static_cast<size_t>(request.feedOffset) + steps) % feeds_.count);

  // Marking items read upstream is best effort: articles are already on the
  // card and must not be lost because the token request failed.
  if (request.markReadIds && request.markReadCount > 0) {
    report(SyncStage::MarkRead, 0, 0, "");
    SyncOutcome markOutcome;
    if (!uploadMarkRead(request.markReadIds, request.markReadCount, markOutcome)) {
      copyBounded(outcome.message, sizeof(outcome.message), "saved; mark-read failed");
    }
  }

  report(SyncStage::Done, 0, 0, "");
  outcome.status = SyncStatus::Ok;
  outcome.stats = stats_;
  if (outcome.message[0] == '\0') copyBounded(outcome.message, sizeof(outcome.message), "synced");
  return outcome;
}

const char* describeSyncStatus(SyncStatus status) {
  switch (status) {
    case SyncStatus::Ok:
      return "synced";
    case SyncStatus::NoConfig:
      return "no config";
    case SyncStatus::NetworkError:
      return "network error";
    case SyncStatus::LoginFailed:
      return "login failed";
    case SyncStatus::HttpError:
      return "server error";
    case SyncStatus::BadResponse:
      return "bad response";
    case SyncStatus::OutOfSpace:
      return "card full";
    case SyncStatus::Aborted:
      return "stopped";
  }
  return "error";
}

}  // namespace freshrss
