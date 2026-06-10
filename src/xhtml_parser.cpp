#include "xhtml_parser.h"
#include <string.h>
#include <ctype.h>

// -----------------------------------------------------------------------
// Buffered character reader
// -----------------------------------------------------------------------
int XhtmlParser::readChar() {
  if (_bufPos >= _bufLen) {
    _bufLen = _file.read((uint8_t*)_buf, sizeof(_buf));
    _bufPos = 0;
    if (_bufLen <= 0) return -1;
  }
  return (unsigned char)_buf[_bufPos++];
}

// -----------------------------------------------------------------------
// Emit helper
// -----------------------------------------------------------------------
bool XhtmlParser::emit(RenderElem& e) {
  return _cb(e, _ctx);
}

// -----------------------------------------------------------------------
// Extract attribute value from a raw tag string.
// e.g. getAttrValue(tag, "src", out, len)  finds  src="..."
// -----------------------------------------------------------------------
bool XhtmlParser::getAttrValue(const char* tag, const char* attr,
                                char* out, int outLen) {
  const char* p = tag;
  int attrLen = strlen(attr);
  while (*p) {
    // find attr name
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

// -----------------------------------------------------------------------
// Read text content (between tags), splitting on whitespace into tokens.
// Stops when '<' is encountered (leaves '<' unconsumed via flag).
// -----------------------------------------------------------------------
bool XhtmlParser::readText(char /*stopAt*/) {
  char word[MAX_TEXT_LEN];
  int  wi = 0;
  bool hadSpace = false;

  auto flushWord = [&]() -> bool {
    if (wi == 0) return true;
    word[wi] = '\0';
    RenderElem e;
    memset(&e, 0, sizeof(e));
    e.type      = (_inHeading) ? ELEM_HEADING : ELEM_TEXT;
    e.fontLevel = _headingLevel;
    // prepend a space if there was whitespace before this word
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
    if (c == '<') {
      // Put back by adjusting buffer pos — we peek at this in parse()
      _bufPos--;  // un-read '<'
      break;
    }
    if (c == '&') {
      // Simple entity decoding
      char entity[16]; int ei = 0;
      int ch2;
      while ((ch2 = readChar()) > 0 && ch2 != ';' && ei < 14)
        entity[ei++] = ch2;
      entity[ei] = '\0';
      char decoded = '?';
      if      (strcmp(entity, "amp")  == 0) decoded = '&';
      else if (strcmp(entity, "lt")   == 0) decoded = '<';
      else if (strcmp(entity, "gt")   == 0) decoded = '>';
      else if (strcmp(entity, "nbsp") == 0) decoded = ' ';
      else if (strcmp(entity, "apos") == 0) decoded = '\'';
      else if (strcmp(entity, "quot") == 0) decoded = '"';
      c = decoded;
    }
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';

    if (c == ' ') {
      if (!flushWord()) return false;
      hadSpace = true;
    } else {
      if (wi < (int)sizeof(word) - 1)
        word[wi++] = (char)c;
    }
  }
  return flushWord();
}

// -----------------------------------------------------------------------
// Process a complete tag (the '<' has already been consumed).
// Reads everything up to and including '>'.
// -----------------------------------------------------------------------
bool XhtmlParser::processTag() {
  // Read tag content into a buffer
  char tag[512]; int ti = 0;
  bool inStr = false; char strDelim = 0;
  while (true) {
    int c = readChar();
    if (c < 0) break;
    if (!inStr && (c == '"' || c == '\'')) { inStr = true; strDelim = c; }
    else if (inStr && c == strDelim)        { inStr = false; }
    if (!inStr && c == '>') break;
    if (ti < (int)sizeof(tag) - 1) tag[ti++] = (char)c;
  }
  tag[ti] = '\0';

  // Skip XML declaration / DOCTYPE / comments
  if (tag[0] == '?' || tag[0] == '!') return true;

  bool isClose = (tag[0] == '/');
  const char* tagName = isClose ? tag + 1 : tag;

  // ---- closing tags ----
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

  // ---- opening / self-closing tags ----
  bool selfClose = (tag[ti - 1] == '/');

  if (strncasecmp(tagName, "p", 1) == 0 && (tag[1] == '\0' || tag[1] == ' ')) {
    _inPara    = true;
    _headingLevel = FONT_BODY;
  } else if (strncasecmp(tagName, "h2", 2) == 0) {
    _inHeading    = true;
    _headingLevel = FONT_H2;
  } else if (strncasecmp(tagName, "h3", 2) == 0) {
    _inHeading    = true;
    _headingLevel = FONT_H3;
  } else if (strncasecmp(tagName, "h4", 2) == 0) {
    _inHeading    = true;
    _headingLevel = FONT_H4;
  } else if (strncasecmp(tagName, "mjx-container", 13) == 0) {
    _inMjx = true;
    // Check for display="true"
    char dispVal[16];
    _mjxDisplay = getAttrValue(tag, "display", dispVal, sizeof(dispVal))
                  && strcasecmp(dispVal, "true") == 0;
  } else if (strncasecmp(tagName, "img", 3) == 0 && _inMjx) {
    // Extract src attribute
    char src[MAX_PATH_LEN];
    if (getAttrValue(tag, "src", src, sizeof(src))) {
      RenderElem e; memset(&e, 0, sizeof(e));
      e.type = _mjxDisplay ? ELEM_IMAGE_BLOCK : ELEM_IMAGE_INLINE;
      // Build full SD path: basePath + src
      snprintf(e.path, MAX_PATH_LEN, "%s%s", _basePath, src);
      if (!emit(e)) return false;
    }
  }
  return true;
}

// -----------------------------------------------------------------------
// Main parse entry point
// -----------------------------------------------------------------------
bool XhtmlParser::parse(const char* sdPath, const char* basePath,
                         ElemCallback cb, void* ctx) {
  _file = SD.open(sdPath);
  if (!_file) {
    Serial.printf("XhtmlParser: cannot open %s\n", sdPath);
    return false;
  }
  strncpy(_basePath, basePath, MAX_PATH_LEN - 1);
  _basePath[MAX_PATH_LEN - 1] = '\0';
  _cb  = cb;
  _ctx = ctx;
  _bufLen = _bufPos = 0;
  _inMjx = _mjxDisplay = _inPara = _inHeading = false;
  _headingLevel = FONT_BODY;

  while (true) {
    int c = readChar();
    if (c < 0) break;
    if (c == '<') {
      if (!processTag()) break;
    } else {
      // put it back and read text
      _bufPos--;
      if (!readText('<')) break;
    }
  }
  _file.close();
  return true;
}
