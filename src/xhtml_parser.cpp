#include "xhtml_parser.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>   // atoi

// ===========================================================================
// Public API
// ===========================================================================

bool XhtmlParser::parse(const char* sdPath, const char* basePath,
                         ElemCallback cb, void* ctx) {
  close();
  _file = SD.open(sdPath);
  if (!_file) {
    Serial.printf("[Parser] Cannot open %s\n", sdPath);
    return false;
  }
  strncpy(_basePath, basePath, MAX_PATH_LEN - 1);
  _basePath[MAX_PATH_LEN - 1] = '\0';

  _bufLen = _bufPos = 0;

  // Reset all parser state
  _inMjx = _mjxDisplay = _inPara = _inHeading = false;
  _headingLevel = FONT_BODY;
  _inStrong     = false;
  _inOl         = false;  _olCounter = 1;  _inLi = false;
  _inTable      = false;  _inTHead = false;
  _inCell       = false;  _cellIsHeader = false;
  _cellBuf[0]   = '\0';   _cellBufLen = 0;

  _open = true;
  _cb   = cb;
  _ctx  = ctx;
  return streamLoop();
}

bool XhtmlParser::resumeParse(ElemCallback cb, void* ctx) {
  if (!_open) return false;
  _cb  = cb;
  _ctx = ctx;
  return streamLoop();
}

void XhtmlParser::close() {
  if (_open) { _file.close(); _open = false; }
}

// ===========================================================================
// Core streaming loop
// Returns true  = stopped early (page full, more content remaining).
// Returns false = EOF.
// ===========================================================================
bool XhtmlParser::streamLoop() {
  while (true) {
    int c = readChar();
    if (c < 0) { close(); return false; }   // EOF
    if (c == '<') {
      if (!processTag()) return true;        // tag triggered page-full
    } else {
      _bufPos--;                             // un-read non-'<'
      if (!readText('<')) return true;
    }
  }
}

// ===========================================================================
// emit() — routes element to callback OR buffers into table-cell accumulator.
//
// When _inCell is true (parser is inside a <td>/<th>), TEXT and IMAGE_INLINE
// elements are appended to _cellBuf instead of calling _cb.  All other element
// types are silently dropped inside cells (e.g. ELEM_BR becomes a space).
// _cb is invoked directly only when _inCell is false.
// ===========================================================================
bool XhtmlParser::emit(RenderElem& e) {
  if (_inCell) {
    if (e.type == ELEM_TEXT || e.type == ELEM_HEADING) {
      int remaining = static_cast<int>(sizeof(_cellBuf)) - _cellBufLen - 1;
      if (remaining > 0) {
        strncat(_cellBuf, e.text, remaining);
        _cellBufLen = static_cast<int>(strlen(_cellBuf));
      }
    } else if (e.type == ELEM_IMAGE_INLINE) {
      int pathLen = static_cast<int>(strlen(e.path));
      int needed  = pathLen + 2;   // sentinel + path + sentinel
      if (_cellBufLen + needed < static_cast<int>(sizeof(_cellBuf)) - 1) {
        _cellBuf[_cellBufLen++] = CELL_IMG_SENTINEL;
        memcpy(_cellBuf + _cellBufLen, e.path, pathLen);
        _cellBufLen += pathLen;
        _cellBuf[_cellBufLen++] = CELL_IMG_SENTINEL;
        _cellBuf[_cellBufLen]   = '\0';
      }
    }
    // All other types (ELEM_BR, ELEM_PARA_BREAK, …) are silently absorbed.
    return true;   // never stop during cell buffering
  }
  return _cb(e, _ctx);
}

// ===========================================================================
// Buffered character reader
// ===========================================================================
int XhtmlParser::readChar() {
  if (_bufPos >= _bufLen) {
    _bufLen = _file.read(reinterpret_cast<uint8_t*>(_buf), sizeof(_buf));
    _bufPos = 0;
    if (_bufLen <= 0) return -1;
  }
  return static_cast<unsigned char>(_buf[_bufPos++]);
}

