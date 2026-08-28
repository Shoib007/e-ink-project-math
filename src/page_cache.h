#pragma once
// ---------------------------------------------------------------------------
// page_cache.h — SD-based page cache for the EPUB reader
//
// All rendered pages for a book are stored in a SINGLE contiguous file:
//   /cache/<book>/pages.dat
//
// Each page occupies exactly FB_SIZE bytes (60 × 800 = 48 000 bytes for a
// 480×800 display).  The byte layout matches the Framebuffer format:
//   MSB-first, 1 bit per pixel, 1 = white, 0 = black.
//   stride = (DISPLAY_W + 7) / 8 bytes per row.
//
// Page N is at byte offset N * FB_SIZE within pages.dat.
//
// A small metadata file /cache/<book>/meta.bin stores:
//   uint32_t magic       = CACHE_MAGIC
//   uint32_t page_count  = total number of cached pages
//   uint32_t source_size = byte size of the XHTML source files (staleness check)
//
// If meta.bin is missing or source_size differs from the actual XHTML file,
// the cache is considered stale and rebuilt from scratch.
//
// This single-file design reduces SD card file-system overhead by ~10×
// compared to the old one-file-per-page approach (fewer directory entries,
// fewer file create/open/close operations, contiguous allocation).
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <SD.h>
#include "framebuffer.h"   // for FB_SIZE, FB_STRIDE

#define CACHE_MAGIC        0x45504744u   // 'EPGD' — bumped to invalidate old per-file caches

struct CacheMeta {
  uint32_t magic;
  uint32_t pageCount;
  uint32_t sourceSize;   // sum of sizes of all XHTML files
};

// ---------------------------------------------------------------------------
// CacheWriter — called from renderTask (Core 1) to persist each page.
//
// Opens a single pages.dat file in begin(), appends each page sequentially,
// and writes meta.bin in finish().  Only 2 file open/close operations total
// (one for pages.dat, one for meta.bin) regardless of page count.
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

    // Open the single contiguous cache file for writing
    char datPath[256];
    snprintf(datPath, sizeof(datPath), "%s/pages.dat", _cacheDir);
    if (SD.exists(datPath)) SD.remove(datPath);
    _datFile = SD.open(datPath, FILE_WRITE);
    if (!_datFile) {
      Serial.printf("[Cache] Cannot create %s\n", datPath);
      return false;
    }
    _pageNum = 0;
    return true;
  }

  bool writePage(int /*idx*/, const uint8_t* buf) {
    if (!_datFile) return false;
    size_t written = _datFile.write(buf, FB_SIZE);
    _pageNum++;
    return written == FB_SIZE;
  }

  bool finish(uint32_t pageCount) {
    if (_datFile) {
      _datFile.close();
    }

    // Write metadata last — presence of meta.bin signals a complete cache
    char metaPath[256];
    snprintf(metaPath, sizeof(metaPath), "%s/meta.bin", _cacheDir);
    if (SD.exists(metaPath)) SD.remove(metaPath);
    File f = SD.open(metaPath, FILE_WRITE);
    if (!f) return false;
    CacheMeta m;
    m.magic      = CACHE_MAGIC;
    m.pageCount  = pageCount;
    m.sourceSize = _sourceSize;
    f.write(reinterpret_cast<const uint8_t*>(&m), sizeof(m));
    f.close();
    Serial.printf("[Cache] %u pages cached in single file.\n", pageCount);
    return true;
  }

private:
  char     _cacheDir[128];
  uint32_t _sourceSize = 0;
  uint32_t _pageNum    = 0;
  File     _datFile;
};

// ---------------------------------------------------------------------------
// CacheReader — called from displayTask (Core 0) to load a page on demand.
// Opens pages.dat once and seeks to the correct offset for each page.
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
  // Opens pages.dat, seeks to idx * FB_SIZE, reads one page, closes.
  // Returns true on success.
  static bool loadPage(const char* cacheDir, int idx, uint8_t* buf) {
    char datPath[256];
    snprintf(datPath, sizeof(datPath), "%s/pages.dat", cacheDir);
    File f = SD.open(datPath);
    if (!f) {
      Serial.printf("[Cache] Missing %s\n", datPath);
      return false;
    }

    if (!f.seek(static_cast<uint32_t>(idx) * FB_SIZE)) {
      Serial.printf("[Cache] Seek failed for page %d\n", idx);
      f.close();
      return false;
    }

    size_t got = f.read(buf, FB_SIZE);
    f.close();
    return got == FB_SIZE;
  }
};
