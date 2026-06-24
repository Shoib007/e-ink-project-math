#include "epub_renderer.h"
#include <string.h>

EpubRenderer* EpubRenderer::s_instance = nullptr;

// ===========================================================================
// PNG I/O callbacks (file-level statics; decodes are serial on Core 1)
// ===========================================================================
static File s_pngFile;

static void* pngOpen(const char* filename, int32_t* size) {
  s_pngFile = SD.open(filename);
  if (!s_pngFile) return nullptr;
  *size = s_pngFile.size();
  return static_cast<void*>(&s_pngFile);
}
static void    pngClose(void* /*h*/)                              { if (s_pngFile) s_pngFile.close(); }
static int32_t pngRead (PNGFILE* /*h*/, uint8_t* buf, int32_t l) { return s_pngFile.read(buf, l); }
static int32_t pngSeek (PNGFILE* /*h*/, int32_t pos)             { return s_pngFile.seek(pos) ? pos : -1; }

// ===========================================================================
// PNG row callback
// ===========================================================================
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

// ===========================================================================
// JPEG callback (TJpg_Decoder)
//
// Called for each 8×8 MCU block.  Tracks decoded image extent for centering,
// then converts RGB565 → luma → 1-bpp and draws into the current slot.
// ===========================================================================
bool EpubRenderer::s_jpgDraw(int16_t x, int16_t y,
                               uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (!s_instance || !bitmap) return false;
  EpubRenderer& inst = *s_instance;

  // Track extent relative to the dest origin
  uint16_t relW = static_cast<uint16_t>(x - inst._imgDestX) + w;
  uint16_t relH = static_cast<uint16_t>(y - inst._imgDestY) + h;
  if (relW > inst._jpgImgW) inst._jpgImgW = relW;
  if (relH > inst._jpgImgH) inst._jpgImgH = relH;

  // Render pixels
  for (uint16_t row = 0; row < h; ++row) {
    for (uint16_t col = 0; col < w; ++col) {
      int16_t px = x + static_cast<int16_t>(col);
      int16_t py = y + static_cast<int16_t>(row);
      if (px < 0 || px >= DISPLAY_W || py < 0 || py >= DISPLAY_H) continue;
      uint16_t p  = bitmap[row * w + col];
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
  return true;  // continue decoding
}

// ===========================================================================
// decodePng — decode a PNG from SD into the current slot at (destX, destY).
// Takes/releases _sdMutex internally.
// ===========================================================================
bool EpubRenderer::decodePng(const char* path,
                               int16_t destX, int16_t destY,
                               uint16_t& outW, uint16_t& outH, bool measureOnly) {
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

  if (!measureOnly) {
    _png.decode(nullptr, 0);   // fires pngRowDraw() for every row
  }
  _png.close();

  if (_sdMutex) xSemaphoreGive(_sdMutex);
  return true;
}

// ===========================================================================
// decodeJpg — decode a JPEG from SD at (destX, destY) via TJpg_Decoder.
// Two-pass centering: first pass tracks _jpgImgW/_jpgImgH, second re-decodes.
// ===========================================================================
bool EpubRenderer::decodeJpg(const char* path,
                               int16_t destX, int16_t destY,
                               uint16_t& outW, uint16_t& outH,
                               uint8_t scale) {
  s_instance = this;
  _imgDestX  = destX;
  _imgDestY  = destY;
  _jpgImgW   = 0;
  _jpgImgH   = 0;

  TJpgDec.setCallback(s_jpgDraw);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setJpgScale(scale);

  if (_sdMutex) xSemaphoreTake(_sdMutex, portMAX_DELAY);

  JRESULT rc = TJpgDec.drawSdJpg(static_cast<int32_t>(destX),
                                   static_cast<int32_t>(destY), path);

  if (_sdMutex) xSemaphoreGive(_sdMutex);

  if (rc != JDR_OK) {
    Serial.printf("[Renderer] JPEG rc=%d for %s\n", rc, path);
  }
  outW = _jpgImgW;
  outH = _jpgImgH;
  return (outW > 0 && outH > 0);
}

// ===========================================================================
// Font selection — maps FontLevel to the correct GFX font object
// ===========================================================================
void EpubRenderer::selectFont(FontLevel level) {
  switch (level) {
    case FONT_H1:        _canvas.setFont(&FreeSansBold18pt7b);       break;
    case FONT_H2:        _canvas.setFont(&FreeSansBold12pt7b);       break;
    case FONT_H3:        _canvas.setFont(&FreeSansBold9pt7b);        break;
    case FONT_H4:        _canvas.setFont(&FreeSansBoldOblique9pt7b); break;
    case FONT_BODY_BOLD: _canvas.setFont(&FreeMonoBold9pt7b);        break;
    default:             _canvas.setFont(&FreeSans9pt7b);            break;
  }
}

// ===========================================================================
// Page management
// ===========================================================================

// Helper: measure the ascent of the body font so we can prime _cy correctly.
// Ascent = distance from baseline to top of tallest glyph = -y1 from getTextBounds.
int16_t EpubRenderer::fontAscent(FontLevel level) {
  selectFont(level);
  int16_t x1, y1; uint16_t bw, bh;
  _canvas.getTextBounds("Ag", 0, 0, &x1, &y1, &bw, &bh);
  return static_cast<int16_t>(-y1);   // y1 is negative in GFX convention
}

void EpubRenderer::beginPage(int slot) {
  g_pool.setCurrent(slot);
  g_pool.clearCurrent();

  // Compute and CACHE the body-font ascent once per page.
  // This is used both to prime _cy and in checkPageOverflow().
  // We must NOT call fontAscent() again during rendering because it calls
  // selectFont(), which would silently reset the active canvas font and cause
  // every heading to render in the body font size.
  _bodyFontAscent = fontAscent(FONT_BODY);

  // _cy is the BASELINE of the first text line.
  // Prime it so the top of the tallest body glyph sits at MARGIN_TOP.
  _cy             = static_cast<int16_t>(MARGIN_TOP + _bodyFontAscent);
  _cx             = MARGIN_LEFT;
  _lineH          = 0;
  _lineHasContent = false;
  _pageFull       = false;
  _wrapLeftMargin = MARGIN_LEFT;
  _rightBound     = DISPLAY_W - MARGIN_RIGHT;
  _inListItem     = false;
  _inTable        = false;
  _inCellRender   = false;
  _tableColCount  = 0;
  _tableCurCol    = 0;
}

void EpubRenderer::endPage() {
  if (_lineHasContent) newLine();
}

void EpubRenderer::signalPageFull() {
  _pageFull = true;
}

// ===========================================================================
// Line management
//
// _cy is always the BASELINE of the current line.
// _lineH is the full line advance = max(ascent + descent) across all elements
// on the line.  On newLine() we advance _cy by _lineH.
// ===========================================================================
void EpubRenderer::newLine(int16_t extraSpacing) {
  _cy += (_lineH > 0 ? _lineH : 14) + LINE_SPACING + extraSpacing;
  _cx             = _wrapLeftMargin;
  _lineH          = 0;
  _lineHasContent = false;
}

void EpubRenderer::checkPageOverflow(int16_t neededH) {
  if (_inCellRender) return;
  // Use the cached _bodyFontAscent — never call selectFont() here!
  // Calling selectFont() mid-draw would reset the canvas font from e.g. H1
  // back to FONT_BODY, causing all headings to render in body-font size.
  int16_t firstBaseline = static_cast<int16_t>(MARGIN_TOP + _bodyFontAscent);
  if (_cy > firstBaseline && _cy + neededH > DISPLAY_H - MARGIN_BOTTOM) {
    signalPageFull();
  }
}

// ===========================================================================
// Drawing helpers — 1-px horizontal and vertical lines
// ===========================================================================
void EpubRenderer::drawHLine(int16_t x, int16_t y, int16_t w) {
  if (y < 0 || y >= DISPLAY_H) return;
  for (int16_t px = x; px < x + w; ++px) {
    if (px < 0 || px >= DISPLAY_W) continue;
    g_pool.drawPixel(px, y, 0);  // 0 = black
  }
}

void EpubRenderer::drawVLine(int16_t x, int16_t y, int16_t h) {
  if (x < 0 || x >= DISPLAY_W) return;
  for (int16_t py = y; py < y + h; ++py) {
    if (py < 0 || py >= DISPLAY_H) continue;
    g_pool.drawPixel(x, py, 0);  // 0 = black
  }
}

// ===========================================================================
// renderWord — draw one word (possibly with a leading space) at the cursor.
//
// BASELINE MODEL: _cy is the text baseline.
//   getTextBounds with y=0 returns font-relative bounds:
//     y1 < 0  (top of bbox above baseline  = ascent)
//     h + y1  (below baseline              = descent)
//   We track _lineH = max over all words of (ascent + descent) = full line advance.
// ===========================================================================
void EpubRenderer::renderWord(const char* word, FontLevel level) {
  if (!word || !word[0]) return;
  selectFont(level);
  _canvas.setTextWrap(false);

  // Measure with y=0 to get purely font-relative offsets
  int16_t x1, y1; uint16_t tw, th;
  _canvas.getTextBounds(word, 0, 0, &x1, &y1, &tw, &th);

  // ascent = pixels above baseline; descent = pixels below baseline
  int16_t ascent  = static_cast<int16_t>(-y1);          // y1 is negative
  int16_t descent = static_cast<int16_t>(th) + y1;      // th - ascent
  int16_t advance = static_cast<int16_t>(tw);

  // Wrap if word won't fit on this line
  if (_cx > _wrapLeftMargin && _cx + advance > _rightBound) {
    newLine();
    if (_pageFull) return;
    // Re-measure after wrap (cursor moved)
    _canvas.getTextBounds(word, 0, 0, &x1, &y1, &tw, &th);
    ascent  = static_cast<int16_t>(-y1);
    descent = static_cast<int16_t>(th) + y1;
    advance = static_cast<int16_t>(tw);
  }

  // Update line height (full advance = ascent + descent)
  int16_t lineAdv = ascent + descent;
  if (lineAdv > _lineH) _lineH = lineAdv;

  // Check page overflow based on descent below current baseline
  checkPageOverflow(descent);
  if (_pageFull) return;

  // Draw at the baseline — GFX cursor IS the baseline for text
  _canvas.setTextColor(0);
  _canvas.setCursor(_cx, _cy);
  _canvas.print(word);

  _cx += advance;
  _lineHasContent = true;
}

// ===========================================================================
// renderInlineImage — inline PNG (math formula fragment)
//
// BASELINE MODEL:
//   MathJax formula PNGs are designed with their optical baseline sitting at
//   roughly 80% of the image height from the top.  To align them with the
//   surrounding text baseline we lower them by INLINE_IMG_DESCENT pixels so
//   the visible math baseline lines up with the text baseline.
//
//   A gap of INLINE_IMG_SPACE pixels is added before the image so there is
//   visual separation from the preceding word.
//   (Both constants are defined in epub_types.h for easy tuning.)
// ===========================================================================
void EpubRenderer::renderInlineImage(const char* path) {
  uint16_t imgW = 0, imgH = 0;

  // Measure only (no pixel decode)
  if (!decodePng(path, 0, 0, imgW, imgH, true)) return;
  if (imgW == 0 || imgH == 0) return;

  // Add pre-image space if something is already on this line
  if (_lineHasContent) _cx += INLINE_IMG_SPACE;

  // Wrap if image won't fit on this line
  if (_cx > _wrapLeftMargin && _cx + static_cast<int16_t>(imgW) > _rightBound) {
    newLine();
    if (_pageFull) return;
  }

  // Vertical placement:
  //   The formula PNG bottom is placed at (baseline + INLINE_IMG_DESCENT)
  //   so the visual math baseline aligns with the text baseline.
  int16_t destX = _cx;
  int16_t destY = _cy - static_cast<int16_t>(imgH) + INLINE_IMG_DESCENT;
  if (destY < 0) destY = 0;

  checkPageOverflow(0);
  if (_pageFull) return;

  // Decode and draw
  decodePng(path, destX, destY, imgW, imgH, false);

  // Advance cursor; add post-image space so the next word has breathing room
  _cx += static_cast<int16_t>(imgW) + INLINE_IMG_SPACE;

  // Line height: image spans (imgH - INLINE_IMG_DESCENT) above baseline
  int16_t imgAscent = static_cast<int16_t>(imgH) - INLINE_IMG_DESCENT;
  if (imgAscent > _lineH) _lineH = imgAscent;
  _lineHasContent = true;
}

// ===========================================================================
// renderBlockImage — block PNG figure (centred, own line)
// ===========================================================================
void EpubRenderer::renderBlockImage(const char* path) {
  if (_lineHasContent) newLine();
  if (_pageFull) return;

  uint16_t imgW = 0, imgH = 0;

  // Measure first (no draw)
  if (!decodePng(path, 0, 0, imgW, imgH, true)) return;
  if (imgW == 0 || imgH == 0) return;

  checkPageOverflow(static_cast<int16_t>(imgH) + LINE_SPACING * 2);
  if (_pageFull) return;

  // Top of the image = top of the current line slot (not the baseline)
  int16_t destY     = static_cast<int16_t>(_cy - _bodyFontAscent);
  if (destY < MARGIN_TOP) destY = MARGIN_TOP;
  int16_t centeredX = (DISPLAY_W - static_cast<int16_t>(imgW)) / 2;
  if (centeredX < 0) centeredX = 0;

  // Decode and draw
  decodePng(path, centeredX, destY, imgW, imgH, false);

  // Advance past the image: move baseline to bottom of image + spacing
  _cy             = destY + static_cast<int16_t>(imgH) + LINE_SPACING * 2 + _bodyFontAscent;
  _cx             = _wrapLeftMargin;
  _lineH          = 0;
  _lineHasContent = false;
}

// ===========================================================================
// readJpegDimensions — manual JPEG SOF header parser (fallback)
//
// TJpgDec only supports baseline DCT (SOF0).  Progressive JPEGs (SOF2) will
// cause getSdJpgSize() to return JDR_FMT3.  This helper reads the raw JFIF
// markers to extract width/height regardless of encoding, so we can at least
// display them by falling back to a raw decode attempt.
// Returns true and fills w/h if found.
// ===========================================================================
static bool readJpegDimensions(const char* path, SemaphoreHandle_t mtx,
                                uint16_t& w, uint16_t& h) {
  w = h = 0;
  if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);
  File f = SD.open(path);
  if (!f) { if (mtx) xSemaphoreGive(mtx); return false; }

  // Check JFIF/EXIF SOI marker
  if (f.read() != 0xFF || f.read() != 0xD8) {
    f.close(); if (mtx) xSemaphoreGive(mtx); return false;
  }

  bool found = false;
  while (!found && f.available() >= 4) {
    // Scan for next marker
    uint8_t b;
    do { b = f.read(); } while (b != 0xFF && f.available());
    while (b == 0xFF && f.available()) b = f.read();  // skip padding
    uint8_t marker = b;

    // Read segment length (big-endian, includes the 2 length bytes)
    uint8_t hi = f.read(), lo = f.read();
    uint16_t segLen = static_cast<uint16_t>((hi << 8) | lo);

    // SOF0 (0xC0), SOF1 (0xC1), SOF2 (0xC2 – progressive)
    if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
      f.read();  // precision byte
      uint8_t h3 = f.read(), h4 = f.read();
      uint8_t w3 = f.read(), w4 = f.read();
      h = static_cast<uint16_t>((h3 << 8) | h4);
      w = static_cast<uint16_t>((w3 << 8) | w4);
      found = true;
    } else {
      // Skip this segment
      int skip = static_cast<int>(segLen) - 2;
      while (skip-- > 0 && f.available()) f.read();
    }
  }
  f.close();
  if (mtx) xSemaphoreGive(mtx);
  return found && w > 0 && h > 0;
}

