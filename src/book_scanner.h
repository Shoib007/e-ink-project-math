#pragma once
// ---------------------------------------------------------------------------
// book_scanner.h — Discover EPUB books from /books/ on the SD card.
//
// Expected SD layout:
//   /books/
//     <bookname>/
//       OEBPS/  -or-  EPUB/          (auto-detected)
//         content.opf                (spine order, optional)
//         chapter_001.xhtml
//         chapter_002.xhtml
//         images/
//         math_images/
//
// BookScanner::scan() fills a caller-supplied BookInfo array and returns the
// number of valid books found.  Each BookInfo contains:
//   name        — directory name used as the display label
//   basePath    — full path to the OEBPS/EPUB folder, with trailing '/'
//   cacheDir    — /cache/<bookname>
//   xhtmlFiles  — ordered list of XHTML filenames (relative to basePath)
//   xhtmlCount  — number of entries in xhtmlFiles
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <SD.h>

#define BOOKS_ROOT       "/books"
#define MAX_BOOKS        8
#define MAX_XHTML_FILES  24

struct BookInfo {
  char name     [32];                       // directory name (display label)
  char basePath [72];                       // e.g. /books/jemh1a2/OEBPS/
  char cacheDir [64];                       // e.g. /cache/jemh1a2
  char xhtmlFiles[MAX_XHTML_FILES][48];    // ordered XHTML filenames (not full paths)
  int  xhtmlCount;
};

// ---------------------------------------------------------------------------
class BookScanner {
public:
  // Scan /books/ for valid EPUB directories.
  // Returns count of books found (0 on error or empty /books/).
  static int scan(BookInfo* books, int maxBooks) {
    File root = SD.open(BOOKS_ROOT);
    if (!root || !root.isDirectory()) {
      Serial.println("[BookScan] /books/ not found on SD");
      if (root) root.close();
      return 0;
    }

    int count = 0;
    File entry;
    while (count < maxBooks) {
      entry = root.openNextFile();
      if (!entry) break;
      if (!entry.isDirectory()) { entry.close(); continue; }

      // Extract directory basename (entry.name() may be a full path)
      const char* fullName = entry.name();
      const char* baseName = strrchr(fullName, '/');
      baseName = (baseName && *(baseName + 1)) ? baseName + 1 : fullName;
      if (baseName[0] == '.') { entry.close(); continue; }  // skip hidden

      // Detect OEBPS/ or EPUB/ subdirectory
      char subPath[80];
      bool found = false;
      const char* subs[] = {"OEBPS", "EPUB"};
      for (int s = 0; s < 2 && !found; s++) {
        snprintf(subPath, sizeof(subPath), "%s/%s/%s", BOOKS_ROOT, baseName, subs[s]);
        File d = SD.open(subPath);
        if (d && d.isDirectory()) found = true;
        if (d) d.close();
      }
      if (!found) { entry.close(); continue; }

      BookInfo& b = books[count];
      strncpy(b.name, baseName, sizeof(b.name) - 1);
      b.name[sizeof(b.name) - 1] = '\0';
      // basePath = subPath + trailing slash
      snprintf(b.basePath, sizeof(b.basePath), "%s/", subPath);
      snprintf(b.cacheDir, sizeof(b.cacheDir), "/cache/%s", baseName);
      b.xhtmlCount = 0;

      // Try OPF spine first, then alphabetical fallback
      char opfPath[96];
      snprintf(opfPath, sizeof(opfPath), "%scontent.opf", b.basePath);
      if (!readSpineFromOpf(opfPath, b)) {
        collectXhtmlAlpha(b.basePath, b);
      }

      if (b.xhtmlCount > 0) {
        Serial.printf("[BookScan] '%s': %d XHTML file(s), base='%s'\n",
                      b.name, b.xhtmlCount, b.basePath);
        ++count;
      } else {
        Serial.printf("[BookScan] Skipping '%s': no XHTML content found\n", baseName);
      }
      entry.close();
    }
    root.close();
    Serial.printf("[BookScan] Total books found: %d\n", count);
    return count;
  }

private:
  // ---------------------------------------------------------------------------
  // OPF spine parser
  // ---------------------------------------------------------------------------
  struct ManifestItem { char id[32]; char href[48]; };

