#pragma once
// ---------------------------------------------------------------------------
// web_globals.h — shared state between main.cpp (app loop) and web_server.cpp
//
// g_sdMutex is defined and owned by main.cpp.  g_reloadBooksRequested is
// defined in web_server.cpp; the web task sets it after an upload finishes
// and main.cpp's loop() consumes it to rescan /books/ and redraw the menu.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <atomic>
#include <freertos/semphr.h>

extern SemaphoreHandle_t g_sdMutex;

extern std::atomic<bool> g_reloadBooksRequested;