// ===========================================================================
// renderBlockImageJpg — block JPEG figure (centred, own line)
// Mirrors renderBlockImage() but uses TJpg_Decoder for the actual decode.
//
// Auto-scaling: picks the smallest TJpgDec scale (1/2/4/8) that makes the
// image fit within DISPLAY_W.  Queries size at the chosen scale so centering
// and page-overflow checks use the real scaled dimensions.
// ===========================================================================
void EpubRenderer::renderBlockImageJpg(const char* path) {
  if (_lineHasContent) newLine();
  if (_pageFull) return;

  // --- Step 1: get native image size at scale=1 ---
  uint16_t nativeW = 0, nativeH = 0;
  TJpgDec.setJpgScale(1);
  if (_sdMutex) xSemaphoreTake(_sdMutex, portMAX_DELAY);
  JRESULT rc = TJpgDec.getSdJpgSize(&nativeW, &nativeH, path);
  if (_sdMutex) xSemaphoreGive(_sdMutex);

  Serial.printf("[JPEG] native size rc=%d  w=%u h=%u  path=%s\n",
                rc, nativeW, nativeH, path);

  if (rc != JDR_OK || nativeW == 0 || nativeH == 0) {
    Serial.printf("[JPEG] getSdJpgSize failed, trying manual SOF parse\n");
    if (!readJpegDimensions(path, _sdMutex, nativeW, nativeH)) {
      Serial.printf("[JPEG] SOF parse also failed – skipping\n");
      return;
    }
    Serial.printf("[JPEG] SOF parse: w=%u h=%u\n", nativeW, nativeH);
  }

  // --- Step 2: pick scale factor to fit inside display width ---
  // TJpgDec valid scales: 1 (full), 2 (1/2), 4 (1/4), 8 (1/8)
  uint8_t  scale  = 1;
  uint16_t imgW   = nativeW;
  uint16_t imgH   = nativeH;
  for (uint8_t s : {(uint8_t)2, (uint8_t)4, (uint8_t)8}) {
    if (imgW <= static_cast<uint16_t>(DISPLAY_W - MARGIN_LEFT - MARGIN_RIGHT)) break;
    scale = s;
    imgW  = nativeW / s;
    imgH  = nativeH / s;
  }
  Serial.printf("[JPEG] using scale=%u  rendered w=%u h=%u\n", scale, imgW, imgH);

  // --- Step 3: re-query at chosen scale so TJpgDec internal state is correct ---
  TJpgDec.setJpgScale(scale);
  if (scale > 1) {
    uint16_t qW = 0, qH = 0;
    if (_sdMutex) xSemaphoreTake(_sdMutex, portMAX_DELAY);
    TJpgDec.getSdJpgSize(&qW, &qH, path);   // primes internal state at this scale
    if (_sdMutex) xSemaphoreGive(_sdMutex);
    if (qW > 0) { imgW = qW; imgH = qH; }   // use exact scaled dimensions if available
  }

  // --- Step 4: page overflow check ---
  checkPageOverflow(static_cast<int16_t>(imgH) + LINE_SPACING * 2);
  if (_pageFull) return;

  // --- Step 5: draw centred ---
  int16_t destY     = static_cast<int16_t>(_cy - _bodyFontAscent);
  if (destY < MARGIN_TOP) destY = MARGIN_TOP;
  int16_t centeredX = (DISPLAY_W - static_cast<int16_t>(imgW)) / 2;
  if (centeredX < 0) centeredX = 0;

  uint16_t outW = 0, outH = 0;
  decodeJpg(path, centeredX, destY, outW, outH, scale);
  if (outW > 0) { imgW = outW; imgH = outH; }   // use actual decoded extent

  Serial.printf("[JPEG] drawn at x=%d y=%d  actual w=%u h=%u\n",
                centeredX, destY, imgW, imgH);

  // Advance past the image
  _cy             = destY + static_cast<int16_t>(imgH) + LINE_SPACING * 2 + _bodyFontAscent;
  _cx             = _wrapLeftMargin;
  _lineH          = 0;
  _lineHasContent = false;
}

