#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>

#include "epub_types.h"
#include "framebuffer.h"
#include "xhtml_parser.h"
#include "epub_renderer.h"

// ---- Pin definitions ----
#define EPD_CS    2
#define EPD_DC    3
#define EPD_RST   5
#define EPD_BUSY  6
#define SD_CS     4

#define BTN_PREV    44
#define BTN_NEXT    43
#define BTN_SELECT   1

// ---- Display ----
GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 2> display(
  GxEPD2_750_T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// ---- Renderer ----
EpubRenderer renderer(display);

// ---- Navigation state ----
volatile int  g_currentPage = 0;
volatile bool g_pageChanged = false;
volatile int  g_direction   = 0;  // +1 next, -1 prev

// Debounce: ignore interrupts within 300ms of last trigger
static volatile unsigned long g_lastBtnTime = 0;
#define DEBOUNCE_MS 300

void IRAM_ATTR onBtnNext() {
  unsigned long now = millis();
  if (now - g_lastBtnTime < DEBOUNCE_MS) return;
  g_lastBtnTime = now;
  g_direction   = +1;
  g_pageChanged = true;
}

void IRAM_ATTR onBtnPrev() {
  unsigned long now = millis();
  if (now - g_lastBtnTime < DEBOUNCE_MS) return;
  g_lastBtnTime = now;
  g_direction   = -1;
  g_pageChanged = true;
}

// ---- Parser callback ----
bool onElement(const RenderElem& elem, void* ctx) {
  static_cast<EpubRenderer*>(ctx)->feed(elem);
  return true;
}

// ---- Show current page with page number indicator ----
void showCurrentPage() {
  Serial.printf("Showing page %d / %d\n", g_currentPage + 1, renderer.pageCount());
  renderer.showPage(g_currentPage);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== EPUB Reader ===");

  // ---- Buttons ----
  pinMode(BTN_NEXT,   INPUT_PULLUP);
  pinMode(BTN_PREV,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_NEXT),   onBtnNext, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_PREV),   onBtnPrev, FALLING);

  // ---- Display ----
  display.init(115200);
  display.setRotation(1);  // Portrait: 480 x 800

  // ---- Framebuffer (PSRAM) ----
  if (!g_fb.init()) {
    Serial.println("ERROR: Framebuffer init failed!");
    return;
  }
  Serial.println("Framebuffer OK");

  // ---- SD ----
  if (!SD.begin(SD_CS)) {
    Serial.println("ERROR: SD init failed!");
    display.setFullWindow();
    display.firstPage();
    do {
      display.fillScreen(GxEPD_WHITE);
      display.setFont(nullptr);
      display.setTextColor(GxEPD_BLACK);
      display.setCursor(10, 20);
      display.print("SD card not found!");
    } while (display.nextPage());
    return;
  }
  Serial.println("SD OK");

  // ---- Layout pass (no display I/O) ----
  Serial.println("Laying out document...");
  XhtmlParser parser;
  if (!renderer.beginDoc()) {
    Serial.println("ERROR: PSRAM allocation failed!");
    return;
  }
  parser.parse("/book3/EPUB/chap_01.xhtml", "/book3/EPUB/", onElement, &renderer);
  renderer.endDoc();
  Serial.printf("Layout done. Pages: %d\n", renderer.pageCount());

  // ---- Show first page ----
  g_currentPage = 0;
  showCurrentPage();
}

void loop() {
  if (g_pageChanged) {
    g_pageChanged = false;

    int next = g_currentPage + g_direction;
    if (next < 0) next = 0;
    if (next >= renderer.pageCount()) next = renderer.pageCount() - 1;

    if (next != g_currentPage) {
      g_currentPage = next;
      showCurrentPage();
    }
  }
}
