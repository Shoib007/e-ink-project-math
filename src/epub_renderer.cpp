#include "epub_renderer.h"
#include <string.h>

EpubRenderer* EpubRenderer::s_instance = nullptr;

// ---------------------------------------------------------------------------
// PNG I/O callbacks (file-level statics; only one PNG decode at a time
// because decodes happen serially on Core 1 under the SD mutex).
// ---------------------------------------------------------------------------
static File s_pngFile;

static void* pngOpen(const char* filename, int32_t* size) {
  s_pngFile = SD.open(filename);
  if (!s_pngFile) return nullptr;
  *size = s_pngFile.size();
  return static_cast<void*>(&s_pngFile);
}
static void    pngClose(void* /*h*/)                         { if (s_pngFile) s_pngFile.close(); }
static int32_t pngRead(PNGFILE* /*h*/, uint8_t* buf, int32_t len) { return s_pngFile.read(buf, len); }
static int32_t pngSeek(PNGFILE* /*h*/, int32_t pos)          { return s_pngFile.seek(pos) ? pos : -1; }

// ---------------------------------------------------------------------------
// PNG row callback
// ---------------------------------------------------------------------------
int EpubRenderer::s_pngDraw(PNGDRAW* pDraw) {
  if (s_instance) s_instance->pngRowDraw(pDraw);
  return 1;
}

void EpubRenderer::pngRowDraw(PNGDRAW* pDraw) {
  _png.getLineAsRGB565(pDraw, _lineBuffer, PNG_RGB565_LITTLE_ENDIAN, 0xffffffffu);
  int16_t py = _imgDestY + static_cast<int16_t>(pDraw->y);
  if (py < 0 || py >= DISPLAY_H) return;
  for (int x = 0; x < pDraw->iWidth; ++x) {
    int16_t px = _imgDestX + static_cast<int16_t>(x);
    if (px < 0 || px >= DISPLAY_W) continue;
    uint16_t p  = _lineBuffer[x];
    uint8_t  r8 = static_cast<uint8_t>(((p >> 11) & 0x1F) << 3);
    uint8_t  g8 = static_cast<uint8_t>(((p >>  5) & 0x3F) << 2);
    uint8_t  b8 = static_cast<uint8_t>(( p        & 0x1F) << 3);
    uint16_t luma = static_cast<uint16_t>(
      (static_cast<uint32_t>(r8) * 299 +
       static_cast<uint32_t>(g8) * 587 +
       static_cast<uint32_t>(b8) * 114) / 1000);
    g_pool.drawPixel(px, py, luma < 180 ? 0 : 1);
  }
}

// ---------------------------------------------------------------------------
// decodePng — single SD open: read header then decode pixels.
// Takes and releases _sdMutex internally.
// Returns true on success; fills outW/outH with image dimensions.
// ---------------------------------------------------------------------------
bool EpubRenderer::decodePng(const char* path,
                              int16_t destX, int16_t destY,
                              uint16_t& outW, uint16_t& outH) {
  s_instance = this;
  _imgDestX  = destX;
  _imgDestY  = destY;

  if (_sdMutex) xSemaphoreTake(_sdMutex, portMAX_DELAY);

  int rc = _png.open(path, pngOpen, pngClose, pngRead, pngSeek, s_pngDraw);
  if (rc != PNG_SUCCESS) {
    if (_sdMutex) xSemaphoreGive(_sdMutex);
    outW = outH = 0;
    return false;
  }

  outW = static_cast<uint16_t>(_png.getWidth());
  outH = static_cast<uint16_t>(_png.getHeight());

  _png.decode(nullptr, 0);   // fires pngRowDraw() for every row
  _png.close();

  if (_sdMutex) xSemaphoreGive(_sdMutex);
  return true;
}

// ---------------------------------------------------------------------------
// Font selection
// ---------------------------------------------------------------------------
void EpubRenderer::selectFont(FontLevel level) {
  switch (level) {
    case FONT_H2: _canvas.setFont(&FreeSansBold18pt7b); break;
    case FONT_H3: _canvas.setFont(&FreeSansBold12pt7b); break;
    case FONT_H4: _canvas.setFont(&FreeSansBold9pt7b);  break;
    default:      _canvas.setFont(&FreeSans9pt7b);       break;
  }
}

// ---------------------------------------------------------------------------
// Page management
// ---------------------------------------------------------------------------
void EpubRenderer::beginPage(int slot) {
  g_pool.setCurrent(slot);
  g_pool.clearCurrent();
  _cx = MARGIN_LEFT;
  _cy = MARGIN_TOP;
  _lineH = 0;
  _lineHasContent = false;
  _pageFull = false;
}

void EpubRenderer::endPage() {
  if (_lineHasContent) newLine();
  // Nothing to flush — pixels are already in the slot buffer.
}

void EpubRenderer::signalPageFull() {
  _pageFull = true;
}

