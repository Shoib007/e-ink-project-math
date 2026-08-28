#include "epub_renderer.h"
#include <string.h>

// Cache_WriteBack_Addr: flush dirty CPU D-cache lines to physical PSRAM.
// Must be called before any SPI/DMA transfer that reads from a PSRAM buffer
// written by the CPU, otherwise the DMA sees stale (old-page) data.
// This is a ROM function, always available on all ESP32-S3 IDF versions.
extern "C" int Cache_WriteBack_Addr(uint32_t addr, uint32_t size);

EpubRenderer* EpubRenderer::s_instance = nullptr;

// ===========================================================================
// RGB565 → luma lookup table (64 KB in PSRAM)
//
// Precomputes the luma value for every possible 16-bit RGB565 pixel.
// Index = the 16-bit pixel value; value = 0-255 luma.
// Threshold for black/white is 180 (matching the original code).
// ===========================================================================
static uint8_t* s_lumaLUT = nullptr;

static void ensureLumaLUT() {
  if (s_lumaLUT) return;
  s_lumaLUT = static_cast<uint8_t*>(
    heap_caps_malloc(65536, MALLOC_CAP_SPIRAM));
  if (!s_lumaLUT) return;
  for (uint32_t i = 0; i < 65536; ++i) {
    uint8_t r8 = static_cast<uint8_t>(((i >> 11) & 0x1F) << 3);
    uint8_t g8 = static_cast<uint8_t>(((i >>  5) & 0x3F) << 2);
    uint8_t b8 = static_cast<uint8_t>(( i        & 0x1F) << 3);
    uint32_t lum = (static_cast<uint32_t>(r8) * 299 +
                    static_cast<uint32_t>(g8) * 587 +
                    static_cast<uint32_t>(b8) * 114) / 1000;
    s_lumaLUT[i] = static_cast<uint8_t>(lum);
  }
}

