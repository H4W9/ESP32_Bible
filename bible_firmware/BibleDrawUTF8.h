// BibleDrawUTF8.h
// Shared pixel-drawing helpers for German private-code characters.
// Used by BibleInterface.cpp (TFT_eSprite) and BibleKeyboard.cpp (TFT_eSPI).
//
// Both TFT_eSPI and TFT_eSprite expose the same drawChar/fillRect API but
// TFT_eSprite's overrides are NOT virtual, so these functions are templated
// on the display type to bind the correct method at compile time.
//
// Private byte codes (same as verse storage):
//   0x80=Ä  0x81=ä  0x82=Ö  0x83=ö  0x84=Ü  0x85=ü  0x86=ß

#pragma once
#include <stdint.h>

// ─────────────────────────────────────────────────────────────────────────────
// bibleDrawSzlig — draw the ß glyph via fillRect calls.
// Returns the advance width (pixels to next character).
//
//   d     — TFT_eSPI or TFT_eSprite instance (template-deduced)
//   x,y   — top-left of the character cell (same coords as drawChar)
//   font  — 1 (GLCD 8px), 2 (Font16 16px), or 4 (Font32 ~26px)
//   color — RGB565 colour
//
// Glyph designs (y = top of character cell):
//   Font 1 (GLCD 8px)  — 6-col canvas, advance 6
//   Font 2 (Font16 16px) — 8-col canvas, advance 7
//   Font 4 (Font32 26px) — 13-col canvas, advance 13
// ─────────────────────────────────────────────────────────────────────────────
template<typename Tft>
int16_t bibleDrawSzlig(Tft& d, int16_t x, int16_t y, uint8_t font, uint16_t color) {
    if (font == 1) {
        // GLCD 8px — 6-col canvas, advance 6
        d.fillRect(x+1, y+0, 2, 1, color);  // top arch
        d.fillRect(x+0, y+1, 1, 6, color);  // left stem
        d.fillRect(x+3, y+1, 1, 2, color);  // upper right
        d.fillRect(x+2, y+3, 1, 1, color);  // diagonal mid
        d.fillRect(x+3, y+4, 1, 1, color);  // mid right
        d.fillRect(x+4, y+5, 1, 2, color);  // lower right
        d.fillRect(x+2, y+7, 2, 1, color);  // bottom tail
        return 6;
    } else if (font == 2) {
        // Font16 16px — 8-col canvas, advance 7
        d.fillRect(x+1, y+3,  3, 1, color);  // top arch
        d.fillRect(x+0, y+4,  1, 9, color);  // left stem
        d.fillRect(x+4, y+4,  1, 3, color);  // upper right
        d.fillRect(x+2, y+7,  2, 1, color);  // mid junction
        d.fillRect(x+4, y+8,  1, 1, color);  // lower top right
        d.fillRect(x+5, y+9,  1, 3, color);  // lower right wider
        d.fillRect(x+4, y+12, 1, 1, color);  // lower bottom right
        d.fillRect(x+2, y+13, 2, 1, color);  // bottom tail
        return 7;
    } else {
        // Font32 ~26px — 13-col canvas, advance 13
        d.fillRect(x+3,  y+2,  5,  1, color);  // top arch row 1
        d.fillRect(x+2,  y+3,  7,  1, color);  // top arch row 2
        d.fillRect(x+2,  y+4,  2,  1, color);  // upper left corner
        d.fillRect(x+7,  y+4,  3,  1, color);  // upper right corner
        d.fillRect(x+1,  y+5,  2, 13, color);  // left stem 2px wide
        d.fillRect(x+8,  y+5,  3,  1, color);  // upper loop right
        d.fillRect(x+9,  y+6,  2,  2, color);  // upper loop right side
        d.fillRect(x+8,  y+8,  3,  1, color);  // upper loop closes
        d.fillRect(x+5,  y+9,  5,  2, color);  // mid junction
        d.fillRect(x+8,  y+11, 3,  1, color);  // lower loop top
        d.fillRect(x+9,  y+12, 3,  1, color);  // lower loop right
        d.fillRect(x+10, y+13, 2,  4, color);  // lower loop right side
        d.fillRect(x+9,  y+17, 3,  1, color);  // lower loop closes
        d.fillRect(x+5,  y+18, 6,  1, color);  // bottom tail row 1
        d.fillRect(x+5,  y+19, 5,  1, color);  // bottom tail row 2
        return 13;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// bibleDrawUmlautDots — draw two diaeresis dots above an already-drawn letter.
// Call immediately after drawChar to overlay the dots at the correct position.
//
//   d        — TFT_eSPI or TFT_eSprite (template-deduced)
//   x        — left edge of the character (same x passed to drawChar)
//   y        — top of the character cell (same y passed to drawChar)
//   adv      — advance width returned by drawChar (pixels)
//   is_lower — true for ä/ö/ü (lowercase), false for Ä/Ö/Ü
//   font     — 1/2 → 1×1 dots;  4 → 2×2 dots
//   color    — RGB565 dot colour
//
// Dot positions:
//   dx1 = x + adv/3         dx2 = x + 2*adv/3   (or x+1 / x+(adv-2) if adv<6)
//   dot_y = y + 3  (font 2 lowercase),  y + 0  (all other cases)
// ─────────────────────────────────────────────────────────────────────────────
template<typename Tft>
void bibleDrawUmlautDots(Tft& d, int16_t x, int16_t y, int16_t adv,
                          bool is_lower, uint8_t font, uint16_t color) {
    uint8_t dot_sz = (font == 4) ? 2 : 1;
    int16_t dot_y  = y + (is_lower && font == 2 ? 3 : 0);
    int16_t dx1 = x + (adv >= 6 ? adv / 3       : (int16_t)1);
    int16_t dx2 = x + (adv >= 6 ? (2 * adv) / 3 : (int16_t)(adv > 2 ? adv - 2 : 1));
    d.fillRect(dx1, dot_y, dot_sz, dot_sz, color);
    d.fillRect(dx2, dot_y, dot_sz, dot_sz, color);
}
