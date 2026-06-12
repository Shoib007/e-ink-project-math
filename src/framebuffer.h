#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <atomic>
#include "epub_types.h"

// 1-bit per pixel framebuffer, MSB-first, 1=white 0=black.
// Dimensions fixed at DISPLAY_W x DISPLAY_H (480x800 portrait).

#define FB_STRIDE   ((DISPLAY_W + 7) / 8)   // bytes per row  = 60
#define FB_SIZE     (FB_STRIDE * DISPLAY_H)  // total bytes    = 48000

// ---------------------------------------------------------------------------
// Sliding window: we keep at most POOL_SLOTS page buffers alive at once.
//
//   slot 0  — previous page  (instant back-nav)
//   slot 1  — current page   (on screen right now)
//   slot 2  — next page      (pre-rendered, shows instantly on NEXT press)
//
// Each slot is allocated once from PSRAM and reused for the entire session.
// 3 × 48 000 B = 144 KB — tiny compared with the 8 MB available.
// ---------------------------------------------------------------------------
#define POOL_SLOTS  3

// ---------------------------------------------------------------------------
// Single framebuffer (one display-worth of pixels)
// ---------------------------------------------------------------------------
class Framebuffer {
public:
  // Allocate from PSRAM once; returns false on OOM.
  bool init() {
    if (_buf) return true;   // already allocated
    _buf = static_cast<uint8_t*>(
      heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM));
    if (!_buf) {
      Serial.printf("[FB] alloc failed (%d bytes)\n", FB_SIZE);
      return false;
    }
    clear();
    return true;
  }

  void clear() { if (_buf) memset(_buf, 0xFF, FB_SIZE); }  // all white

  inline void drawPixel(int16_t x, int16_t y, uint8_t color) {
    if (static_cast<uint16_t>(x) >= DISPLAY_W ||
        static_cast<uint16_t>(y) >= DISPLAY_H) return;
    uint8_t* b = _buf + static_cast<int>(y) * FB_STRIDE + (x >> 3);
    uint8_t  m = 0x80u >> (x & 7);
    if (color == 0) *b &= ~m; else *b |= m;
  }

  const uint8_t* buf()   const { return _buf; }
  bool           valid() const { return _buf != nullptr; }

private:
  uint8_t* _buf = nullptr;
};

// ---------------------------------------------------------------------------
// SlotPool — three fixed PSRAM slots used as a ring.
//
// Thread-safety model:
//   • Core 1 (renderTask) calls  setCurrent() / clearCurrent() / drawPixel()
//     only on the slot it was handed; it never touches the others.
//   • Core 0 (displayTask) calls pageBuf() on any slot to read pixels.
//   • No slot is simultaneously written by Core 1 and read by Core 0:
//     Core 0 guarantees the slot it is displaying is not the render target
//     (enforced by the "render ahead" semaphore in main.cpp).
// ---------------------------------------------------------------------------
class SlotPool {
public:
  // Allocate all POOL_SLOTS buffers upfront.  Must be called before
  // any rendering starts (i.e., from setup() on Core 0).
  bool init() {
    for (int i = 0; i < POOL_SLOTS; ++i)
      if (!_slots[i].init()) return false;
    return true;
  }

  // Select which slot the renderer writes into.
  void setCurrent(int slot) { _current = slot; }

  // Wipe the current render slot to white.
  void clearCurrent() {
    if (_current >= 0 && _current < POOL_SLOTS)
      _slots[_current].clear();
  }

  // Write a pixel into the current render slot (called from Core 1).
  inline void drawPixel(int16_t x, int16_t y, uint8_t color) {
    if (_current >= 0 && _current < POOL_SLOTS)
      _slots[_current].drawPixel(x, y, color);
  }

  // Read pixels from any slot (called from Core 0 to push to e-ink).
  const uint8_t* slotBuf(int slot) const {
    if (slot < 0 || slot >= POOL_SLOTS) return nullptr;
    return _slots[slot].buf();
  }

private:
  Framebuffer _slots[POOL_SLOTS];
  int         _current = -1;
};

// Global pool — shared between renderer (Core 1) and display push (Core 0)
extern SlotPool g_pool;
