#pragma once
#include <Arduino.h>
#include <SD.h>
#include "epub_types.h"

// Callback invoked for each parsed render element.
// Return false to stop parsing immediately (page-full signal).
typedef bool (*ElemCallback)(const RenderElem& elem, void* ctx);

// ---------------------------------------------------------------------------
// XhtmlParser — streaming pull-parser for EPUB XHTML.
//
// Supported elements
//   Block/text:  h1, h2, h3, h4, p, strong/b, br
//   Lists:       ol (with start="N"), li
//   Images:      img inside mjx-container (PNG inline/block)
//                img class="math-inline" anywhere (PNG inline)
//                img with .jpg/.jpeg extension (JPEG block)
//                img with .png extension outside mjx-container (PNG block)
//   Tables:      table, thead, tbody, tr, th, td
//                   Mixed cell content (text + inline PNGs) is accumulated
//                   into the cell's text buffer using CELL_IMG_SENTINEL.
//
// Usage (same as before):
//   XhtmlParser parser;
//   parser.parse(path, basePath, cb, ctx);   // first page
//   while (parser.resumeParse(cb, ctx)) { … } // subsequent pages
//   parser.close();
// ---------------------------------------------------------------------------
class XhtmlParser {
public:
  ~XhtmlParser() { close(); }

  bool parse(const char* sdPath, const char* basePath,
             ElemCallback cb, void* ctx);
  bool resumeParse(ElemCallback cb, void* ctx);
  void close();

private:
  File      _file;
  char      _buf[256];
  int       _bufLen = 0;
  int       _bufPos = 0;

  // ---- Block / text context -----------------------------------------------
  bool      _inMjx        = false;
  bool      _mjxDisplay   = false;
  bool      _inPara       = false;
  bool      _inHeading    = false;
  FontLevel _headingLevel = FONT_BODY;
  bool      _inStrong     = false;   // inside <strong> or <b>

  // ---- Ordered-list context -----------------------------------------------
  bool      _inOl         = false;
  int       _olCounter    = 1;       // next list-item number (respects start="N")
  bool      _inLi         = false;

  // ---- Table context -------------------------------------------------------
  bool      _inTable      = false;
  bool      _inTHead      = false;   // inside <thead>
  bool      _inCell       = false;   // inside <td> or <th> — emit() buffers content
  bool      _cellIsHeader = false;   // true when current cell tag was <th>
  char      _cellBuf[300];           // accumulated cell text + CELL_IMG_SENTINEL image paths
  int       _cellBufLen   = 0;

  // ---- Book paths ---------------------------------------------------------
  char      _basePath[MAX_PATH_LEN];

  ElemCallback _cb  = nullptr;
  void*        _ctx = nullptr;
  bool         _open = false;

  // ---- Internals ----------------------------------------------------------
  int  readChar();
  bool readText(char stopAt);
  bool processTag();
  bool getAttrValue(const char* tag, const char* attr, char* out, int outLen);

  // emit() routes through cell-buffering when _inCell is true.
  bool emit(RenderElem& e);

  bool streamLoop();
};
