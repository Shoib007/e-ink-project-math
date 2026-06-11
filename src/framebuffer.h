#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "epub_types.h"

// 1-bit per pixel framebuffer, MSB-first, 1=white 0=black.
// Dimensions fixed at DISPLAY_W x DISPLAY_H (480x800 portrait).

#define FB_STRIDE   ((DISPLAY_W + 7) / 8)   // bytes per row  = 60
#define FB_SIZE     (FB_STRIDE * DISPLAY_H)  // total bytes    = 48000

// Maximum number of pre-rendered page buffers kept in PSRAM.
// 48000 * 64 = ~3MB — fits in 8MB PSRAM.
#define MAX_PAGE_BUFFERS 64

// -----------------------------------------------------------------------
// Single framebuffer (one page worth of pixels)
// -----------------------------------------------------------------------
class Framebuffer {
public:
  bool init() {
    if (_buf) return true;
    _buf = (uint8_t*)heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM);
    if (!_buf) { Serial.printf("FB alloc failed (%d bytes)\n", FB_SIZE); return false; }
    clear();
    return true;
  }

  void clear() { if (_buf) memset(_buf, 0xFF, FB_SIZE); }  // all white

  inline void drawPixel(int16_t x, int16_t y, uint8_t color) {
    if ((uint16_t)x >= DISPLAY_W || (uint16_t)y >= DISPLAY_H) return;
    uint8_t* b = _buf + (int)y * FB_STRIDE + (x >> 3);
    uint8_t  m = 0x80 >> (x & 7);
    if (color == 0) *b &= ~m; else *b |= m;
  }

  const uint8_t* buf() const { return _buf; }
  bool valid() const { return _buf != nullptr; }

private:
  uint8_t* _buf = nullptr;
};

// -----------------------------------------------------------------------
// PageBufferPool — allocates one Framebuffer per page in PSRAM.
// The active render target is set via setCurrent().
// After all pages are rendered, showPage(n) copies page n to the display.
// -----------------------------------------------------------------------
class PageBufferPool {
public:
  // Allocate a new page buffer. Returns page index or -1 on failure.
  int allocPage() {
    if (_count >= MAX_PAGE_BUFFERS) return -1;
    if (!_pages[_count].init()) return -1;
    return _count++;
  }

  // Set the page being actively rendered into
  bool setCurrent(int index) {
    if (index < 0 || index >= _count) return false;
    _current = index;
    return true;
  }

  // drawPixel into the current page buffer
  inline void drawPixel(int16_t x, int16_t y, uint8_t color) {
    if (_current >= 0) _pages[_current].drawPixel(x, y, color);
  }

  // Clear the current page to white
  void clearCurrent() {
    if (_current >= 0) _pages[_current].clear();
  }

  const uint8_t* pageBuf(int index) const {
    if (index < 0 || index >= _count) return nullptr;
    return _pages[index].buf();
  }

  int pageCount() const { return _count; }

private:
  Framebuffer _pages[MAX_PAGE_BUFFERS];
  int         _count   = 0;
  int         _current = -1;
};

// Global pool — shared between renderer and display push
extern PageBufferPool g_pool;

// Single working framebuffer reused for layout-time size probing
extern Framebuffer g_fb;
