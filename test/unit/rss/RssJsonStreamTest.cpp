// Incremental JSON scanner: token shapes, escapes, bounded string chunking and
// error reporting. The scanner is what keeps a large FreshRSS response out of
// RAM, so chunking and error behaviour are tested directly.

#include <algorithm>
#include <string>
#include <vector>

// Relative includes keep this test self-contained: the host test harness has no
// compile database of its own.
#include "../../../lib/FreshRss/src/FreshRssJson.h"
#include "../../common/test_utils.h"

using freshrss::JsonScanner;

namespace {

struct Event {
  JsonScanner::Token token;
  std::string text;
  long long number = 0;
  bool boolean = false;
};

std::vector<Event> scan(const std::string& input, size_t chunkSize = 1) {
  std::vector<Event> events;
  JsonScanner scanner;
  for (size_t offset = 0; offset < input.size();) {
    const size_t count = std::min(chunkSize, input.size() - offset);
    size_t consumed = 0;
    const JsonScanner::Token token = scanner.feed(input.data() + offset, count, &consumed);
    offset += consumed;
    if (token == JsonScanner::Token::None) {
      if (consumed == 0) break;  // defensive: never loop forever
      continue;
    }
    Event event;
    event.token = token;
    event.text.assign(scanner.text(), scanner.length());
    event.number = scanner.numberValue();
    event.boolean = scanner.boolValue();
    events.push_back(event);
  }
  const JsonScanner::Token tail = scanner.finish();
  if (tail != JsonScanner::Token::None) {
    Event event;
    event.token = tail;
    event.text.assign(scanner.text(), scanner.length());
    event.number = scanner.numberValue();
    events.push_back(event);
  }
  return events;
}

std::string render(const std::vector<Event>& events) {
  std::string out;
  for (const Event& event : events) {
    switch (event.token) {
      case JsonScanner::Token::ObjectBegin:
        out += "{";
        break;
      case JsonScanner::Token::ObjectEnd:
        out += "}";
        break;
      case JsonScanner::Token::ArrayBegin:
        out += "[";
        break;
      case JsonScanner::Token::ArrayEnd:
        out += "]";
        break;
      case JsonScanner::Token::Key:
        out += "k(" + event.text + ")";
        break;
      case JsonScanner::Token::StringChunk:
        out += "s(" + event.text + ")";
        break;
      case JsonScanner::Token::StringEnd:
        out += "S(" + event.text + ")";
        break;
      case JsonScanner::Token::Number:
        out += "n(" + std::to_string(event.number) + ")";
        break;
      case JsonScanner::Token::Bool:
        out += event.boolean ? "b(true)" : "b(false)";
        break;
      case JsonScanner::Token::Null:
        out += "null";
        break;
      case JsonScanner::Token::Error:
        out += "ERR(" + event.text + ")";
        break;
      case JsonScanner::Token::None:
        break;
    }
  }
  return out;
}

bool hasError(const std::vector<Event>& events) {
  for (const Event& event : events) {
    if (event.token == JsonScanner::Token::Error) return true;
  }
  return false;
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("FreshRss JSON scanner");

  runner.expectEqual("{}", render(scan("{}")), "scan: empty object");
  runner.expectEqual("{k(a)S(1)k(b)n(2)k(c)b(true)k(d)nullk(e)[S(x)]}",
                     render(scan(R"({"a":"1","b":2,"c":true,"d":null,"e":["x"]})")), "scan: mixed value kinds");

  // Numbers keep their integer part and flag fractions.
  {
    const auto events = scan(R"([-12,3.5,1e3,0])");
    runner.expectEqual("[n(-12)n(3)n(1)n(0)]", render(events), "scan: numbers parsed");
  }
  {
    JsonScanner scanner;
    std::string text = "42.5";
    scanner.feed(text.data(), text.size(), nullptr);
    const JsonScanner::Token token = scanner.finish();
    runner.expectTrue(token == JsonScanner::Token::Number, "scan: trailing number flushed at end");
    runner.expectEq<long long>(42, scanner.numberValue(), "scan: fraction truncated");
    runner.expectTrue(scanner.fractional(), "scan: fraction flagged");
  }

  // Escapes, including a surrogate pair that must fold into one code point.
  runner.expectEqual("S(quote\" back\\ slash/ tab\t nl\n)", render(scan(R"("quote\" back\\ slash\/ tab\t nl\n")")),
                     "scan: common escapes decoded");
  runner.expectEqual("S(\xC3\xA9)", render(scan(R"("\u00e9")")), "scan: \\u decoded to UTF-8");
  runner.expectEqual("S(\xF0\x9F\x98\x80)", render(scan(R"("\ud83d\ude00")")), "scan: surrogate pair folded");
  runner.expectEqual("S(?)", render(scan(R"("\ud83d")")), "scan: lone surrogate replaced");
  runner.expectEqual("S(&)", render(scan(R"("\u0026")")), "scan: ampersand from escape");

  // A long string value is handed out in bounded chunks; the caller can stream
  // it without ever holding the whole value.
  {
    std::string body(700, 'x');
    // JSON escapes inside the value: y\"z\\w decodes to y"z\w, so the decoded
    // length matches the input length.
    body.replace(10, 5, "y\\\"z\\\\w");
    const std::string input = "\"" + body + "\"";
    const auto events = scan(input);
    size_t chunks = 0;
    size_t total = 0;
    bool sawEnd = false;
    for (const Event& event : events) {
      if (event.token == JsonScanner::Token::StringChunk) {
        ++chunks;
        total += event.text.size();
        runner.expectTrue(event.text.size() < JsonScanner::kTextCapacity, "scan: chunk stays inside the buffer");
      } else if (event.token == JsonScanner::Token::StringEnd) {
        sawEnd = true;
        total += event.text.size();
      }
    }
    runner.expectTrue(chunks >= 2, "scan: long value split into chunks");
    runner.expectTrue(sawEnd, "scan: long value terminated");
    // The escapes decode back to the original 700 characters.
    runner.expectEq<size_t>(700, total, "scan: chunked value keeps every byte");
  }

  // Chunk boundaries must not corrupt tokens: the same input at several read
  // sizes produces the same token stream.
  {
    const std::string input = R"({"items":[{"id":"tag:x","title":"Hello \u00e9","n":17},{"id":"tag:y"}]})";
    const std::string reference = render(scan(input, 4096));
    for (size_t size : {1u, 2u, 3u, 5u, 7u, 13u, 64u}) {
      runner.expectEqual(reference, render(scan(input, size)), "scan: read size " + std::to_string(size) + " matches");
    }
  }

  // Errors.
  runner.expectTrue(hasError(scan(R"({"a":"\q"})")), "scan: unknown escape is an error");
  runner.expectTrue(hasError(scan(R"({"a":"unterminated)")), "scan: unterminated string is an error");
  runner.expectTrue(hasError(scan(R"({"a":1} trailing)")), "scan: trailing text after the root value is an error");
  runner.expectTrue(hasError(scan(R"({"a":[1,2})")), "scan: mismatched bracket is an error");
  runner.expectTrue(hasError(scan(R"({"a":@})")), "scan: stray character is an error");
  runner.expectTrue(hasError(scan(R"({"a":tru})")), "scan: truncated literal is an error");
  runner.expectTrue(hasError(scan(R"({"a":1,})")), "scan: trailing comma is an error");
  {
    std::string deep;
    for (int i = 0; i < 20; ++i) deep += "[";
    runner.expectTrue(hasError(scan(deep)), "scan: nesting limit reported");
  }
  {
    JsonScanner scanner;
    const std::string input = "{\"a\":1";
    scanner.feed(input.data(), input.size(), nullptr);
    const JsonScanner::Token token = scanner.finish();
    runner.expectTrue(token == JsonScanner::Token::Error, "scan: truncated container reported at end");
  }

  // The scanner reports its own failure state so a collector can stop early.
  {
    // Feeding a whole buffer keeps returning tokens; the caller loops until the
    // bytes are consumed. failed() then reports the error that was returned.
    JsonScanner scanner;
    const std::string input = R"({"a":"\q"})";
    JsonScanner::Token last = JsonScanner::Token::None;
    for (size_t i = 0; i < input.size(); ++i) {
      const JsonScanner::Token token = scanner.feed(input[i]);
      if (token != JsonScanner::Token::None) last = token;
    }
    runner.expectTrue(last == JsonScanner::Token::Error, "scan: bad escape reported while draining");
    runner.expectTrue(scanner.failed(), "scan: failed() set after an error");
    runner.expectTrue(scanner.text()[0] != '\0', "scan: error text present");
  }

  return runner.allPassed() ? 0 : 1;
}
