#pragma once

#include <cstddef>
#include <cstdint>

// Incremental JSON scanner for FreshRSS (Google Reader API) responses.
//
// The scanner is fed one byte at a time and returns a token when one is
// complete. Long strings are delivered as bounded chunks, so an article body of
// any size streams through a fixed buffer instead of being buffered whole.
// Nothing is allocated on the heap and no Arduino type is used, which keeps the
// whole module testable on the host.

namespace freshrss {

class JsonScanner {
 public:
  enum class Token : uint8_t {
    None,         // needs more input
    ObjectBegin,  // '{'
    ObjectEnd,    // '}'
    ArrayBegin,   // '['
    ArrayEnd,     // ']'
    Key,          // object member name (decoded, truncated to kTextCapacity)
    StringChunk,  // decoded part of a string value
    StringEnd,    // the string value ended; text() holds its last bytes
    Number,       // integer or fractional number
    Bool,         // true or false
    Null,
    Error,  // malformed input; text() holds the reason
  };

  static constexpr size_t kTextCapacity = 256;
  static constexpr uint8_t kMaxDepth = 12;
  // A token ends on the first byte that no longer belongs to it, and that byte
  // must be reprocessed. Two slots cover the worst case: a queued lookahead plus
  // the byte of the current call.
  static constexpr uint8_t kMaxPending = 2;

  void reset() { *this = JsonScanner{}; }

  // Consumes one byte. Returns None while a token is still incomplete.
  Token feed(char c);

  // Flushes a trailing number/literal and reports truncation at the end of a
  // response.
  Token finish();

  // Convenience for tests and short buffers.
  Token feed(const char* text, size_t length, size_t* consumed = nullptr);

  Token token() const { return token_; }
  const char* text() const { return text_; }
  size_t length() const { return length_; }
  // A token exceeded kTextCapacity. Keys and ids report this; long string
  // values simply arrive as several StringChunk tokens and do not.
  bool overflow() const { return overflow_; }
  uint8_t depth() const { return depth_; }
  bool failed() const { return token_ == Token::Error; }

  long long numberValue() const { return number_; }
  bool fractional() const { return fractional_; }
  bool boolValue() const { return bool_; }

 private:
  enum class State : uint8_t {
    Value,       // a value may start here (start of input, after '[', after ':')
    KeyOrEnd,    // inside an object: a key or '}' may start here
    ValueOrEnd,  // inside an array: a value or ']' may start here
    CommaOrEnd,  // a value just ended: ',' or a closer may start here
    Colon,       // a key just ended: ':' must follow
    String,      // reading a string
    Number,      // reading a number
    Literal,     // reading true/false/null
    Done,        // the top-level value ended
    Bad,         // an error was reported
  };

  Token setToken(Token token) {
    token_ = token;
    return token;
  }
  Token step(char c);
  Token fail(const char* reason);
  Token beginString(bool key);
  Token beginNumber(char first);
  Token beginLiteral(char first);
  Token closeContainer(char closer);
  void beginText();
  void appendText(char c);
  // Set when a chunk token was handed out: the buffer is cleared before the
  // next byte is appended, not before the caller reads the chunk.
  bool flushPending_ = false;
  void stash(char c);
  bool takeStashed(char& c);

  // Container stack: '{' or '[' per open container.
  char stack_[kMaxDepth] = {};
  uint8_t depth_ = 0;

  State state_ = State::Value;
  bool stringIsKey_ = false;
  bool escape_ = false;
  bool unicode_ = false;
  uint8_t unicodeDigits_ = 0;
  uint32_t unicodeValue_ = 0;
  uint32_t pendingHighSurrogate_ = 0;
  bool literalIsTrue_ = false;
  bool literalIsFalse_ = false;

  // Bytes that ended the previous token and still need processing.
  char pending_[kMaxPending] = {};
  uint8_t pendingCount_ = 0;
  bool pendingOverflow_ = false;

  char text_[kTextCapacity] = {};
  size_t length_ = 0;
  bool overflow_ = false;
  Token token_ = Token::None;
  long long number_ = 0;
  bool numberNegative_ = false;
  bool fractional_ = false;
  bool bool_ = false;
  // Set by ',' so a stray closer can be rejected as a trailing comma.
  bool afterComma_ = false;
};

}  // namespace freshrss
