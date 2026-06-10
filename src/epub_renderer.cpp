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
static void pngClose(void* /*handle*/) {
  if (s_pngFile) s_pngFile.close();
}
static int32_t pngRead(PNGFILE* /*handle*/, uint8_t* buf, int32_t len) {
  return s_pngFile.read(buf, len);
}
static int32_t pngSeek(PNGFILE* /*handle*/, int32_t pos) {
  return s_pngFile.seek(pos) ? pos : -1;
}

// -----------------------------------------------------------------------
// PNG row callback — one row at a time
// -----------------------------------------------------------------------
int EpubRenderer::s_pngDraw(PNGDRAW* pDraw) {
  if (s_instance) s_instance->pngRowDraw(pDraw);
  return 1;
}

void EpubRenderer::pngRowDraw(PNGDRAW* pDraw) {
  // PNG_RGB565_BIG_ENDIAN gives us bytes in network order; swap to little-endian
  // so R/G/B extraction works correctly on the ESP32 (little-endian CPU)
  _png.getLineAsRGB565(pDraw, _lineBuffer, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

  int16_t py = _imgDestY + pDraw->y;
  int16_t px = _imgDestX;

  // Skip rows that fall outside display bounds
  if (py < 0 || py >= DISPLAY_H) return;

  for (int x = 0; x < pDraw->iWidth; x++) {
    // Skip pixels outside display bounds
    if (px + x < 0 || px + x >= DISPLAY_W) continue;

    uint16_t p = _lineBuffer[x];
    // RGB565 little-endian: RRRRRGGGGGGBBBBB
    uint8_t R = (p >> 11) & 0x1F;   // 5 bits → 0-31
    uint8_t G = (p >>  5) & 0x3F;   // 6 bits → 0-63
    uint8_t B =  p        & 0x1F;   // 5 bits → 0-31

    // Expand to 8-bit range for proper luma calculation
    uint8_t r8 = (R << 3) | (R >> 2);  // 0-255
    uint8_t g8 = (G << 2) | (G >> 4);  // 0-255
    uint8_t b8 = (B << 3) | (B >> 2);  // 0-255

    // ITU-R BT.601 luma
    uint16_t luma = (r8 * 299 + g8 * 587 + b8 * 114) / 1000;

    // High threshold: formula images have white background with dark ink.
    // Anything lighter than 180 is treated as white (kills anti-alias noise).
    uint16_t color = (luma < 180) ? GxEPD_BLACK : GxEPD_WHITE;
    _disp.drawPixel(px + x, py, color);
  }
}

// -----------------------------------------------------------------------
// PNG size probe (open, read header, close — no pixel decode)
// -----------------------------------------------------------------------
bool EpubRenderer::pngGetSize(const char* path, uint16_t& w, uint16_t& h) {
  s_instance = this;
  // Use a no-op draw callback just to open and read dimensions
  int rc = _png.open(path, pngOpen, pngClose, pngRead, pngSeek, s_pngDraw);
  if (rc != PNG_SUCCESS) { w = h = 0; return false; }
  w = _png.getWidth();
  h = _png.getHeight();
  _png.close();
  return true;
}

// -----------------------------------------------------------------------
// Full PNG decode onto display at destX, destY
// -----------------------------------------------------------------------
bool EpubRenderer::decodePng(const char* path, int16_t destX, int16_t destY) {
  s_instance = this;
  _imgDestX  = destX;
  _imgDestY  = destY;
  int rc = _png.open(path, pngOpen, pngClose, pngRead, pngSeek, s_pngDraw);
  if (rc != PNG_SUCCESS) {
    Serial.printf("PNG open failed: %s (%d)\n", path, rc);
    return false;
  }
  _png.decode(nullptr, 0);
  _png.close();
  return true;
}

// -----------------------------------------------------------------------
// Font selection
// -----------------------------------------------------------------------
void EpubRenderer::selectFont(FontLevel level) {
  switch (level) {
    case FONT_H2: _disp.setFont(&FreeSansBold18pt7b); break;
    case FONT_H3: _disp.setFont(&FreeSansBold12pt7b); break;
    case FONT_H4: _disp.setFont(&FreeSansBold9pt7b);  break;
    default:      _disp.setFont(&FreeSans9pt7b);       break;
  }
}

// -----------------------------------------------------------------------
// Flush accumulated draw commands through GxEPD2's firstPage/nextPage loop.
// This ensures every half-page buffer sees the full page content.
// -----------------------------------------------------------------------
void EpubRenderer::flushPage() {
  if (_cmdCount == 0) return;

  _disp.setFullWindow();
  _disp.firstPage();
  do {
    _disp.fillScreen(GxEPD_WHITE);
    _disp.setTextColor(GxEPD_BLACK);

    for (int i = 0; i < _cmdCount; i++) {
      const DrawCmd& cmd = _cmds[i];
      if (cmd.type == DC_TEXT) {
        selectFont(cmd.fontLevel);
        _disp.setTextColor(GxEPD_BLACK);
        _disp.setCursor(cmd.x, cmd.y);
        _disp.print(cmd.text);
      } else {
        // DC_IMAGE — decode from SD on every pass
        decodePng(cmd.path, cmd.imgX, cmd.imgY);
      }
    }
  } while (_disp.nextPage());

  // Reset for next page
  _cmdCount = 0;
  _cx = MARGIN_LEFT;
  _cy = MARGIN_TOP;
  _lineH = 0;
  _lineHasContent = false;
}

// -----------------------------------------------------------------------
// Line management
// -----------------------------------------------------------------------
void EpubRenderer::newLine(int16_t extraSpacing) {
  int16_t advance = (_lineH > 0 ? _lineH : 14) + LINE_SPACING + extraSpacing;
  _cy += advance;
  _cx  = MARGIN_LEFT;
  _lineH = 0;
  _lineHasContent = false;
}

void EpubRenderer::checkPageOverflow(int16_t neededH) {
  if (_cy + neededH > DISPLAY_H - MARGIN_BOTTOM) {
    flushPage();
  }
}

// -----------------------------------------------------------------------
// Layout: word
// -----------------------------------------------------------------------
void EpubRenderer::layoutWord(const char* word, FontLevel level) {
  if (!word || word[0] == '\0') return;
  selectFont(level);

  int16_t x1, y1; uint16_t tw, th;
  _disp.getTextBounds(word, _cx, 0, &x1, &y1, &tw, &th);

  int16_t availW = DISPLAY_W - MARGIN_RIGHT - _cx;
  if ((int16_t)tw > availW && _cx > MARGIN_LEFT) {
    newLine();
    _disp.getTextBounds(word, _cx, 0, &x1, &y1, &tw, &th);
  }

  int16_t ascent = -y1;
  if ((int16_t)th > _lineH) _lineH = (int16_t)th;
  checkPageOverflow(th);

  // Record draw command
  if (_cmdCount < MAX_DRAW_CMDS) {
    DrawCmd& cmd = _cmds[_cmdCount++];
    cmd.type      = DC_TEXT;
    cmd.fontLevel = level;
    cmd.x         = _cx;
    cmd.y         = _cy + ascent;  // GFX baseline y
    strncpy(cmd.text, word, MAX_TEXT_LEN - 1);
    cmd.text[MAX_TEXT_LEN - 1] = '\0';
  }

  _cx += (int16_t)tw;
  _lineHasContent = true;
}

// -----------------------------------------------------------------------
// Layout: inline image
// -----------------------------------------------------------------------
void EpubRenderer::layoutInlineImage(const char* path) {
  uint16_t imgW, imgH;
  if (!pngGetSize(path, imgW, imgH)) return;

  int16_t availW = DISPLAY_W - MARGIN_RIGHT - _cx;
  if ((int16_t)imgW > availW && _cx > MARGIN_LEFT) newLine();

  checkPageOverflow(imgH);

  int16_t destX = _cx;
  int16_t destY = _cy - imgH / 2;
  if (destY < 0) destY = 0;

  if (_cmdCount < MAX_DRAW_CMDS) {
    DrawCmd& cmd = _cmds[_cmdCount++];
    cmd.type = DC_IMAGE;
    cmd.imgX = destX;
    cmd.imgY = destY;
    strncpy(cmd.path, path, MAX_PATH_LEN - 1);
    cmd.path[MAX_PATH_LEN - 1] = '\0';
  }

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

  int16_t destX = (DISPLAY_W - (int16_t)imgW) / 2;
  int16_t destY = _cy;

  if (_cmdCount < MAX_DRAW_CMDS) {
    DrawCmd& cmd = _cmds[_cmdCount++];
    cmd.type = DC_IMAGE;
    cmd.imgX = destX;
    cmd.imgY = destY;
    strncpy(cmd.path, path, MAX_PATH_LEN - 1);
    cmd.path[MAX_PATH_LEN - 1] = '\0';
  }

  _cy += (int16_t)imgH + LINE_SPACING * 2;
  _cx  = MARGIN_LEFT;
  _lineH = 0;
  _lineHasContent = false;
}

// -----------------------------------------------------------------------
// Public interface
// -----------------------------------------------------------------------
void EpubRenderer::beginDoc() {
  _cmdCount = 0;
  _cx = MARGIN_LEFT;
  _cy = MARGIN_TOP;
  _lineH = 0;
  _lineHasContent = false;
}

void EpubRenderer::endDoc() {
  if (_lineHasContent) newLine();
  flushPage();
}

void EpubRenderer::feed(const RenderElem& elem) {
  switch (elem.type) {
    case ELEM_TEXT:
    case ELEM_HEADING:
      layoutWord(elem.text, elem.fontLevel);
      break;
    case ELEM_IMAGE_INLINE:
      layoutInlineImage(elem.path);
      break;
    case ELEM_IMAGE_BLOCK:
      layoutBlockImage(elem.path);
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
}
