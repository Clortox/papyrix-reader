#include "FreshRssJson.h"

#include <cstdio>
#include <cstring>

#include "FreshRssText.h"

namespace freshrss {

void JsonScanner::beginText() {
  length_ = 0;
  overflow_ = false;
  text_[0] = '\0';
}

void JsonScanner::appendText(char c) {
  if (length_ + 1 < kTextCapacity) {
    text_[length_++] = c;
    text_[length_] = '\0';
  } else {
    overflow_ = true;
  }
}

void JsonScanner::stash(char c) {
  if (pendingCount_ >= kMaxPending) {
    pendingOverflow_ = true;
    return;
  }
  pending_[pendingCount_++] = c;
}

bool JsonScanner::takeStashed(char& c) {
  if (pendingCount_ == 0) return false;
  c = pending_[0];
  if (--pendingCount_ > 0) pending_[0] = pending_[1];
  return true;
}

JsonScanner::Token JsonScanner::fail(const char* reason) {
  state_ = State::Bad;
  beginText();
  copyBounded(text_, sizeof(text_), reason);
  length_ = std::strlen(text_);
  return setToken(Token::Error);
}

JsonScanner::Token JsonScanner::beginString(bool key) {
  stringIsKey_ = key;
  escape_ = false;
  unicode_ = false;
  unicodeDigits_ = 0;
  unicodeValue_ = 0;
  pendingHighSurrogate_ = 0;
  beginText();
  state_ = State::String;
  return Token::None;
}

JsonScanner::Token JsonScanner::beginNumber(char first) {
  numberNegative_ = false;
  fractional_ = false;
  number_ = 0;
  beginText();
  state_ = State::Number;
  if (first == '-') {
    numberNegative_ = true;
    appendText(first);
    return Token::None;
  }
  number_ = first - '0';
  appendText(first);
  return Token::None;
}

JsonScanner::Token JsonScanner::beginLiteral(char first) {
  literalIsTrue_ = first == 't';
  literalIsFalse_ = first == 'f';
  beginText();
  appendText(first);
  state_ = State::Literal;
  return Token::None;
}

JsonScanner::Token JsonScanner::closeContainer(char closer) {
  const char opener = closer == '}' ? '{' : '[';
  if (depth_ == 0 || stack_[depth_ - 1] != opener) {
    char reason[32];
    std::snprintf(reason, sizeof(reason), "unexpected '%c'", closer);
    return fail(reason);
  }
  --depth_;
  state_ = depth_ == 0 ? State::Done : State::CommaOrEnd;
  return setToken(closer == '}' ? Token::ObjectEnd : Token::ArrayEnd);
}

JsonScanner::Token JsonScanner::step(char c) {
  if (flushPending_) {
    // The previous step handed out a full chunk; start the next one empty.
    flushPending_ = false;
    length_ = 0;
    text_[0] = '\0';
  }
  switch (state_) {
    case State::Value:
    case State::ValueOrEnd: {
      if (isJsonSpace(c)) return Token::None;
      if (c == ']') {
        if (state_ == State::Value || afterComma_) {
          return fail(afterComma_ ? "unexpected trailing comma" : "unexpected ']'");
        }
        return closeContainer(']');
      }
      afterComma_ = false;
      if (c == '{') {
        if (depth_ >= kMaxDepth) return fail("too many nested objects");
        stack_[depth_++] = '{';
        state_ = State::KeyOrEnd;
        return setToken(Token::ObjectBegin);
      }
      if (c == '[') {
        if (depth_ >= kMaxDepth) return fail("too many nested arrays");
        stack_[depth_++] = '[';
        state_ = State::ValueOrEnd;
        return setToken(Token::ArrayBegin);
      }
      if (c == '"') return beginString(false);
      if (c == '-' || (c >= '0' && c <= '9')) return beginNumber(c);
      if (c == 't' || c == 'f' || c == 'n') return beginLiteral(c);
      char reason[32];
      std::snprintf(reason, sizeof(reason), "unexpected character 0x%02X", static_cast<unsigned char>(c));
      return fail(reason);
    }
    case State::KeyOrEnd: {
      if (isJsonSpace(c)) return Token::None;
      if (c == '}') {
        if (afterComma_) return fail("unexpected trailing comma");
        return closeContainer('}');
      }
      if (c != '"') return fail("expected a key");
      afterComma_ = false;
      return beginString(true);
    }
    case State::Colon: {
      if (isJsonSpace(c)) return Token::None;
      if (c != ':') return fail("expected ':'");
      state_ = State::Value;
      return Token::None;
    }
    case State::CommaOrEnd: {
      if (isJsonSpace(c)) return Token::None;
      if (c == ',') {
        afterComma_ = true;
        state_ = (depth_ > 0 && stack_[depth_ - 1] == '{') ? State::KeyOrEnd : State::ValueOrEnd;
        return Token::None;
      }
      if (c == '}') return closeContainer('}');
      if (c == ']') return closeContainer(']');
      return fail("expected ',' or a closing bracket");
    }
    case State::String: {
      if (unicode_) {
        const int digit = hexValue(c);
        if (digit < 0) return fail("bad \\u escape");
        unicodeValue_ = (unicodeValue_ << 4) | static_cast<uint32_t>(digit);
        if (++unicodeDigits_ < 4) return Token::None;
        unicode_ = false;
        if (pendingHighSurrogate_ != 0) {
          if (unicodeValue_ >= 0xDC00 && unicodeValue_ <= 0xDFFF) {
            const uint32_t combined = 0x10000 + ((pendingHighSurrogate_ - 0xD800) << 10) + (unicodeValue_ - 0xDC00);
            pendingHighSurrogate_ = 0;
            utf8Append(text_, kTextCapacity, length_, combined);
          } else {
            utf8Append(text_, kTextCapacity, length_, pendingHighSurrogate_);
            pendingHighSurrogate_ = 0;
            if (unicodeValue_ >= 0xD800 && unicodeValue_ <= 0xDBFF)
              pendingHighSurrogate_ = unicodeValue_;
            else
              utf8Append(text_, kTextCapacity, length_, unicodeValue_);
          }
        } else if (unicodeValue_ >= 0xD800 && unicodeValue_ <= 0xDBFF) {
          pendingHighSurrogate_ = unicodeValue_;
        } else {
          utf8Append(text_, kTextCapacity, length_, unicodeValue_);
        }
      } else if (escape_) {
        escape_ = false;
        switch (c) {
          case '"':
          case '\\':
          case '/':
            appendText(c);
            break;
          case 'b':
            appendText('\b');
            break;
          case 'f':
            appendText('\f');
            break;
          case 'n':
            appendText('\n');
            break;
          case 'r':
            appendText('\r');
            break;
          case 't':
            appendText('\t');
            break;
          case 'u':
            unicode_ = true;
            unicodeDigits_ = 0;
            unicodeValue_ = 0;
            break;
          default:
            return fail("unknown string escape");
        }
      } else if (c == '\\') {
        escape_ = true;
        return Token::None;
      } else if (c == '"') {
        if (pendingHighSurrogate_ != 0) {
          utf8Append(text_, kTextCapacity, length_, pendingHighSurrogate_);
          pendingHighSurrogate_ = 0;
        }
        length_ = std::strlen(text_);
        if (stringIsKey_) {
          state_ = State::Colon;
          return setToken(Token::Key);
        }
        state_ = State::CommaOrEnd;
        // The last piece of the value rides on StringEnd so a streaming caller
        // handles every byte exactly once.
        return setToken(Token::StringEnd);
      } else {
        appendText(c);
      }
      length_ = std::strlen(text_);
      if (!stringIsKey_ && length_ + 1 >= kTextCapacity) {
        // Hand the full buffer out; it is cleared just before the next byte.
        flushPending_ = true;
        return setToken(Token::StringChunk);
      }
      return Token::None;
    }
    case State::Number: {
      const bool partOfNumber = (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-';
      if (partOfNumber) {
        if (c >= '0' && c <= '9' && !fractional_) {
          // Saturate well above any timestamp or count this client handles; a
          // low cap silently truncated epoch seconds.
          if (number_ < 922337203685477580LL) number_ = number_ * 10 + (c - '0');
        } else if (c == '.' || c == 'e' || c == 'E') {
          fractional_ = true;
        }
        appendText(c);
        return Token::None;
      }
      stash(c);
      state_ = depth_ == 0 ? State::Done : State::CommaOrEnd;
      length_ = std::strlen(text_);
      if (numberNegative_) number_ = -number_;
      return setToken(Token::Number);
    }
    case State::Literal: {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        appendText(toLowerAscii(c));
        return Token::None;
      }
      stash(c);
      length_ = std::strlen(text_);
      state_ = depth_ == 0 ? State::Done : State::CommaOrEnd;
      if (std::strcmp(text_, "true") == 0) {
        bool_ = true;
        return setToken(Token::Bool);
      }
      if (std::strcmp(text_, "false") == 0) {
        bool_ = false;
        return setToken(Token::Bool);
      }
      if (std::strcmp(text_, "null") == 0) return setToken(Token::Null);
      return fail("unrecognized literal");
    }
    case State::Done:
      // Anything but whitespace after a complete document means the stream is
      // not the JSON response that was expected.
      return isJsonSpace(c) ? Token::None : fail("trailing content after the document");
    case State::Bad:
      return Token::None;
  }
  return Token::None;
}

JsonScanner::Token JsonScanner::feed(char c) {
  if (state_ == State::Bad) return token_;
  // A byte that ended the previous token is replayed first; the byte of this
  // call stays queued behind it.
  char stashed = 0;
  if (takeStashed(stashed)) {
    const Token produced = step(stashed);
    if (produced != Token::None) {
      stash(c);
      return produced;
    }
  }
  token_ = Token::None;
  return step(c);
}

JsonScanner::Token JsonScanner::finish() {
  if (state_ == State::Bad) return token_;
  // A trailing number or literal has no separator, and a queued lookahead may
  // still close the last container.
  if (state_ == State::Number || state_ == State::Literal) return feed('\n');
  if (state_ == State::String) return fail("unterminated string");
  if (pendingCount_ > 0) return feed('\n');
  if (state_ == State::Done) return Token::None;
  if (depth_ != 0) return fail("response ended inside a container");
  return Token::None;
}

JsonScanner::Token JsonScanner::feed(const char* text, size_t length, size_t* consumed) {
  if (!text) {
    if (consumed) *consumed = 0;
    return Token::None;
  }
  for (size_t i = 0; i < length; ++i) {
    const Token token = feed(text[i]);
    if (token != Token::None) {
      if (consumed) *consumed = i + 1;
      return token;
    }
  }
  if (consumed) *consumed = length;
  return Token::None;
}

}  // namespace freshrss