// ===========================================================================
// measureCellHeight
//
// Simulates word-wrap within colW without drawing anything.
// Returns the content height (px) needed for the given cell text.
// Handles CELL_IMG_SENTINEL-embedded images (estimated at 40px wide).
// ===========================================================================
int16_t EpubRenderer::measureCellHeight(const char* text,
                                         FontLevel   level,
                                         int16_t     colW) {
  selectFont(level);

  // Measure reference glyph height
  int16_t bx, by; uint16_t bw, bh;
  _canvas.getTextBounds("Ag", 0, 0, &bx, &by, &bw, &bh);
  int16_t fontH = static_cast<int16_t>(bh > 0 ? bh : 14);

  int16_t     cx        = 0;
  int16_t     lineCount = 1;
  const char* p         = text;
  char        word[64];
  int         wi = 0;

  auto measureWord = [&]() {
    if (wi == 0) return;
    word[wi] = '\0'; wi = 0;
    uint16_t tw, th; int16_t x1, y1;
    _canvas.getTextBounds(word, 0, 0, &x1, &y1, &tw, &th);
    int16_t needed = (cx > 0) ? static_cast<int16_t>(tw) + 4
                               : static_cast<int16_t>(tw);
    if (cx > 0 && cx + needed > colW) { ++lineCount; cx = static_cast<int16_t>(tw); }
    else                                cx += needed;
  };

  while (*p) {
    if (*p == CELL_IMG_SENTINEL) {
      measureWord();
      ++p;
      while (*p && *p != CELL_IMG_SENTINEL) ++p;
      if (*p) ++p;
      // Inline image: estimate 40px wide
      if (cx + 40 > colW && cx > 0) { ++lineCount; cx = 40; }
      else cx += 42;
      continue;
    }
    char c = *p++;
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      measureWord();
    } else {
      if (wi < 63) word[wi++] = c;
    }
  }
  measureWord();

  return lineCount * (fontH + LINE_SPACING) + 2;
}

