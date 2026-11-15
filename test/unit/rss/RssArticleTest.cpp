// Offline article files: deterministic names, dates, and the streaming HTML
// sanitizer that keeps feed markup inside what the reader's XML pipeline can
// handle.

#include <string>
#include <vector>

#include "../../../lib/FreshRss/src/FreshRssArticle.h"
#include "../../common/test_utils.h"

using namespace freshrss;

namespace {

class StringSink : public ByteSink {
 public:
  bool write(const char* data, size_t length) override {
    content.append(data, length);
    return true;
  }
  std::string content;
};

std::string sanitize(const std::string& input, size_t chunkSize = 4096) {
  StringSink sink;
  ArticleWriter writer;
  if (!writer.begin(sink, 64 * 1024, "Title", "Feed", nullptr, nullptr, 1794746040, "tag:x")) return "<begin failed>";
  for (size_t offset = 0; offset < input.size();) {
    const size_t count = std::min(chunkSize, input.size() - offset);
    if (!writer.writeContent(input.data() + offset, count)) return sink.content + "<write failed>";
    offset += count;
  }
  if (!writer.end()) return sink.content + "<end failed>";
  return sink.content;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("FreshRss articles");

  // --- dates ----------------------------------------------------------------
  {
    CivilDate date{};
    epochToCivil(0, 0, date);
    runner.expectEq(1970, date.year, "date: epoch year");
    runner.expectEq(1, date.month, "date: epoch month");
    runner.expectEq(1, date.day, "date: epoch day");
    epochToCivil(1794746040u, 0, date);
    runner.expectEq(2026, date.year, "date: 2026 year");
    runner.expectEq(11, date.month, "date: 2026 month");
    runner.expectEq(15, date.day, "date: 2026 day");
    runner.expectEq(12, date.hour, "date: 2026 hour");
    runner.expectEq(34, date.minute, "date: 2026 minute");
    epochToCivil(1709164800u, 0, date);
    runner.expectEq(2024, date.year, "date: leap year");
    runner.expectEq(2, date.month, "date: leap month");
    runner.expectEq(29, date.day, "date: leap day");
    epochToCivil(1794746040u, 120, date);
    runner.expectEq(14, date.hour, "date: positive offset applied");
    epochToCivil(1794746040u, -300, date);
    runner.expectEq(7, date.hour, "date: negative offset applied");
    epochToCivil(1704067140u, 0, date);
    runner.expectEq(31, date.day, "date: year end day");
    runner.expectEq(23, date.hour, "date: year end hour");
  }
  {
    CivilDate date{};
    epochToCivil(1794746040u, 0, date);
    char iso[16];
    runner.expectEqual("2026-11-15", std::string(iso, formatIsoDate(date, iso, sizeof(iso))), "date: ISO format");
    char longDate[48];
    runner.expectEqual("15 November 2026, 12:34",
                       std::string(longDate, formatLongDate(date, longDate, sizeof(longDate))), "date: long format");
  }

  // --- names ----------------------------------------------------------------
  {
    runner.expectNe(hashItemId("tag:google.com,2005:reader/item/abc"),
                    hashItemId("tag:google.com,2005:reader/item/abd"), "hash: distinct ids differ");
    runner.expectEq<uint64_t>(hashItemId("same"), hashItemId("same"), "hash: stable for the same id");
    runner.expectTrue(hashItemId("") != 0, "hash: empty id still hashes");
  }
  {
    char out[96];
    sanitizeFileComponent(out, sizeof(out), "Why  Rust\tWon: 2026!", "feed");
    runner.expectEqual("Why-Rust-Won-2026", std::string(out), "name: unsafe characters collapsed");
    sanitizeFileComponent(out, sizeof(out), "///", "feed");
    runner.expectEqual("feed", std::string(out), "name: fallback used when nothing remains");
    sanitizeFileComponent(out, sizeof(out), "a/b\\c:d*e?f\"g<h>i|j", "x");
    runner.expectEqual("a-b-c-d-e-f-g-h-i-j", std::string(out), "name: path separators replaced");
    sanitizeFileComponent(out, sizeof(out), "Ünïcödé Feed", "x");
    runner.expectEqual("Ünïcödé-Feed", std::string(out), "name: non-ASCII kept");
    sanitizeFileComponent(out, sizeof(out), ".hidden.", "x");
    runner.expectEqual("hidden", std::string(out), "name: leading and trailing dots trimmed");
  }
  {
    char out[128];
    // Spaces become '-' so a folder name is unambiguous in the file browser.
    runner.expectEqual("/RSS/Ars-Technica",
                       std::string(out, buildFeedDirectory(out, sizeof(out), "/RSS", "Ars Technica")),
                       "name: feed directory");
    buildFeedDirectory(out, sizeof(out), "/News", "Bad/Name");
    runner.expectEqual("/News/Bad-Name", std::string(out), "name: feed directory sanitized");
    buildFeedDirectory(out, sizeof(out), "", "Feed");
    runner.expectEqual("/RSS/Feed", std::string(out), "name: default folder used");
  }
  {
    char out[128];
    const uint64_t hash = hashItemId("tag:google.com,2005:reader/item/1a2b3c");
    buildArticleFileName(out, sizeof(out), "Hello, World!", 1794746040u, 0, hash);
    const std::string name(out);
    runner.expectTrue(name.rfind("2026-11-15-Hello-World-", 0) == 0, "name: article name starts with date and slug");
    runner.expectTrue(name.size() > 4 && name.compare(name.size() - 5, 5, ".html") == 0,
                      "name: article name ends in .html");
    char again[128];
    buildArticleFileName(again, sizeof(again), "Hello, World!", 1794746040u, 0, hash);
    runner.expectEqual(name, std::string(again), "name: article name is deterministic");
    // An item without a publication date falls back to the fetch time.
    buildArticleFileName(out, sizeof(out), "No Date", 0, 1794746040u, hash);
    runner.expectTrue(std::string(out).rfind("2026-11-15-", 0) == 0, "name: fallback epoch used");
  }

  // --- document -------------------------------------------------------------
  {
    const std::string document = sanitize("<p>Hello</p><p>World</p>");
    runner.expectTrue(contains(document, "<!DOCTYPE html>"), "doc: doctype written");
    runner.expectTrue(contains(document, "<title>Title - Feed</title>"), "doc: title includes the feed");
    runner.expectTrue(contains(document, "<meta name=\"rss-item\" content=\"tag:x\"/>"), "doc: item id recorded");
    runner.expectTrue(contains(document, "<h1>Title</h1>"), "doc: heading written");
    runner.expectTrue(contains(document, "Feed - 15 November 2026, 12:34"), "doc: byline written");
    runner.expectTrue(contains(document, "<p>Hello</p><p>World</p>"), "doc: content preserved");
    runner.expectTrue(contains(document, "</body></html>"), "doc: closed properly");

    char id[96];
    runner.expectTrue(extractItemId(document.data(), document.size(), id, sizeof(id)), "doc: item id extracted");
    runner.expectEqual("tag:x", std::string(id), "doc: extracted id matches");
  }
  {
    StringSink sink;
    ArticleWriter writer;
    writer.begin(sink, 64 * 1024, "Escaped & <Title>", "Feed & Co", "Author <a>", "https://x/?a=1&b=2", 1794746040u,
                 "tag:&:x");
    writer.end();
    runner.expectTrue(contains(sink.content, "<title>Escaped &amp; &lt;Title&gt; - Feed &amp; Co</title>"),
                      "doc: title and feed escaped");
    runner.expectTrue(contains(sink.content, "content=\"tag:&amp;:x\""), "doc: item id escaped");
    char id[96];
    extractItemId(sink.content.data(), sink.content.size(), id, sizeof(id));
    runner.expectEqual("tag:&:x", std::string(id), "doc: escaped item id round-trips");
  }

  // --- sanitizer ------------------------------------------------------------
  {
    const std::string document = sanitize("<p>a</p><script>alert('x')</script><p>b</p>");
    runner.expectTrue(contains(document, "<p>a</p>"), "sanitize: text before script kept");
    runner.expectTrue(contains(document, "<p>b</p>"), "sanitize: text after script kept");
    runner.expectFalse(contains(document, "alert"), "sanitize: script content removed");
    runner.expectFalse(contains(document, "<script"), "sanitize: script tag removed");
  }
  {
    const std::string document =
        sanitize("<style>p{color:red}</style><iframe src=\"x\"></iframe><noscript>n</noscript><p>keep</p>");
    runner.expectFalse(contains(document, "color:red"), "sanitize: style content removed");
    runner.expectFalse(contains(document, "iframe"), "sanitize: iframe removed");
    runner.expectFalse(contains(document, "noscript"), "sanitize: noscript removed");
    runner.expectTrue(contains(document, "<p>keep</p>"), "sanitize: following content kept");
  }
  {
    // Nested same-name elements must not end suppression early.
    const std::string document = sanitize("<script>if (a<b) { x('</script>'); }</script><p>after</p>");
    runner.expectFalse(contains(document, "x("), "sanitize: nested script text removed");
    runner.expectTrue(contains(document, "<p>after</p>"), "sanitize: content after nested script kept");
  }
  {
    const std::string document = sanitize("<p onclick=\"steal()\" style=\"x\" class=\"y\" href=\"z\">t</p>");
    runner.expectFalse(contains(document, "onclick"), "sanitize: event handler dropped");
    runner.expectFalse(contains(document, "style="), "sanitize: inline style dropped");
    runner.expectFalse(contains(document, "class="), "sanitize: class dropped");
    runner.expectTrue(contains(document, "<p>t</p>"), "sanitize: unsupported attributes removed");
  }
  {
    const std::string document = sanitize("<a href=\"https://x/?a=1&amp;b=2\" onclick=\"y\">link</a>");
    runner.expectTrue(contains(document, "<a href=\"https://x/?a=1&amp;b=2\">link</a>"), "sanitize: anchor href kept");
  }
  {
    const std::string document = sanitize("<div><span>plain</span><custom>kept text</custom></div>");
    runner.expectTrue(contains(document, "<div><span>plain</span>kept text</div>"),
                      "sanitize: unknown tags unwrapped but text kept");
  }
  {
    const std::string document = sanitize("<!-- comment with > inside --><p>after</p>");
    runner.expectFalse(contains(document, "comment"), "sanitize: comment removed");
    runner.expectTrue(contains(document, "<p>after</p>"), "sanitize: content after comment kept");
  }
  {
    const std::string document = sanitize("<p>a &amp; b &nbsp; c &#8212; d</p>");
    runner.expectTrue(contains(document, "a &amp; b &nbsp; c &#8212; d"), "sanitize: valid entities preserved");
  }
  {
    const std::string document = sanitize("<p>Tom & Jerry &notanentity; x</p>");
    runner.expectTrue(contains(document, "Tom &amp; Jerry"), "sanitize: bare ampersand escaped");
    runner.expectTrue(contains(document, "&amp;notanentity;"), "sanitize: invalid entity escaped");
  }
  {
    const std::string document = sanitize("<p>1 < 2 and 3 > 2</p>");
    runner.expectTrue(contains(document, "1 &lt; 2 and 3 &gt; 2"), "sanitize: comparison signs escaped");
  }
  {
    const std::string document = sanitize("<p>tab\there</p><p>ctrl\x01char</p>");
    runner.expectTrue(contains(document, "tab\there"), "sanitize: tab kept");
    runner.expectFalse(contains(document, "ctrl\x01char"), "sanitize: control character dropped");
  }
  {
    // Every byte boundary of a realistic fragment must produce the same output.
    const std::string input =
        "<p>Intro <b>bold</b></p><script>bad()</script><p>a &amp; b</p><ul><li>one</li><li>two</li></ul>";
    const std::string reference = sanitize(input, 4096);
    for (size_t size : {1u, 2u, 3u, 5u, 8u, 13u}) {
      runner.expectEqual(reference, sanitize(input, size),
                         "sanitize: chunked input at size " + std::to_string(size) + " matches");
    }
  }

  // --- limits ---------------------------------------------------------------
  {
    StringSink sink;
    ArticleWriter writer;
    writer.begin(sink, 200, "T", "F", nullptr, nullptr, 1794746040u, "tag:x");
    std::string body(400, 'a');
    const bool ok = writer.writeContent(body.data(), body.size());
    runner.expectFalse(ok, "limit: oversized body refused");
    runner.expectTrue(writer.overflowed(), "limit: overflow flagged");
    runner.expectFalse(writer.end(), "limit: end reports failure after overflow");
    runner.expectTrue(sink.content.size() <= 200, "limit: sink never exceeds the budget");
  }
  {
    StringSink sink;
    ArticleWriter writer;
    writer.begin(sink, 64 * 1024, "T", "F", nullptr, nullptr, 1794746040u, "tag:x");
    writer.writeContent("<p>x</p>", 8);
    writer.writeAuthorLine("Jane <Doe>");
    writer.end();
    runner.expectTrue(contains(sink.content, "<p><em>Jane &lt;Doe&gt;</em></p>"), "byline: late author appended");
  }

  return runner.allPassed() ? 0 : 1;
}
