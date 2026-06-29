#pragma once
// ===========================================================================
// table_renderer.h
//
// Self-contained table renderer that works in pure PIXEL coordinates.
// It has NO knowledge of the text-baseline (_cy) system used by EpubRenderer.
//
// Design principles
// -----------------
//   1. All Y values are PIXEL coordinates (y=0 = screen top edge).
//   2. The table fully measures every row before drawing anything.
//   3. If a row does not fit the page it is NOT drawn.
//      The caller gets a ROW_PAGE_FULL result and must:
//        a) flush the current page
//        b) call continueOnPage() with the new page top pixel
//        c) call endRow() again — the buffered row will be re-drawn.
//   4. No Adafruit GFX text-cursor state is touched here.
//      Cell text is drawn via caller-supplied callbacks so this file has no
//      dependency on EpubRenderer internals.
// ===========================================================================

#include <Arduino.h>
#include "epub_types.h"

// ---------------------------------------------------------------------------
// Callback signatures — the caller (EpubRenderer) provides these as thin
// wrappers around its own private methods.
// ---------------------------------------------------------------------------

// Draw a horizontal line at pixel (x, y) of width w.
using DrawHLineFn   = void(*)(int16_t x, int16_t y, int16_t w, void* ctx);

// Draw a vertical line at pixel (x, y) of height h.
using DrawVLineFn   = void(*)(int16_t x, int16_t y, int16_t h, void* ctx);

// Measure the height (px) that 'text' rendered in 'lvl' would occupy inside
// a column of pixel width 'colW'.
using MeasureTextFn = int16_t(*)(const char* text, FontLevel lvl,
                                  int16_t colW, void* ctx);

// Draw the content of one cell.
//   pixX, pixY : top-left pixel of the CONTENT area (padding already applied)
//   colW       : width of the content area
//   cellH      : total row height (content + padding, for vertical centering)
using DrawCellFn    = void(*)(const char* text, FontLevel lvl,
                               int16_t pixX, int16_t pixY,
                               int16_t colW, int16_t cellH,
                               void* ctx);

// ===========================================================================
// TableRenderer
// ===========================================================================
class TableRenderer {
public:
    static constexpr int  MAX_COLS      = 8;
    static constexpr int  BORDER_W      = TABLE_BORDER_W;
    static constexpr int  CELL_PAD      = TABLE_CELL_PAD;

    enum RowResult { ROW_OK, ROW_PAGE_FULL };

    struct Cell {
        char      text[MAX_TEXT_LEN];
        FontLevel fontLevel;
    };

    // ------------------------------------------------------------------
    // begin() — call at ELEM_TABLE_START.
    //   topPixelY  : pixel Y of the very top of the first row (top border).
    //   pageBottom : last usable pixel row = DISPLAY_H - MARGIN_BOTTOM - 1.
    // ------------------------------------------------------------------
    void begin(int16_t topPixelY, int16_t pageBottom);

    // ------------------------------------------------------------------
    // Row building — called for each row in sequence.
    // ------------------------------------------------------------------
    void startRow();
    void addCell(const char* text, FontLevel lvl);

    // endRow() measures, checks overflow, draws if space, returns result.
    // If ROW_PAGE_FULL: call continueOnPage(), then endRow() again.
    RowResult endRow(DrawHLineFn hLine, DrawVLineFn vLine,
                     MeasureTextFn measure, DrawCellFn drawCell,
                     void* ctx);

    // ------------------------------------------------------------------
    // continueOnPage() — start a new page mid-table.
    //   newTopPixelY  : top pixel of the new page's usable area.
    //   newPageBottom : bottom limit on the new page.
    // After calling this, endRow() will retry drawing the pending row.
    // ------------------------------------------------------------------
    void continueOnPage(int16_t newTopPixelY, int16_t newPageBottom);

    // ------------------------------------------------------------------
    // finish() — draws the final bottom border.  Call at ELEM_TABLE_END.
    // Returns the pixel Y immediately below the bottom border.
    // ------------------------------------------------------------------
    int16_t finish(DrawHLineFn hLine, void* ctx);

    // ------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------
    bool    isActive()      const { return _active; }
    int16_t currentPixelY() const { return _curY; }   // next row's top border Y
    bool    hasPendingRow() const { return _pendingRow; }

private:
    bool    _active      = false;
    bool    _pendingRow  = false;   // true when a row is buffered but not drawn
    int16_t _curY        = 0;       // pixel Y of the NEXT top border to draw
    int16_t _pageBottom  = 0;

    int     _colCount    = 0;
    int16_t _colBorderX[MAX_COLS]; // pixel X of each column's left border line
    int16_t _colContentX[MAX_COLS];// pixel X of content start (border + pad)
    int16_t _colContentW[MAX_COLS];// pixel width of content area

    int     _cellCount   = 0;
    Cell    _cells[MAX_COLS];

    void    _computeColumnLayout();
    int16_t _measureRowH(MeasureTextFn measure, void* ctx) const;
    void    _drawRow(DrawHLineFn hLine, DrawVLineFn vLine,
                     DrawCellFn drawCell, void* ctx, int16_t rowH);
};
