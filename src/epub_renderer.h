#pragma once
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <SD.h>
#include <PNGdec.h>
#include "epub_types.h"

typedef GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 2> DisplayType;

// -----------------------------------------------------------------------
// A laid-out draw command ready to paint onto the display.
// The layout pass produces these; the paint pass replays them.
// -----------------------------------------------------------------------
enum DrawCmdType { DC_TEXT, DC_IMAGE };

struct DrawCmd {
  DrawCmdType type;
  // DC_TEXT
  char      text[MAX_TEXT_LEN];
  FontLevel fontLevel;
  int16_t   x, y;          // cursor position (GFX setCursor x, baseline y)
  // DC_IMAGE
  char      path[MAX_PATH_LEN];
  int16_t   imgX, imgY;    // top-left destination
};

// Maximum draw commands per page (adjust if you hit the limit)
#define MAX_DRAW_CMDS 512

// -----------------------------------------------------------------------
// EpubRenderer
// Two-phase: layout() accumulates DrawCmds, paint() replays them through
// the GxEPD2 firstPage/nextPage loop so every half-page sees all content.
// -----------------------------------------------------------------------
class EpubRenderer {
public:
  EpubRenderer(DisplayType& disp) : _disp(disp) {}

  void beginDoc();
  void feed(const RenderElem& elem);
  void endDoc();

  // PNG row callback — do not call directly
  void pngRowDraw(PNGDRAW* pDraw);

private:
  DisplayType& _disp;

  // ---- Layout state ----
  int16_t  _cx = MARGIN_LEFT;
  int16_t  _cy = MARGIN_TOP;
  int16_t  _lineH = 0;
  bool     _lineHasContent = false;

  // ---- Per-page draw command buffer ----
  DrawCmd  _cmds[MAX_DRAW_CMDS];
  int      _cmdCount = 0;

  // ---- Paint state (used during GxEPD2 loop) ----
  int16_t  _imgDestX = 0;
  int16_t  _imgDestY = 0;

  // ---- Helpers ----
  void selectFont(FontLevel level);
  void newLine(int16_t extraSpacing = 0);
  void checkPageOverflow(int16_t neededH);

  // Layout helpers — add DrawCmds, no display I/O
  void layoutWord(const char* word, FontLevel level);
  void layoutInlineImage(const char* path);
  void layoutBlockImage(const char* path);

  // Flush current page to display and reset for next page
  void flushPage();

  // Get image dimensions without decoding pixels
  bool pngGetSize(const char* path, uint16_t& w, uint16_t& h);

  // Decode PNG to display at (_imgDestX, _imgDestY)
  bool decodePng(const char* path, int16_t destX, int16_t destY);

  static int  s_pngDraw(PNGDRAW* pDraw);
  static EpubRenderer* s_instance;

  uint16_t _lineBuffer[DISPLAY_W];
  PNG      _png;
};
