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
// FbCanvas — thin Adafruit_GFX canvas that writes into the active page
// buffer in the PageBufferPool. Gives us full font/text rendering free.
// -----------------------------------------------------------------------
class FbCanvas : public Adafruit_GFX {
public:
  FbCanvas() : Adafruit_GFX(DISPLAY_W, DISPLAY_H) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    g_pool.drawPixel(x, y, color ? 1 : 0);
  }
};

// -----------------------------------------------------------------------
// EpubRenderer
//
// Feed phase (setup): parses XHTML, renders text+images directly into
//   per-page PSRAM buffers. All SD/PNG/font work happens here once.
//
// Show phase (button press): copies a pre-built PSRAM buffer into
//   GxEPD2's internal buffer and triggers one e-ink refresh.
//   No SD access, no PNG decode, no font rendering at show time.
// -----------------------------------------------------------------------
class EpubRenderer {
public:
  EpubRenderer(DisplayType& disp) : _disp(disp) {}

  bool beginDoc();
  void feed(const RenderElem& elem);
  void endDoc();

  // Copy pre-rendered page buffer to display — ONE e-ink refresh cycle
  void showPage(int index);
  int  pageCount() const { return g_pool.pageCount(); }

  void pngRowDraw(PNGDRAW* pDraw);  // PNG row callback, do not call directly

private:
  DisplayType& _disp;
  FbCanvas     _canvas;

  // Layout cursor
  int16_t _cx = MARGIN_LEFT;
  int16_t _cy = MARGIN_TOP;
  int16_t _lineH = 0;
  bool    _lineHasContent = false;

  // PNG decode destination (pixels go into active page buffer)
  int16_t _imgDestX = 0;
  int16_t _imgDestY = 0;

  void selectFont(FontLevel level);
  void newLine(int16_t extraSpacing = 0);
  void breakPage();
  void checkPageOverflow(int16_t neededH);

  void renderWord(const char* word, FontLevel level);
  void renderInlineImage(const char* path);
  void renderBlockImage(const char* path);

  bool pngGetSize(const char* path, uint16_t& w, uint16_t& h);
  bool decodePngToPool(const char* path, int16_t destX, int16_t destY);

  static int  s_pngDraw(PNGDRAW* pDraw);
  static EpubRenderer* s_instance;

  uint16_t _lineBuffer[DISPLAY_W];  // one decoded PNG row, internal RAM
  PNG      _png;
};