// ---------------------------------------------------------------------------
// Line management
// ---------------------------------------------------------------------------
void EpubRenderer::newLine(int16_t extraSpacing) {
  _cy += (_lineH > 0 ? _lineH : 14) + LINE_SPACING + extraSpacing;
  _cx = MARGIN_LEFT;
  _lineH = 0;
  _lineHasContent = false;
}

void EpubRenderer::checkPageOverflow(int16_t neededH) {
  if (_cy + neededH > DISPLAY_H - MARGIN_BOTTOM)
    signalPageFull();
}

// ---------------------------------------------------------------------------
// Render helpers
// ---------------------------------------------------------------------------
void EpubRenderer::renderWord(const char* word, FontLevel level) {
  if (!word || !word[0]) return;
  selectFont(level);
  _canvas.setTextWrap(false);

  int16_t x1, y1;
  uint16_t tw, th;
  _canvas.getTextBounds(word, _cx, 0, &x1, &y1, &tw, &th);

  // Word doesn't fit on this line → wrap
  if (static_cast<int16_t>(tw) > DISPLAY_W - MARGIN_RIGHT - _cx &&
      _cx > MARGIN_LEFT) {
    newLine();
    _canvas.getTextBounds(word, _cx, 0, &x1, &y1, &tw, &th);
  }
  if (static_cast<int16_t>(th) > _lineH) _lineH = static_cast<int16_t>(th);
  checkPageOverflow(th);
  if (_pageFull) return;   // don't draw on a full page

  _canvas.setTextColor(0);
  _canvas.setCursor(_cx, _cy + (-y1));
  _canvas.print(word);

  _cx += static_cast<int16_t>(tw);
  _lineHasContent = true;
}

void EpubRenderer::renderInlineImage(const char* path) {
  // We decode directly into the slot at a temporary position, then
  // adjust cursor.  We probe the size by just attempting the decode
  // at the current position (position is corrected for baseline).

  // First check if an image of unknown size might overflow — use a
  // conservative estimate; if it overflows we'll know immediately.
  // Actual approach: decode to a scratch position and record size.
  // Because decodePng renders into the current slot anyway, we can
  // just compute destX/destY speculatively and clip in pngRowDraw.

  uint16_t imgW = 0, imgH = 0;

  // Decode with destY at current cursor top; pngRowDraw clips out-of-bounds.
  int16_t destX = _cx;
  int16_t destY = _cy;

  if (!decodePng(path, destX, destY, imgW, imgH)) return;
  if (imgW == 0 || imgH == 0) return;

  // Check if it would have wrapped horizontally — if so, re-decode on
  // the next line.  This is the only case we must decode twice; it is
  // rare because inline images are typically small formula fragments.
  if (static_cast<int16_t>(imgW) > DISPLAY_W - MARGIN_RIGHT - _cx &&
      _cx > MARGIN_LEFT) {
    newLine();
    if (_pageFull) return;
    destX = _cx;
    destY = _cy;
    decodePng(path, destX, destY, imgW, imgH);
  }

  checkPageOverflow(imgH);
  // (pixels already drawn — overflow check is advisory for the line cursor)

  if (static_cast<int16_t>(imgH) > _lineH) _lineH = static_cast<int16_t>(imgH);
  _cx += static_cast<int16_t>(imgW) + 2;
  _lineHasContent = true;
}

void EpubRenderer::renderBlockImage(const char* path) {
  if (_lineHasContent) newLine();
  if (_pageFull) return;

  // Phase 1: decode at x=0 to learn the image dimensions cheaply.
  // pngRowDraw clips any out-of-bounds pixels, so this is safe even
  // though the image may not be centred yet.
  uint16_t imgW = 0, imgH = 0;
  int16_t  destY = _cy;
  if (!decodePng(path, 0, destY, imgW, imgH)) return;
  if (imgW == 0 || imgH == 0) return;

  checkPageOverflow(imgH + LINE_SPACING * 2);
  if (_pageFull) return;

  // Phase 2: if the image is narrower than the display, re-decode
  // centred and overwrite what phase 1 drew.
  int16_t centeredX = (DISPLAY_W - static_cast<int16_t>(imgW)) / 2;
  if (centeredX > 0) {
    // Erase the left-aligned pixels from phase 1 (set rows to white).
    for (int16_t row = destY;
         row < destY + static_cast<int16_t>(imgH) && row < DISPLAY_H;
         ++row) {
      for (int16_t col = 0;
           col < static_cast<int16_t>(imgW) && col < DISPLAY_W;
           ++col)
        g_pool.drawPixel(col, row, 1);  // 1 = white
    }
    // Re-decode at the centred x position.
    decodePng(path, centeredX, destY, imgW, imgH);
  }

  _cy += static_cast<int16_t>(imgH) + LINE_SPACING * 2;
  _cx = MARGIN_LEFT;
  _lineH = 0;
  _lineHasContent = false;
}