// ===========================================================================
// renderCellContent
//
// Draws a single table cell's content into the rectangle (x, y, w, rowH).
// Saves and restores all cursor / bound state so the outer renderer is
// completely unaffected.  Page-overflow checks are suppressed while inside
// this function (_inCellRender = true).
// ===========================================================================
void EpubRenderer::renderCellContent(const TableCell& cell,
                                      int16_t x, int16_t y, int16_t w) {
  // ---- Save outer state ---------------------------------------------------
  int16_t savedCx    = _cx;
  int16_t savedCy    = _cy;
  int16_t savedLineH = _lineH;
  bool    savedLine  = _lineHasContent;
  int16_t savedWrap  = _wrapLeftMargin;
  int16_t savedRight = _rightBound;
  bool    savedFull  = _pageFull;

  // ---- Set cell-local state -----------------------------------------------
  _cx             = x;
  _cy             = y;
  _lineH          = 0;
  _lineHasContent = false;
  _wrapLeftMargin = x;
  _rightBound     = x + w;
  _pageFull       = false;
  _inCellRender   = true;

  // ---- Walk cell text, handle CELL_IMG_SENTINEL-embedded image paths -------
  const char* p        = cell.text;
  char        word[MAX_TEXT_LEN];
  int         wi       = 0;
  bool        hadSpace = false;

  auto flushCellWord = [&]() {
    if (wi == 0) return;
    word[wi] = '\0'; wi = 0;
    char tmp[MAX_TEXT_LEN];
    if (hadSpace) {
      snprintf(tmp, sizeof(tmp), " %s", word);
      renderWord(tmp, cell.fontLevel);
    } else {
      renderWord(word, cell.fontLevel);
    }
    hadSpace = false;
  };

  while (*p) {
    if (*p == CELL_IMG_SENTINEL) {
      flushCellWord();
      ++p;
      char imgPath[MAX_PATH_LEN]; int pi = 0;
      while (*p && *p != CELL_IMG_SENTINEL && pi < MAX_PATH_LEN - 1)
        imgPath[pi++] = *p++;
      imgPath[pi] = '\0';
      if (*p == CELL_IMG_SENTINEL) ++p;

      // Decode inline PNG at current cell cursor — apply same spacing as
      // renderInlineImage() (pre/post gap; descent not applied in cell context
      // since cell _cy is top-of-cell, not a text baseline).
      uint16_t imgW = 0, imgH = 0;
      if (_lineHasContent) _cx += INLINE_IMG_SPACE;
      decodePng(imgPath, _cx, _cy, imgW, imgH, false);
      if (imgW > 0) {
        _cx += static_cast<int16_t>(imgW) + INLINE_IMG_SPACE;
        if (static_cast<int16_t>(imgH) > _lineH) _lineH = static_cast<int16_t>(imgH);
        _lineHasContent = true;
      }
      hadSpace = false;
      continue;
    }
    char c = *p++;
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      flushCellWord();
      hadSpace = true;
    } else {
      if (wi < static_cast<int>(sizeof(word)) - 1) word[wi++] = c;
    }
  }
  flushCellWord();

  // ---- Restore outer state ------------------------------------------------
  _inCellRender   = false;
  _cx             = savedCx;
  _cy             = savedCy;
  _lineH          = savedLineH;
  _lineHasContent = savedLine;
  _wrapLeftMargin = savedWrap;
  _rightBound     = savedRight;
  _pageFull       = savedFull;
}

