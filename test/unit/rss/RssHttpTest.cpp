// HTTP request writing and response framing for the FreshRSS client: form
// encoding, request construction, Content-Length and chunked bodies, and the
// error paths a flaky server produces.

#include <string>

#include "../../common/freshrss_test_fakes.h"
#include "../../common/test_utils.h"

using freshrss::HttpRequest;
using freshrss::HttpResponseHead;
using freshrss::HttpSession;

namespace {

std::string readAll(HttpSession& session, size_t block = 64) {
  std::string body;
  char buffer[64];
  while (true) {
    const int got = session.readBody(buffer, std::min(block, sizeof(buffer)));
    if (got <= 0) break;
    body.append(buffer, static_cast<size_t>(got));
  }
  return body;
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("FreshRss HTTP");

  // --- encoding helpers -----------------------------------------------------
  {
    char out[128];
    runner.expectEq<size_t>(13, freshrss::formEncodeAppend(out, sizeof(out), 0, "a b&c=d"),
                            "form: encoded length returned");
    runner.expectEqual("a%20b%26c%3Dd", std::string(out), "form: percent encoding");
    runner.expectEq<size_t>(7, freshrss::formEncodeAppend(out, sizeof(out), 0, "Ab-_.~9"), "form: unreserved kept");
    runner.expectEqual("Ab-_.~9", std::string(out), "form: unreserved unchanged");
    freshrss::formEncodeAppend(out, sizeof(out), 0, "\xC3\xA9");
    runner.expectEqual("%C3%A9", std::string(out), "form: UTF-8 bytes encoded");
    runner.expectEq<size_t>(0, freshrss::formEncodeAppend(out, 4, 0, "abcd"), "form: overflow reports zero");
  }
  {
    char path[96];
    runner.expectEq<size_t>(
        35, freshrss::buildRequestPath(path, sizeof(path), "/api/greader.php", "/reader/api/0/token", nullptr),
        "path: base and API joined");
    runner.expectEqual("/api/greader.php/reader/api/0/token", std::string(path), "path: joined value");
    freshrss::buildRequestPath(path, sizeof(path), "/api/greader.php", "/reader/api/0/subscription/list",
                               "output=json");
    runner.expectEqual("/api/greader.php/reader/api/0/subscription/list?output=json", std::string(path),
                       "path: query appended");
    runner.expectEq<size_t>(0, freshrss::buildRequestPath(path, 4, "/api", "/x", nullptr),
                            "path: overflow reports zero");
    runner.expectEq<size_t>(0, freshrss::buildRequestPath(path, 16, "/api/greader.php", "/reader/api/0/token", nullptr),
                            "path: long path refused");
  }
  {
    char host[64];
    freshrss::buildHostHeader(host, sizeof(host), "example.net", 443, true);
    runner.expectEqual("example.net", std::string(host), "host: default TLS port omitted");
    freshrss::buildHostHeader(host, sizeof(host), "example.net", 8080, false);
    runner.expectEqual("example.net:8080", std::string(host), "host: non-default port included");
  }
  runner.expectEqual("API password rejected", std::string(freshrss::describeHttpStatus(401)), "status: 401 explained");
  runner.expectEqual("API address not found", std::string(freshrss::describeHttpStatus(404)), "status: 404 explained");
  runner.expectEqual("server error", std::string(freshrss::describeHttpStatus(503)), "status: 503 explained");

  // --- request construction -------------------------------------------------
  {
    freshrss_test::ScriptedTransport transport;
    transport.queue(freshrss_test::httpResponse(200, "OK"));
    HttpSession session(transport);
    HttpRequest request;
    request.method = "POST";
    request.path = "/api/greader.php/accounts/ClientLogin";
    request.host = "example.net";
    request.body = "Email=alice&Passwd=secret";
    request.bodyLength = std::strlen(request.body);
    HttpResponseHead head;
    char error[64] = {};
    runner.expectTrue(session.request(request, head, error, sizeof(error)), "request: response accepted");
    runner.expectEq(200, head.status, "request: status parsed");
    runner.expectEq<long long>(2, head.contentLength, "request: content length parsed");
    const std::string expected =
        "POST /api/greader.php/accounts/ClientLogin HTTP/1.1\r\n"
        "Host: example.net\r\n"
        "User-Agent: PapyriX-RSS\r\nAccept: application/json\r\nAccept-Encoding: identity\r\n"
        "Connection: keep-alive\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 25\r\n"
        "\r\n"
        "Email=alice&Passwd=secret";
    runner.expectEqual(expected, transport.requests, "request: exact bytes on the wire");
  }
  {
    freshrss_test::ScriptedTransport transport;
    transport.queue(freshrss_test::httpResponse(200, "{}"));
    HttpSession session(transport);
    HttpRequest request;
    request.path = "/api/greader.php/reader/api/0/subscription/list?output=json";
    request.host = "example.net";
    request.authorization = "GoogleLogin auth=alice/abc";
    request.keepAlive = false;
    HttpResponseHead head;
    session.request(request, head, nullptr, 0);
    runner.expectTrue(transport.requests.find("Authorization: GoogleLogin auth=alice/abc\r\n") != std::string::npos,
                      "request: authorization header sent");
    runner.expectTrue(transport.requests.find("Connection: close\r\n") != std::string::npos,
                      "request: close requested when asked");
    runner.expectFalse(head.keepAlive, "request: keep-alive off for close requests");
  }

  // --- body framing ---------------------------------------------------------
  {
    freshrss_test::ScriptedTransport transport;
    transport.queue(freshrss_test::httpResponse(200, "hello body", "Connection: close\r\n"));
    HttpSession session(transport);
    HttpRequest request;
    request.path = "/x";
    request.host = "h";
    HttpResponseHead head;
    session.request(request, head, nullptr, 0);
    runner.expectFalse(head.keepAlive, "body: Connection: close honoured");
    runner.expectEqual("hello body", readAll(session), "body: content-length body decoded");
  }
  {
    const std::string body = "chunk one, chunk two, chunk three";
    freshrss_test::ScriptedTransport transport;
    transport.queue(freshrss_test::chunkedResponse(body, 7));
    HttpSession session(transport);
    HttpRequest request;
    request.path = "/x";
    HttpRequest ignored;
    HttpResponseHead head;
    session.request(request, head, nullptr, 0);
    runner.expectTrue(head.chunked, "body: chunked flag parsed");
    runner.expectEqual(body, readAll(session), "body: chunked body decoded");
  }
  {
    // One byte per read stresses framing that spans reads.
    const std::string body = "abcdefghijklmnopqrstuvwxyz0123456789";
    freshrss_test::ScriptedTransport transport;
    transport.chunkSize = 1;
    transport.queue(freshrss_test::chunkedResponse(body, 5));
    HttpSession session(transport);
    HttpRequest request;
    request.path = "/x";
    HttpResponseHead head;
    session.request(request, head, nullptr, 0);
    runner.expectEqual(body, readAll(session, 1), "body: chunked body survives byte-wise reads");
  }
  {
    freshrss_test::ScriptedTransport transport;
    transport.queue("HTTP/1.1 204 No Content\r\n\r\n");
    HttpSession session(transport);
    HttpRequest request;
    request.path = "/x";
    HttpResponseHead head;
    runner.expectTrue(session.request(request, head, nullptr, 0), "body: 204 accepted");
    char buffer[8];
    runner.expectEq(0, session.readBody(buffer, sizeof(buffer)), "body: 204 has no body");
  }
  {
    freshrss_test::ScriptedTransport transport;
    transport.queue(freshrss_test::httpResponse(200, "0123456789"));
    HttpSession session(transport);
    HttpRequest request;
    request.path = "/x";
    HttpResponseHead head;
    session.request(request, head, nullptr, 0);
    char buffer[32];
    runner.expectEq(5, session.readBody(buffer, 5), "body: partial reads");
    runner.expectEqual("01234", std::string(buffer, 5), "body: partial read content");
    runner.expectEq(5, session.readBody(buffer, 5), "body: remaining read");
    runner.expectEq(0, session.readBody(buffer, 5), "body: end of body reported");
  }
  {
    freshrss_test::ScriptedTransport transport;
    transport.queue(freshrss_test::httpResponse(200, "tiny"));
    HttpSession session(transport);
    HttpRequest request;
    request.path = "/x";
    HttpResponseHead head;
    session.request(request, head, nullptr, 0);
    char buffer[32];
    runner.expectEq(4, session.readBodyAll(buffer, sizeof(buffer)), "body: readBodyAll length");
    runner.expectEqual("tiny", std::string(buffer), "body: readBodyAll content");
    runner.expectEq(0, session.readBodyAll(buffer, 3), "body: re-reading a finished body is empty");

    // A body that cannot fit must be reported, not silently truncated.
    freshrss_test::ScriptedTransport small;
    small.queue(freshrss_test::httpResponse(200, "0123456789"));
    HttpSession smallSession(small);
    HttpRequest smallRequest;
    smallRequest.path = "/x";
    HttpResponseHead smallHead;
    smallSession.request(smallRequest, smallHead, nullptr, 0);
    char smallBuffer[5];
    runner.expectEq(-1, smallSession.readBodyAll(smallBuffer, sizeof(smallBuffer)), "body: readBodyAll cap enforced");
    runner.expectTrue(smallSession.error()[0] != '\0', "body: readBodyAll cap explained");
  }

  // --- error paths ----------------------------------------------------------
  {
    freshrss_test::ScriptedTransport transport;
    transport.queue("HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort");
    HttpSession session(transport);
    HttpRequest request;
    request.path = "/x";
    HttpResponseHead head;
    session.request(request, head, nullptr, 0);
    char buffer[32];
    runner.expectEq(5, session.readBody(buffer, sizeof(buffer)), "error: partial body delivered first");
    runner.expectEq(-1, session.readBody(buffer, sizeof(buffer)), "error: early close detected");
    runner.expectTrue(session.error()[0] != '\0', "error: message recorded");
  }
  {
    freshrss_test::ScriptedTransport transport;
    transport.queue("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nabc");
    HttpSession session(transport);
    HttpRequest request;
    request.path = "/x";
    HttpResponseHead head;
    session.request(request, head, nullptr, 0);
    char buffer[32];
    int total = 0;
    int got = 0;
    while ((got = session.readBody(buffer, sizeof(buffer))) > 0) total += got;
    runner.expectEq(3, total, "error: readable chunk data delivered first");
    runner.expectEq(-1, got, "error: truncated chunked body detected");
  }
  {
    freshrss_test::ScriptedTransport transport;
    // A header block larger than the session's bound must be refused, however
    // well formed each individual line is.
    std::string longHeader = "HTTP/1.1 200 OK\r\n";
    for (int i = 0; i < 40; ++i) {
      longHeader += "X-Pad-" + std::to_string(i) + ": " + std::string(60, 'a') + "\r\n";
    }
    longHeader += "\r\n";
    transport.queue(longHeader);
    HttpSession session(transport);
    HttpRequest request;
    request.path = "/x";
    HttpResponseHead head;
    char error[64] = {};
    runner.expectFalse(session.request(request, head, error, sizeof(error)), "error: oversized header rejected");
    runner.expectTrue(std::string(error).find("too large") != std::string::npos,
                      "error: oversized header explained");
  }
  {
    freshrss_test::ScriptedTransport transport;
    transport.queue("not http at all\r\n\r\n");
    HttpSession session(transport);
    HttpRequest request;
    request.path = "/x";
    HttpResponseHead head;
    char error[64] = {};
    runner.expectFalse(session.request(request, head, error, sizeof(error)), "error: non-HTTP response rejected");
  }
  {
    freshrss_test::ScriptedTransport transport;
    transport.failConnect = true;
    runner.expectFalse(transport.connect("h", 80, false, true, nullptr, 1000), "error: connect failure surfaced");
  }

  return runner.allPassed() ? 0 : 1;
}
