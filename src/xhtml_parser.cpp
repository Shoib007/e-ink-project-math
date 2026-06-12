#include "xhtml_parser.h"
#include <string.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool XhtmlParser::parse(const char* sdPath, const char* basePath,
                         ElemCallback cb, void* ctx) {
  // Open fresh
  close();
  _file = SD.open(sdPath);
  if (!_file) {
    Serial.printf("[Parser] Cannot open %s\n", sdPath);
    return false;
  }
  strncpy(_basePath, basePath, MAX_PATH_LEN - 1);
  _basePath[MAX_PATH_LEN - 1] = '\0';

  _bufLen = _bufPos = 0;
  _inMjx = _mjxDisplay = _inPara = _inHeading = false;
  _headingLevel = FONT_BODY;
  _open = true;

  _cb  = cb;
  _ctx = ctx;
  return streamLoop();   // true = stopped early (more content), false = EOF
}

bool XhtmlParser::resumeParse(ElemCallback cb, void* ctx) {
  if (!_open) return false;
  _cb  = cb;
  _ctx = ctx;
  return streamLoop();
}

void XhtmlParser::close() {
  if (_open) {
    _file.close();
    _open = false;
  }
}

// ---------------------------------------------------------------------------
// Core streaming loop
// Returns true  if cb returned false (page full — stop early, more content).
// Returns false at EOF.
// ---------------------------------------------------------------------------
bool XhtmlParser::streamLoop() {
  while (true) {
    int c = readChar();
    if (c < 0) {
      // EOF
      close();
      return false;
    }
    if (c == '<') {
      if (!processTag()) {
        // processTag() returns false when emit() returned false (cb said stop)
        return true;   // more content remaining
      }
    } else {
      _bufPos--;   // un-read the non-'<' char
      if (!readText('<')) {
        // readText() returns false when emit() returned false
        return true;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Emit
// ---------------------------------------------------------------------------
bool XhtmlParser::emit(RenderElem& e) {
  return _cb(e, _ctx);
}

// ---------------------------------------------------------------------------
// Buffered character reader
// ---------------------------------------------------------------------------
int XhtmlParser::readChar() {
  if (_bufPos >= _bufLen) {
    _bufLen = _file.read(reinterpret_cast<uint8_t*>(_buf), sizeof(_buf));
    _bufPos = 0;
    if (_bufLen <= 0) return -1;
  }
  return static_cast<unsigned char>(_buf[_bufPos++]);
}

// ---------------------------------------------------------------------------
// Attribute extraction
// ---------------------------------------------------------------------------
bool XhtmlParser::getAttrValue(const char* tag, const char* attr,
                                char* out, int outLen) {
  const char* p      = tag;
  int         attrLen = strlen(attr);
  while (*p) {
    if (strncasecmp(p, attr, attrLen) == 0) {
      const char* q = p + attrLen;
      while (*q == ' ') q++;
      if (*q == '=') {
        q++;
        while (*q == ' ') q++;
        char delim = (*q == '"' || *q == '\'') ? *q++ : ' ';
        int i = 0;
        while (*q && *q != delim && i < outLen - 1)
          out[i++] = *q++;
        out[i] = '\0';
        return true;
      }
    }
    p++;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Text content reader — splits on whitespace, emits ELEM_TEXT tokens.
// Returns false when emit() returns false (stop signal from caller).
// ---------------------------------------------------------------------------
bool XhtmlParser::readText(char /*stopAt*/) {
  char word[MAX_TEXT_LEN];
  int  wi       = 0;
  bool hadSpace = false;

  auto flushWord = [&]() -> bool {
    if (wi == 0) return true;
    word[wi] = '\0';
    RenderElem e;
    memset(&e, 0, sizeof(e));
    e.type      = _inHeading ? ELEM_HEADING : ELEM_TEXT;
    e.fontLevel = _headingLevel;
    if (hadSpace) {
      char tmp[MAX_TEXT_LEN];
      snprintf(tmp, sizeof(tmp), " %s", word);
      strncpy(e.text, tmp, MAX_TEXT_LEN - 1);
    } else {
      strncpy(e.text, word, MAX_TEXT_LEN - 1);
    }
    wi       = 0;
    hadSpace = false;
    return emit(e);
  };

  while (true) {
    int c = readChar();
    if (c < 0) break;
    if (c == '<') { _bufPos--; break; }   // un-read '<'

    if (c == '&') {
      char entity[16]; int ei = 0; int ch2;
      while ((ch2 = readChar()) > 0 && ch2 != ';' && ei < 14)
        entity[ei++] = ch2;
      entity[ei] = '\0';
      if      (strcmp(entity, "amp")  == 0) c = '&';
      else if (strcmp(entity, "lt")   == 0) c = '<';
      else if (strcmp(entity, "gt")   == 0) c = '>';
      else if (strcmp(entity, "nbsp") == 0) c = ' ';
      else if (strcmp(entity, "apos") == 0) c = '\'';
      else if (strcmp(entity, "quot") == 0) c = '"';
      else                                   c = '?';
    }
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';

    if (c == ' ') {
      if (!flushWord()) return false;
      hadSpace = true;
    } else {
      if (wi < static_cast<int>(sizeof(word)) - 1)
        word[wi++] = static_cast<char>(c);
    }
  }
  return flushWord();
}

// ---------------------------------------------------------------------------
// Tag processor — reads up to and including '>', dispatches.
// Returns false when emit() returns false (stop signal).
// ---------------------------------------------------------------------------
bool XhtmlParser::processTag() {
  char tag[512]; int ti = 0;
  bool inStr = false; char strDelim = 0;
  while (true) {
    int c = readChar();
    if (c < 0) break;
    if (!inStr && (c == '"' || c == '\'')) { inStr = true;  strDelim = static_cast<char>(c); }
    else if (inStr && c == strDelim)        { inStr = false; }
    if (!inStr && c == '>') break;
    if (ti < static_cast<int>(sizeof(tag)) - 1) tag[ti++] = static_cast<char>(c);
  }
  tag[ti] = '\0';

  if (tag[0] == '?' || tag[0] == '!') return true;   // skip XML/DOCTYPE/comments

  bool       isClose  = (tag[0] == '/');
  const char* tagName = isClose ? tag + 1 : tag;

  if (isClose) {
    if (strncasecmp(tagName, "p", 1) == 0 && strlen(tagName) == 1) {
      _inPara = false;
      RenderElem e; memset(&e, 0, sizeof(e));
      e.type = ELEM_PARA_BREAK;
      if (!emit(e)) return false;
    } else if (strncasecmp(tagName, "h2", 2) == 0 ||
               strncasecmp(tagName, "h3", 2) == 0 ||
               strncasecmp(tagName, "h4", 2) == 0) {
      _inHeading = false;
      RenderElem e; memset(&e, 0, sizeof(e));
      e.type = ELEM_HEADING_BREAK;
      if (!emit(e)) return false;
    } else if (strncasecmp(tagName, "mjx-container", 13) == 0) {
      _inMjx = false;
    }
    return true;
  }

  // Opening / self-closing
  if (strncasecmp(tagName, "p", 1) == 0 && (tag[1] == '\0' || tag[1] == ' ')) {
    _inPara       = true;
    _headingLevel = FONT_BODY;
  } else if (strncasecmp(tagName, "h2", 2) == 0) {
    _inHeading = true; _headingLevel = FONT_H2;
  } else if (strncasecmp(tagName, "h3", 2) == 0) {
    _inHeading = true; _headingLevel = FONT_H3;
  } else if (strncasecmp(tagName, "h4", 2) == 0) {
    _inHeading = true; _headingLevel = FONT_H4;
  } else if (strncasecmp(tagName, "mjx-container", 13) == 0) {
    _inMjx = true;
    char dispVal[16];
    _mjxDisplay = getAttrValue(tag, "display", dispVal, sizeof(dispVal)) &&
                  strcasecmp(dispVal, "true") == 0;
  } else if (strncasecmp(tagName, "img", 3) == 0 && _inMjx) {
    char src[MAX_PATH_LEN];
    if (getAttrValue(tag, "src", src, sizeof(src))) {
      RenderElem e; memset(&e, 0, sizeof(e));
      e.type = _mjxDisplay ? ELEM_IMAGE_BLOCK : ELEM_IMAGE_INLINE;
      snprintf(e.path, MAX_PATH_LEN, "%s%s", _basePath, src);
      if (!emit(e)) return false;
    }
  }
  return true;
}
