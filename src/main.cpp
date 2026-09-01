#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <time.h>

#include "epub_types.h"
#include "framebuffer.h"
#include "xhtml_parser.h"
#include "epub_renderer.h"
#include "page_cache.h"
#include "book_scanner.h"
#include "web_globals.h"
#include "web_server.h"

// ---------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------
#define EPD_CS     2
#define EPD_DC     3
#define EPD_RST    5
#define EPD_BUSY   6
#define SD_CS      4
#define BTN_PREV   44
#define BTN_NEXT   43
#define BTN_SELECT  1

// ---------------------------------------------------------------------------
// Display + renderer
// ---------------------------------------------------------------------------
GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 2> display(
  GxEPD2_750_T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);
EpubRenderer renderer(display);

// ---------------------------------------------------------------------------
// App States & Books
// ---------------------------------------------------------------------------
enum AppState { STATE_BOOK_SELECT, STATE_CACHE_BUILD, STATE_READ };
static AppState g_appState = STATE_BOOK_SELECT;

static BookInfo g_books[MAX_BOOKS];
static int g_numBooks = 0;
static int g_selectedBookIdx = 0;
static BookInfo* g_currentBook = nullptr;
static uint32_t g_totalSourceSize = 0;

// ---------------------------------------------------------------------------
// RTOS objects
// ---------------------------------------------------------------------------
enum NavCmd : uint8_t { NAV_NEXT = 0, NAV_PREV = 1 };

SemaphoreHandle_t g_sdMutex       = nullptr;
QueueHandle_t     g_navQueue      = nullptr;
SemaphoreHandle_t g_pageCachedSem = nullptr;  // Core 1 gives, Core 0 takes
TaskHandle_t      g_displayTaskHandle = nullptr;

// ---------------------------------------------------------------------------
// Slot ring (read mode)
// ---------------------------------------------------------------------------
enum Role { PREV = 0, CUR = 1, NEXT = 2 };
static int  g_role[3]      = {0, 1, 2};
static int  g_slotPage[3]  = {-1, -1, -1};
static bool g_slotValid[3] = {false, false, false};

static int  g_totalPages   = 0;

static void formatHeaderDateTime(char* out, size_t outSize) {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    snprintf(out, outSize, "Time unavailable");
    return;
  }
  struct tm localTime;
  localtime_r(&now, &localTime);
  strftime(out, outSize, "%d %b %Y  %I:%M %p", &localTime);
}

// Manual full refresh trigger
static std::atomic<bool> g_forceFullRefresh{false};

// ---------------------------------------------------------------------------
// Cache-build state (Core 1 render task)
// ---------------------------------------------------------------------------
static std::atomic<int>  g_pagesBuilt{0};
static std::atomic<bool> g_buildDone{false};

struct OverflowElem { bool valid = false; RenderElem elem; };
static OverflowElem g_overflow;

static bool onElement(const RenderElem& elem, void* /*ctx*/) {
  bool ok = renderer.feed(elem);
  if (!ok) {
    g_overflow.valid = true;
    g_overflow.elem  = elem;
    return false;
  }
  return true;
}

