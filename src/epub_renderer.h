#pragma once
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Adafruit_GFX.h>
#include <SD.h>
#include <PNGdec.h>
#include "epub_types.h"
#include "framebuffer.h"

typedef GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 2> DisplayType;

// -----------------------------------------------------------------------
// Draw command — one text word or one image at a fixed position
// -----------------------------------------------------------------------
enum DrawCmdType : uint8_t { DC_TEXT, DC_IMAGE };

struct DrawCmd {
  DrawCmdType type;
  FontLevel   fontLevel;
  int16_t     x, y;
  char        text[MAX_TEXT_LEN];
  char        path[MAX_PATH_LEN];
};

#define TOTAL_CMDS  8192
#define MAX_PAGES     64

// -----------------------------------------------------------------------
// Thin GFX canvas that writes pixels into the global Framebuffer.
// Inherits Adafruit_GFX so we get all text/font rendering for free.
// -----------------------------------------------------------------------
class FbCanvas : public Adafruit_GFX {
public:
  FbCanvas() : Adafruit_GFX(DISPLAY_W, DISPLAY_H) {}

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    // GFX passes 0=black, 1=white (same convention as GxEPD_BLACK/WHITE)
    g_fb.drawPixel(x, y, color ? 1 : 0);
  }
};

// -----------------------------------------------------------------------
// EpubRenderer
// -----------------------------------------------------------------------
class EpubRenderer {
public:
  EpubRenderer(DisplayType& disp) : _disp(disp) {}

  bool beginDoc();
  void feed(const RenderElem& elem);
  void endDoc();

  // Render page index into framebuffer then push to display — one refresh
  void showPage(int index);
  int  pageCount() const { return _pageCount; }

  void pngRowDraw(PNGDRAW* pDraw);

private:
  DisplayType& _disp;
  FbCanvas     _canvas;

  DrawCmd*  _pool     = nullptr;
  int       _poolUsed = 0;

  struct PageInfo { int start; int count; };
  PageInfo  _pageInfo[MAX_PAGES];
  int       _pageCount = 0;

  int16_t _cx = MARGIN_LEFT;
  int16_t _cy = MARGIN_TOP;
  int16_t _lineH = 0;
  bool    _lineHasContent = false;

  int16_t  _imgDestX = 0;
  int16_t  _imgDestY = 0;

  void selectFont(FontLevel level);
  void newLine(int16_t extraSpacing = 0);
  void breakPage();
  void checkPageOverflow(int16_t neededH);
  void addCmd(const DrawCmd& cmd);

  void layoutWord(const char* word, FontLevel level);
  void layoutInlineImage(const char* path);
  void layoutBlockImage(const char* path);

  bool pngGetSize(const char* path, uint16_t& w, uint16_t& h);
  bool decodePngToFb(const char* path, int16_t destX, int16_t destY);

  static int  s_pngDraw(PNGDRAW* pDraw);
  static EpubRenderer* s_instance;

  uint16_t _lineBuffer[DISPLAY_W];
  PNG      _png;
};
