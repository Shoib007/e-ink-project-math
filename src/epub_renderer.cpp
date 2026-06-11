#include "epub_renderer.h"
#include <string.h>
#include <esp_heap_caps.h>

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
// PNG decode — writes directly into framebuffer (NOT display)
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
    // Write into framebuffer: 0=black, 1=white
    g_fb.drawPixel(px, py, luma < 180 ? 0 : 1);
  }
}

bool EpubRenderer::pngGetSize(const char* path, uint16_t& w, uint16_t& h) {
  s_instance = this;
  int rc = _png.open(path, pngOpen, pngClose, pngRead, pngSeek, s_pngDraw);
  if (rc != PNG_SUCCESS) { w = h = 0; return false; }
  w = _png.getWidth(); h = _png.getHeight();
  _png.close();
  return true;
}

bool EpubRenderer::decodePngToFb(const char* path, int16_t destX, int16_t destY) {
  s_instance = this;
  _imgDestX = destX; _imgDestY = destY;
  int rc = _png.open(path, pngOpen, pngClose, pngRead, pngSeek, s_pngDraw);
  if (rc != PNG_SUCCESS) return false;
  _png.decode(nullptr, 0);
  _png.close();
  return true;
}

// -----------------------------------------------------------------------
// Font selection — on the FbCanvas, not the display
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
// PSRAM allocation
// -----------------------------------------------------------------------
bool EpubRenderer::beginDoc() {
  if (!_pool) {
    _pool = (DrawCmd*)heap_caps_malloc(
        TOTAL_CMDS * sizeof(DrawCmd), MALLOC_CAP_SPIRAM);
    if (!_pool) {
      Serial.printf("ERROR: PSRAM pool alloc failed\n");
      return false;
    }
  }
  _poolUsed   = 0;
  _pageCount  = 1;
  _pageInfo[0] = {0, 0};
  _cx = MARGIN_LEFT; _cy = MARGIN_TOP;
  _lineH = 0; _lineHasContent = false;
  return true;
}

void EpubRenderer::endDoc() {
  if (_lineHasContent) newLine();
}

// -----------------------------------------------------------------------
// Command pool
// -----------------------------------------------------------------------
void EpubRenderer::addCmd(const DrawCmd& cmd) {
  if (_poolUsed >= TOTAL_CMDS) { Serial.println("WARN: cmd pool full"); return; }
  _pool[_poolUsed++] = cmd;
  _pageInfo[_pageCount - 1].count++;
}

void EpubRenderer::breakPage() {
  if (_pageCount >= MAX_PAGES) { Serial.println("WARN: max pages"); return; }
  _pageInfo[_pageCount] = { _poolUsed, 0 };
  _pageCount++;
  _cx = MARGIN_LEFT; _cy = MARGIN_TOP;
  _lineH = 0; _lineHasContent = false;
}

// -----------------------------------------------------------------------
// Line layout
// -----------------------------------------------------------------------
void EpubRenderer::newLine(int16_t extraSpacing) {
  _cy += (_lineH > 0 ? _lineH : 14) + LINE_SPACING + extraSpacing;
  _cx = MARGIN_LEFT; _lineH = 0; _lineHasContent = false;
}

void EpubRenderer::checkPageOverflow(int16_t neededH) {
  if (_cy + neededH > DISPLAY_H - MARGIN_BOTTOM) breakPage();
}

// -----------------------------------------------------------------------
// Layout: word
// -----------------------------------------------------------------------
void EpubRenderer::layoutWord(const char* word, FontLevel level) {
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

  DrawCmd cmd; memset(&cmd, 0, sizeof(cmd));
  cmd.type = DC_TEXT; cmd.fontLevel = level;
  cmd.x = _cx; cmd.y = _cy + (-y1);
  strncpy(cmd.text, word, MAX_TEXT_LEN - 1);
  addCmd(cmd);

  _cx += (int16_t)tw;
  _lineHasContent = true;
}

