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

// ESP32-S3 ROM function for flushing D-cache to physical PSRAM
extern "C" int Cache_WriteBack_Addr(uint32_t addr, uint32_t size);

#define CACHE_MAGIC        0x45504743u   // 'EPGC' — bump version to invalidate old cache

struct CacheMeta {
  uint32_t magic;
  uint32_t pageCount;
  uint32_t sourceSize;   // sum of sizes of all XHTML files
};

// Build the SD path for page index `idx` (e.g. index 3 → "/cache/jemh1a2/p0003.bin")
inline void cachePagePath(const char* cacheDir, int idx, char* out, int outLen) {
  snprintf(out, outLen, "%s/p%04d.bin", cacheDir, idx);
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
  bool begin(const char* cacheDir, uint32_t totalSourceSize) {
    strncpy(_cacheDir, cacheDir, sizeof(_cacheDir) - 1);
    _cacheDir[sizeof(_cacheDir) - 1] = '\0';
    _sourceSize = totalSourceSize;

    // Ensure /cache exists
    if (!SD.exists("/cache")) SD.mkdir("/cache");
    // Ensure /cache/bookname exists
    if (!SD.exists(_cacheDir)) SD.mkdir(_cacheDir);
    return true;
  }

  bool writePage(int idx, const uint8_t* buf) {
    char path[256];
    cachePagePath(_cacheDir, idx, path, sizeof(path));
    // Remove stale file if it exists
    if (SD.exists(path)) SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
      Serial.printf("[Cache] Cannot write %s\n", path);
      return false;
    }

    // Flush CPU D-cache to physical PSRAM before SD write (ESP32-S3 CRITICAL)
    // Without this, SD card may receive stale cached data instead of actual rendered pixels
    Cache_WriteBack_Addr(reinterpret_cast<uint32_t>(buf), FB_SIZE);

    size_t written = f.write(buf, FB_SIZE);
    f.close();
    return written == FB_SIZE;
  }

  bool finish(uint32_t pageCount) {
    char metaPath[256];
    snprintf(metaPath, sizeof(metaPath), "%s/meta.bin", _cacheDir);

    // Write metadata last — presence of meta.bin signals a complete cache
    if (SD.exists(metaPath)) SD.remove(metaPath);
    File f = SD.open(metaPath, FILE_WRITE);
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
  char     _cacheDir[128];
  uint32_t _sourceSize = 0;
};

// ---------------------------------------------------------------------------
// CacheReader — called from displayTask (Core 0) to load a page on demand.
// Reads directly from SD into a caller-supplied PSRAM buffer.
// ---------------------------------------------------------------------------
class CacheReader {
public:
  // Check whether a valid, up-to-date cache exists for this book.
  // Returns true and fills meta if valid; false otherwise.
  static bool probe(const char* cacheDir, uint32_t actualTotalSize, CacheMeta& meta) {
    char metaPath[256];
    snprintf(metaPath, sizeof(metaPath), "%s/meta.bin", cacheDir);

    File f = SD.open(metaPath);
    if (!f) return false;
    if (f.read(reinterpret_cast<uint8_t*>(&meta), sizeof(meta)) != sizeof(meta)) {
      f.close(); return false;
    }
    f.close();
    if (meta.magic != CACHE_MAGIC) return false;

    // Check source file size for staleness
    return actualTotalSize == meta.sourceSize;
  }

  // Load page `idx` into `buf` (must be FB_SIZE bytes).
  // Returns true on success.
  static bool loadPage(const char* cacheDir, int idx, uint8_t* buf) {
    char path[256];
    cachePagePath(cacheDir, idx, path, sizeof(path));
    File f = SD.open(path);
    if (!f) { Serial.printf("[Cache] Missing %s\n", path); return false; }
    
    size_t got = f.read(buf, FB_SIZE);
    f.close();
    return got == FB_SIZE;
  }
};