// ---------------------------------------------------------------------------
// feed() — called from Core 1 for each parsed element.
// Returns false if the page is full (caller must stop + endPage).
// ---------------------------------------------------------------------------
bool EpubRenderer::feed(const RenderElem& elem) {
  switch (elem.type) {
    case ELEM_TEXT:
    case ELEM_HEADING:
      renderWord(elem.text, elem.fontLevel);
      break;
    case ELEM_IMAGE_INLINE:
      renderInlineImage(elem.path);
      break;
    case ELEM_IMAGE_BLOCK:
      renderBlockImage(elem.path);
      break;
    case ELEM_PARA_BREAK:
      if (_lineHasContent) newLine();
      newLine(PARA_SPACING);
      break;
    case ELEM_HEADING_BREAK:
      if (_lineHasContent) newLine();
      newLine(HEADING_SPACING);
      break;
  }
  return !_pageFull;
}

// ---------------------------------------------------------------------------
// Portrait → native panel orientation
//
// Our page bitmaps are laid out for setRotation(1) (480×800 portrait).
// writeImage() writes in the panel's native landscape coordinates (800×480)
// and does NOT apply Adafruit_GFX rotation — unlike fillScreen/print used for
// status screens.  Remap here so cached pages match the loading screen.
// ---------------------------------------------------------------------------
static uint8_t* s_nativeBuf = nullptr;

static const uint8_t* portraitToNative(const uint8_t* portrait) {
  if (!portrait) return nullptr;
  if (!s_nativeBuf) {
    s_nativeBuf = static_cast<uint8_t*>(
      heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM));
    if (!s_nativeBuf) return nullptr;
  }

  constexpr int16_t panelW     = GxEPD2_750_T7::WIDTH;   // 800
  constexpr int16_t panelH     = GxEPD2_750_T7::HEIGHT;  // 480
  constexpr int     panelStride = (panelW + 7) / 8;

  memset(s_nativeBuf, 0xFF, FB_SIZE);
  for (int16_t ny = 0; ny < panelH; ++ny) {
    for (int16_t nx = 0; nx < panelW; ++nx) {
      // Inverse of GxEPD2_BW drawPixel rotation 1:
      //   native x = panelW - 1 - portrait y
      //   native y = portrait x
      const int16_t px = ny;
      const int16_t py = panelW - 1 - nx;
      const uint8_t srcByte = portrait[py * FB_STRIDE + (px >> 3)];
      const bool white = (srcByte >> (7 - (px & 7))) & 1;
      uint8_t* dstByte = &s_nativeBuf[ny * panelStride + (nx >> 3)];
      const uint8_t mask = static_cast<uint8_t>(0x80u >> (nx & 7));
      if (white) *dstByte |= mask;
      else       *dstByte &= static_cast<uint8_t>(~mask);
    }
  }
  return s_nativeBuf;
}

// ---------------------------------------------------------------------------
// showPageFull — full refresh for the very first page after power-on.
// This initialises both controller frame buffers so partial updates
// work correctly afterwards (the GD7965 needs both buffers equal).
//
// Uses writeImage() + refresh(false) which is the correct GxEPD2 sequence.
// No drawPixel loop — direct DMA bitmap transfer.
// ---------------------------------------------------------------------------
void EpubRenderer::showPageFull(const uint8_t* buf) {
  const uint8_t* native = portraitToNative(buf);
  if (!native) return;
  // Write to the "new" buffer (native 800×480 panel coordinates)
  _disp.writeImage(native, 0, 0, GxEPD2_750_T7::WIDTH, GxEPD2_750_T7::HEIGHT,
                   false, false, false);
  _disp.refresh(false);   // false = full refresh waveform (~3.7 s)
  // Write the same data to the "previous" buffer so the next partial
  // update computes a zero diff (all white→no change on first partial).
  _disp.epd2.writeImageToPrevious(native, 0, 0, GxEPD2_750_T7::WIDTH,
                                  GxEPD2_750_T7::HEIGHT, false, false, false);
}

// ---------------------------------------------------------------------------
// showPagePartial — fast partial refresh for all pages after the first.
// ~1.6 s vs ~3.7 s for full refresh.
// The controller computes the diff between "previous" and "new" buffers and
// drives only changed pixels — clean, no ghosting.
// ---------------------------------------------------------------------------
void EpubRenderer::showPagePartial(const uint8_t* buf) {
  const uint8_t* native = portraitToNative(buf);
  if (!native) return;
  // Write new content to the "new" controller buffer
  _disp.writeImage(native, 0, 0, GxEPD2_750_T7::WIDTH, GxEPD2_750_T7::HEIGHT,
                   false, false, false);
  _disp.refresh(true);    // true = partial refresh waveform (~1.6 s)
  // Sync the "previous" buffer with the new content for the next diff
  _disp.epd2.writeImageToPrevious(native, 0, 0, GxEPD2_750_T7::WIDTH,
                                  GxEPD2_750_T7::HEIGHT, false, false, false);
}

void EpubRenderer::showSlotFull(int slot) {
  const uint8_t* buf = g_pool.slotBuf(slot);
  showPageFull(buf);
}

void EpubRenderer::showSlotPartial(int slot) {
  const uint8_t* buf = g_pool.slotBuf(slot);
  showPagePartial(buf);
}