// ===========================================================================
// Attribute extraction
// ===========================================================================
bool XhtmlParser::getAttrValue(const char* tag, const char* attr,
                                char* out, int outLen) {
  const char* p      = tag;
  int         attrLen = static_cast<int>(strlen(attr));
  while (*p) {
    if (strncasecmp(p, attr, attrLen) == 0) {
      const char* q = p + attrLen;
      while (*q == ' ') ++q;
      if (*q == '=') {
        ++q;
        while (*q == ' ') ++q;
        char delim = (*q == '"' || *q == '\'') ? *q++ : ' ';
        int i = 0;
        while (*q && *q != delim && i < outLen - 1)
          out[i++] = *q++;
        out[i] = '\0';
        return true;
      }
    }
    ++p;
  }
  return false;
}

// ===========================================================================
// Text content reader
// Reads until '<', splits on whitespace, emits ELEM_TEXT / ELEM_HEADING tokens.
// Returns false when emit() returns false (page-full stop signal).
// ===========================================================================
bool XhtmlParser::readText(char /*stopAt*/) {
  char word[MAX_TEXT_LEN];
  int  wi       = 0;
  bool hadSpace = false;

  auto flushWord = [&]() -> bool {
    if (wi == 0 && !hadSpace) return true;
    
    RenderElem e;
    memset(&e, 0, sizeof(e));
    e.type      = _inHeading ? ELEM_HEADING : ELEM_TEXT;
    e.fontLevel = _inHeading   ? _headingLevel
                : _inStrong    ? FONT_BODY_BOLD
                : FONT_BODY;
                
    if (wi == 0 && hadSpace) {
      strcpy(e.text, " ");
    } else {
      word[wi] = '\0';
      if (hadSpace) {
        char tmp[MAX_TEXT_LEN];
        snprintf(tmp, sizeof(tmp), " %s", word);
        strncpy(e.text, tmp, MAX_TEXT_LEN - 1);
      } else {
        strncpy(e.text, word, MAX_TEXT_LEN - 1);
      }
    }
    
    wi       = 0;
    hadSpace = false;
    return emit(e);
  };

  while (true) {
    int c = readChar();
    if (c < 0) break;
    if (c == '<') { _bufPos--; break; }

    // HTML entity decoding
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

// ===========================================================================
// Tag processor
// Reads characters up to and including '>', then dispatches on the tag name.
// Returns false when emit() returned false (page-full stop signal).
// ===========================================================================
bool XhtmlParser::processTag() {
  char tag[1024]; int ti = 0;  // Increased from 512 to 1024 for long image paths
  bool inStr = false; char strDelim = 0;

  while (true) {
    int c = readChar();
    if (c < 0) break;
    if (!inStr && (c == '"' || c == '\'')) { inStr = true;  strDelim = static_cast<char>(c); }
    else if (inStr && c == strDelim)        { inStr = false; }
    if (!inStr && c == '>') break;
    if (ti < static_cast<int>(sizeof(tag)) - 1)
      tag[ti++] = static_cast<char>(c);
  }
  tag[ti] = '\0';

  // Skip XML declaration, DOCTYPE, comments
  if (tag[0] == '?' || tag[0] == '!') return true;

  bool        isClose = (tag[0] == '/');
  const char* tagName = isClose ? tag + 1 : tag;

  // =========================================================================
  // CLOSING TAGS
  // =========================================================================
  if (isClose) {
    // ---- Headings ---------------------------------------------------------
    if ((strncasecmp(tagName, "h1", 2) == 0 ||
         strncasecmp(tagName, "h2", 2) == 0 ||
         strncasecmp(tagName, "h3", 2) == 0 ||
         strncasecmp(tagName, "h4", 2) == 0) &&
        (tagName[2] == '\0' || tagName[2] == ' ')) {
      _inHeading = false;
      RenderElem e; memset(&e, 0, sizeof(e));
      e.type = ELEM_HEADING_BREAK;
      if (!emit(e)) return false;
    }
    // ---- Paragraph --------------------------------------------------------
    else if (strncasecmp(tagName, "p", 1) == 0 && tagName[1] == '\0') {
      _inPara = false;
      RenderElem e; memset(&e, 0, sizeof(e));
      e.type = ELEM_PARA_BREAK;
      if (!emit(e)) return false;
    }
    // ---- Bold / strong ----------------------------------------------------
    else if ((strncasecmp(tagName, "strong", 6) == 0 && (tagName[6]=='\0'||tagName[6]==' ')) ||
             (strncasecmp(tagName, "b",      1) == 0 && (tagName[1]=='\0'||tagName[1]==' '))) {
      _inStrong = false;
    }
    // ---- List item --------------------------------------------------------
    else if (strncasecmp(tagName, "li", 2) == 0 && tagName[2] == '\0') {
      _inLi = false;
      RenderElem e; memset(&e, 0, sizeof(e));
      e.type = ELEM_PARA_BREAK;
      if (!emit(e)) return false;
    }
    // ---- Ordered list -----------------------------------------------------
    else if (strncasecmp(tagName, "ol", 2) == 0 && tagName[2] == '\0') {
      _inOl = false;
    }
    // ---- Table ------------------------------------------------------------
    else if (strncasecmp(tagName, "table", 5) == 0 && (tagName[5]=='\0'||tagName[5]==' ')) {
      _inTable = false;
      RenderElem e; memset(&e, 0, sizeof(e));
      e.type = ELEM_TABLE_END;
      if (!emit(e)) return false;
    }
    else if (strncasecmp(tagName, "thead", 5) == 0) { _inTHead = false; }
    else if (strncasecmp(tagName, "tbody", 5) == 0) { /* ignored */ }
    // ---- Table row --------------------------------------------------------
    else if (strncasecmp(tagName, "tr", 2) == 0 && tagName[2] == '\0') {
      if (_inTable) {
        RenderElem e; memset(&e, 0, sizeof(e));
        e.type = ELEM_TABLE_ROW_END;
        if (!emit(e)) return false;
      }
    }
    // ---- Table cell flush -------------------------------------------------
    else if ((strncasecmp(tagName, "th", 2) == 0 ||
              strncasecmp(tagName, "td", 2) == 0) && tagName[2] == '\0') {
      if (_inCell) {
        _inCell = false;
        _cellBuf[_cellBufLen] = '\0';

        // Trim leading whitespace
        const char* trimmed = _cellBuf;
        while (*trimmed == ' ') ++trimmed;

        RenderElem e; memset(&e, 0, sizeof(e));
        e.type      = _cellIsHeader ? ELEM_TABLE_HEADER_CELL : ELEM_TABLE_DATA_CELL;
        e.fontLevel = _cellIsHeader ? FONT_BODY_BOLD : FONT_BODY;
        strncpy(e.text, trimmed, MAX_TEXT_LEN - 1);
        // Call _cb directly (emit() interception is already disabled by _inCell=false)
        if (!_cb(e, _ctx)) return false;
      }
    }
    // ---- MathJax container ------------------------------------------------
    else if (strncasecmp(tagName, "mjx-container", 13) == 0) {
      _inMjx = false;
    }
    return true;
  }

  // =========================================================================
  // OPENING / SELF-CLOSING TAGS
  // =========================================================================

  // ---- Headings -----------------------------------------------------------
  if      (strncasecmp(tagName,"h1",2)==0 && (tagName[2]=='\0'||tagName[2]==' ')) {
    _inHeading = true; _headingLevel = FONT_H1;
  } else if (strncasecmp(tagName,"h2",2)==0 && (tagName[2]=='\0'||tagName[2]==' ')) {
    _inHeading = true; _headingLevel = FONT_H2;
  } else if (strncasecmp(tagName,"h3",2)==0 && (tagName[2]=='\0'||tagName[2]==' ')) {
    _inHeading = true; _headingLevel = FONT_H3;
  } else if (strncasecmp(tagName,"h4",2)==0 && (tagName[2]=='\0'||tagName[2]==' ')) {
    _inHeading = true; _headingLevel = FONT_H4;
  }
  // ---- Paragraph ----------------------------------------------------------
  else if (strncasecmp(tagName,"p",1)==0 && (tagName[1]=='\0'||tagName[1]==' ')) {
    _inPara = true; _headingLevel = FONT_BODY;
  }
  // ---- Line break (self-closing) ------------------------------------------
  else if (strncasecmp(tagName,"br",2)==0 &&
           (tagName[2]=='\0'||tagName[2]==' '||tagName[2]=='/')) {
    RenderElem e; memset(&e, 0, sizeof(e));
    e.type = ELEM_BR;
    if (!emit(e)) return false;
  }
  // ---- Bold / strong ------------------------------------------------------
  else if ((strncasecmp(tagName,"strong",6)==0 && (tagName[6]=='\0'||tagName[6]==' ')) ||
           (strncasecmp(tagName,"b",1)==0     && (tagName[1]=='\0'||tagName[1]==' '))) {
    _inStrong = true;
  }
  // ---- Ordered list -------------------------------------------------------
  else if (strncasecmp(tagName,"ol",2)==0 && (tagName[2]=='\0'||tagName[2]==' ')) {
    _inOl = true;
    char startVal[8] = "1";
    getAttrValue(tag, "start", startVal, sizeof(startVal));
    _olCounter = atoi(startVal);
    if (_olCounter < 1) _olCounter = 1;
  }
  // ---- List item ----------------------------------------------------------
  else if (strncasecmp(tagName,"li",2)==0 && (tagName[2]=='\0'||tagName[2]==' ')) {
    if (_inOl) {
      _inLi = true;
      RenderElem e; memset(&e, 0, sizeof(e));
      e.type    = ELEM_OL_ITEM_START;
      e.listNum = static_cast<uint8_t>(_olCounter > 255 ? 255 : _olCounter);
      ++_olCounter;
      if (!emit(e)) return false;
    }
  }
  // ---- Table --------------------------------------------------------------
  else if (strncasecmp(tagName,"table",5)==0 && (tagName[5]=='\0'||tagName[5]==' ')) {
    _inTable = true;
    RenderElem e; memset(&e, 0, sizeof(e));
    e.type = ELEM_TABLE_START;
    if (!emit(e)) return false;
  }
  else if (strncasecmp(tagName,"thead",5)==0) { _inTHead = true; }
  else if (strncasecmp(tagName,"tbody",5)==0) { /* ignored */ }
  // ---- Table row ----------------------------------------------------------
  else if (strncasecmp(tagName,"tr",2)==0 && (tagName[2]=='\0'||tagName[2]==' ')) {
    /* ROW_END is emitted on </tr>; nothing on opening */
  }
  // ---- Table cells --------------------------------------------------------
  else if (strncasecmp(tagName,"th",2)==0 && (tagName[2]=='\0'||tagName[2]==' ')) {
    _inCell       = true;
    _cellIsHeader = true;
    _cellBuf[0]   = '\0';
    _cellBufLen   = 0;
  }
  else if (strncasecmp(tagName,"td",2)==0 && (tagName[2]=='\0'||tagName[2]==' ')) {
    _inCell       = true;
    _cellIsHeader = false;
    _cellBuf[0]   = '\0';
    _cellBufLen   = 0;
  }
  // ---- MathJax container --------------------------------------------------
  else if (strncasecmp(tagName,"mjx-container",13)==0) {
    _inMjx = true;
    char dispVal[16];
    _mjxDisplay = getAttrValue(tag,"display",dispVal,sizeof(dispVal)) &&
                  strcasecmp(dispVal,"true") == 0;
  }
  // ---- Images — all cases -------------------------------------------------
  else if (strncasecmp(tagName,"img",3)==0 &&
           (tagName[3]=='\0'||tagName[3]==' '||tagName[3]=='/')) {

    char src[MAX_PATH_LEN] = {0};
    if (!getAttrValue(tag, "src", src, sizeof(src))) return true;  // no src → skip

    // Determine element type
    char     classVal[32] = {0};
    getAttrValue(tag, "class", classVal, sizeof(classVal));
    bool isMathInline = (strncasecmp(classVal, "math-inline", 11) == 0);

    ElemType imgType;
    if (_inMjx) {
      // Inside a <mjx-container>: respect display="true" flag
      imgType = _mjxDisplay ? ELEM_IMAGE_BLOCK : ELEM_IMAGE_INLINE;
    } else if (isMathInline) {
      // class="math-inline" anywhere outside mjx-container
      imgType = ELEM_IMAGE_INLINE;
    } else {
      // Standalone figure: detect JPEG vs PNG by file extension
      const char* ext = strrchr(src, '.');
      bool isJpeg = ext && (strcasecmp(ext, ".jpg")  == 0 ||
                             strcasecmp(ext, ".jpeg") == 0);
      imgType = isJpeg ? ELEM_IMAGE_BLOCK_JPG : ELEM_IMAGE_BLOCK;
    }

    RenderElem e; memset(&e, 0, sizeof(e));
    e.type = imgType;
    snprintf(e.path, MAX_PATH_LEN, "%s%s", _basePath, src);
    Serial.printf("[Parser] Image path: %s\n", e.path);  // DEBUG
    if (!emit(e)) return false;
  }

  return true;
}