static void cacheBuildTask(void* /*param*/) {
  Serial.printf("[Core1] Cache build task on core %d\n", xPortGetCoreID());

  CacheWriter writer;
  writer.begin(g_currentBook->cacheDir, g_totalSourceSize);

  int slotIdx = 0;
  int pageNum = 0;

  for (int i = 0; i < g_currentBook->xhtmlCount; i++) {
    char xhtmlPath[128];
    snprintf(xhtmlPath, sizeof(xhtmlPath), "%s%s", g_currentBook->basePath, g_currentBook->xhtmlFiles[i]);
    
    Serial.printf("[Core1] Parsing %s\n", xhtmlPath);
    XhtmlParser parser;
    bool more = true;
    bool first = true;
    bool parserDone = false;

    while (!parserDone) {
      renderer.beginPage(slotIdx);

      if (g_overflow.valid) {
        g_overflow.valid = false;
        if (!renderer.feed(g_overflow.elem))
          Serial.println("[Core1] WARN: element too large, dropped.");
      }

      if (first) {
        more = parser.parse(xhtmlPath, g_currentBook->basePath, onElement, nullptr);
        first = false;
      } else {
        more = parser.resumeParse(onElement, nullptr);
      }

      if (!more && !g_overflow.valid) {
        parserDone = true;
      }

      renderer.endPage();

      // ---- DIAG (temporary): per-page heap / stack / integrity check ----
      Serial.printf("[C1] p%u free=%u swh=%u\n", pageNum,
                    ESP.getFreeHeap(), uxTaskGetStackHighWaterMark(nullptr));
      heap_caps_check_integrity_all(true);
      // -------------------------------------------------------------------

      xSemaphoreTake(g_sdMutex, portMAX_DELAY);
      writer.writePage(pageNum, g_pool.slotBuf(slotIdx));
      xSemaphoreGive(g_sdMutex);

      pageNum++;
      g_pagesBuilt.store(pageNum, std::memory_order_release);
      xSemaphoreGive(g_pageCachedSem);
    }
    parser.close();
  }

  writer.finish(pageNum);
  g_totalPages = pageNum;
  g_buildDone.store(true, std::memory_order_release);
  xSemaphoreGive(g_pageCachedSem);
  Serial.printf("[Core1] Cache complete: %d pages.\n", pageNum);
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Slot helpers (read mode)
// ---------------------------------------------------------------------------
static bool loadSlot(int slot, int pageIdx) {
  if (pageIdx < 0 || pageIdx >= g_totalPages) return false;
  uint8_t* buf = const_cast<uint8_t*>(g_pool.slotBuf(slot));
  if (!buf) return false;
  bool ok = CacheReader::loadPage(g_currentBook->cacheDir, pageIdx, buf);
  if (ok) {
    g_slotPage[slot]  = pageIdx;
    g_slotValid[slot] = true;
  }
  return ok;
}

#define PARTIAL_REFRESH_MAX 5  // Do full refresh every 5 pages to clear ghosting
static bool g_firstShow     = true;
static int  g_partialCount  = 0;

static void showSlot(int slot) {
  if (!g_pool.slotBuf(slot)) return;
  char dateTime[32];
  formatHeaderDateTime(dateTime, sizeof(dateTime));
  int pageNumber = g_slotPage[slot] + 1;
  const char* bookName = g_currentBook ? g_currentBook->name : "";
  
  // Force full refresh if requested (e.g., user held SELECT button)
  bool forceFullRequested = g_forceFullRefresh.load(std::memory_order_relaxed);
  if (forceFullRequested) {
    g_forceFullRefresh.store(false, std::memory_order_relaxed);
    g_partialCount = 0;
    renderer.showPageFull(slot, bookName, dateTime, pageNumber, g_totalPages);
    Serial.println("[Display] Manual full refresh triggered.");
    return;
  }
  
  bool doFull = g_firstShow || (g_partialCount >= PARTIAL_REFRESH_MAX);
  if (doFull) {
    g_firstShow    = false;
    g_partialCount = 0;
    renderer.showPageFull(slot, bookName, dateTime, pageNumber, g_totalPages);
  } else {
    ++g_partialCount;
    renderer.showPagePartial(slot, bookName, dateTime, pageNumber, g_totalPages);
  }
}

static void rotateFwd() {
  int freed    = g_role[PREV];
  g_role[PREV] = g_role[CUR];
  g_role[CUR]  = g_role[NEXT];
  g_role[NEXT] = freed;
  g_slotValid[freed] = false;
  g_slotPage[freed]  = -1;
}

static void rotateBwd() {
  int stale    = g_role[NEXT];
  g_role[NEXT] = g_role[CUR];
  g_role[CUR]  = g_role[PREV];
  g_role[PREV] = stale;
}

// ---------------------------------------------------------------------------
// Progress screen (cache build only) — drawn by Core 0
// ---------------------------------------------------------------------------
static void showProgress(int built, int /*total*/) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(nullptr);
    display.setTextColor(GxEPD_BLACK);
    char line1[40], line2[40];
    snprintf(line1, sizeof(line1), "Building cache...");
    snprintf(line2, sizeof(line2), "Page %d rendered", built);
    display.setCursor(10, display.height() / 2 - 20);
    display.print(line1);
    display.setCursor(10, display.height() / 2 + 4);
    display.print(line2);
  } while (display.nextPage());
}

