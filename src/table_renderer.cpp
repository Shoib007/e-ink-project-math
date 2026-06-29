// ===========================================================================
// table_renderer.cpp
//
// Implementation of TableRenderer.  See table_renderer.h for the design.
//
// Coordinate contract (never broken):
//   _curY           = pixel Y of the NEXT ROW's top border line
//   rowH            = height of the cell CONTENT area (no borders included)
//   total row pixel = BORDER_W (top) + rowH + rowH's bottom pad already included
//
// A "row" occupies pixels [_curY, _curY + BORDER_W + rowH) on screen.
// After drawing, _curY advances by (BORDER_W + rowH).
// The final bottom border is drawn at _curY when finish() is called.
// ===========================================================================

#include "table_renderer.h"
#include "epub_types.h"
#include <string.h>

// ---------------------------------------------------------------------------
// begin() — reset all state, record the starting pixel Y.
// ---------------------------------------------------------------------------
void TableRenderer::begin(int16_t topPixelY, int16_t pageBottom) {
    _active      = true;
    _pendingRow  = false;
    _curY        = topPixelY;
    _pageBottom  = pageBottom;
    _colCount    = 0;
    _cellCount   = 0;
}

// ---------------------------------------------------------------------------
// startRow() — clear the cell buffer for a new row.
// ---------------------------------------------------------------------------
void TableRenderer::startRow() {
    _cellCount = 0;
}

// ---------------------------------------------------------------------------
// addCell() — buffer one cell.
// ---------------------------------------------------------------------------
void TableRenderer::addCell(const char* text, FontLevel lvl) {
    if (_cellCount >= MAX_COLS) return;
    strncpy(_cells[_cellCount].text, text, MAX_TEXT_LEN - 1);
    _cells[_cellCount].text[MAX_TEXT_LEN - 1] = '\0';
    _cells[_cellCount].fontLevel = lvl;
    ++_cellCount;
}

// ---------------------------------------------------------------------------
// _computeColumnLayout() — divide the display width equally among columns.
// Called once when _colCount first becomes known (from the first row).
// ---------------------------------------------------------------------------
void TableRenderer::_computeColumnLayout() {
    // Total usable table width between the left and right margins
    const int16_t tableW  = static_cast<int16_t>(DISPLAY_W - MARGIN_LEFT - MARGIN_RIGHT);
    // Each column needs one left border; the rightmost column's right border
    // is the final border drawn by finish(). So total border pixels = colCount.
    const int16_t totalBorderPx = static_cast<int16_t>(_colCount) * BORDER_W;
    const int16_t totalContentW = tableW - totalBorderPx;
    const int16_t baseColW      = totalContentW / static_cast<int16_t>(_colCount);
    const int16_t remainder     = totalContentW - baseColW * static_cast<int16_t>(_colCount);

    int16_t x = MARGIN_LEFT;
    for (int i = 0; i < _colCount; ++i) {
        _colBorderX[i]  = x;                          // this column's left border
        _colContentX[i] = x + BORDER_W + CELL_PAD;   // content starts after border+pad
        int16_t cw      = baseColW + (i == _colCount - 1 ? remainder : 0);
        _colContentW[i] = cw - 2 * CELL_PAD;          // subtract left+right cell padding
        x += BORDER_W + cw;
    }
}

// ---------------------------------------------------------------------------
// _measureRowH() — returns the tallest cell content height across all cells.
//   The returned value is the CONTENT height; borders and padding are NOT
//   included here — they are added by the caller.
// ---------------------------------------------------------------------------
int16_t TableRenderer::_measureRowH(MeasureTextFn measure, void* ctx) const {
    int16_t maxH = 4;   // minimum content height
    for (int i = 0; i < _cellCount && i < _colCount; ++i) {
        int16_t h = measure(_cells[i].text, _cells[i].fontLevel,
                            _colContentW[i], ctx);
        if (h > maxH) maxH = h;
    }
    return maxH;
}