// ===========================================================================
// renderTableRow
//
// Renders the cells buffered in _tableCells[0.._tableCurCol-1]:
//   • First row:  compute equal column widths spanning MARGIN_LEFT…MARGIN_RIGHT.
//   • Every row:  measure max cell height, check page overflow,
//                 draw top border + vertical borders, render each cell.
//
// Note: if the row triggers page-full, the row is dropped (content lost).
// For this ebook's small tables this edge case is unlikely.
// ===========================================================================
void EpubRenderer::renderTableRow() {
  if (_tableCurCol == 0) return;

  // ---- First row: establish column layout ---------------------------------
  if (_tableColCount == 0) {
    _tableColCount = _tableCurCol;
    int16_t tableW  = DISPLAY_W - MARGIN_LEFT - MARGIN_RIGHT;
    int16_t borders = static_cast<int16_t>(_tableColCount + 1) * TABLE_BORDER_W;
    int16_t colW    = (tableW - borders) / static_cast<int16_t>(_tableColCount);
    int16_t extra   = (tableW - borders) - colW * static_cast<int16_t>(_tableColCount);

    for (int i = 0; i < _tableColCount; ++i) {
      _tableColX[i] = MARGIN_LEFT
                    + static_cast<int16_t>(i + 1) * TABLE_BORDER_W
                    + static_cast<int16_t>(i) * colW;
      // Last column gets the rounding remainder so the right border is flush
      _tableColW[i] = (i == _tableColCount - 1) ? colW + extra : colW;
    }
  }

  // ---- Measure row height -------------------------------------------------
  int16_t rowH = 4;   // minimum
  int     cols = (_tableCurCol < _tableColCount) ? _tableCurCol : _tableColCount;
  for (int i = 0; i < cols; ++i) {
    int16_t h = measureCellHeight(_tableCells[i].text,
                                   _tableCells[i].fontLevel,
                                   _tableColW[i] - 2 * TABLE_CELL_PAD);
    if (h > rowH) rowH = h;
  }
  rowH += 2 * TABLE_CELL_PAD;   // add top + bottom cell padding

  // ---- Page overflow check (at row level) ---------------------------------
  checkPageOverflow(rowH + TABLE_BORDER_W);
  if (_pageFull) return;

  // ---- Top border of this row ---------------------------------------------
  drawHLine(MARGIN_LEFT, _cy, DISPLAY_W - MARGIN_LEFT - MARGIN_RIGHT);
  _cy += TABLE_BORDER_W;

  // ---- Render each cell + its left vertical border ------------------------
  for (int i = 0; i < cols; ++i) {
    drawVLine(_tableColX[i] - TABLE_BORDER_W, _cy, rowH);
    renderCellContent(_tableCells[i],
                      _tableColX[i] + TABLE_CELL_PAD,
                      _cy + TABLE_CELL_PAD,
                      _tableColW[i] - 2 * TABLE_CELL_PAD);
  }
  // Right outer border
  int16_t rightX = _tableColX[_tableColCount - 1] + _tableColW[_tableColCount - 1];
  drawVLine(rightX, _cy, rowH);

  _cy += rowH;
}