// -----------------------------------------------------------------------
// Layout: inline image
// -----------------------------------------------------------------------
void EpubRenderer::layoutInlineImage(const char* path) {
  uint16_t imgW, imgH;
  if (!pngGetSize(path, imgW, imgH)) return;

  if ((int16_t)imgW > DISPLAY_W - MARGIN_RIGHT - _cx && _cx > MARGIN_LEFT)
    newLine();
  checkPageOverflow(imgH);

  DrawCmd cmd; memset(&cmd, 0, sizeof(cmd));
  cmd.type = DC_IMAGE;
  cmd.x = _cx;
  cmd.y = _cy - (int16_t)(imgH / 2);
  if (cmd.y < 0) cmd.y = 0;
  strncpy(cmd.path, path, MAX_PATH_LEN - 1);
  addCmd(cmd);

  if ((int16_t)imgH > _lineH) _lineH = (int16_t)imgH;
  _cx += (int16_t)imgW + 2;
  _lineHasContent = true;
}

// -----------------------------------------------------------------------
// Layout: block image
// -----------------------------------------------------------------------
void EpubRenderer::layoutBlockImage(const char* path) {
  if (_lineHasContent) newLine();
  uint16_t imgW, imgH;
  if (!pngGetSize(path, imgW, imgH)) return;
  checkPageOverflow(imgH + LINE_SPACING * 2);

  DrawCmd cmd; memset(&cmd, 0, sizeof(cmd));
  cmd.type = DC_IMAGE;
  cmd.x = (DISPLAY_W - (int16_t)imgW) / 2;
  cmd.y = _cy;
  strncpy(cmd.path, path, MAX_PATH_LEN - 1);
  addCmd(cmd);

  _cy += (int16_t)imgH + LINE_SPACING * 2;
  _cx = MARGIN_LEFT; _lineH = 0; _lineHasContent = false;
}

// -----------------------------------------------------------------------
// Feed
// -----------------------------------------------------------------------
void EpubRenderer::feed(const RenderElem& elem) {
  switch (elem.type) {
    case ELEM_TEXT:
    case ELEM_HEADING:      layoutWord(elem.text, elem.fontLevel); break;
    case ELEM_IMAGE_INLINE: layoutInlineImage(elem.path);          break;
    case ELEM_IMAGE_BLOCK:  layoutBlockImage(elem.path);           break;
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
// showPage — render everything into PSRAM framebuffer, then copy into
// GxEPD2's internal buffer via its firstPage/nextPage loop.
// Result: ONE full e-ink refresh cycle regardless of content complexity.
// -----------------------------------------------------------------------
void EpubRenderer::showPage(int index) {
  if (!_pool || index < 0 || index >= _pageCount) return;
  const PageInfo& pi = _pageInfo[index];

  // ---- Step 1: render all commands into PSRAM framebuffer ----
  // No display I/O at all during this step.
  g_fb.clear();
  _canvas.setTextColor(0);  // 0 = black
  for (int i = 0; i < pi.count; i++) {
    const DrawCmd& cmd = _pool[pi.start + i];
    if (cmd.type == DC_TEXT) {
      selectFont(cmd.fontLevel);
      _canvas.setTextColor(0);
      _canvas.setCursor(cmd.x, cmd.y);
      _canvas.print(cmd.text);
    } else {
      decodePngToFb(cmd.path, cmd.x, cmd.y);
    }
  }

  // ---- Step 2: copy PSRAM framebuffer into GxEPD2's paged buffer ----
  // The firstPage/nextPage loop runs twice (400px per pass).
  // Each pass: copy only the rows that belong to the current half-page
  // from our framebuffer into GxEPD2 via drawPixel().
  // This is a simple memory copy dressed as drawPixel calls — fast.
  _disp.setFullWindow();
  _disp.firstPage();
  do {
    // GxEPD2 tracks which page (half) is current internally.
    // We fill its buffer by calling drawPixel for every pixel.
    // Since drawPixel clips to the current page window automatically,
    // we can safely iterate the full display height — out-of-range
    // rows are discarded by GxEPD2 with no overhead.
    const uint8_t* fb = g_fb.buf();
    for (int y = 0; y < DISPLAY_H; y++) {
      const uint8_t* row = fb + y * FB_STRIDE;
      for (int x = 0; x < DISPLAY_W; x++) {
        uint8_t bit = (row[x >> 3] >> (7 - (x & 7))) & 1;
        _disp.drawPixel(x, y, bit ? GxEPD_WHITE : GxEPD_BLACK);
      }
    }
  } while (_disp.nextPage());
}