// ---------------------------------------------------------------------------
// _drawRow() — draw one row starting at _curY.
//   rowH : content height (result of _measureRowH, not including borders/pad)
//
// Pixel layout of one row:
//   _curY                       ← top border (BORDER_W pixels tall)
//   _curY + BORDER_W            ← top padding start  (CELL_PAD pixels)
//   _curY + BORDER_W + CELL_PAD ← content top
//   _curY + BORDER_W + CELL_PAD + rowH ← content bottom
//   ... + CELL_PAD              ← bottom of cell interior (no bottom border here;
//                                  next row's top border serves as this row's bottom)
//
// Vertical lines run from (_curY + BORDER_W) for rowH + 2*CELL_PAD pixels
// so they connect top-border to where the next row's top-border will be.
// ---------------------------------------------------------------------------
void TableRenderer::_drawRow(DrawHLineFn  hLine,
                              DrawVLineFn  vLine,
                              DrawCellFn   drawCell,
                              void*        ctx,
                              int16_t      rowH) {
    const int16_t cellAreaH  = rowH + 2 * CELL_PAD;   // interior height (no borders)
    const int16_t tableLineW = static_cast<int16_t>(DISPLAY_W - MARGIN_LEFT - MARGIN_RIGHT);

    // ── Top border ───────────────────────────────────────────────────────────
    hLine(MARGIN_LEFT, _curY, tableLineW, ctx);

    // ── Vertical lines + cell content ────────────────────────────────────────
    const int16_t contentTop = _curY + BORDER_W + CELL_PAD;
    const int cols = (_cellCount < _colCount) ? _cellCount : _colCount;

    for (int i = 0; i < cols; ++i) {
        // Left vertical border of this column
        vLine(_colBorderX[i], _curY + BORDER_W, cellAreaH, ctx);

        // Cell content
        drawCell(_cells[i].text,
                 _cells[i].fontLevel,
                 _colContentX[i],
                 contentTop,
                 _colContentW[i],
                 rowH,
                 ctx);
    }

    // Right outer vertical border (right edge of last column)
    if (_colCount > 0) {
        int16_t rightX = _colBorderX[_colCount - 1]
                       + BORDER_W
                       + _colContentW[_colCount - 1]
                       + 2 * CELL_PAD;
        vLine(rightX, _curY + BORDER_W, cellAreaH, ctx);
    }

    // Advance current Y past this row
    _curY += BORDER_W + cellAreaH;
}

// ---------------------------------------------------------------------------
// endRow() — measure, check overflow, draw if space, signal result.
// ---------------------------------------------------------------------------
TableRenderer::RowResult TableRenderer::endRow(
        DrawHLineFn   hLine,
        DrawVLineFn   vLine,
        MeasureTextFn measure,
        DrawCellFn    drawCell,
        void*         ctx) {

    if (_cellCount == 0) return ROW_OK;

    // First row sets column count and layout
    if (_colCount == 0) {
        _colCount = _cellCount;
        _computeColumnLayout();
    }

    // Measure
    int16_t rowH      = _measureRowH(measure, ctx);
    int16_t cellAreaH = rowH + 2 * CELL_PAD;
    int16_t totalH    = BORDER_W + cellAreaH;  // space this row will consume

    // Overflow check: does this row fit between _curY and _pageBottom?
    // We need at least totalH pixels plus BORDER_W for the final bottom border.
    if (_curY + totalH + BORDER_W > _pageBottom) {
        _pendingRow = true;
        return ROW_PAGE_FULL;
    }

    _pendingRow = false;
    _drawRow(hLine, vLine, drawCell, ctx, rowH);
    return ROW_OK;
}

// ---------------------------------------------------------------------------
// continueOnPage() — resume the table on a new page.
// The buffered row (_cells/_cellCount) is still intact; call endRow() again.
// ---------------------------------------------------------------------------
void TableRenderer::continueOnPage(int16_t newTopPixelY, int16_t newPageBottom) {
    _curY       = newTopPixelY;
    _pageBottom = newPageBottom;
    _pendingRow = false;   // endRow() will re-measure and redraw
}

// ---------------------------------------------------------------------------
// finish() — draw the final bottom border and deactivate.
// Returns the pixel Y immediately below the bottom border.
// ---------------------------------------------------------------------------
int16_t TableRenderer::finish(DrawHLineFn hLine, void* ctx) {
    if (!_active) return _curY;
    hLine(MARGIN_LEFT, _curY,
          static_cast<int16_t>(DISPLAY_W - MARGIN_LEFT - MARGIN_RIGHT), ctx);
    _curY   += BORDER_W;
    _active  = false;
    return _curY;
}
