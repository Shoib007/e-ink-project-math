#pragma once
#include <GxEPD2_BW.h>

// ---- Fonts ------------------------------------------------------------------
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBoldOblique9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>

#include <Adafruit_GFX.h>
#include <SD.h>
#include <PNGdec.h>
#include <TJpg_Decoder.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "epub_types.h"
#include "framebuffer.h"
#include "table_renderer.h"

typedef GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 2> DisplayType;

// ---------------------------------------------------------------------------
// FbCanvas — an Adafruit_GFX-compatible canvas that draws directly into the
// shared SlotPool framebuffer (no secondary heap allocation).
// ---------------------------------------------------------------------------
class FbCanvas : public Adafruit_GFX {
public:
  FbCanvas() : Adafruit_GFX(DISPLAY_W, DISPLAY_H) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    g_pool.drawPixel(x, y, color ? 1 : 0);
  }
};

// ---------------------------------------------------------------------------
// EpubRenderer — consumes RenderElem tokens from XhtmlParser and renders them
// into the SlotPool framebuffer.  Supports:
//
//   Text          h1…h4 headings, body text, strong/bold, ordered-list items
//   Images        inline PNG math, block PNG figures, block JPEG figures
//   Tables        fixed equal-column layout; row-level page breaks
//   Layout        word wrap, line break (<br>), paragraph breaks
// ---------------------------------------------------------------------------
class EpubRenderer {
public:
  explicit EpubRenderer(DisplayType& disp) : _disp(disp) {}

  void setSdMutex(SemaphoreHandle_t m) { _sdMutex = m; }

  // ---- Render side (Core 1) -----------------------------------------------
  void beginPage(int slot);
  bool feed(const RenderElem& elem);
  void endPage();

  // ---- Display side (Core 0) ----------------------------------------------
  void showPageFull   (int slot, const char* bookName, const char* dateTime,
                       int pageNumber, int totalPages);
  void showPagePartial(int slot, const char* bookName, const char* dateTime,
                       int pageNumber, int totalPages);

  // PNG row callback (called from static s_pngDraw → pngRowDraw)
  void pngRowDraw(PNGDRAW* pDraw);

private:
  DisplayType& _disp;
  FbCanvas     _canvas;

  // ---- Layout cursor ------------------------------------------------------
  int16_t _cx             = MARGIN_LEFT;
  int16_t _cy             = MARGIN_TOP;
  int16_t _lineH          = 0;
  bool    _lineHasContent = false;

  // Cached body-font ascent — computed once in beginPage(), used in
  // checkPageOverflow() so we never call selectFont() mid-draw.
  int16_t _bodyFontAscent = 12;   // sensible default until beginPage() runs

  // ---- Font metrics cache (computed once per page in beginPage()) ----------
  // Indexed by FontLevel.  Avoids repeated getTextBounds() calls per word.
  struct FontMetrics { int16_t ascent; int16_t descent; int16_t lineAdv; };
  FontMetrics _fontMetrics[6] = {};
  FontLevel   _curFontLevel   = static_cast<FontLevel>(-1);  // tracks active font

  // Wrap bounds — changed for list items and table cells
  int16_t _wrapLeftMargin = MARGIN_LEFT;
  int16_t _rightBound     = DISPLAY_W - MARGIN_RIGHT;

  bool    _pageFull   = false;
  bool    _inListItem = false;

  // ---- Table state --------------------------------------------------------
  TableRenderer _table;
  bool          _inTable    = false;   // true between ELEM_TABLE_START / ELEM_TABLE_END

  // ---- Image decode -------------------------------------------------------
  int16_t  _imgDestX = 0;
  int16_t  _imgDestY = 0;
  uint16_t _lineBuffer[DISPLAY_W];
  PNG      _png;

  // Inline image scaling support
  float    _inlineImgScale = 1.0f;
  int16_t  _inlineImgDestX = 0;
  int16_t  _inlineImgDestY = 0;
  uint16_t _inlineImgScaledW = 0;
  uint16_t _inlineImgScaledH = 0;

  // JPEG: track decoded extent for two-pass centering
  uint16_t _jpgImgW = 0;
  uint16_t _jpgImgH = 0;

  SemaphoreHandle_t _sdMutex = nullptr;

  // ---- Private helpers ----------------------------------------------------
  int16_t fontAscent(FontLevel level);
  void    cacheAllFontMetrics();
  void    selectFont(FontLevel level);
  void    newLine(int16_t extraSpacing = 0);
  void    signalPageFull();
  void    checkPageOverflow(int16_t neededH);

  void    renderWord        (const char* word, FontLevel level);
  void    renderInlineImage (const char* path);
  void    renderBlockImage  (const char* path);
  void    renderBlockImageJpg(const char* path);

  void    renderTableCell (const char* text, FontLevel lvl,
                            int16_t pixX, int16_t pixY,
                            int16_t colW, int16_t cellH);
  int16_t measureTableCell(const char* text, FontLevel lvl, int16_t colW);

  // ---- Table row accumulation (pending row buffer) ------------------------
  static const int MAX_TABLE_COLS = 8;
  struct TableCell {
    char      text[MAX_TEXT_LEN];
    FontLevel fontLevel;
  };
  bool       _inCellRender = false;  // suppresses overflow checks inside cells
  int        _tableCurCol  = 0;
  TableCell  _tableCells[MAX_TABLE_COLS];

  void    drawHLine(int16_t x, int16_t y, int16_t w);
  void    drawVLine(int16_t x, int16_t y, int16_t h);
  void    drawPageChrome(int slot, const char* bookName, const char* dateTime,
                         int pageNumber, int totalPages);

  bool    decodePng(const char* path, int16_t destX, int16_t destY,
                    uint16_t& outW, uint16_t& outH, bool measureOnly = false);
  bool    decodeJpg(const char* path, int16_t destX, int16_t destY,
                    uint16_t& outW, uint16_t& outH, uint8_t scale = 1);

  static int           s_pngDraw(PNGDRAW* pDraw);
  static bool          s_jpgDraw(int16_t x, int16_t y,
                                  uint16_t w, uint16_t h,
                                  uint16_t* bitmap);
  static EpubRenderer* s_instance;

  // Portrait→native conversion for GD7965 e-ink controller (file-level static in .cpp)
};
