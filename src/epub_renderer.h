#pragma once
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Adafruit_GFX.h>
#include <SD.h>
#include <PNGdec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "epub_types.h"
#include "framebuffer.h"

typedef GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 2> DisplayType;

// ---------------------------------------------------------------------------
// FbCanvas — thin Adafruit_GFX canvas that writes into the active slot of
// the SlotPool.  Gives us all GFX text-rendering primitives for free.
// ---------------------------------------------------------------------------
class FbCanvas : public Adafruit_GFX {
public:
  FbCanvas() : Adafruit_GFX(DISPLAY_W, DISPLAY_H) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    g_pool.drawPixel(x, y, color ? 1 : 0);
  }
};

// ---------------------------------------------------------------------------
// EpubRenderer
//
// Renders one page at a time into a caller-chosen SlotPool slot.
//
// Typical call sequence per page (from Core 1):
//
//   renderer.beginPage(slot);   // aim renderer at this slot
//   for each element:
//     renderer.feed(elem);      // may trigger breakPage internally —
//                               // in that case feed() returns false to
//                               // signal "page full, stop feeding"
//   renderer.endPage();         // flush trailing partial line
//
// showPage(slot) is called by Core 0 (displayTask) to push a completed
// slot to the e-ink panel.  It only does a memory-copy + SPI transfer —
// no SD access, no PNG decode.
// ---------------------------------------------------------------------------
class EpubRenderer {
public:
  explicit EpubRenderer(DisplayType& disp) : _disp(disp) {}

  // Set the SD/SPI mutex — must be called before any feed() calls.
  void setSdMutex(SemaphoreHandle_t m) { _sdMutex = m; }

  // --- Render side (Core 1) ---

  // Aim the renderer at `slot` and clear it to white.
  void beginPage(int slot);

  // Feed one element.  Returns true normally.
  // Returns false if the page became full (breakPage triggered internally);
  // the caller must stop and call endPage() immediately in that case.
  bool feed(const RenderElem& elem);

  // Flush the last line of the current page.
  void endPage();

  // --- Display side (Core 0) ---

  // FIRST page after power-on: full refresh (initialises both controller buffers).
  // Must be called exactly once before any showPagePartial() calls.
  void showPageFull(const uint8_t* buf);

  // Subsequent pages: fast partial refresh (~1.6 s vs ~3.7 s full).
  // The display controller uses a differential waveform — only changed pixels
  // are driven, so there is no ghosting.
  void showPagePartial(const uint8_t* buf);

  // Legacy: show from a slot index (used during sliding-window operation
  // when pages are loaded from the SD cache into a PSRAM slot).
  void showSlotFull(int slot);
  void showSlotPartial(int slot);

  // PNG row callback — called by PNGdec, do not call directly.
  void pngRowDraw(PNGDRAW* pDraw);

private:
  DisplayType& _disp;
  FbCanvas     _canvas;

  // Layout cursor — reset at beginPage()
  int16_t _cx = MARGIN_LEFT;
  int16_t _cy = MARGIN_TOP;
  int16_t _lineH = 0;
  bool    _lineHasContent = false;

  // Set true when the current page overflowed mid-feed().
  // Cleared by beginPage().
  bool    _pageFull = false;

  // PNG decode destination
  int16_t _imgDestX = 0;
  int16_t _imgDestY = 0;

  void selectFont(FontLevel level);
  void newLine(int16_t extraSpacing = 0);

  // Marks the page as full — does NOT allocate a new slot.
  // feed() returns false after this is called.
  void signalPageFull();

  void checkPageOverflow(int16_t neededH);

  void renderWord(const char* word, FontLevel level);
  void renderInlineImage(const char* path);
  void renderBlockImage(const char* path);

  // Opens PNG, reads header (width/height), then decodes pixels — single open.
  // Mutex must NOT be held by the caller; this method takes/gives it.
  bool decodePng(const char* path, int16_t destX, int16_t destY,
                 uint16_t& outW, uint16_t& outH);

  static int          s_pngDraw(PNGDRAW* pDraw);
  static EpubRenderer* s_instance;

  SemaphoreHandle_t _sdMutex = nullptr;
  uint16_t          _lineBuffer[DISPLAY_W];
  PNG               _png;
};
