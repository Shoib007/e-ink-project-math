#include "epub_renderer.h"
#include <string.h>

EpubRenderer* EpubRenderer::s_instance = nullptr;

// -----------------------------------------------------------------------
// PNG file I/O callbacks
// -----------------------------------------------------------------------
static File s_pngFile;

static void* pngOpen(const char* filename, int32_t* size) {
  s_pngFile = SD.open(filename);
  if (!s_pngFile) return nullptr;
  *size = s_pngFile.size();
  return (void*)&s_pngFile;
}
static void pngClose(void* /*h*/) { if (s_pngFile) s_pngFile.close(); }
static int32_t pngRead(PNGFILE* /*h*/, uint8_t* buf, int32_t len) {
  return s_pngFile.read(buf, len);
}
static int32_t pngSeek(PNGFILE* /*h*/, int32_t pos) {
  return s_pngFile.seek(pos) ? pos : -1;
}

// -----------------------------------------------------------------------
// PNG row callback — writes decoded pixels into active page buffer
// -----------------------------------------------------------------------
int EpubRenderer::s_pngDraw(PNGDRAW* pDraw) {
  if (s_instance) s_instance->pngRowDraw(pDraw);
  return 1;
}

void EpubRenderer::pngRowDraw(PNGDRAW* pDraw) {
  _png.getLineAsRGB565(pDraw, _lineBuffer, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  int16_t py = _imgDestY + pDraw->y;
  if (py < 0 || py >= DISPLAY_H) return;
  for (int x = 0; x < pDraw->iWidth; x++) {
    int16_t px = _imgDestX + x;
    if (px < 0 || px >= DISPLAY_W) continue;
    uint16_t p = _lineBuffer[x];
    uint8_t r8 = ((p >> 11) & 0x1F) << 3;
    uint8_t g8 = ((p >>  5) & 0x3F) << 2;
    uint8_t b8 = ( p        & 0x1F) << 3;
    uint16_t luma = (r8 * 299 + g8 * 587 + b8 * 114) / 1000;
    g_pool.drawPixel(px, py, luma < 180 ? 0 : 1);
  }
}

// -----------------------------------------------------------------------
// PNG size probe — open header only, no pixel decode
// -----------------------------------------------------------------------
bool EpubRenderer::pngGetSize(const char* path, uint16_t& w, uint16_t& h) {
  s_instance = this;
  int rc = _png.open(path, pngOpen, pngClose, pngRead, pngSeek, s_pngDraw);
  if (rc != PNG_SUCCESS) { w = h = 0; return false; }
  w = _png.getWidth(); h = _png.getHeight();
  _png.close();
  return true;
}

// -----------------------------------------------------------------------
// Decode PNG directly into active page buffer in pool
// -----------------------------------------------------------------------
bool EpubRenderer::decodePngToPool(const char* path, int16_t destX, int16_t destY) {
  s_instance = this;
  _imgDestX = destX; _imgDestY = destY;
  int rc = _png.open(path, pngOpen, pngClose, pngRead, pngSeek, s_pngDraw);
  if (rc != PNG_SUCCESS) return false;
  _png.decode(nullptr, 0);
  _png.close();
  return true;
}

// -----------------------------------------------------------------------
// Font selection — on FbCanvas (routes to page pool via drawPixel)
// -----------------------------------------------------------------------
void EpubRenderer::selectFont(FontLevel level) {
  switch (level) {
    case FONT_H2: _canvas.setFont(&FreeSansBold18pt7b); break;
    case FONT_H3: _canvas.setFont(&FreeSansBold12pt7b); break;
    case FONT_H4: _canvas.setFont(&FreeSansBold9pt7b);  break;
    default:      _canvas.setFont(&FreeSans9pt7b);       break;
  }
}

// -----------------------------------------------------------------------
// Page management
// -----------------------------------------------------------------------
bool EpubRenderer::beginDoc() {
  // Allocate and activate page 0
  int idx = g_pool.allocPage();
  if (idx < 0) { Serial.println("ERROR: pool alloc failed for page 0"); return false; }
  g_pool.setCurrent(idx);
  g_pool.clearCurrent();

  _cx = MARGIN_LEFT; _cy = MARGIN_TOP;
  _lineH = 0; _lineHasContent = false;
  return true;
}

void EpubRenderer::endDoc() {
  if (_lineHasContent) newLine();
  // Current page buffer is already fully rendered — nothing to flush
  Serial.printf("Pre-render complete. %d pages in PSRAM.\n", g_pool.pageCount());
}

void EpubRenderer::breakPage() {
  // Allocate a new page buffer and make it the render target
  int idx = g_pool.allocPage();
  if (idx < 0) { Serial.println("WARN: max pages reached"); return; }
  g_pool.setCurrent(idx);
  g_pool.clearCurrent();

  _cx = MARGIN_LEFT; _cy = MARGIN_TOP;
  _lineH = 0; _lineHasContent = false;
}

// -----------------------------------------------------------------------
// Line management
// -----------------------------------------------------------------------
void EpubRenderer::newLine(int16_t extraSpacing) {
  _cy += (_lineH > 0 ? _lineH : 14) + LINE_SPACING + extraSpacing;
  _cx = MARGIN_LEFT; _lineH = 0; _lineHasContent = false;
}

void EpubRenderer::checkPageOverflow(int16_t neededH) {
  if (_cy + neededH > DISPLAY_H - MARGIN_BOTTOM) breakPage();
}

// -----------------------------------------------------------------------
// Render: word (text into current page buffer via FbCanvas)
// -----------------------------------------------------------------------
void EpubRenderer::renderWord(const char* word, FontLevel level) {
  if (!word || !word[0]) return;
  selectFont(level);
  _canvas.setTextWrap(false);

  int16_t x1, y1; uint16_t tw, th;
  _canvas.getTextBounds(word, _cx, 0, &x1, &y1, &tw, &th);

  if ((int16_t)tw > DISPLAY_W - MARGIN_RIGHT - _cx && _cx > MARGIN_LEFT) {
    newLine();
    _canvas.getTextBounds(word, _cx, 0, &x1, &y1, &tw, &th);
  }
  if ((int16_t)th > _lineH) _lineH = (int16_t)th;
  checkPageOverflow(th);

  _canvas.setTextColor(0);  // 0 = black
  _canvas.setCursor(_cx, _cy + (-y1));
  _canvas.print(word);

  _cx += (int16_t)tw;
  _lineHasContent = true;
}

// -----------------------------------------------------------------------
// Render: inline image (decode PNG into current page buffer)
// -----------------------------------------------------------------------
void EpubRenderer::renderInlineImage(const char* path) {
  uint16_t imgW, imgH;
  if (!pngGetSize(path, imgW, imgH)) return;

  if ((int16_t)imgW > DISPLAY_W - MARGIN_RIGHT - _cx && _cx > MARGIN_LEFT)
    newLine();
  checkPageOverflow(imgH);

  int16_t destX = _cx;
  int16_t destY = _cy - (int16_t)(imgH / 2);
  if (destY < 0) destY = 0;

  decodePngToPool(path, destX, destY);

  if ((int16_t)imgH > _lineH) _lineH = (int16_t)imgH;
  _cx += (int16_t)imgW + 2;
  _lineHasContent = true;
}

// -----------------------------------------------------------------------
// Render: block image (centered, own line)
// -----------------------------------------------------------------------
void EpubRenderer::renderBlockImage(const char* path) {
  if (_lineHasContent) newLine();
  uint16_t imgW, imgH;
  if (!pngGetSize(path, imgW, imgH)) return;
  checkPageOverflow(imgH + LINE_SPACING * 2);

  int16_t destX = (DISPLAY_W - (int16_t)imgW) / 2;
  decodePngToPool(path, destX, _cy);

  _cy += (int16_t)imgH + LINE_SPACING * 2;
  _cx = MARGIN_LEFT; _lineH = 0; _lineHasContent = false;
}

// -----------------------------------------------------------------------
// Feed — called by parser during setup, renders directly into page buffers
// -----------------------------------------------------------------------
void EpubRenderer::feed(const RenderElem& elem) {
  switch (elem.type) {
    case ELEM_TEXT:
    case ELEM_HEADING:      renderWord(elem.text, elem.fontLevel); break;
    case ELEM_IMAGE_INLINE: renderInlineImage(elem.path);          break;
    case ELEM_IMAGE_BLOCK:  renderBlockImage(elem.path);           break;
    case ELEM_PARA_BREAK:
      if (_lineHasContent) newLine();
      newLine(PARA_SPACING);
      break;
    case ELEM_HEADING_BREAK:
      if (_lineHasContent) newLine();
      newLine(HEADING_SPACING);
      break;
  }
}

// -----------------------------------------------------------------------
// showPage — copy pre-rendered PSRAM buffer into GxEPD2, one refresh.
// No SD access. No PNG decode. No font rendering. Just a memory copy.
// Total time = copy loop (~100ms) + e-ink waveform (~1.5-2s).
// -----------------------------------------------------------------------
void EpubRenderer::showPage(int index) {
  const uint8_t* fb = g_pool.pageBuf(index);
  if (!fb) return;

  _disp.setFullWindow();
  _disp.firstPage();
  do {
    const uint8_t* src = fb;
    for (int y = 0; y < DISPLAY_H; y++) {
      const uint8_t* row = src + y * FB_STRIDE;
      for (int x = 0; x < DISPLAY_W; x++) {
        uint8_t bit = (row[x >> 3] >> (7 - (x & 7))) & 1;
        _disp.drawPixel(x, y, bit ? GxEPD_WHITE : GxEPD_BLACK);
      }
    }
  } while (_disp.nextPage());
}
