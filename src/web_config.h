#pragma once
// ---------------------------------------------------------------------------
// web_config.h — WiFi credentials for the upload web server
//
// STA mode is tried first.  If it fails (or WIFI_SSID is empty) the device
// becomes a SoftAP with SSID AP_SSID — connect to {SSID} and browse to
// http://192.168.4.1
// ---------------------------------------------------------------------------

// Home/phone network to join (STA).  Leave empty to force AP mode.
#ifndef WIFI_SSID
#define WIFI_SSID     "abc"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "abcdefhi"
#endif

// SoftAP used when STA fails / is not configured.
#ifndef AP_SSID
#define AP_SSID       "EPUB-Reader"
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD   ""            // "" = open network
#endif

#define WEB_SERVER_PORT     80
#define MAX_ZIP_UPLOAD_BYTES (4 * 1024 * 1024)   // cap for ZIP body buffered in PSRAM