// ===========================================================================
// feed() — dispatcher for all element types (called from Core 1)
//
// Returns false when the page is full (caller must call endPage() and begin a
// new slot, then re-feed the overflowing element).
// ===========================================================================
bool EpubRenderer::feed(const RenderElem& elem) {
  if (_pageFull) return false;

  switch (elem.type) {

    // ---- Text ---------------------------------------------------------------
    case ELEM_TEXT:
    case ELEM_HEADING:
      renderWord(elem.text, elem.fontLevel);
      break;

    // ---- Images -------------------------------------------------------------
    case ELEM_IMAGE_INLINE:
      renderInlineImage(elem.path);
      break;
    case ELEM_IMAGE_BLOCK:
      renderBlockImage(elem.path);
      break;
    case ELEM_IMAGE_BLOCK_JPG:
      renderBlockImageJpg(elem.path);
      break;

    // ---- Paragraph break ----------------------------------------------------
    case ELEM_PARA_BREAK:
      if (_lineHasContent) newLine();
      // If we just finished a list item, restore normal margins
      if (_inListItem) {
        _inListItem     = false;
        _wrapLeftMargin = MARGIN_LEFT;
        _rightBound     = DISPLAY_W - MARGIN_RIGHT;
      }
      newLine(PARA_SPACING);
      break;

    // ---- Heading break ------------------------------------------------------
    case ELEM_HEADING_BREAK:
      if (_lineHasContent) newLine();
      newLine(HEADING_SPACING);
      break;

    // ---- Forced line break --------------------------------------------------
    case ELEM_BR:
      newLine(0);
      break;

    // ---- Ordered list item --------------------------------------------------
    case ELEM_OL_ITEM_START: {
      if (_lineHasContent) newLine();
      _inListItem     = true;
      _wrapLeftMargin = MARGIN_LEFT;   // temporarily; set again after prefix
      _rightBound     = DISPLAY_W - MARGIN_RIGHT;
      _cx             = MARGIN_LEFT;
      // Print "N. " in bold at the left margin
      char prefix[8];
      snprintf(prefix, sizeof(prefix), "%d. ", elem.listNum);
      renderWord(prefix, FONT_BODY_BOLD);
      // Continuation lines wrap to just after the printed number
      _wrapLeftMargin = _cx;
      break;
    }

    // ---- Table --------------------------------------------------------------
    case ELEM_TABLE_START:
      if (_lineHasContent) newLine();
      _inTable       = true;
      _tableColCount = 0;
      _tableCurCol   = 0;
      break;

    case ELEM_TABLE_HEADER_CELL:
    case ELEM_TABLE_DATA_CELL:
      if (_tableCurCol < MAX_TABLE_COLS) {
        strncpy(_tableCells[_tableCurCol].text, elem.text, 255);
        _tableCells[_tableCurCol].text[255] = '\0';
        _tableCells[_tableCurCol].fontLevel = elem.fontLevel;
        ++_tableCurCol;
      }
      break;

    case ELEM_TABLE_ROW_END:
      if (_inTable) renderTableRow();
      // If the row triggered page-full, do NOT reset the column buffer.
      // This allows the next page to re-feed this row and successfully render it.
      if (!_pageFull) {
        _tableCurCol = 0;
      }
      break;

    case ELEM_TABLE_END:
      if (_inTable) {
        // Bottom border
        drawHLine(MARGIN_LEFT, _cy, DISPLAY_W - MARGIN_LEFT - MARGIN_RIGHT);
        _cy           += TABLE_BORDER_W + PARA_SPACING;
        _tableColCount = 0;
        _tableCurCol   = 0;
        _inTable       = false;
      }
      break;
  }
  return !_pageFull;
}

