#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Buffer sizes
// ---------------------------------------------------------------------------
#define MAX_TEXT_LEN   256   // larger to hold table cell text + embedded image sentinels
#define MAX_PATH_LEN    96   // larger for deeper book directory structures

// ---------------------------------------------------------------------------
// Display dimensions (portrait, rotation=1)
// ---------------------------------------------------------------------------
#define DISPLAY_W      480
#define DISPLAY_H      800

// ---------------------------------------------------------------------------
// Layout margins and spacing
// ---------------------------------------------------------------------------
#define MARGIN_LEFT     10
#define MARGIN_RIGHT    10
#define MARGIN_TOP      10
#define MARGIN_BOTTOM   10
#define LINE_SPACING     4   // extra pixels between lines
#define PARA_SPACING     8   // extra pixels between paragraphs
#define HEADING_SPACING 12   // extra pixels after headings

// Inline image (math formula PNG) spacing and alignment
#define INLINE_IMG_SPACE    4   // gap (px) added before AND after an inline image
#define INLINE_IMG_DESCENT  3   // pixels the image is lowered so its optical baseline
                                // aligns with the text baseline (MathJax PNGs include
                                // ~3px of descender whitespace at their bottom)
#define LIST_INDENT     24   // left indent (px) for <li> content, measured dynamically

// ---------------------------------------------------------------------------
// Table rendering
// ---------------------------------------------------------------------------
#define TABLE_BORDER_W      1   // border line thickness in pixels
#define TABLE_CELL_PAD      3   // padding (px) inside each cell (top/bottom + left/right)
#define TABLE_AFTER_SPACING 8   // extra gap (px) below the table bottom border

// ---------------------------------------------------------------------------
// Sentinel for mixed-content table cells.
//
// When a <td> or <th> contains both text and inline images, the parser encodes
// the entire cell content as a single string using ASCII Unit Separator (0x1F)
// to delimit image paths:
//
//   "Text before "  +  '\x1F'  +  "/sd/path/image.png"  +  '\x1F'  +  " text after"
//
// The renderer's renderCellContent() scans for these sentinels and calls
// decodePng() for each embedded path.  0x1F never appears in valid text or
// file paths, so false positives are impossible.
// ---------------------------------------------------------------------------
#define CELL_IMG_SENTINEL  '\x1F'

// ---------------------------------------------------------------------------
// Font levels — mapped to Adafruit GFX font objects in EpubRenderer::selectFont()
//
//   FONT_BODY        FreeSans9pt7b              body paragraphs
//   FONT_BODY_BOLD   FreeMonoBold9pt7b          <strong> / <b> / table headers
//   FONT_H4          FreeSansBoldOblique9pt7b   <h4>
//   FONT_H3          FreeSansBold9pt7b          <h3>
//   FONT_H2          FreeSansBold12pt7b         <h2>
//   FONT_H1          FreeSansBold18pt7b         <h1>
// ---------------------------------------------------------------------------
enum FontLevel {
  FONT_BODY        = 0,
  FONT_BODY_BOLD   = 1,
  FONT_H4          = 2,
  FONT_H3          = 3,
  FONT_H2          = 4,
  FONT_H1          = 5,
};

// ---------------------------------------------------------------------------
// Element types — emitted by XhtmlParser, consumed by EpubRenderer::feed()
// ---------------------------------------------------------------------------
enum ElemType {
  // ---- Text ---------------------------------------------------------------
  ELEM_TEXT,              // plain-text word run; fontLevel = FONT_BODY / FONT_BODY_BOLD
  ELEM_HEADING,           // heading word run; fontLevel = FONT_H1 … FONT_H4

  // ---- Images -------------------------------------------------------------
  ELEM_IMAGE_INLINE,      // inline PNG (math formula or class="math-inline")
  ELEM_IMAGE_BLOCK,       // block PNG figure (centred, own line)
  ELEM_IMAGE_BLOCK_JPG,   // block JPEG figure (centred, own line)

  // ---- Block breaks -------------------------------------------------------
  ELEM_PARA_BREAK,        // end of <p> or <li>
  ELEM_HEADING_BREAK,     // end of heading block (h1…h4)
  ELEM_BR,                // forced line break <br />

  // ---- Ordered lists ------------------------------------------------------
  ELEM_OL_ITEM_START,     // <li> inside <ol>; listNum = 1-based item number

  // ---- Tables -------------------------------------------------------------
  ELEM_TABLE_START,       // <table> opened
  ELEM_TABLE_HEADER_CELL, // <th> cell content; text[] may contain CELL_IMG_SENTINEL runs
  ELEM_TABLE_DATA_CELL,   // <td> cell content; same encoding
  ELEM_TABLE_ROW_END,     // </tr>
  ELEM_TABLE_END,         // </table>
};

// ---------------------------------------------------------------------------
// RenderElem — single unit of work passed from parser to renderer via callback.
// ---------------------------------------------------------------------------
struct RenderElem {
  ElemType  type;
  char      text[MAX_TEXT_LEN];   // ELEM_TEXT, ELEM_HEADING, table cells
  char      path[MAX_PATH_LEN];   // ELEM_IMAGE_* — full SD path
  FontLevel fontLevel;            // text/heading font; header cells = FONT_BODY_BOLD
  uint8_t   listNum;              // ELEM_OL_ITEM_START: 1-based item number
  uint8_t   tableCol;             // reserved for future proportional column widths
};
