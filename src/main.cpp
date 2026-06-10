#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>

#include "epub_types.h"
#include "xhtml_parser.h"
#include "epub_renderer.h"

// ---- Pin definitions for XIAO ESP32-S3 ----
#define EPD_CS    2
#define EPD_DC    3
#define EPD_RST   5
#define EPD_BUSY  6
#define SD_CS     4

// ---- Display object ----
GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 2> display(
  GxEPD2_750_T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// ---- Renderer ----
EpubRenderer renderer(display);

// ---- Element counter for debug ----
static int s_elemCount = 0;

// ---- Parser callback ----
bool onElement(const RenderElem& elem, void* ctx) {
  EpubRenderer* r = static_cast<EpubRenderer*>(ctx);

  // Print first 30 elements so we can see parsing is working
  if (s_elemCount < 30) {
    switch (elem.type) {
      case ELEM_TEXT:
        Serial.printf("[%d] TEXT      font=%d '%s'\n", s_elemCount, elem.fontLevel, elem.text);
        break;
      case ELEM_HEADING:
        Serial.printf("[%d] HEADING   font=%d '%s'\n", s_elemCount, elem.fontLevel, elem.text);
        break;
      case ELEM_IMAGE_INLINE:
        Serial.printf("[%d] IMG_INLINE  '%s'\n", s_elemCount, elem.path);
        break;
      case ELEM_IMAGE_BLOCK:
        Serial.printf("[%d] IMG_BLOCK   '%s'\n", s_elemCount, elem.path);
        break;
      case ELEM_PARA_BREAK:
        Serial.printf("[%d] PARA_BREAK\n", s_elemCount);
        break;
      case ELEM_HEADING_BREAK:
        Serial.printf("[%d] HEADING_BREAK\n", s_elemCount);
        break;
    }
  }
  s_elemCount++;

  r->feed(elem);
  return true;
}

// ---- Draw a simple test pattern directly ----
void testDisplay() {
  Serial.println("Testing display...");
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(nullptr);  // built-in tiny font
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(10, 10);
    display.print("Display OK");
    display.setCursor(10, 30);
    display.print("XIAO ESP32-S3");
    // Draw a border rectangle
    display.drawRect(5, 5, display.width() - 10, display.height() - 10, GxEPD_BLACK);
    // Draw a horizontal line at mid-screen to check full height renders
    display.drawLine(0, display.height() / 2, display.width(), display.height() / 2, GxEPD_BLACK);
  } while (display.nextPage());
  Serial.println("Display test done.");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== EPUB Reader Boot ===");

  // ---- Init display ----
  Serial.println("Initialising display...");
  display.init(115200);
  display.setRotation(1);  // Portrait: width=480, height=800
  Serial.printf("Display size: %d x %d\n", display.width(), display.height());

  // ---- Test display first ----
  testDisplay();
  delay(3000);  // let the refresh complete and user observe

  // ---- Init SD ----
  Serial.println("Initialising SD...");
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

  // ---- Check the XHTML file exists ----
  const char* xhtmlPath = "/book3/EPUB/chap_01.xhtml";
  File f = SD.open(xhtmlPath);
  if (!f) {
    Serial.printf("ERROR: Cannot open %s\n", xhtmlPath);
    return;
  }
  Serial.printf("XHTML file size: %d bytes\n", f.size());
  f.close();

  // ---- Check first image exists ----
  const char* imgPath = "/book3/EPUB/images/image_1_28d69ab8.png";
  File img = SD.open(imgPath);
  if (!img) {
    Serial.printf("WARNING: Cannot open test image %s\n", imgPath);
  } else {
    Serial.printf("Test image size: %d bytes\n", img.size());
    img.close();
  }

  // ---- Parse and render ----
  Serial.println("Starting parse+render...");
  s_elemCount = 0;
  XhtmlParser parser;
  renderer.beginDoc();
  bool ok = parser.parse(xhtmlPath, "/book3/EPUB/", onElement, &renderer);
  renderer.endDoc();

  Serial.printf("Parse %s. Total elements: %d\n", ok ? "OK" : "FAILED", s_elemCount);
  Serial.println("Done.");
}

void loop() {}