static void runCacheBuildMonitor() {
  int lastShown = -1;
  while (true) {
    xSemaphoreTake(g_pageCachedSem, portMAX_DELAY);
    if (g_buildDone.load(std::memory_order_acquire)) break;
    int built = g_pagesBuilt.load(std::memory_order_acquire);
    if (built != lastShown) {
      lastShown = built;
      xSemaphoreTake(g_sdMutex, portMAX_DELAY);
      showProgress(built, 0);
      xSemaphoreGive(g_sdMutex);
    }
  }
  while (xSemaphoreTake(g_pageCachedSem, 0) == pdTRUE) {}
  Serial.println("[Core0] Cache build complete.");
}

// ---------------------------------------------------------------------------
// displayTask — Core 0, read mode
// ---------------------------------------------------------------------------
static void displayTask(void* /*param*/) {
  g_firstShow = true;
  g_partialCount = 0;

  g_role[PREV] = 0; g_role[CUR] = 1; g_role[NEXT] = 2;
  for(int i=0; i<3; i++) { g_slotPage[i] = -1; g_slotValid[i] = false; }

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  loadSlot(g_role[CUR],  0);
  if (g_totalPages > 1) loadSlot(g_role[NEXT], 1);
  xSemaphoreGive(g_sdMutex);

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  showSlot(g_role[CUR]);
  xSemaphoreGive(g_sdMutex);
  Serial.printf("[Display] Page 1 shown (full refresh).\n");

  NavCmd cmd;
  for (;;) {
    if (xQueueReceive(g_navQueue, &cmd, portMAX_DELAY) != pdTRUE) continue;

    if (cmd == NAV_NEXT) {
      int curPage = g_slotPage[g_role[CUR]];
      if (curPage >= g_totalPages - 1) continue;

      bool nextReady = g_slotValid[g_role[NEXT]] && (g_slotPage[g_role[NEXT]] == curPage + 1);
      if (!nextReady) {
        xSemaphoreTake(g_sdMutex, portMAX_DELAY);
        loadSlot(g_role[NEXT], curPage + 1);
        xSemaphoreGive(g_sdMutex);
      }

      // Show the page FIRST (user sees it immediately)
      xSemaphoreTake(g_sdMutex, portMAX_DELAY);
      showSlot(g_role[NEXT]);
      xSemaphoreGive(g_sdMutex);

      rotateFwd();

      // THEN pre-fetch the page TWO steps ahead in background.
      // This prediction ensures the next NAV_NEXT is instant if the user
      // pages forward sequentially (99% of use cases).
      int prefetchPage = g_slotPage[g_role[CUR]] + 1;
      if (prefetchPage < g_totalPages && prefetchPage != g_slotPage[g_role[NEXT]]) {
        xSemaphoreTake(g_sdMutex, portMAX_DELAY);
        loadSlot(g_role[NEXT], prefetchPage);
        xSemaphoreGive(g_sdMutex);
      }
    } else {
      int curPage = g_slotPage[g_role[CUR]];
      if (curPage <= 0) continue;

      bool prevReady = g_slotValid[g_role[PREV]] && (g_slotPage[g_role[PREV]] == curPage - 1);
      if (!prevReady) {
        xSemaphoreTake(g_sdMutex, portMAX_DELAY);
        loadSlot(g_role[PREV], curPage - 1);
        xSemaphoreGive(g_sdMutex);
      }

      xSemaphoreTake(g_sdMutex, portMAX_DELAY);
      showSlot(g_role[PREV]);
      xSemaphoreGive(g_sdMutex);

      rotateBwd();

      // Show the page first, then pre-fetch two steps back
      g_slotValid[g_role[PREV]] = false;
      g_slotPage [g_role[PREV]] = -1;

      int prefetchPage = g_slotPage[g_role[CUR]] - 1;
      if (prefetchPage >= 0 && prefetchPage != g_slotPage[g_role[PREV]]) {
        xSemaphoreTake(g_sdMutex, portMAX_DELAY);
        loadSlot(g_role[PREV], prefetchPage);
        xSemaphoreGive(g_sdMutex);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Menu rendering
// ---------------------------------------------------------------------------
#define MENU_PARTIAL_REFRESH_MAX 5  // Full refresh after 5 partial menu updates
static int  g_menuPartialCount = 0;

static void showMenu(bool partial = false) {
  xSemaphoreTake(g_sdMutex, portMAX_DELAY);

  // Boot menu and every boundary use full refresh; navigations use partial
  // (GxEPD2 paged partial = full-screen fast partial for GD7965 panels).
  bool doFull = !partial || (g_menuPartialCount >= MENU_PARTIAL_REFRESH_MAX);
  if (doFull) {
    display.setFullWindow();
    g_menuPartialCount = 0;
  } else {
    display.setPartialWindow(0, 0, display.width(), display.height());
    ++g_menuPartialCount;
  }

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(nullptr);
    display.setCursor(10, 20);
    display.print("Select Book:");

    for (int i=0; i<g_numBooks; i++) {
      display.setCursor(20, 60 + i*30);
      if (i == g_selectedBookIdx) {
        display.print("> ");
      } else {
        display.print("  ");
      }
      display.print(g_books[i].name);
    }
  } while (display.nextPage());
  xSemaphoreGive(g_sdMutex);
}

// ---------------------------------------------------------------------------
// Status screen
// ---------------------------------------------------------------------------
static void showStatus(const char* l1, const char* l2 = nullptr) {
  if(g_sdMutex) xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(nullptr);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(10, display.height() / 2 - 10);
    display.print(l1);
    if (l2) { display.setCursor(10, display.height() / 2 + 10); display.print(l2); }
  } while (display.nextPage());
  if(g_sdMutex) xSemaphoreGive(g_sdMutex);
}

// ---------------------------------------------------------------------------
// Start Book Process
// ---------------------------------------------------------------------------
static void startBook(int idx) {
  g_currentBook = &g_books[idx];
  
  uint32_t totalSize = 0;
  for (int i=0; i<g_currentBook->xhtmlCount; i++) {
    char path[128];
    snprintf(path, sizeof(path), "%s%s", g_currentBook->basePath, g_currentBook->xhtmlFiles[i]);
    File f = SD.open(path);
    if (f) {
      totalSize += f.size();
      f.close();
    }
  }
  g_totalSourceSize = totalSize;

  CacheMeta meta;
  bool cacheValid = CacheReader::probe(g_currentBook->cacheDir, g_totalSourceSize, meta);

  if (!cacheValid) {
    g_appState = STATE_CACHE_BUILD;
    Serial.println("Cache stale/missing. Building...");
    showStatus("Building cache...", g_currentBook->name);

    g_pagesBuilt.store(0);
    g_buildDone.store(false);

    xTaskCreatePinnedToCore(cacheBuildTask, "CacheBuild", 8192, nullptr, 1, nullptr, 1);
    runCacheBuildMonitor();
    
    CacheReader::probe(g_currentBook->cacheDir, g_totalSourceSize, meta);
  }

  g_totalPages = static_cast<int>(meta.pageCount);
  Serial.printf("Cache ready: %d pages.\n", g_totalPages);

  if (g_totalPages <= 0) {
    showStatus("No pages found!");
    g_appState = STATE_BOOK_SELECT;
    return;
  }

  g_appState = STATE_READ;
  showStatus("Loading...", "Please wait");

  xTaskCreatePinnedToCore(displayTask, "Display", 8192, &g_displayTaskHandle, 2, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Buttons & loop
// ---------------------------------------------------------------------------
static std::atomic<bool> g_btnSelectPending{false};
static std::atomic<int>  g_btnNextCount{0};
static std::atomic<int>  g_btnPrevCount{0};

static volatile int64_t  g_btnNextTime = 0;
static volatile int64_t  g_btnPrevTime = 0;
static volatile int64_t  g_btnSelectTime = 0;
static volatile int64_t  g_btnSelectPressStart = 0;  // Track when SELECT was first pressed
#define DEBOUNCE_US 50000LL
#define LONG_PRESS_US 1000000LL  // 1 second for long press

void IRAM_ATTR onBtnNext() {
  int64_t now = esp_timer_get_time();
  if (now - g_btnNextTime >= DEBOUNCE_US) {
    g_btnNextTime = now;
    g_btnNextCount.fetch_add(1, std::memory_order_relaxed);
  }
}
void IRAM_ATTR onBtnPrev() {
  int64_t now = esp_timer_get_time();
  if (now - g_btnPrevTime >= DEBOUNCE_US) {
    g_btnPrevTime = now;
    g_btnPrevCount.fetch_add(1, std::memory_order_relaxed);
  }
}
void IRAM_ATTR onBtnSelect() {
  int64_t now = esp_timer_get_time();
  if (now - g_btnSelectTime >= DEBOUNCE_US) {
    g_btnSelectTime = now;
    g_btnSelectPressStart = now;  // Track when button was first pressed
    g_btnSelectPending.store(true, std::memory_order_relaxed);
  }
}

void loop() {
  // --- Serial command handler (e.g. !CLEARCACHE from Python tool) ---
  if (Serial.available()) {
    char cmd[32];
    int len = Serial.readBytesUntil('\n', cmd, sizeof(cmd) - 1);
    cmd[len] = '\0';
    if (strncmp(cmd, "!CLEARCACHE", 11) == 0) {
      Serial.println("[Cache] Deleting cache...");
      xSemaphoreTake(g_sdMutex, portMAX_DELAY);
      // Delete meta.bin for the current book (forces rebuild on next boot)
      if (g_currentBook) {
        char metaPath[128];
        snprintf(metaPath, sizeof(metaPath), "%s/meta.bin", g_currentBook->cacheDir);
        if (SD.exists(metaPath)) {
          SD.remove(metaPath);
          Serial.printf("[Cache] Deleted %s\n", metaPath);
        }
        char datPath[128];
        snprintf(datPath, sizeof(datPath), "%s/pages.dat", g_currentBook->cacheDir);
        if (SD.exists(datPath)) {
          SD.remove(datPath);
          Serial.printf("[Cache] Deleted %s\n", datPath);
        }
      } else {
        Serial.println("[Cache] No book loaded, scanning all cache dirs...");
        File root = SD.open("/cache");
        if (root && root.isDirectory()) {
          File entry;
          while ((entry = root.openNextFile())) {
            if (entry.isDirectory()) {
              char pathBuf[128];
              const char* name = entry.name();
              const char* base = strrchr(name, '/');
              base = (base && *(base + 1)) ? base + 1 : name;
              snprintf(pathBuf, sizeof(pathBuf), "/cache/%s/meta.bin", base);
              if (SD.exists(pathBuf)) SD.remove(pathBuf);
              snprintf(pathBuf, sizeof(pathBuf), "/cache/%s/pages.dat", base);
              if (SD.exists(pathBuf)) SD.remove(pathBuf);
            }
            entry.close();
          }
        }
        Serial.println("[Cache] All cache cleared.");
      }
      xSemaphoreGive(g_sdMutex);
      Serial.println("[Cache] Restarting...");
      delay(500);
      ESP.restart();
    }
  }

  // --- Web upload finished: rescan /books/ and redraw the menu ---
  if (g_reloadBooksRequested.load(std::memory_order_relaxed) &&
      g_appState == STATE_BOOK_SELECT) {
    g_reloadBooksRequested.store(false, std::memory_order_relaxed);
    Serial.println("[Menu] Rescanning books after upload...");
    int n = 0;
    xSemaphoreTake(g_sdMutex, portMAX_DELAY);
    n = BookScanner::scan(g_books, MAX_BOOKS);
    xSemaphoreGive(g_sdMutex);
    g_numBooks = n;
    if (g_numBooks > 0) {
      if (g_selectedBookIdx >= g_numBooks)
        g_selectedBookIdx = g_numBooks - 1;
      showMenu();   // full refresh so the new book list is drawn crisply
    } else {
      Serial.println("[Menu] Warning: no books found after upload.");
    }
  }

  // NEXT: consume every click since last drain. In the menu, coalesce them
  // into a single jump so rapid taps land on the Nth book in one refresh.
  int nextClicks = g_btnNextCount.exchange(0, std::memory_order_relaxed);
  if (nextClicks > 0) {
    if (g_appState == STATE_BOOK_SELECT) {
      if (g_numBooks > 0) {
        g_selectedBookIdx = (g_selectedBookIdx + nextClicks) % g_numBooks;
        showMenu(true);
      }
    } else if (g_appState == STATE_READ) {
      NavCmd c = NAV_NEXT;
      xQueueSend(g_navQueue, &c, 0);
    }
  }

  // PREV button
  int prevClicks = g_btnPrevCount.exchange(0, std::memory_order_relaxed);
  if (prevClicks > 0) {
    if (g_appState == STATE_BOOK_SELECT) {
      if (g_numBooks > 0) {
        g_selectedBookIdx = (g_selectedBookIdx - prevClicks + g_numBooks) % g_numBooks;
        showMenu(true);
      }
    } else if (g_appState == STATE_READ) {
      NavCmd c = NAV_PREV;
      xQueueSend(g_navQueue, &c, 0);
    }
  }

  // Handle SELECT button with long press detection
  if (g_btnSelectPending.load(std::memory_order_relaxed)) {
    // Wait a moment to check if it's a long press
    delay(50);  // Debounce
    
    if (digitalRead(BTN_SELECT) == LOW) {
      // Button still pressed - check duration
      int64_t pressStart = g_btnSelectPressStart;
      delay(1000);  // Wait for potential long press
      
      if (digitalRead(BTN_SELECT) == LOW) {
        // LONG PRESS (held for 1+ second)
        g_btnSelectPending.store(false, std::memory_order_relaxed);
        
        if (g_appState == STATE_READ) {
          Serial.println("[Button] SELECT long press - forcing full refresh");
          g_forceFullRefresh.store(true, std::memory_order_relaxed);
          
          // Re-display current page with full refresh
          xSemaphoreTake(g_sdMutex, portMAX_DELAY);
          showSlot(g_role[CUR]);
          xSemaphoreGive(g_sdMutex);
        }
        
        // Wait for button release
        while (digitalRead(BTN_SELECT) == LOW) delay(10);
      } else {
        // SHORT PRESS (released within 1 second)
        g_btnSelectPending.store(false, std::memory_order_relaxed);
        
        if (g_appState == STATE_BOOK_SELECT) {
          startBook(g_selectedBookIdx);
        } else if (g_appState == STATE_READ) {
          // Return to menu
          if (g_displayTaskHandle) {
            vTaskDelete(g_displayTaskHandle);
            g_displayTaskHandle = nullptr;
          }
          g_appState = STATE_BOOK_SELECT;
          showMenu(true);
        }
      }
    } else {
      // Button already released
      g_btnSelectPending.store(false, std::memory_order_relaxed);
    }
  }

  vTaskDelay(pdMS_TO_TICKS(5));
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== EPUB Reader (Core %d) ===\n", xPortGetCoreID());

  pinMode(BTN_NEXT,   INPUT_PULLUP);
  pinMode(BTN_PREV,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_NEXT), onBtnNext, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_PREV), onBtnPrev, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_SELECT), onBtnSelect, FALLING);

  display.init(115200);
  display.setRotation(1);

  if (!SD.begin(SD_CS)) {
    showStatus("SD card not found!");
    Serial.println("ERROR: SD init failed");
    return;
  }
  Serial.println("SD OK");

  if (!g_pool.init()) {
    showStatus("PSRAM alloc failed!");
    Serial.println("ERROR: PSRAM alloc");
    return;
  }

  g_sdMutex       = xSemaphoreCreateMutex();
  g_navQueue      = xQueueCreate(4, sizeof(NavCmd));
  g_pageCachedSem = xSemaphoreCreateCounting(256, 0);
  if (!g_sdMutex || !g_navQueue || !g_pageCachedSem) {
    Serial.println("ERROR: RTOS objects");
    return;
  }
  renderer.setSdMutex(g_sdMutex);

  // Start the upload web server (WiFi STA, SoftAP fallback) on its own task.
  webSetup();

  // Discover books
  g_numBooks = BookScanner::scan(g_books, MAX_BOOKS);
  if (g_numBooks <= 0) {
    showStatus("No books found in /books/");
    return;
  }

  g_appState = STATE_BOOK_SELECT;
  g_selectedBookIdx = 0;
  showMenu();
}