// ===========================================================================
// Portrait → native panel orientation
//
// Our bitmaps are laid out for setRotation(1) (480×800 portrait).
// writeImage() writes in native landscape (800×480) without GFX rotation.
// ===========================================================================
static uint8_t* s_nativeBuf = nullptr;

static const uint8_t* portraitToNative(const uint8_t* portrait) {
  if (!portrait) return nullptr;
  if (!s_nativeBuf) {
    s_nativeBuf = static_cast<uint8_t*>(
      heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM));
    if (!s_nativeBuf) return nullptr;
  }

  constexpr int16_t panelW      = GxEPD2_750_T7::WIDTH;   // 800
  constexpr int16_t panelH      = GxEPD2_750_T7::HEIGHT;  // 480
  constexpr int     panelStride = (panelW + 7) / 8;

  memset(s_nativeBuf, 0xFF, FB_SIZE);
  for (int16_t ny = 0; ny < panelH; ++ny) {
    for (int16_t nx = 0; nx < panelW; ++nx) {
      const int16_t px      = ny;
      const int16_t py      = panelW - 1 - nx;
      const uint8_t srcByte = portrait[py * FB_STRIDE + (px >> 3)];
      const bool    white   = (srcByte >> (7 - (px & 7))) & 1;
      uint8_t*      dstByte = &s_nativeBuf[ny * panelStride + (nx >> 3)];
      const uint8_t mask    = static_cast<uint8_t>(0x80u >> (nx & 7));
      if (white) *dstByte |=  mask;
      else       *dstByte &= static_cast<uint8_t>(~mask);
    }
  }
  return s_nativeBuf;
}

