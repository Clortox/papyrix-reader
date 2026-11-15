// Config decoding and endpoint parsing for the FreshRSS mini-app: the SD-card
// JSON file, URL splitting, sync scheduling and the optional feed filter.

#include <cstdio>
#include <cstring>
#include <string>

// Relative includes keep this test self-contained: the host test harness has no
// compile database of its own.
#include "../../../lib/FreshRss/src/FreshRssConfig.h"
#include "../../common/test_utils.h"

using namespace freshrss;

namespace {

Config parse(const std::string& text, bool& ok, std::string& error) {
  Config config;
  char message[96] = {};
  ok = parseConfigJson(text.data(), text.size(), config, message, sizeof(message));
  error = message;
  return config;
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("FreshRss config");

  // --- endpoint parsing -----------------------------------------------------
  {
    Endpoint endpoint;
    char error[96] = {};
    runner.expectTrue(parseServerUrl("https://freshrss.example.net/api/greader.php", endpoint, error, sizeof(error)),
                      "server: canonical URL parses");
    runner.expectTrue(endpoint.tls, "server: https selects TLS");
    runner.expectEq<uint16_t>(443, endpoint.port, "server: default TLS port");
    runner.expectEqual("freshrss.example.net", endpoint.host, "server: host split");
    runner.expectEqual("/api/greader.php", endpoint.basePath, "server: path kept");
  }
  {
    Endpoint endpoint;
    char error[96] = {};
    runner.expectTrue(parseServerUrl("http://192.168.1.10:8080", endpoint, error, sizeof(error)),
                      "server: plain HTTP without path parses");
    runner.expectFalse(endpoint.tls, "server: http stays plain");
    runner.expectEq<uint16_t>(8080, endpoint.port, "server: explicit port");
    runner.expectEqual("/api/greader.php", endpoint.basePath, "server: API path appended when missing");
  }
  {
    Endpoint endpoint;
    char error[96] = {};
    runner.expectTrue(parseServerUrl("https://example.net/freshrss", endpoint, error, sizeof(error)),
                      "server: sub-directory install parses");
    runner.expectEqual("/freshrss/api/greader.php", endpoint.basePath, "server: prefix kept before API path");
  }
  {
    Endpoint endpoint;
    char error[96] = {};
    runner.expectTrue(parseServerUrl("https://[2001:db8::1]:8443/rss/api/greader.php", endpoint, error, sizeof(error)),
                      "server: IPv6 literal parses");
    runner.expectEqual("2001:db8::1", endpoint.host, "server: IPv6 host without brackets");
    runner.expectEq<uint16_t>(8443, endpoint.port, "server: IPv6 port");
    runner.expectEqual("/rss/api/greader.php", endpoint.basePath, "server: IPv6 path");
  }
  {
    Endpoint endpoint;
    char error[96] = {};
    runner.expectFalse(parseServerUrl("freshrss.example.net/api/greader.php", endpoint, error, sizeof(error)),
                       "server: missing scheme rejected");
    runner.expectTrue(std::strstr(error, "http") != nullptr, "server: scheme error is explained");
    runner.expectFalse(parseServerUrl("https://", endpoint, error, sizeof(error)), "server: empty host rejected");
    runner.expectFalse(parseServerUrl("https://host:0/", endpoint, error, sizeof(error)), "server: port zero rejected");
    runner.expectFalse(parseServerUrl("https://host:99999/", endpoint, error, sizeof(error)),
                       "server: out-of-range port rejected");
    runner.expectFalse(parseServerUrl("https://host:80x/", endpoint, error, sizeof(error)),
                       "server: non-numeric port rejected");
  }

  // --- config JSON ----------------------------------------------------------
  {
    bool ok = false;
    std::string error;
    const Config config = parse(
        R"({
          "server": "https://freshrss.example.net/api/greader.php/",
          "user": "alice",
          "apiPassword": "Abcdef123456",
          "folder": "RSS/",
          "articlesPerFeed": 7,
          "maxFeeds": 3,
          "syncIntervalMinutes": 30,
          "keepPerFeed": 12,
          "maxArticleBytes": 40000,
          "markReadOnOpen": false,
          "verifyTls": false,
          "includeRead": true,
          "feeds": "Ars Technica, feed/13",
          "_note": "unknown keys are skipped"
        })",
        ok, error);
    runner.expectTrue(ok, "config: valid file parses");
    runner.expectEqual("https://freshrss.example.net/api/greader.php", config.server, "config: trailing slash trimmed");
    runner.expectEqual("alice", config.user, "config: user decoded");
    runner.expectEqual("Abcdef123456", config.apiPassword, "config: API password decoded");
    runner.expectEqual("/RSS", config.folder, "config: folder normalized to an absolute path");
    runner.expectEq<uint16_t>(7, config.articlesPerFeed, "config: articlesPerFeed decoded");
    runner.expectEq<uint8_t>(3, config.maxFeeds, "config: maxFeeds decoded");
    runner.expectEq<uint16_t>(30, config.syncIntervalMinutes, "config: sync interval decoded");
    runner.expectEq<uint16_t>(12, config.keepPerFeed, "config: keepPerFeed decoded");
    runner.expectEq<uint32_t>(40000, config.maxArticleBytes, "config: maxArticleBytes decoded");
    runner.expectFalse(config.markReadOnOpen, "config: markReadOnOpen decoded");
    runner.expectFalse(config.verifyTls, "config: verifyTls decoded");
    runner.expectTrue(config.includeRead, "config: includeRead decoded");
    runner.expectTrue(config.hasFeedFilter(), "config: feed filter present");
    runner.expectTrue(feedAllowed(config, "feed/13", "Hacker News"), "filter: numeric id matches");
    runner.expectTrue(feedAllowed(config, "feed/9", "ars technica"), "filter: title matches case-insensitively");
    runner.expectFalse(feedAllowed(config, "feed/9", "Other"), "filter: unrelated feed excluded");
  }
  {
    bool ok = false;
    std::string error;
    const Config config = parse(
        "{\"server\":\"https://h/api/greader.php\",\"user\":\"u\",\"apiPassword\":\"p\",\"unknown\":{\"a\":[1,2,"
        "{\"b\":null}]},\"also\":[true,false,\"x\"]}",
        ok, error);
    runner.expectTrue(ok, "config: nested unknown values are skipped");
    runner.expectEqual("u", config.user, "config: values after skipped containers decode");
  }
  {
    bool ok = false;
    std::string error;
    parse("\xEF\xBB\xBF{\"user\":\"bom\"}", ok, error);
    runner.expectTrue(ok, "config: UTF-8 BOM tolerated");
  }
  {
    bool ok = false;
    std::string error;
    const Config config = parse(R"({"user":"esc\u00e1pe\n\"x\"","apiPassword":"p"})", ok, error);
    runner.expectTrue(ok, "config: escapes decode");
    runner.expectEqual("esc\xC3\xA1pe\n\"x\"", config.user, "config: \\u and \\n decoded");
  }
  {
    bool ok = false;
    std::string error;
    parse("{\"user\":\"u\",}", ok, error);
    runner.expectFalse(ok, "config: trailing comma rejected");
    runner.expectTrue(error.find("comma") != std::string::npos, "config: trailing comma explained");
    parse("{\"user\":5}", ok, error);
    runner.expectFalse(ok, "config: wrong value type rejected");
    parse("{\"user\":\"u\"", ok, error);
    runner.expectFalse(ok, "config: truncated object rejected");
    parse("", ok, error);
    runner.expectFalse(ok, "config: empty file rejected");
  }
  {
    bool ok = false;
    std::string error;
    const Config config = parse(
        R"({"articlesPerFeed":900,"maxFeeds":99,"syncIntervalMinutes":1,"keepPerFeed":0,"maxArticleBytes":10,
            "folder":"/"})",
        ok, error);
    runner.expectTrue(ok, "config: out-of-range numbers still parse");
    runner.expectEq<uint16_t>(kMaxArticlesPerFeed, config.articlesPerFeed, "config: articlesPerFeed clamped high");
    runner.expectEq<uint8_t>(kMaxMaxFeeds, config.maxFeeds, "config: maxFeeds clamped high");
    runner.expectEq<uint8_t>(64, kMaxMaxFeeds, "config: the cap covers a large subscription list");
    runner.expectEq<uint16_t>(kMinSyncIntervalMinutes, config.syncIntervalMinutes, "config: interval clamped low");
    runner.expectEq<uint16_t>(kMinKeepPerFeed, config.keepPerFeed, "config: keepPerFeed clamped low");
    runner.expectEq<uint32_t>(kMinArticleBytes, config.maxArticleBytes, "config: maxArticleBytes clamped low");
    runner.expectEqual("/RSS", config.folder, "config: card root refused as folder");
  }
  {
    Config config;
    char error[96] = {};
    runner.expectFalse(validateConfig(config, error, sizeof(error)), "validate: empty config rejected");
    runner.expectEqual("server is not set", std::string(error), "validate: missing server message");
    std::snprintf(config.server, sizeof(config.server), "https://host/api/greader.php");
    validateConfig(config, error, sizeof(error));
    runner.expectEqual("user is not set", std::string(error), "validate: missing user message");
    std::snprintf(config.user, sizeof(config.user), "alice");
    validateConfig(config, error, sizeof(error));
    runner.expectEqual("apiPassword is not set", std::string(error), "validate: missing API password message");
    std::snprintf(config.apiPassword, sizeof(config.apiPassword), "secret");
    runner.expectTrue(validateConfig(config, error, sizeof(error)), "validate: complete config accepted");
  }

  // --- sync scheduling ------------------------------------------------------
  {
    Config config;
    config.syncIntervalMinutes = 60;
    runner.expectTrue(syncDue(0, 1000000, config, true), "schedule: never synced is due");
    runner.expectTrue(syncDue(0, 1000000, config, false), "schedule: zero timestamp is due");
    runner.expectFalse(syncDue(1000, 1000 + 3599, config, false), "schedule: inside the interval is not due");
    runner.expectTrue(syncDue(1000, 1000 + 3600, config, false), "schedule: interval boundary is due");
    runner.expectTrue(syncDue(1000, 0, config, false), "schedule: unknown clock syncs");
    runner.expectTrue(syncDue(2000, 1000, config, false), "schedule: clock moved back syncs");
  }

  // --- example file ---------------------------------------------------------
  {
    char text[2048];
    const size_t length = writeExampleConfig(text, sizeof(text));
    runner.expectTrue(length > 100, "example: template written");
    bool ok = false;
    std::string error;
    Config config = parse(std::string(text, length), ok, error);
    runner.expectTrue(ok, "example: template is valid JSON");
    char message[96] = {};
    runner.expectFalse(validateConfig(config, message, sizeof(message)), "example: template needs user input");
    runner.expectEqual("server is not set", std::string(message), "example: template has an empty server");
  }

  return runner.allPassed() ? 0 : 1;
}
