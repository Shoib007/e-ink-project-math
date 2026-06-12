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
// Usage for sliding-window rendering:
//
//   XhtmlParser parser;
//
//   // First page: opens the file and streams until callback returns false.
//   parser.parse(path, basePath, cb, ctx);
//
//   // Subsequent pages: resumes from where the last parse() / resumeParse()
//   // stopped.  Returns true if there is more content, false at EOF.
//   while (parser.resumeParse(cb, ctx)) { ... }
//
// The file is kept open between calls.  Call close() when done.
// ---------------------------------------------------------------------------
class XhtmlParser {
public:
  ~XhtmlParser() { close(); }

  // Open the file and parse until cb returns false or EOF.
  // Returns true if stopped early (more content remains), false at EOF.
  bool parse(const char* sdPath, const char* basePath,
             ElemCallback cb, void* ctx);

  // Continue parsing from where we stopped.
  // Returns true if stopped early, false at EOF.
  bool resumeParse(ElemCallback cb, void* ctx);

  // Close the file (called automatically on destruction).
  void close();

private:
  File      _file;
  char      _buf[256];
  int       _bufLen = 0;
  int       _bufPos = 0;

  bool      _inMjx       = false;
  bool      _mjxDisplay  = false;
  bool      _inPara      = false;
  bool      _inHeading   = false;
  FontLevel _headingLevel = FONT_BODY;
  char      _basePath[MAX_PATH_LEN];

  ElemCallback _cb  = nullptr;
  void*        _ctx = nullptr;

  bool      _open = false;   // true while file is open and usable

  int  readChar();
  bool readText(char stopAt);
  bool processTag();
  bool getAttrValue(const char* tag, const char* attr, char* out, int outLen);
  bool emit(RenderElem& e);

  // Core streaming loop — returns true if cb returned false (stop-early),
  // false at EOF.
  bool streamLoop();
};