// Fast luma lookup: returns black(0) or white(1) for a 16-bit RGB565 pixel.
static inline uint8_t lumaBW(uint16_t pixel) {
  return (s_lumaLUT && s_lumaLUT[pixel] < 180) ? 0 : 1;
}

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
  ensureLumaLUT();
  uint16_t* buf = _lineBuffer;
  bool dynamicBuf = false;
  if (pDraw->iWidth > DISPLAY_W) {
    buf = static_cast<uint16_t*>(heap_caps_malloc(pDraw->iWidth * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!buf) return;
    dynamicBuf = true;
  }

  // Debug: print first row info
  if (pDraw->y == 0) {
    Serial.printf("[PNG] width=%d pixelType=%d hasAlpha=%d scale=%.2f\n", 
                  pDraw->iWidth, pDraw->iPixelType, pDraw->iHasAlpha, _inlineImgScale);
  }

  _png.getLineAsRGB565(pDraw, buf, PNG_RGB565_LITTLE_ENDIAN, 0xFFFF);  // Use RGB565 white (0xFFFF) for transparent pixels
  
  // Handle scaling for inline images
  if (_inlineImgScale < 1.0f) {
    // Downsampling mode: only render rows that map to scaled output
    int scaledRow = static_cast<int>(static_cast<float>(pDraw->y) * _inlineImgScale);
    if (scaledRow >= static_cast<int>(_inlineImgScaledH)) {
      if (dynamicBuf) free(buf);
      return;  // Skip rows beyond scaled height
    }
    
    int16_t py = _inlineImgDestY + scaledRow;
    if (py >= 0 && py < DISPLAY_H) {
      // Sample pixels with nearest-neighbor
      for (uint16_t sx = 0; sx < _inlineImgScaledW; ++sx) {
        int srcX = static_cast<int>(static_cast<float>(sx) / _inlineImgScale);
        if (srcX >= pDraw->iWidth) srcX = pDraw->iWidth - 1;
        
        int16_t px = _inlineImgDestX + static_cast<int16_t>(sx);
        if (px < 0 || px >= DISPLAY_W) continue;
        
        uint16_t p  = buf[srcX];
        
        // Skip white/near-white pixels (transparent background)
        // Only draw the actual dark formula content
        if (p >= 0xF7DE) continue;  // Skip if RGB565 >= (248,252,248) - very light pixels
        
        uint8_t  r8 = static_cast<uint8_t>(((p >> 11) & 0x1F) << 3);
        uint8_t  g8 = static_cast<uint8_t>(((p >>  5) & 0x3F) << 2);
        uint8_t  b8 = static_cast<uint8_t>(( p        & 0x1F) << 3);
        uint16_t luma = static_cast<uint16_t>(
          (static_cast<uint32_t>(r8) * 299 +
           static_cast<uint32_t>(g8) * 587 +
           static_cast<uint32_t>(b8) * 114) / 1000);
        
        // Only draw dark pixels (the formula itself), skip light background
        if (luma >= 240) continue;
        
        g_pool.drawPixel(px, py, luma < 128 ? 0 : 1);
      }
    }
  } else {
    // No scaling - render at original size
    int16_t py = _imgDestY + static_cast<int16_t>(pDraw->y);
    if (py >= 0 && py < DISPLAY_H) {
      int maxCols = pDraw->iWidth < DISPLAY_W ? pDraw->iWidth : DISPLAY_W;
      for (int x = 0; x < maxCols; ++x) {
        int16_t px = _imgDestX + static_cast<int16_t>(x);
        if (px < 0 || px >= DISPLAY_W) continue;
        
        uint16_t p  = buf[x];
        
        // Skip white/near-white pixels (transparent background)
        // Only draw the actual dark content
        if (p >= 0xF7DE) continue;  // Skip if RGB565 >= (248,252,248) - very light pixels
        
        uint8_t  r8 = static_cast<uint8_t>(((p >> 11) & 0x1F) << 3);
        uint8_t  g8 = static_cast<uint8_t>(((p >>  5) & 0x3F) << 2);
        uint8_t  b8 = static_cast<uint8_t>(( p        & 0x1F) << 3);
        uint16_t luma = static_cast<uint16_t>(
          (static_cast<uint32_t>(r8) * 299 +
           static_cast<uint32_t>(g8) * 587 +
           static_cast<uint32_t>(b8) * 114) / 1000);
        
        // Only draw dark pixels, skip light background
        if (luma >= 240) continue;
        
        g_pool.drawPixel(px, py, luma < 128 ? 0 : 1);
      }
    }
  }

  if (dynamicBuf) {
    free(buf);
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
  ensureLumaLUT();

  // Track extent relative to the dest origin
  uint16_t relW = static_cast<uint16_t>(x - inst._imgDestX) + w;
  uint16_t relH = static_cast<uint16_t>(y - inst._imgDestY) + h;
  if (relW > inst._jpgImgW) inst._jpgImgW = relW;
  if (relH > inst._jpgImgH) inst._jpgImgH = relH;

  // Render pixels using LUT
  for (uint16_t row = 0; row < h; ++row) {
    for (uint16_t col = 0; col < w; ++col) {
      int16_t px = x + static_cast<int16_t>(col);
      int16_t py = y + static_cast<int16_t>(row);
      if (px < 0 || px >= DISPLAY_W || py < 0 || py >= DISPLAY_H) continue;
      g_pool.drawPixel(px, py, lumaBW(bitmap[row * w + col]));
    }
  }
  return true;
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
// Skips the call if the font is already active (avoids hundreds of redundant
// setFont() calls per page for text-heavy content).
// ===========================================================================
void EpubRenderer::selectFont(FontLevel level) {
  if (level == _curFontLevel) return;
  _curFontLevel = level;
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
// Font metrics cache — compute ascent/descent/lineAdv for all font levels
// once per page.  Avoids repeated getTextBounds() in renderWord().
// ===========================================================================
void EpubRenderer::cacheAllFontMetrics() {
  for (int i = 0; i <= FONT_H1; ++i) {
    FontLevel lvl = static_cast<FontLevel>(i);
    _curFontLevel = static_cast<FontLevel>(-1);  // force selectFont to apply
    selectFont(lvl);
    int16_t x1, y1; uint16_t bw, bh;
    _canvas.getTextBounds("Ag", 0, 0, &x1, &y1, &bw, &bh);
    _fontMetrics[i].ascent  = static_cast<int16_t>(-y1);
    _fontMetrics[i].descent = static_cast<int16_t>(bh) + y1;
    _fontMetrics[i].lineAdv = static_cast<int16_t>(bh);
  }
  _curFontLevel = static_cast<FontLevel>(-1);  // reset so first selectFont works
}

// ===========================================================================
// Page management
// ===========================================================================

// Helper: measure the ascent of the body font so we can prime _cy correctly.
// Ascent = distance from baseline to top of tallest glyph = -y1 from getTextBounds.
int16_t EpubRenderer::fontAscent(FontLevel level) {
  if (_fontMetrics[level].ascent != 0) return _fontMetrics[level].ascent;
  selectFont(level);
  int16_t x1, y1; uint16_t bw, bh;
  _canvas.getTextBounds("Ag", 0, 0, &x1, &y1, &bw, &bh);
  return static_cast<int16_t>(-y1);   // y1 is negative in GFX convention
}

void EpubRenderer::beginPage(int slot) {
  g_pool.setCurrent(slot);
  g_pool.clearCurrent();

  // Pre-compute ascent/descent for ALL font levels once per page.
  // This eliminates thousands of getTextBounds() calls during rendering.
  cacheAllFontMetrics();

  // _bodyFontAscent is the cached FONT_BODY ascent from the metrics table.
  _bodyFontAscent = _fontMetrics[FONT_BODY].ascent;

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
  _inCellRender   = false;

  // If a table was mid-row when the previous page filled up, resume it at
  // the top of this new page.  _tableCurCol and the cell buffer are preserved
  // by feed() (it does NOT reset them on ROW_PAGE_FULL).
  if (_inTable && _table.isActive()) {
    int16_t newTop = _cy - _bodyFontAscent;   // = MARGIN_TOP
    int16_t newBot = DISPLAY_H - MARGIN_BOTTOM;
    _table.continueOnPage(newTop, newBot);
    // _cy stays at MARGIN_TOP + _bodyFontAscent; the table row will update it.
  } else {
    _inTable     = false;
    _tableCurCol = 0;
  }
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
//
// Operate directly on the current slot's framebuffer for batch speed.
// H-line: set/clear full bytes with memset, handle edge pixels with masks.
// V-line: stride by FB_STRIDE per row, set/clear single bits.
// ===========================================================================
void EpubRenderer::drawHLine(int16_t x, int16_t y, int16_t w) {
  if (y < 0 || y >= DISPLAY_H || w <= 0) return;
  // Clamp to display bounds
  if (x < 0) { w += x; x = 0; }
  if (x + w > DISPLAY_W) w = DISPLAY_W - x;
  if (w <= 0) return;

  uint8_t* row = const_cast<uint8_t*>(g_pool.slotBuf(g_pool.currentSlot())) + y * FB_STRIDE;

  int startByte = x >> 3;
  int startBit  = x & 7;
  int endBit    = (x + w) & 7;
  int endByte   = (x + w - 1) >> 3;

  if (startByte == endByte) {
    // All pixels within one byte
    uint8_t mask = 0;
    for (int i = 0; i < w; ++i) mask |= (0x80u >> (startBit + i));
    row[startByte] &= ~mask;  // set black
    return;
  }

  // First partial byte
  if (startBit > 0) {
    uint8_t mask = 0xFFu >> startBit;  // bits from startBit to bit 7
    row[startByte] &= ~mask;
    startByte++;
  }

  // Full middle bytes — clear to black (0x00)
  if (startByte <= endByte) {
    memset(row + startByte, 0x00, endByte - startByte);
  }

  // Last partial byte
  if (endBit > 0 && endByte >= startByte) {
    uint8_t mask = ~(0xFFu >> endBit);  // bits from bit 7 down to endBit
    row[endByte] &= ~mask;
  }
}

void EpubRenderer::drawVLine(int16_t x, int16_t y, int16_t h) {
  if (x < 0 || x >= DISPLAY_W || h <= 0) return;
  if (y < 0) { h += y; y = 0; }
  if (y + h > DISPLAY_H) h = DISPLAY_H - y;
  if (h <= 0) return;

  const uint8_t bitMask = 0x80u >> (x & 7);
  const int     byteOff = x >> 3;
  uint8_t* base = const_cast<uint8_t*>(g_pool.slotBuf(g_pool.currentSlot())) + y * FB_STRIDE + byteOff;

  for (int16_t i = 0; i < h; ++i) {
    *base &= ~bitMask;  // set black
    base += FB_STRIDE;
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

  // Use cached ascent/descent (computed once per page in beginPage()).
  // Only call getTextBounds() for the pixel width (advance) — we can't
  // cache that per-word, but we avoid the redundant ascent/descent work.
  int16_t x1, y1; uint16_t tw, th;
  _canvas.getTextBounds(word, 0, 0, &x1, &y1, &tw, &th);

  const FontMetrics& fm = _fontMetrics[level];
  int16_t ascent  = fm.ascent;
  int16_t descent = fm.descent;
  int16_t advance = static_cast<int16_t>(tw);

  // Wrap if word won't fit on this line
  if (_cx > _wrapLeftMargin && _cx + advance > _rightBound) {
    newLine();
    if (_pageFull) return;
    // Re-measure width only after cursor moved (ascent/descent unchanged)
    _canvas.getTextBounds(word, 0, 0, &x1, &y1, &tw, &th);
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

  // Calculate target height: match typical body text height (~18px)
  // This makes formulas integrate naturally with surrounding text
  const uint16_t TARGET_HEIGHT = 18;
  float scale = 1.0f;
  uint16_t scaledW = imgW;
  uint16_t scaledH = imgH;
  
  if (imgH > TARGET_HEIGHT) {
    scale = static_cast<float>(TARGET_HEIGHT) / static_cast<float>(imgH);
    scaledW = static_cast<uint16_t>(static_cast<float>(imgW) * scale);
    scaledH = TARGET_HEIGHT;
    Serial.printf("[PNG] Scaling inline image from %ux%u to %ux%u (scale=%.2f)\n", 
                  imgW, imgH, scaledW, scaledH, scale);
  }

  // Add pre-image space if something is already on this line
  if (_lineHasContent) _cx += INLINE_IMG_SPACE;

  // Wrap if scaled image won't fit on this line
  if (_cx > _wrapLeftMargin && _cx + static_cast<int16_t>(scaledW) > _rightBound) {
    newLine();
    if (_pageFull) return;
  }

  // Vertical placement with scaled height
  int16_t destX = _cx;
  int16_t destY = _cy - static_cast<int16_t>(scaledH) + INLINE_IMG_DESCENT;
  if (destY < 0) destY = 0;

  checkPageOverflow(0);
  if (_pageFull) return;

  // Store scale info for pngRowDraw to use during decode
  _inlineImgScale = scale;
  _inlineImgDestX = destX;
  _inlineImgDestY = destY;
  _inlineImgScaledW = scaledW;
  _inlineImgScaledH = scaledH;
  
  // Decode - pngRowDraw will handle downsampling
  decodePng(path, destX, destY, imgW, imgH, false);
  
  _inlineImgScale = 1.0f;  // Reset

  // Advance cursor using scaled width
  _cx += static_cast<int16_t>(scaledW) + INLINE_IMG_SPACE;

  // Line height using scaled height
  int16_t imgAscent = static_cast<int16_t>(scaledH) - INLINE_IMG_DESCENT;
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

  // --- Step 1: get native image size via SOF marker parse (single SD open) ---
  // readJpegDimensions() scans JFIF/EXIF markers directly — no full decode.
  // This replaces the old getSdJpgSize() + fallback approach (2-4 SD opens).
  uint16_t nativeW = 0, nativeH = 0;
  if (!readJpegDimensions(path, _sdMutex, nativeW, nativeH)) {
    // Fallback: try TJpgDec's size query if SOF parse fails
    TJpgDec.setJpgScale(1);
    if (_sdMutex) xSemaphoreTake(_sdMutex, portMAX_DELAY);
    JRESULT rc = TJpgDec.getSdJpgSize(&nativeW, &nativeH, path);
    if (_sdMutex) xSemaphoreGive(_sdMutex);
    if (rc != JDR_OK || nativeW == 0 || nativeH == 0) {
      Serial.printf("[JPEG] dimension query failed for %s\n", path);
      return;
    }
  }
  Serial.printf("[JPEG] native size w=%u h=%u  path=%s\n", nativeW, nativeH, path);

  // --- Step 2: pick scale factor to fit inside display width ---
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

  // --- Step 3: set scale for decode (no second getSdJpgSize — dimensions are exact) ---
  TJpgDec.setJpgScale(scale);

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
// measureTableCell — callback supplied to TableRenderer::endRow().
//
// Simulates word-wrap within colW without drawing anything.
// Returns the CONTENT height (px) needed; padding is added by TableRenderer.
// Handles CELL_IMG_SENTINEL-embedded images (estimated height).
// ===========================================================================
int16_t EpubRenderer::measureTableCell(const char* text,
                                        FontLevel   level,
                                        int16_t     colW) {
  selectFont(level);

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
    else                               cx += needed;
  };

  while (*p) {
    if (*p == CELL_IMG_SENTINEL) {
      measureWord();
      ++p;
      while (*p && *p != CELL_IMG_SENTINEL) ++p;
      if (*p) ++p;
      // Estimate inline image as one line tall
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
// renderTableCell — callback supplied to TableRenderer::endRow().
//
// Draws one cell's text content into the pixel rectangle
// (pixX, pixY) .. (pixX+colW, pixY+cellH).
//
// pixX, pixY : TOP-LEFT pixel of the content area (padding already removed).
// colW       : pixel width of the content area.
// cellH      : pixel height of the content area (used for vertical centring).
//
// _cy is temporarily set to the TEXT BASELINE of the first line, computed as:
//   pixY + fontAscent(level)
// so that renderWord() draws correctly without touching the outer _cy.
// ===========================================================================
void EpubRenderer::renderTableCell(const char* text, FontLevel level,
                                    int16_t pixX, int16_t pixY,
                                    int16_t colW,  int16_t /*cellH*/) {
  // ---- Save outer state ---------------------------------------------------
  int16_t savedCx    = _cx;
  int16_t savedCy    = _cy;
  int16_t savedLineH = _lineH;
  bool    savedLine  = _lineHasContent;
  int16_t savedWrap  = _wrapLeftMargin;
  int16_t savedRight = _rightBound;
  bool    savedFull  = _pageFull;

  // ---- Set cell-local state -----------------------------------------------
  // _cy is a TEXT BASELINE.  Position it so the TOP of the first glyph sits
  // exactly at pixY (top of the content area).
  int16_t cellAscent = fontAscent(level);
  _cx             = pixX;
  _cy             = pixY + cellAscent;    // baseline = top + ascent
  _lineH          = 0;
  _lineHasContent = false;
  _wrapLeftMargin = pixX;
  _rightBound     = pixX + colW;
  _pageFull       = false;
  _inCellRender   = true;   // suppress checkPageOverflow() inside the cell

  // ---- Walk cell text, handle CELL_IMG_SENTINEL-embedded image paths -------
  const char* p  = text;
  char        word[MAX_TEXT_LEN];
  int         wi = 0;
  bool        hadSpace = false;

  auto flushWord = [&]() {
    if (wi == 0) return;
    word[wi] = '\0'; wi = 0;
    char tmp[MAX_TEXT_LEN];
    if (hadSpace) {
      snprintf(tmp, sizeof(tmp), " %s", word);
      renderWord(tmp, level);
    } else {
      renderWord(word, level);
    }
    hadSpace = false;
  };

  while (*p) {
    if (*p == CELL_IMG_SENTINEL) {
      flushWord();
      ++p;
      char imgPath[MAX_PATH_LEN]; int pi = 0;
      while (*p && *p != CELL_IMG_SENTINEL && pi < MAX_PATH_LEN - 1)
        imgPath[pi++] = *p++;
      imgPath[pi] = '\0';
      if (*p == CELL_IMG_SENTINEL) ++p;

      // Measure then draw inline PNG, aligned to text baseline
      uint16_t imgW = 0, imgH = 0;
      if (decodePng(imgPath, 0, 0, imgW, imgH, true) && imgW > 0 && imgH > 0) {
        if (_lineHasContent) _cx += INLINE_IMG_SPACE;
        int16_t imgDestY = _cy - static_cast<int16_t>(imgH) + INLINE_IMG_DESCENT;
        decodePng(imgPath, _cx, imgDestY, imgW, imgH, false);
        _cx += static_cast<int16_t>(imgW) + INLINE_IMG_SPACE;
        int16_t imgAscent = static_cast<int16_t>(imgH) - INLINE_IMG_DESCENT;
        if (imgAscent > _lineH) _lineH = imgAscent;
        _lineHasContent = true;
      }
      hadSpace = false;
      continue;
    }
    char c = *p++;
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      flushWord();
      hadSpace = true;
    } else {
      if (wi < static_cast<int>(sizeof(word)) - 1) word[wi++] = c;
    }
  }
  flushWord();

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
    // The table uses TableRenderer which works in PURE PIXEL coordinates,
    // completely decoupled from the text-baseline (_cy) system.
    //
    // _cy is a text baseline; the table top pixel = _cy - _bodyFontAscent.
    // After the table we advance _cy by (tablePixelH + PARA_SPACING).
    // -------------------------------------------------------------------------
    case ELEM_TABLE_START: {
      if (_lineHasContent) newLine();
      _inTable = true;
      _tableCurCol = 0;
      // top pixel of table = top of current line slot
      int16_t topPixel = _cy - _bodyFontAscent;
      int16_t botPixel = DISPLAY_H - MARGIN_BOTTOM;
      _table.begin(topPixel, botPixel);
      break;
    }

    case ELEM_TABLE_HEADER_CELL:
    case ELEM_TABLE_DATA_CELL:
      if (_tableCurCol < MAX_TABLE_COLS) {
        strncpy(_tableCells[_tableCurCol].text, elem.text, MAX_TEXT_LEN - 1);
        _tableCells[_tableCurCol].text[MAX_TEXT_LEN - 1] = '\0';
        _tableCells[_tableCurCol].fontLevel = elem.fontLevel;
        ++_tableCurCol;
      }
      break;

    case ELEM_TABLE_ROW_END: {
      if (!_inTable || _tableCurCol == 0) break;

      // Build the row in the table engine
      _table.startRow();
      for (int i = 0; i < _tableCurCol; ++i)
        _table.addCell(_tableCells[i].text, _tableCells[i].fontLevel);

      // ---- Callbacks (thin lambdas → EpubRenderer private methods) --------
      auto hLineCb = [](int16_t x, int16_t y, int16_t w, void* ctx) {
        static_cast<EpubRenderer*>(ctx)->drawHLine(x, y, w);
      };
      auto vLineCb = [](int16_t x, int16_t y, int16_t h, void* ctx) {
        static_cast<EpubRenderer*>(ctx)->drawVLine(x, y, h);
      };
      auto measureCb = [](const char* txt, FontLevel lvl, int16_t colW, void* ctx) -> int16_t {
        return static_cast<EpubRenderer*>(ctx)->measureTableCell(txt, lvl, colW);
      };
      auto drawCb = [](const char* txt, FontLevel lvl,
                        int16_t px, int16_t py, int16_t colW, int16_t cellH,
                        void* ctx) {
        static_cast<EpubRenderer*>(ctx)->renderTableCell(txt, lvl, px, py, colW, cellH);
      };

      TableRenderer::RowResult r = _table.endRow(hLineCb, vLineCb, measureCb, drawCb, this);

      if (r == TableRenderer::ROW_PAGE_FULL) {
        // Page is full: advance _cy past page boundary, signal overflow.
        // The overflow mechanism in main.cpp will start a new page and
        // re-feed ELEM_TABLE_ROW_END; continueOnPage() is called from
        // ELEM_TABLE_START re-processing — but since we are MID-table we
        // cannot re-emit ELEM_TABLE_START.  Instead we signal page-full so
        // main.cpp calls beginPage() for us, then we call continueOnPage().
        signalPageFull();
        // Don't reset _tableCurCol — the row buffer must survive for retry.
      } else {
        _tableCurCol = 0;
        // Sync _cy: move it so the next text after the last drawn row starts
        // below the last table row.  _table.currentPixelY() is the pixel Y
        // of the NEXT top border (= bottom of last drawn row).
        // Set _cy = tableBottom + _bodyFontAscent (baseline of a body line
        // whose top sits exactly at tableBottom).
        _cy = _table.currentPixelY() + _bodyFontAscent;
      }
      break;
    }

    case ELEM_TABLE_END: {
      if (_inTable) {
        // Draw the final bottom border and get the pixel Y below it.
        auto hLineCb = [](int16_t x, int16_t y, int16_t w, void* ctx) {
          static_cast<EpubRenderer*>(ctx)->drawHLine(x, y, w);
        };
        int16_t endPixel = _table.finish(hLineCb, this);
        // Advance _cy to just below the table bottom border + dedicated gap.
        // Tune TABLE_AFTER_SPACING in epub_types.h if the gap needs adjusting.
        _cy      = endPixel + TABLE_AFTER_SPACING + _bodyFontAscent;
        _cx      = MARGIN_LEFT;
        _lineH   = 0;
        _lineHasContent = false;
        _tableCurCol    = 0;
        _inTable        = false;
      }
      break;
    }
  }
  return !_pageFull;
}

// ===========================================================================
// Portrait → native panel orientation
//
// Our bitmaps are laid out for setRotation(1) (480×800 portrait).
// writeImage() writes in native landscape (800×480) without GFX rotation.
//
// The mapping is: native pixel (nx, ny) = portrait pixel (ny, 799-nx).
// Byte-level transposition: for each dest byte at (ny, nx_byte), reads 8
// source bytes (all from row ny, each at a different column) and packs one
// bit from each into the destination byte.  ~8× fewer iterations than the
// original per-pixel loop.
// s_nativeBuf is allocated in internal SRAM (~48 KB) for fast CPU access.
// ===========================================================================
static uint8_t* s_nativeBuf = nullptr;

static const uint8_t* portraitToNative(const uint8_t* portrait) {
  if (!portrait) return nullptr;

  if (!s_nativeBuf) {
    s_nativeBuf = static_cast<uint8_t*>(
      heap_caps_aligned_alloc(64, FB_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_nativeBuf) {
      // Fallback to PSRAM if internal SRAM is full
      s_nativeBuf = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(64, FB_SIZE, MALLOC_CAP_SPIRAM));
    }
    if (!s_nativeBuf) {
      Serial.println("[Renderer] ERROR: Failed to allocate s_nativeBuf!");
      return nullptr;
    }
  }

  constexpr int16_t panelW      = GxEPD2_750_T7::WIDTH;   // 800
  constexpr int16_t panelH      = GxEPD2_750_T7::HEIGHT;  // 480
  constexpr int     panelStride = (panelW + 7) / 8;       // 100

  memset(s_nativeBuf, 0xFF, FB_SIZE);

  for (int16_t ny = 0; ny < panelH; ++ny) {
    uint8_t*      dstRow = s_nativeBuf + ny * panelStride;
    const int     bp     = ny & 7;            // bit position within source byte
    const uint8_t bit    = static_cast<uint8_t>(0x80u >> bp);  // mask for this bit

    for (int16_t nxByte = 0; nxByte < panelStride; ++nxByte) {
      uint8_t dst = 0;
      const int nxBase = nxByte << 3;
      for (int k = 0; k < 8; ++k) {
        const int16_t nx = nxBase + k;
        if (nx >= panelW) break;
        // Portrait source: px = ny, py = 799 - nx
        const int16_t py = panelW - 1 - nx;
        const uint8_t srcByte = portrait[py * FB_STRIDE + (ny >> 3)];
        if (srcByte & bit)
          dst |= static_cast<uint8_t>(0x80u >> k);
      }
      dstRow[nxByte] = dst;
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
  // Flush CPU D-cache → physical PSRAM before SPI reads it for the display transfer.
  Cache_WriteBack_Addr(reinterpret_cast<uint32_t>(native), FB_SIZE);
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
  // Flush CPU D-cache → physical PSRAM before SPI reads it for the display transfer.
  Cache_WriteBack_Addr(reinterpret_cast<uint32_t>(native), FB_SIZE);
  _disp.writeImage(native, 0, 0,
                   GxEPD2_750_T7::WIDTH, GxEPD2_750_T7::HEIGHT,
                   false, false, false);
  _disp.refresh(true);
  _disp.epd2.writeImageToPrevious(native, 0, 0,
                                   GxEPD2_750_T7::WIDTH, GxEPD2_750_T7::HEIGHT,
                                   false, false, false);
}