  // Read content.opf, extract spine item hrefs in spine order.
  // Returns true if at least one XHTML file was found in the spine.
  static bool readSpineFromOpf(const char* opfPath, BookInfo& b) {
    File f = SD.open(opfPath);
    if (!f) return false;

    size_t sz = f.size();
    if (sz == 0 || sz > 16384) { f.close(); return false; }

    char* buf = new char[sz + 1];
    if (!buf) { f.close(); return false; }
    f.read(reinterpret_cast<uint8_t*>(buf), sz);
    buf[sz] = '\0';
    f.close();

    // Static to avoid large stack allocation; scan() is single-threaded at boot.
    static ManifestItem manifest[MAX_XHTML_FILES];
    int mCount = 0;

    // --- Pass 1: collect manifest <item id="…" href="…"> ---
    const char* p = buf;
    while ((p = strstr(p, "<item ")) != nullptr && mCount < MAX_XHTML_FILES) {
      const char* tagEnd = strchr(p, '>');
      if (!tagEnd) break;

      char id[32] = {}, href[48] = {};
      extractAttr(p, tagEnd, " id=\"",   id,   sizeof(id));
      extractAttr(p, tagEnd, " href=\"", href, sizeof(href));

      if (id[0] && href[0] &&
          (strstr(href, ".xhtml") || strstr(href, ".html"))) {
        strncpy(manifest[mCount].id,   id,   sizeof(manifest[mCount].id)   - 1);
        strncpy(manifest[mCount].href, href, sizeof(manifest[mCount].href) - 1);
        manifest[mCount].id  [sizeof(manifest[mCount].id)   - 1] = '\0';
        manifest[mCount].href[sizeof(manifest[mCount].href) - 1] = '\0';
        mCount++;
      }
      p = tagEnd + 1;
    }

    // --- Pass 2: walk <spine> idrefs ---
    b.xhtmlCount = 0;
    const char* spineStart = strstr(buf, "<spine");
    const char* spineEnd   = strstr(buf, "</spine>");
    if (!spineStart || !spineEnd || spineStart >= spineEnd) {
      delete[] buf;
      return false;
    }

    p = spineStart;
    while (p < spineEnd && b.xhtmlCount < MAX_XHTML_FILES) {
      const char* ref = strstr(p, "idref=\"");
      if (!ref || ref >= spineEnd) break;
      ref += 7;
      const char* refEnd = strchr(ref, '"');
      if (!refEnd || refEnd >= spineEnd) break;

      char idref[32] = {};
      int  rlen = (int)(refEnd - ref);
      if (rlen > 31) rlen = 31;
      strncpy(idref, ref, rlen);

      for (int i = 0; i < mCount; i++) {
        if (strcmp(manifest[i].id, idref) == 0) {
          strncpy(b.xhtmlFiles[b.xhtmlCount], manifest[i].href, 47);
          b.xhtmlFiles[b.xhtmlCount][47] = '\0';
          b.xhtmlCount++;
          break;
        }
      }
      p = refEnd + 1;
    }

    delete[] buf;
    return b.xhtmlCount > 0;
  }

  // Extract a quoted attribute value from a tag substring.
  static void extractAttr(const char* tagStart, const char* tagEnd,
                           const char* attrName, char* out, int outLen) {
    const char* pos = strstr(tagStart, attrName);
    if (!pos || pos >= tagEnd) { out[0] = '\0'; return; }
    pos += strlen(attrName);
    const char* end = strchr(pos, '"');
    if (!end || end >= tagEnd) { out[0] = '\0'; return; }
    int len = (int)(end - pos);
    if (len >= outLen) len = outLen - 1;
    strncpy(out, pos, len);
    out[len] = '\0';
  }

  // ---------------------------------------------------------------------------
  // Alphabetical fallback: collect *.xhtml files from basePath, sorted.
  // Skips common non-content files: nav.xhtml, toc.xhtml, title.xhtml, cover.xhtml
  // ---------------------------------------------------------------------------
  static void collectXhtmlAlpha(const char* basePath, BookInfo& b) {
    // Strip trailing slash for SD.open
    char dir[72];
    strncpy(dir, basePath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    int dlen = strlen(dir);
    if (dlen > 0 && dir[dlen - 1] == '/') dir[dlen - 1] = '\0';

    File d = SD.open(dir);
    if (!d) return;

    b.xhtmlCount = 0;
    File entry;
    while ((entry = d.openNextFile()) && b.xhtmlCount < MAX_XHTML_FILES) {
      if (entry.isDirectory()) { entry.close(); continue; }

      const char* fn   = entry.name();
      const char* base = strrchr(fn, '/');
      base = (base && *(base + 1)) ? base + 1 : fn;

      int nlen = strlen(base);
      bool isXhtml = (nlen > 6 && strcmp(base + nlen - 6, ".xhtml") == 0) ||
                     (nlen > 5 && strcmp(base + nlen - 5, ".html")  == 0);
      if (!isXhtml) { entry.close(); continue; }

      // Skip typical non-content files
      if (strncmp(base, "nav",   3) == 0 ||
          strncmp(base, "toc",   3) == 0 ||
          strncmp(base, "title", 5) == 0 ||
          strncmp(base, "cover", 5) == 0) {
        entry.close(); continue;
      }

      strncpy(b.xhtmlFiles[b.xhtmlCount], base, 47);
      b.xhtmlFiles[b.xhtmlCount][47] = '\0';
      b.xhtmlCount++;
      entry.close();
    }
    d.close();

    // Insertion sort alphabetically
    for (int i = 1; i < b.xhtmlCount; i++) {
      char tmp[48];
      strncpy(tmp, b.xhtmlFiles[i], 47); tmp[47] = '\0';
      int j = i - 1;
      while (j >= 0 && strcmp(b.xhtmlFiles[j], tmp) > 0) {
        strncpy(b.xhtmlFiles[j + 1], b.xhtmlFiles[j], 47);
        j--;
      }
      strncpy(b.xhtmlFiles[j + 1], tmp, 47);
    }
  }
};
