/*
 * main.cpp — EPUB Reader  (XIAO ESP32-S3 + GxEPD2 7.5" e-ink)
 * =============================================================
 *
 * BOOT FLOW
 * ---------
 *  1. Check SD for a valid page cache (/cache/p0000.bin … meta.bin).
 *     If valid → jump straight to READ MODE.
 *     If stale / missing → enter CACHE BUILD MODE.
 *
 *  CACHE BUILD MODE (one-time, ~same wall-clock as before but only once)
 *  -----------------------------------------------------------------------
 *  Core 0:  Shows "Building cache N/M…" progress screen with a full refresh
 *           after the first page and partial refreshes for the rest.
 *  Core 1:  Streams XHTML → renders pages one by one → writes each raw
 *           48 KB bitmap to SD as /cache/pNNNN.bin.
 *           Signals Core 0 after each page via g_pageCachedSem.
 *           When done writes /cache/meta.bin (marks cache complete).
 *
 *  READ MODE  (every subsequent boot, or after cache build completes)
 *  -----------------------------------------------------------------------
 *  3 PSRAM slots (prev / current / next) of 48 KB each.
 *  Displaying a page = SD.read(48 KB) + writeImage() + refresh(partial).
 *    Total:  ~200 ms SD read  +  ~1 600 ms partial waveform  =  ~1.8 s
 *    vs old: ~200 ms copy loop  +  ~3 700 ms full waveform   =  ~3.9 s
 *
 *  PARTIAL REFRESH CORRECTNESS
 *  -----------------------------------------------------------------------
 *  The GD7965 controller uses a differential waveform that compares its
 *  "previous" and "new" frame buffers to decide which pixels to drive.
 *  We must keep them in sync:
 *    - First page after power-on: showPageFull()  → writes BOTH buffers.
 *    - All later pages:           showPagePartial() → writes new, copies
 *                                 same data to previous so next diff is clean.
 *
 *  SLIDING WINDOW
 *  -----------------------------------------------------------------------
 *  g_role[PREV/CUR/NEXT] maps logical role → physical slot (0-2).
 *  On NEXT: show slot[NEXT], rotate roles, load new NEXT from SD cache.
 *  On PREV: show slot[PREV], rotate roles backward.
 *  Loading from cache happens in displayTask itself (SD read is ~200 ms,
 *  fast enough to be invisible compared with the 1.6 s waveform).
 *
 *  SPI BUS
 *  -----------------------------------------------------------------------
 *  SD and e-ink share SPI.  g_sdMutex serialises them.
 *  During cache build:
 *    Core 1 takes mutex only while writing a page file to SD.
 *    Core 0 takes mutex for display operations.
 *  During read mode:
 *    displayTask takes mutex for SD load + e-ink transfer together.
 *    No Core 1 activity on SPI after cache is complete.
 *
 *  BUTTONS
 *  -----------------------------------------------------------------------
 *  ISR → 50 ms hardware debounce → atomic flag → loop() → g_navQueue.
 *  loop() never blocks, never touches SPI.
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <esp_timer.h>
#include <atomic>

#include "epub_types.h"
#include "framebuffer.h"
#include "xhtml_parser.h"
#include "epub_renderer.h"
#include "page_cache.h"

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
// RTOS objects
// ---------------------------------------------------------------------------
enum NavCmd : uint8_t { NAV_NEXT = 0, NAV_PREV = 1 };

SemaphoreHandle_t g_sdMutex       = nullptr;
QueueHandle_t     g_navQueue      = nullptr;

// Used only during cache build:
SemaphoreHandle_t g_pageCachedSem = nullptr;  // Core 1 gives, Core 0 takes

// ---------------------------------------------------------------------------
// Slot ring (read mode)
// ---------------------------------------------------------------------------
enum Role { PREV = 0, CUR = 1, NEXT = 2 };
static int  g_role[3]      = {0, 1, 2};
static int  g_slotPage[3]  = {-1, -1, -1};
static bool g_slotValid[3] = {false, false, false};

static int  g_totalPages   = 0;    // set after cache is confirmed/built

// ---------------------------------------------------------------------------
// Cache-build state (Core 1 render task)
// ---------------------------------------------------------------------------
static std::atomic<int>  g_pagesBuilt{0};
static std::atomic<bool> g_buildDone{false};

// Overflow element for page-break tracking
struct OverflowElem { bool valid = false; RenderElem elem; };
static OverflowElem g_overflow;
static int          g_nextPageIdx = 1;

static bool onElement(const RenderElem& elem, void* /*ctx*/) {
  bool ok = renderer.feed(elem);
  if (!ok) {
    g_overflow.valid = true;
    g_overflow.elem  = elem;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// cacheBuildTask — Core 1
// Renders all pages sequentially, writes each to SD, signals Core 0.
// ---------------------------------------------------------------------------
static void cacheBuildTask(void* /*param*/) {
  Serial.printf("[Core1] Cache build task on core %d\n", xPortGetCoreID());

  // Get source file size for staleness detection
  File src = SD.open(XHTML_PATH);
  uint32_t srcSize = src ? src.size() : 0;
  if (src) src.close();

  CacheWriter writer;
  writer.begin(srcSize);

  XhtmlParser parser;
  bool parserDone = false;
  int  slotIdx    = 0;   // we reuse slot 0 as the render scratch buffer

  // --- Page 0 ---
  renderer.beginPage(slotIdx);
  bool more = parser.parse(XHTML_PATH, "/book3/EPUB/", onElement, nullptr);
  renderer.endPage();

  // Write page 0 to SD (take mutex so Core 0's display ops don't race)
  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  writer.writePage(0, g_pool.slotBuf(slotIdx));
  xSemaphoreGive(g_sdMutex);

  g_pagesBuilt.store(1, std::memory_order_release);
  xSemaphoreGive(g_pageCachedSem);   // tell Core 0: first page is ready

  if (!more && !g_overflow.valid) {
    writer.finish(1);
    g_totalPages = 1;
    g_buildDone.store(true, std::memory_order_release);
    xSemaphoreGive(g_pageCachedSem);
    vTaskDelete(nullptr); return;
  }

  // --- Pages 1 … N ---
  while (!parserDone) {
    renderer.beginPage(slotIdx);

    if (g_overflow.valid) {
      g_overflow.valid = false;
      if (!renderer.feed(g_overflow.elem))
        Serial.println("[Core1] WARN: element too large, dropped.");
    }

    more = parser.resumeParse(onElement, nullptr);
    if (!more && !g_overflow.valid) parserDone = true;

    renderer.endPage();

    int pageNum = g_nextPageIdx++;
    xSemaphoreTake(g_sdMutex, portMAX_DELAY);
    writer.writePage(pageNum, g_pool.slotBuf(slotIdx));
    xSemaphoreGive(g_sdMutex);

    g_pagesBuilt.store(pageNum + 1, std::memory_order_release);
    xSemaphoreGive(g_pageCachedSem);
  }

  int total = g_pagesBuilt.load(std::memory_order_relaxed);
  writer.finish(static_cast<uint32_t>(total));
  g_totalPages = total;
  g_buildDone.store(true, std::memory_order_release);
  xSemaphoreGive(g_pageCachedSem);   // wake Core 0 in case it's still waiting
  parser.close();
  Serial.printf("[Core1] Cache complete: %d pages.\n", total);
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Slot helpers (read mode)
// ---------------------------------------------------------------------------
// Load a page from SD cache into a PSRAM slot.
// Caller must hold g_sdMutex.
static bool loadSlot(int slot, int pageIdx) {
  if (pageIdx < 0 || pageIdx >= g_totalPages) return false;
  uint8_t* buf = const_cast<uint8_t*>(g_pool.slotBuf(slot));
  if (!buf) return false;
  bool ok = CacheReader::loadPage(pageIdx, buf);
  if (ok) {
    g_slotPage[slot]  = pageIdx;
    g_slotValid[slot] = true;
  }
  return ok;
}

// ---------------------------------------------------------------------------
// showSlot — push a PSRAM slot to the e-ink panel.
// `full` = true only for the very first page after power-on.
// ---------------------------------------------------------------------------
static bool g_firstShow = true;

static void showSlot(int slot) {
  const uint8_t* buf = g_pool.slotBuf(slot);
  if (!buf) return;
  if (g_firstShow) {
    g_firstShow = false;
    renderer.showPageFull(buf);
  } else {
    renderer.showPagePartial(buf);
  }
}

// ---------------------------------------------------------------------------
// Slot rotation helpers
// ---------------------------------------------------------------------------
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
  // stale slot remains valid — it will be recycled on next forward nav
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

// ---------------------------------------------------------------------------
// Cache build monitor — runs on Core 0 during build phase.
// Shows progress, then exits when g_buildDone is set.
// ---------------------------------------------------------------------------
static void runCacheBuildMonitor() {
  int lastShown = -1;
  while (true) {
    // Wait for Core 1 to signal a page is done
    xSemaphoreTake(g_pageCachedSem, portMAX_DELAY);

    if (g_buildDone.load(std::memory_order_acquire)) break;

    int built = g_pagesBuilt.load(std::memory_order_acquire);
    if (built != lastShown) {
      lastShown = built;
      // Show progress (takes g_sdMutex internally via display)
      xSemaphoreTake(g_sdMutex, portMAX_DELAY);
      showProgress(built, 0);
      xSemaphoreGive(g_sdMutex);
    }
  }
  // Drain any remaining semaphore tokens
  while (xSemaphoreTake(g_pageCachedSem, 0) == pdTRUE) {}
  Serial.println("[Core0] Cache build complete.");
}

// ---------------------------------------------------------------------------
// displayTask — Core 0, read mode
// ---------------------------------------------------------------------------
#define DISPLAY_STACK 4096

static void displayTask(void* /*param*/) {

  // Pre-load slots: PREV (n/a on page 0), CUR=page0, NEXT=page1
  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  loadSlot(g_role[CUR],  0);
  if (g_totalPages > 1) loadSlot(g_role[NEXT], 1);
  xSemaphoreGive(g_sdMutex);

  // Show page 0 with FULL refresh (required for first display)
  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  showSlot(g_role[CUR]);
  xSemaphoreGive(g_sdMutex);
  Serial.printf("[Display] Page 1 shown (full refresh).\n");

  NavCmd cmd;
  for (;;) {
    if (xQueueReceive(g_navQueue, &cmd, portMAX_DELAY) != pdTRUE) continue;

    // ------- NEXT -------
    if (cmd == NAV_NEXT) {
      int curPage = g_slotPage[g_role[CUR]];
      if (curPage >= g_totalPages - 1) {
        Serial.println("[Display] Last page.");
        continue;
      }

      // Is NEXT slot pre-loaded?
      if (!g_slotValid[g_role[NEXT]]) {
        // Load it now (shouldn't normally happen — we pre-load in the loop)
        xSemaphoreTake(g_sdMutex, portMAX_DELAY);
        loadSlot(g_role[NEXT], curPage + 1);
        xSemaphoreGive(g_sdMutex);
      }

      // Show NEXT page
      xSemaphoreTake(g_sdMutex, portMAX_DELAY);
      showSlot(g_role[NEXT]);
      xSemaphoreGive(g_sdMutex);
      Serial.printf("[Display] Page %d (partial).\n", g_slotPage[g_role[NEXT]] + 1);

      rotateFwd();

      // Pre-load the new NEXT slot (page after current)
      int newNextPage = g_slotPage[g_role[CUR]] + 1;
      if (newNextPage < g_totalPages) {
        xSemaphoreTake(g_sdMutex, portMAX_DELAY);
        loadSlot(g_role[NEXT], newNextPage);
        xSemaphoreGive(g_sdMutex);
      }
    }

    // ------- PREV -------
    else {
      int curPage = g_slotPage[g_role[CUR]];
      if (curPage <= 0) {
        Serial.println("[Display] First page.");
        continue;
      }

      // Is PREV slot valid?
      if (!g_slotValid[g_role[PREV]]) {
        xSemaphoreTake(g_sdMutex, portMAX_DELAY);
        loadSlot(g_role[PREV], curPage - 1);
        xSemaphoreGive(g_sdMutex);
      }

      xSemaphoreTake(g_sdMutex, portMAX_DELAY);
      showSlot(g_role[PREV]);
      xSemaphoreGive(g_sdMutex);
      Serial.printf("[Display] Page %d (partial, back).\n", g_slotPage[g_role[PREV]] + 1);

      rotateBwd();

      // Pre-load the new PREV slot
      int newPrevPage = g_slotPage[g_role[CUR]] - 1;
      if (newPrevPage >= 0) {
        xSemaphoreTake(g_sdMutex, portMAX_DELAY);
        loadSlot(g_role[PREV], newPrevPage);
        xSemaphoreGive(g_sdMutex);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------
static std::atomic<bool> g_btnNextPending{false};
static std::atomic<bool> g_btnPrevPending{false};
static volatile int64_t  g_btnNextTime = 0;
static volatile int64_t  g_btnPrevTime = 0;
#define DEBOUNCE_US 50000LL

void IRAM_ATTR onBtnNext() {
  int64_t now = esp_timer_get_time();
  if (now - g_btnNextTime >= DEBOUNCE_US) {
    g_btnNextTime = now;
    g_btnNextPending.store(true, std::memory_order_relaxed);
  }
}
void IRAM_ATTR onBtnPrev() {
  int64_t now = esp_timer_get_time();
  if (now - g_btnPrevTime >= DEBOUNCE_US) {
    g_btnPrevTime = now;
    g_btnPrevPending.store(true, std::memory_order_relaxed);
  }
}

void loop() {
  if (g_btnNextPending.load(std::memory_order_relaxed)) {
    g_btnNextPending.store(false, std::memory_order_relaxed);
    if (digitalRead(BTN_NEXT) == LOW) {
      NavCmd c = NAV_NEXT;
      xQueueSend(g_navQueue, &c, 0);
    }
  }
  if (g_btnPrevPending.load(std::memory_order_relaxed)) {
    g_btnPrevPending.store(false, std::memory_order_relaxed);
    if (digitalRead(BTN_PREV) == LOW) {
      NavCmd c = NAV_PREV;
      xQueueSend(g_navQueue, &c, 0);
    }
  }
  vTaskDelay(pdMS_TO_TICKS(5));
}

// ---------------------------------------------------------------------------
// Status screen
// ---------------------------------------------------------------------------
static void showStatus(const char* l1, const char* l2 = nullptr) {
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

  // -----------------------------------------------------------------------
  // Check cache
  // -----------------------------------------------------------------------
  CacheMeta meta;
  bool cacheValid = CacheReader::probe(meta);

  if (!cacheValid) {
    // ---- CACHE BUILD MODE ----
    Serial.println("Cache stale/missing. Building...");
    showStatus("Building cache...", "First boot only");

    // Launch Core 1 render+cache task
    xTaskCreatePinnedToCore(cacheBuildTask, "CacheBuild", 8192,
                            nullptr, 1, nullptr, 1);

    // Core 0 shows progress until build is done
    runCacheBuildMonitor();

    // Re-read metadata now that build is complete
    CacheReader::probe(meta);
  }

  g_totalPages = static_cast<int>(meta.pageCount);
  Serial.printf("Cache ready: %d pages.\n", g_totalPages);

  if (g_totalPages <= 0) {
    showStatus("No pages found!");
    return;
  }

  // -----------------------------------------------------------------------
  // READ MODE — launch display task
  // -----------------------------------------------------------------------
  showStatus("Loading...", "Please wait");

  xTaskCreatePinnedToCore(displayTask, "Display", DISPLAY_STACK,
                          nullptr, 2, nullptr, 0);
}
