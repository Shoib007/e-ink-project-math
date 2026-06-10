#pragma once
#include <Arduino.h>

// Maximum lengths for strings stored in render elements
#define MAX_TEXT_LEN   128
#define MAX_PATH_LEN    80

// Display dimensions (portrait, rotation=1)
#define DISPLAY_W      480
#define DISPLAY_H      800

// Layout margins and spacing
#define MARGIN_LEFT     10
#define MARGIN_RIGHT    10
#define MARGIN_TOP      10
#define MARGIN_BOTTOM   10
#define LINE_SPACING     4   // extra pixels between lines
#define PARA_SPACING     8   // extra pixels between paragraphs
#define HEADING_SPACING 12   // extra pixels after headings

// Font size levels
enum FontLevel {
  FONT_BODY   = 0,
  FONT_H4     = 1,
  FONT_H3     = 2,
  FONT_H2     = 3
};

// Types of render elements produced by the parser
enum ElemType {
  ELEM_TEXT,          // a run of plain text, rendered inline
  ELEM_IMAGE_INLINE,  // inline formula image (vertical-align: middle)
  ELEM_IMAGE_BLOCK,   // display/block formula image (centered, own line)
  ELEM_PARA_BREAK,    // end of <p>
  ELEM_HEADING,       // heading text with level
  ELEM_HEADING_BREAK  // end of heading block
};

struct RenderElem {
  ElemType type;
  char     text[MAX_TEXT_LEN];   // for ELEM_TEXT / ELEM_HEADING
  char     path[MAX_PATH_LEN];   // for ELEM_IMAGE_*  (SD path)
  FontLevel fontLevel;           // for headings and text
};
