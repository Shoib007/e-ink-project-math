#pragma once
// ---------------------------------------------------------------------------
// page_cache.h — SD-based page cache for the EPUB reader
//
// Each rendered page is stored as a raw 1-bpp bitmap on the SD card:
//   /cache/p0000.bin, /cache/p0001.bin, ...
//
// Each file is exactly FB_SIZE bytes (60 × 800 = 48 000 bytes for a 480×800
// display).  The byte layout matches the Framebuffer format:
//   MSB-first, 1 bit per pixel, 1 = white, 0 = black.
//   stride = (DISPLAY_W + 7) / 8 bytes per row.
//
// A small metadata file /cache/meta.bin stores:
//   uint32_t magic       = CACHE_MAGIC
//   uint32_t page_count  = total number of cached pages
//   uint32_t source_size = byte size of the XHTML source file (staleness check)
//
// If meta.bin is missing or source_size differs from the actual XHTML file,
// the cache is considered stale and rebuilt from scratch.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <SD.h>
#include "framebuffer.h"   // for FB_SIZE, FB_STRIDE

#define CACHE_DIR          "/cache"
#define CACHE_META_PATH    "/cache/meta.bin"
#define CACHE_MAGIC        0x45504742u   // 'EPGB'

// Source XHTML path — used for staleness check
#define XHTML_PATH         "/book3/EPUB/chap_01.xhtml"

struct CacheMeta {
  uint32_t magic;
  uint32_t pageCount;
  uint32_t sourceSize;   // size of XHTML file at render time
};

// Build the SD path for page index `idx`  (e.g. index 3 → "/cache/p0003.bin")
inline void cachePagePath(int idx, char* out, int outLen) {
  snprintf(out, outLen, "%s/p%04d.bin", CACHE_DIR, idx);
}

// ---------------------------------------------------------------------------
// CacheWriter — called from renderTask (Core 1) to persist each page.
// Usage:
//   CacheWriter w;
//   w.begin(xhtmlFileSize);
//   for each page:
//     w.writePage(pageIndex, pixelBuffer);  // pixelBuffer is FB_SIZE bytes
//   w.finish(totalPages);
// ---------------------------------------------------------------------------
class CacheWriter {
public:
  bool begin(uint32_t sourceSize) {
    _sourceSize = sourceSize;
    // Ensure /cache directory exists
    if (!SD.exists(CACHE_DIR)) SD.mkdir(CACHE_DIR);
    return true;
  }

  bool writePage(int idx, const uint8_t* buf) {
    char path[32];
    cachePagePath(idx, path, sizeof(path));
    // Remove stale file if it exists
    if (SD.exists(path)) SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
      Serial.printf("[Cache] Cannot write %s\n", path);
      return false;
    }
    size_t written = f.write(buf, FB_SIZE);
    f.close();
    return written == FB_SIZE;
  }

  bool finish(uint32_t pageCount) {
    // Write metadata last — presence of meta.bin signals a complete cache
    if (SD.exists(CACHE_META_PATH)) SD.remove(CACHE_META_PATH);
    File f = SD.open(CACHE_META_PATH, FILE_WRITE);
    if (!f) return false;
    CacheMeta m;
    m.magic      = CACHE_MAGIC;
    m.pageCount  = pageCount;
    m.sourceSize = _sourceSize;
    f.write(reinterpret_cast<const uint8_t*>(&m), sizeof(m));
    f.close();
    Serial.printf("[Cache] %u pages cached.\n", pageCount);
    return true;
  }

private:
  uint32_t _sourceSize = 0;
};

// ---------------------------------------------------------------------------
// CacheReader — called from displayTask (Core 0) to load a page on demand.
// Reads directly from SD into a caller-supplied PSRAM buffer.
// ---------------------------------------------------------------------------
class CacheReader {
public:
  // Check whether a valid, up-to-date cache exists.
  // Returns true and fills meta if valid; false otherwise.
  static bool probe(CacheMeta& meta) {
    File f = SD.open(CACHE_META_PATH);
    if (!f) return false;
    if (f.read(reinterpret_cast<uint8_t*>(&meta), sizeof(meta)) != sizeof(meta)) {
      f.close(); return false;
    }
    f.close();
    if (meta.magic != CACHE_MAGIC) return false;

    // Check source file size for staleness
    File src = SD.open(XHTML_PATH);
    if (!src) return false;
    uint32_t actualSize = src.size();
    src.close();
    return actualSize == meta.sourceSize;
  }

  // Load page `idx` into `buf` (must be FB_SIZE bytes).
  // Returns true on success.
  static bool loadPage(int idx, uint8_t* buf) {
    char path[32];
    cachePagePath(idx, path, sizeof(path));
    File f = SD.open(path);
    if (!f) { Serial.printf("[Cache] Missing %s\n", path); return false; }
    size_t got = f.read(buf, FB_SIZE);
    f.close();
    return got == FB_SIZE;
  }
};
