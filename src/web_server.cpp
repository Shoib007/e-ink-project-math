// web_server.cpp — WiFi (STA with SoftAP fallback) upload server.
//
// Endpoints
//   GET  /                         -> serves data/index.html from LittleFS
//   POST /upload/folder            -> one file per request, named by
//                                     X-Book-Name / X-Rel-Path headers, raw body
//   POST /upload/zip               -> raw ZIP body buffered in PSRAM, then
//                                     extracted (auto-flattened) into the new book
//   POST|GET /upload/done          -> after all of a folder's files (or the zip)
//                                     succeeded; triggers the main loop to rescan
//
// The upload body is delivered by the ESP32 WebServer via its "raw" path
// (non-multipart), which calls the handler registered with onFileUpload()
// repeatedly with RAW_START / RAW_WRITE / RAW_END chunks.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "web_server.h"
#include "web_globals.h"
#include "web_config.h"
#include "miniz.h"

std::atomic<bool> g_reloadBooksRequested{false};

static WebServer g_server(WEB_SERVER_PORT);

// ---------------------------------------------------------------------------
// Upload state (single client; the WebServer library serialises requests)
// ---------------------------------------------------------------------------
enum UploadMode { UP_NONE, UP_FOLDER, UP_ZIP };

static struct {
  UploadMode mode     = UP_NONE;
  String     bookName;
  String     relPath;
  File       outFile;             // folder upload target
  uint8_t*   zipBuf   = nullptr;  // zip upload buffer (PSRAM)
  size_t     zipCap   = 0;
  size_t     zipLen   = 0;
  size_t     expectLen = 0;
  bool       failed   = false;
} g_ctx;

// ---------------------------------------------------------------------------
// Path hygiene
// ---------------------------------------------------------------------------
static String cleanBookName(const String& raw) {
  String n = raw;
  n.trim();
  for (unsigned i = 0; i < n.length(); i++) {
    char c = n[i];
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|' || c == '.')
      n.setCharAt(i, '_');
  }
  if (n.length() > 30) n = n.substring(0, 30);
  return n;
}

