#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "epub_types.h"

// 1-bit per pixel framebuffer stored in PSRAM.
// Bit layout matches GxEPD2: MSB first, 1=white 0=black (same as Adafruit GFX).
// Dimensions fixed at DISPLAY_W x DISPLAY_H.

#define FB_STRIDE   ((DISPLAY_W + 7) / 8)   // bytes per row = 60
#define FB_SIZE     (FB_STRIDE * DISPLAY_H)  // total bytes  = 48000

class Framebuffer {
public:
  // Allocate in PSRAM. Returns false on failure.
  bool init() {
    if (_buf) return true;
    _buf = (uint8_t*)heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM);
    if (!_buf) {
      Serial.printf("FB: PSRAM alloc failed (%d bytes)\n", FB_SIZE);
      return false;
    }
    clear();
    return true;
  }

  void clear() {
    if (_buf) memset(_buf, 0xFF, FB_SIZE);  // 0xFF = all white
  }

  // Draw a single pixel. color: 0=black, 1=white (matches GxEPD_BLACK/WHITE)
  inline void drawPixel(int16_t x, int16_t y, uint8_t color) {
    if ((uint16_t)x >= DISPLAY_W || (uint16_t)y >= DISPLAY_H) return;
    uint8_t* byte = _buf + (int)y * FB_STRIDE + (x >> 3);
    uint8_t  bit  = 0x80 >> (x & 7);
    if (color == 0) *byte &= ~bit;   // black
    else            *byte |=  bit;   // white
  }

  // Raw buffer pointer — passed directly to GxEPD2 for transfer
  const uint8_t* buf() const { return _buf; }

private:
  uint8_t* _buf = nullptr;
};

// Global singleton — shared between renderer and display push
extern Framebuffer g_fb;