// ===========================================================================
// showPageFull — full refresh for first page (initialises both GD7965 buffers)
// ===========================================================================
void EpubRenderer::showPageFull(int slot) {
  const uint8_t* buf    = g_pool.slotBuf(slot);
  const uint8_t* native = portraitToNative(buf);
  if (!native) return;
  _disp.writeImage(native, 0, 0,
                   GxEPD2_750_T7::WIDTH, GxEPD2_750_T7::HEIGHT,
                   false, false, false);
  _disp.refresh(false);
  _disp.epd2.writeImageToPrevious(native, 0, 0,
                                   GxEPD2_750_T7::WIDTH, GxEPD2_750_T7::HEIGHT,
                                   false, false, false);
}

// ===========================================================================
// showPagePartial — fast partial refresh for all subsequent pages
// ===========================================================================
void EpubRenderer::showPagePartial(int slot) {
  const uint8_t* buf    = g_pool.slotBuf(slot);
  const uint8_t* native = portraitToNative(buf);
  if (!native) return;
  _disp.writeImage(native, 0, 0,
                   GxEPD2_750_T7::WIDTH, GxEPD2_750_T7::HEIGHT,
                   false, false, false);
  _disp.refresh(true);
  _disp.epd2.writeImageToPrevious(native, 0, 0,
                                   GxEPD2_750_T7::WIDTH, GxEPD2_750_T7::HEIGHT,
                                   false, false, false);
}