// Sanitise a slash-separated relative path.  Returns false on "..".
// Hidden / junk segments (__MACOSX, .DS_Store, dotfiles) are dropped.
static bool cleanPath(String raw, String& safe) {
  raw.replace('\\', '/');
  safe = "";
  int start = 0;
  for (;;) {
    int slash = raw.indexOf('/', start);
    String seg = (slash == -1) ? raw.substring(start) : raw.substring(start, slash);
    seg.trim();
    if (seg.length() > 0) {
      if (seg == "..") return false;
      if (seg.startsWith(".") || seg == "__MACOSX") continue;
      for (unsigned i = 0; i < seg.length(); i++) {
        char c = seg[i];
        if (c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
          seg.setCharAt(i, '_');
      }
      if (seg.length() > 60) seg = seg.substring(0, 60);
      if (seg.length() == 0) continue;
      if (safe.length() > 0) safe += "/";
      safe += seg;
    }
    if (slash == -1) break;
    start = slash + 1;
  }
  return true;
}

// Create every parent directory of `dir` (which must not end in '/').
static bool ensureDirs(String dir) {
  while (dir.length() > 0 && dir.endsWith("/")) dir = dir.substring(0, dir.length() - 1);
  if (dir.length() == 0) return false;

  String path;
  int i = dir.startsWith("/") ? 1 : 0;
  while (i < (int)dir.length()) {
    int j = dir.indexOf('/', i);
    if (j == -1) j = dir.length();
    String seg = dir.substring(i, j);
    i = j + 1;
    if (seg.length() == 0) continue;
    path += "/" + seg;
    if (path != dir) {
      if (!SD.exists(path.c_str())) {
        if (!SD.mkdir(path.c_str())) {
          Serial.printf("[Web] mkdir failed: %s\n", path.c_str());
          return false;
        }
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Raw-upload handler (called by WebServer for non-multipart POST bodies)
// ---------------------------------------------------------------------------
static void onUploadData() {
  // Multipart is not used by our page; abort cleanly instead of crashing.
  if (g_server.header("Content-Type").startsWith("multipart/")) {
    g_ctx.failed = true;
    return;
  }

  HTTPRaw& raw = g_server.raw();

  if (raw.status == RAW_START) {
    // Fresh request — reset shared state.
    if (g_ctx.mode == UP_ZIP && g_ctx.zipBuf) {
      heap_caps_free(g_ctx.zipBuf);
      g_ctx.zipBuf = nullptr;
    }
    g_ctx.zipCap = 0; g_ctx.zipLen = 0; g_ctx.expectLen = 0;
    if (g_ctx.outFile) { g_ctx.outFile.close(); }
    g_ctx.failed = false;

    const String& uri = g_server.uri();
    g_ctx.bookName = cleanBookName(g_server.header("X-Book-Name"));

    if (uri == "/upload/folder") {
      g_ctx.mode = UP_FOLDER;
      String safeRel;
      if (g_ctx.bookName.length() == 0 || !cleanPath(g_server.header("X-Rel-Path"), safeRel) || safeRel.length() == 0) {
        g_ctx.failed = true;
        return;
      }
      g_ctx.relPath = safeRel;
      String full   = String("/books/") + g_ctx.bookName + "/" + safeRel;
      String dir    = full.substring(0, full.lastIndexOf('/'));
      xSemaphoreTake(g_sdMutex, portMAX_DELAY);
      bool dirsOk = ensureDirs(dir);
      if (dirsOk) g_ctx.outFile = SD.open(full.c_str(), FILE_WRITE);
      xSemaphoreGive(g_sdMutex);
      if (!dirsOk || !g_ctx.outFile) {
        Serial.printf("[Web] folder: open failed: %s\n", full.c_str());
        g_ctx.failed = true;
      }
    } else if (uri == "/upload/zip") {
      g_ctx.mode = UP_ZIP;
      g_ctx.expectLen = (size_t)g_server.clientContentLength();
      size_t freeSpi = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      size_t cap     = (freeSpi > (512 * 1024)) ? (freeSpi - 512 * 1024) : 0;
      if (cap > MAX_ZIP_UPLOAD_BYTES) cap = MAX_ZIP_UPLOAD_BYTES;
      if (g_ctx.expectLen == 0 || g_ctx.expectLen > cap || g_ctx.bookName.length() == 0) {
        Serial.printf("[Web] zip: bad length %u cap %u\n", (unsigned)g_ctx.expectLen, (unsigned)cap);
        g_ctx.failed = true;
        return;
      }
      g_ctx.zipCap = g_ctx.expectLen;
      g_ctx.zipBuf = (uint8_t*)heap_caps_malloc(g_ctx.zipCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!g_ctx.zipBuf) {
        Serial.println("[Web] zip: PSRAM alloc failed");
        g_ctx.failed = true;
      }
    } else {
      g_ctx.mode = UP_NONE;  // /upload/done etc.
    }
    return;
  }

  if (raw.status == RAW_WRITE) {
    if (g_ctx.mode == UP_FOLDER && g_ctx.outFile && raw.currentSize > 0) {
      xSemaphoreTake(g_sdMutex, portMAX_DELAY);
      size_t w = g_ctx.outFile.write(raw.buf, raw.currentSize);
      xSemaphoreGive(g_sdMutex);
      if (w != raw.currentSize) {
        Serial.println("[Web] folder: short write");
        g_ctx.failed = true;
      }
    } else if (g_ctx.mode == UP_ZIP && g_ctx.zipBuf) {
      if (g_ctx.zipLen + raw.currentSize <= g_ctx.zipCap) {
        memcpy(g_ctx.zipBuf + g_ctx.zipLen, raw.buf, raw.currentSize);
        g_ctx.zipLen += raw.currentSize;
      } else {
        Serial.println("[Web] zip: buffer overflow");
        g_ctx.failed = true;
      }
    }
    return;
  }

  if (raw.status == RAW_END) {
    if (g_ctx.mode == UP_FOLDER && g_ctx.outFile) {
      xSemaphoreTake(g_sdMutex, portMAX_DELAY);
      g_ctx.outFile.close();
      xSemaphoreGive(g_sdMutex);
    }
    return;
  }

  if (raw.status == RAW_ABORTED) {
    if (g_ctx.outFile) g_ctx.outFile.close();
    g_ctx.failed = true;
  }
}

// ---------------------------------------------------------------------------
// ZIP extraction (miniz), with "auto-flatten": if every entry lives under a
// single top folder and nothing is at the archive root, strip that folder.
// ---------------------------------------------------------------------------
static size_t zipWriteCb(void* pOpaque, mz_uint64 /*ofs*/, const void* pBuf, size_t n) {
  File* f = static_cast<File*>(pOpaque);
  size_t w = 0;
  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  w = f->write(static_cast<const uint8_t*>(pBuf), n);
  xSemaphoreGive(g_sdMutex);
  return (w == n) ? n : 0;   // 0 abort
}

static bool extractZip(const String& bookName, const uint8_t* data, size_t size) {
  mz_zip_archive zip;
  mz_zip_zero_struct(&zip);
  if (!mz_zip_reader_init_mem(&zip, data, size, 0)) {
    Serial.printf("[Web] ZIP: init failed (err %d)\n", (int)mz_zip_peek_last_error(&zip));
    return false;
  }
  mz_uint n = mz_zip_reader_get_num_files(&zip);
  Serial.printf("[Web] ZIP: %u entries\n", (unsigned)n);

  // Decide whether to flatten a single top-level folder.
  String prefix;
  {
    String top;
    bool any = false, rootFile = false;
    for (mz_uint i = 0; i < n; i++) {
      if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
      char nm[512];
      mz_zip_reader_get_filename(&zip, i, nm, sizeof(nm));
      for (char* p = nm; *p; p++) if (*p == '\\') *p = '/';
      if (nm[0] == '/') memmove(nm, nm + 1, strlen(nm));
      const char* slash = strchr(nm, '/');
      if (!slash) { rootFile = true; }
      else {
        String seg = String(nm).substring(0, (int)(slash - nm));
        if (top.length() == 0) top = seg;
        else if (top != seg) rootFile = true;
      }
      any = true;
    }
    if (any && !rootFile && top.length() > 0) {
      prefix = top + "/";
      Serial.printf("[Web] ZIP: flattening top folder '%s'\n", prefix.c_str());
    }
  }

  bool ok = true;
  for (mz_uint i = 0; i < n && ok; i++) {
    if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
    if (!mz_zip_reader_is_file_supported(&zip, i)) {
      Serial.printf("[Web] ZIP: unsupported entry #%u, skipping\n", (unsigned)i);
      continue;
    }

    char nm[512];
    mz_zip_reader_get_filename(&zip, i, nm, sizeof(nm));
    for (char* p = nm; *p; p++) if (*p == '\\') *p = '/';
    if (nm[0] == '/') memmove(nm, nm + 1, strlen(nm));
    if (strstr(nm, "__MACOSX") || strstr(nm, ".DS_Store")) continue;

    String rel(nm);
    if (prefix.length() > 0 && rel.startsWith(prefix)) rel = rel.substring(prefix.length());
    String safe;
    if (!cleanPath(rel, safe) || safe.length() == 0) continue;

    String full = String("/books/") + bookName + "/" + safe;
    String dir  = full.substring(0, full.lastIndexOf('/'));

    xSemaphoreTake(g_sdMutex, portMAX_DELAY);
    bool dirsOk = ensureDirs(dir);
    File f = dirsOk ? SD.open(full.c_str(), FILE_WRITE) : File();
    xSemaphoreGive(g_sdMutex);
    if (!f) {
      Serial.printf("[Web] ZIP: cannot create %s\n", full.c_str());
      ok = false;
      break;
    }

    ok = mz_zip_reader_extract_to_callback(&zip, i, zipWriteCb, &f, 0);
    f.close();
    if (!ok) {
      Serial.printf("[Web] ZIP: extract failed for %s (err %d)\n", nm, (int)mz_zip_peek_last_error(&zip));
      SD.remove(full.c_str());
      break;
    }
  }

  mz_zip_reader_end(&zip);
  return ok;
}

// ---------------------------------------------------------------------------
// Handler functions
// ---------------------------------------------------------------------------
static void onRoot() {
  File f = LittleFS.open("/index.html", "r");
  if (!f) {
    g_server.send(200, "text/html",
      "<h1>Upload page missing</h1><p>Flash data/ with: pio run -t uploadfs</p>");
    return;
  }
  g_server.streamFile(f, "text/html");
  f.close();
}

static void onFolderDone() {
  if (g_ctx.failed) { g_server.send(500, "text/plain", "FAIL"); return; }
  g_server.send(200, "text/plain", "OK");
}

static void onZipDone() {
  bool ok = false;
  if (!g_ctx.failed && g_ctx.zipBuf && g_ctx.zipLen == g_ctx.expectLen && g_ctx.bookName.length() > 0) {
    Serial.printf("[Web] ZIP: extracting %u bytes into /books/%s\n",
                  (unsigned)g_ctx.zipLen, g_ctx.bookName.c_str());
    ok = extractZip(g_ctx.bookName, g_ctx.zipBuf, g_ctx.zipLen);
  }
  if (g_ctx.zipBuf) { heap_caps_free(g_ctx.zipBuf); g_ctx.zipBuf = nullptr; }
  g_ctx.zipLen = g_ctx.zipCap = g_ctx.expectLen = 0;
  if (ok) g_server.send(200, "text/plain", "OK");
  else    g_server.send(500, "text/plain", "FAIL");
}

static void onDone() {
  if (g_ctx.failed) { g_server.send(500, "text/plain", "FAIL"); return; }
  g_server.send(200, "text/plain", "OK");
  g_reloadBooksRequested.store(true, std::memory_order_relaxed);
  Serial.println("[Web] Upload complete - reload request set");
}

static void onNotFound() {
  g_server.send(404, "text/plain", "Not found");
}

// ---------------------------------------------------------------------------
// Web task
// ---------------------------------------------------------------------------
static void webTask(void* /*param*/) {
  Serial.printf("[Web] Task on core %d\n", xPortGetCoreID());

  bool sta = false;
  if (strlen(WIFI_SSID) > 0) {
    Serial.printf("[Web] Connecting STA '%s'...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    TickType_t t0 = xTaskGetTickCount();
    while (WiFi.status() != WL_CONNECTED &&
           (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(15000)) {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (WiFi.status() == WL_CONNECTED) {
      sta = true;
      Serial.printf("[Web] STA connected: http://%s\n", WiFi.localIP().toString().c_str());
      configTzTime("IST-5:30", "pool.ntp.org", "time.nist.gov");
      Serial.println("[Web] NTP sync started (Asia/Kolkata)");
    } else {
      Serial.println("[Web] STA failed - falling back to SoftAP");
      WiFi.disconnect(true);
    }
  } else {
    Serial.println("[Web] No STA SSID - using SoftAP");
  }

  if (!sta) {
    WiFi.mode(WIFI_AP_STA);
    bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD);
    if (apOk)
      Serial.printf("[Web] SoftAP '%s' -> http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
    else
      Serial.println("[Web] SoftAP failed to start");
  }

  if (!LittleFS.begin(true)) {
    Serial.println("[Web] ERROR: LittleFS mount failed");
  } else {
    Serial.println("[Web] LittleFS mounted");
  }

  // Upload handler must be registered BEFORE routes (captured at registration).
  g_server.onFileUpload(onUploadData);
  static const char* hdrs[] = {"Content-Type", "X-Book-Name", "X-Rel-Path"};
  g_server.collectHeaders(hdrs, 3);
  g_server.on("/",            HTTP_GET, onRoot);
  g_server.on("/upload/folder", HTTP_POST, onFolderDone);
  g_server.on("/upload/zip",    HTTP_POST, onZipDone);
  g_server.on("/upload/done",   HTTP_ANY,  onDone);
  g_server.onNotFound(onNotFound);
  g_server.begin();
  Serial.printf("[Web] Server listening on port %d\n", (int)WEB_SERVER_PORT);

  for (;;) {
    g_server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(3));
  }
}

void webSetup() {
  xTaskCreatePinnedToCore(webTask, "WebServer", 16384, nullptr, 1, nullptr, 0);
  Serial.println("[Web] Task created");
}
