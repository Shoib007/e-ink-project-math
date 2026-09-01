#pragma once
// Start the web upload server (WiFi + HTTP) as a background FreeRTOS task.
// Safe to call once from setup(); boots STA first, falls back to SoftAP.
void webSetup();