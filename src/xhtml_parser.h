#pragma once
#include <Arduino.h>
#include <SD.h>
#include "epub_types.h"

// Callback invoked for each parsed render element.
// Return false to stop parsing early.
typedef bool (*ElemCallback)(const RenderElem& elem, void* ctx);

// Streaming pull-parser for the EPUB XHTML format.
// Reads the file from SD in small chunks; never loads the whole file.
class XhtmlParser {
public:
  // Parse the file at sdPath, calling cb for each element.
  // basePath is the SD directory prefix for image paths (e.g. "/book3/EPUB/")
  bool parse(const char* sdPath, const char* basePath,
             ElemCallback cb, void* ctx);

private:
  // ---- internal helpers ----
  File     _file;
  char     _buf[256];   // read buffer
  int      _bufLen = 0;
  int      _bufPos = 0;

  // Current parser state
  bool     _inMjx      = false;  // inside <mjx-container>
  bool     _mjxDisplay = false;  // display="true" on current mjx-container
  bool     _inPara     = false;
  bool     _inHeading  = false;
  FontLevel _headingLevel = FONT_H2;
  char     _basePath[MAX_PATH_LEN];

  ElemCallback _cb;
  void*        _ctx;

  // Read one character from SD (buffered)
  int  readChar();
  // Read until '<' building text content; emit ELEM_TEXT tokens
  bool readText(char stopAt);
  // Read and process a complete tag starting after '<'
  bool processTag();
  // Extract attribute value from a tag string
  bool getAttrValue(const char* tag, const char* attr,
                    char* out, int outLen);
  // Emit a single element via callback
  bool emit(RenderElem& e);
};
