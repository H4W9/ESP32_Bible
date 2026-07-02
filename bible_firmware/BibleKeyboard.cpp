// BibleKeyboard.cpp
// Touch keyboard for MarauderBible — adapted from ESP32Marauder TouchKeyboard.
// Adds German character keys (ä/Ä  ö/Ö  ü/Ü  ß) to the bottom control row.
// German chars output private byte codes 0x80-0x86 (same as verse storage).

#include "BibleKeyboard.h"

// Shared VLW font pointers (defined in BibleInterface.cpp) — avoids duplicating the
// font arrays in this translation unit. Used to render the options strip X-Small.
extern const uint8_t* g_kb_font_small;   // X-Small
extern const uint8_t* g_kb_font_main;    // normal UI size

#ifdef HAS_TOUCH

#include <string.h>
#include <Arduino.h>

#ifdef HAS_CAP_TOUCH
#  include "ft6336.h"
#else
// XPT2046 threshold (same value used in BibleInterface::pollTouch)
#  define KB_XPT_THRESHOLD 600
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Layout
// ─────────────────────────────────────────────────────────────────────────────
static const int KB_ROWS = 5;   // 4 char rows + 1 control row
static const int KB_COLS = 10;  // all rows are on a 10-column grid

static const char ROW0_ALPHA[] = "1234567890";
static const char ROW1_ALPHA[] = "qwertyuiop";
static const char ROW2_ALPHA[] = "asdfghjkl";
static const char ROW3_ALPHA[] = "zxcvbnm.";  // cols 0-7; cols 8-9 = CAPS key

static const char ROW0_SYM[] = "!@#$%^&*()";
static const char ROW1_SYM[] = "`~-_=+[]{}";
static const char ROW2_SYM[] = "\\|;:'\"<>";
static const char ROW3_SYM[] = ",./?\0\0\0\0";

// Control row (row 4) columns:
//  0 = CANCEL   1 = SYM/ABC   2 = ä/Ä   3 = ö/Ö   4 = ü/Ü   5 = ß
//  6-7 = SPACE (2 cols)   8 = BKSP   9 = OK

enum KbLayout { KB_ALPHA = 0, KB_SYMBOLS };
enum KbResult  { KBR_NONE, KBR_CHANGED, KBR_DONE, KBR_CANCEL, KBR_LAYOUT, KBR_CAPS };

// ─────────────────────────────────────────────────────────────────────────────
// Geometry helpers
// ─────────────────────────────────────────────────────────────────────────────
static inline int16_t kbH(uint16_t sh)   { return (int16_t)(sh / 2); }
static inline int16_t kbY(uint16_t sh)   { return (int16_t)(sh - kbH(sh)); }
static inline int16_t cellW(uint16_t sw) { return (int16_t)(sw / KB_COLS); }
static inline int16_t cellH(uint16_t sh) { return (int16_t)(kbH(sh) / KB_ROWS); }

// Options strip sits between the text area and the keyboard. Row count is dynamic
// (Partial Match, Ignore Punctuation, optional Scope, optional Translation picker).
static const int16_t OPT_ROW_H = 28;  // px per option row
static int16_t g_opt_rows = 3;        // set by bibleKeyboardInput() per mode
static inline int16_t optH()          { return OPT_ROW_H * g_opt_rows; }

// ─────────────────────────────────────────────────────────────────────────────
// Touch polling (raw, no debounce — debounce is handled by the main loop)
// ─────────────────────────────────────────────────────────────────────────────
static bool kb_rawTouch(TFT_eSPI& tft, uint16_t* x, uint16_t* y) {
#ifdef HAS_CAP_TOUCH
    if (!ft6336_update(x, y)) return false;
    // FT6336 reports panel-native (portrait) coords; map them to the active
    // rotation so they match the keyboard's tft.width()/height() layout.
    uint8_t  rot = tft.getRotation() & 3;
    uint16_t W0  = (rot & 1) ? (uint16_t)tft.height() : (uint16_t)tft.width();
    uint16_t H0  = (rot & 1) ? (uint16_t)tft.width()  : (uint16_t)tft.height();
    uint16_t rx = *x, ry = *y;
    switch (rot) {
        case 0: *x = rx;                       *y = ry;                       break;
        case 1: *x = ry;                       *y = (uint16_t)(W0 - 1 - rx);  break;
        case 2: *x = (uint16_t)(W0 - 1 - rx);  *y = (uint16_t)(H0 - 1 - ry);  break;
        case 3: *x = (uint16_t)(H0 - 1 - ry);  *y = rx;                       break;
    }
    return true;
#else
    return tft.getTouch(x, y, KB_XPT_THRESHOLD);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Drawing helpers
// ─────────────────────────────────────────────────────────────────────────────

// UTF-8 encode a Latin-1 code point (0x80..0xFF → 2 bytes) into a 3-byte buffer.
static void kbUtf8(uint16_t uni, char out[3]) {
    if (uni < 0x80) { out[0] = (char)uni; out[1] = 0; }
    else { out[0] = (char)(0xC0 | (uni >> 6)); out[1] = (char)(0x80 | (uni & 0x3F)); out[2] = 0; }
}

// Draw an umlaut key label — the real glyph from the loaded smooth UI font (which
// includes Ä ä Ö ö Ü ü natively), mapped from the base letter.
static void drawUmlautLabel(TFT_eSPI& tft,
                             int16_t kx, int16_t ky, int16_t cw, int16_t ch,
                             char base, uint16_t key_fg, uint16_t key_bg) {
    uint16_t uni;
    switch (base) {
        case 'A': uni = 0xC4; break; case 'a': uni = 0xE4; break;
        case 'O': uni = 0xD6; break; case 'o': uni = 0xF6; break;
        case 'U': uni = 0xDC; break; default:  uni = 0xFC; break;   // u/U
    }
    char u8[3]; kbUtf8(uni, u8);
    int16_t ty = ky + (ch - 16) / 2;
    tft.setTextColor(key_fg, key_bg);
    tft.drawCentreString(u8, kx + cw / 2, ty, 2);
}

// Draw the ß key label — real glyph (U+00DF) from the loaded smooth UI font.
static void drawSzligLabel(TFT_eSPI& tft,
                            int16_t kx, int16_t ky, int16_t cw, int16_t ch,
                            uint16_t key_fg, uint16_t key_bg) {
    char u8[3]; kbUtf8(0xDF, u8);
    int16_t ty = ky + (ch - 16) / 2;
    tft.setTextColor(key_fg, key_bg);
    tft.drawCentreString(u8, kx + cw / 2, ty, 2);
}

// Draw the options strip. Rows in fixed order (only present ones are drawn):
//   Partial Match · Ignore Punctuation · [Scope:] · [Translation "< name >" picker]
static void drawOptions(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                        uint16_t scrW, uint16_t scrH,
                        bool partial_match, bool ignore_punct,
                        bool has_scope, uint8_t scope,
                        const char* scope_label, const char* const* scope_opts,
                        uint8_t scope_count,
                        bool has_trans, const char* trans_label, const char* trans_name) {
    tft.loadFont(g_kb_font_small);   // X-Small for the option labels/buttons
    int16_t  optTy  = ((int16_t)OPT_ROW_H - tft.fontHeight()) / 2;
    if (optTy < 0) optTy = 0;
    int16_t  optY   = kbY(scrH) - optH();
    uint16_t opt_bg = (bg == TFT_WHITE) ? (uint16_t)0xBDF7 : (uint16_t)0x1082;
    uint16_t bdr    = (bg == TFT_WHITE) ? (uint16_t)0x8430 : (uint16_t)0x4208;
    uint16_t hi_bg  = (uint16_t)0x07E0;

    tft.fillRect(0, optY, (int16_t)scrW, optH(), opt_bg);
    tft.drawFastHLine(0, optY, (int16_t)scrW, bdr);

    auto rowLine = [&](int16_t rY) { tft.drawFastHLine(0, rY + OPT_ROW_H, (int16_t)scrW, bdr); };

    auto drawCheckRow = [&](int16_t rY, bool checked, const char* label) {
        int16_t cby = rY + (OPT_ROW_H - 12) / 2;
        tft.drawRect(6, cby, 12, 12, fg);
        tft.fillRect(8, cby + 2, 8, 8, checked ? hi_bg : opt_bg);
        tft.setTextColor(fg, opt_bg);
        tft.drawString(label, 24, rY + optTy, 1);
        rowLine(rY);
    };
    auto drawScopeRow = [&](int16_t rY) {
        static const char* DEF_SCOPES[3] = { "Bible", "Section", "Book" };
        const char* label = scope_label ? scope_label : "Scope:";
        const char* const* opts = scope_opts ? scope_opts : DEF_SCOPES;
        uint8_t cnt = scope_count ? scope_count : 3;
        tft.setTextColor(fg, opt_bg);
        tft.drawString(label, 6, rY + optTy, 1);
        int16_t bx = 6 + (int16_t)tft.textWidth(label, 1) + 8;
        for (uint8_t i = 0; i < cnt; i++) {
            int16_t bw  = (int16_t)tft.textWidth(opts[i], 1) + 8;
            bool    sel = (scope == i);
            tft.fillRoundRect(bx, rY + 4, bw, OPT_ROW_H - 8, 3, sel ? hi_bg : opt_bg);
            tft.drawRoundRect(bx, rY + 4, bw, OPT_ROW_H - 8, 3, sel ? fg : bdr);
            tft.setTextColor(sel ? (uint16_t)TFT_BLACK : fg, sel ? hi_bg : opt_bg);
            tft.drawString(opts[i], bx + 4, rY + optTy, 1);
            bx += bw + 4;
        }
        rowLine(rY);
    };
    // Translation / songbook / dictionary picker: "Label:  < name >" (tap to cycle).
    auto drawPickRow = [&](int16_t rY, const char* label, const char* name) {
        tft.setTextColor(fg, opt_bg);
        tft.drawString(label, 6, rY + optTy, 1);
        char nm[24];                                   // clip long names to fit the row
        strncpy(nm, name ? name : "-", sizeof(nm) - 1);
        nm[sizeof(nm) - 1] = 0;
        size_t nl = strlen(nm);                        // drop a split UTF-8 lead byte
        if (nl > 0 && (uint8_t)nm[nl - 1] >= 0xC0) nm[nl - 1] = 0;
        char b[28];
        snprintf(b, sizeof(b), "< %s >", nm);
        int16_t lx = 6 + (int16_t)tft.textWidth(label, 1) + 6;
        int16_t bw = (int16_t)tft.textWidth(b, 1) + 10;
        if (lx + bw > (int16_t)scrW - 4) bw = (int16_t)scrW - 4 - lx;
        tft.fillRoundRect(lx, rY + 4, bw, OPT_ROW_H - 8, 3, hi_bg);
        tft.drawRoundRect(lx, rY + 4, bw, OPT_ROW_H - 8, 3, fg);
        tft.setTextColor((uint16_t)TFT_BLACK, hi_bg);
        tft.drawString(b, lx + 5, rY + optTy, 1);
        rowLine(rY);
    };

    int16_t rY = optY;
    drawCheckRow(rY, partial_match, "Partial Match");     rY += OPT_ROW_H;
    drawCheckRow(rY, ignore_punct, "Ignore Punctuation"); rY += OPT_ROW_H;
    if (has_scope) { drawScopeRow(rY);                    rY += OPT_ROW_H; }
    if (has_trans) { drawPickRow(rY, trans_label ? trans_label : "Trans:", trans_name); rY += OPT_ROW_H; }

    tft.loadFont(g_kb_font_main);   // restore the normal UI size
}

// Draw the text area (top half of screen minus options strip): title + current buffer.
// German private codes 0x80-0x86 are mapped to their real Unicode glyphs and drawn
// with the loaded smooth UI font (which includes Ä ä Ö ö Ü ü ß).
static void drawTextArea(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                          uint16_t scrW, uint16_t scrH,
                          const char* title, const char* buffer,
                          bool show_opts = false) {
    int16_t areaH = kbY(scrH) - (show_opts ? optH() : 0);
    tft.fillRect(0, 0, (int16_t)scrW, areaH, bg);

    // Leave 7px top margin so umlaut dots (drawn 3px above the character top)
    // have room even without a title line.
    int16_t y = 7;
    if (title && title[0]) {
        tft.setTextColor(TFT_GREEN, bg);
        tft.drawString(title, 4, y, 2);
        y += 22;
    }

    tft.setTextColor(fg, bg);
    // Draw the buffer with the loaded smooth UI font. Private umlaut codes map to
    // their real Unicode glyph; drawGlyph advances the cursor for us.
    tft.setCursor(4, y);
    const char* p = buffer ? buffer : "";
    for (; *p && tft.getCursorX() < (int16_t)scrW - 10; p++) {
        uint8_t c = (uint8_t)*p;
        uint16_t uni;
        switch (c) {
            case 0x80: uni = 0xC4; break; case 0x81: uni = 0xE4; break;
            case 0x82: uni = 0xD6; break; case 0x83: uni = 0xF6; break;
            case 0x84: uni = 0xDC; break; case 0x85: uni = 0xFC; break;
            case 0x86: uni = 0xDF; break; default:   uni = c;    break;
        }
        tft.drawGlyph(uni);
    }
    int16_t x = tft.getCursorX();
    // Blinking cursor bar
    tft.fillRect(x, y, 2, 18, fg);
}

// Draw the full keyboard.
static void drawKeyboard(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                          uint16_t scrW, uint16_t scrH,
                          KbLayout layout, bool caps) {
    int16_t kY = kbY(scrH);
    int16_t kH = kbH(scrH);
    int16_t cW = cellW(scrW);
    int16_t cH = cellH(scrH);

    // Slight tone difference between keyboard area and content area
    uint16_t key_bg = (bg == TFT_WHITE) ? (uint16_t)0xBDF7 : (uint16_t)0x1082;
    uint16_t key_fg = fg;
    uint16_t bdr    = (bg == TFT_WHITE) ? (uint16_t)0x8430 : (uint16_t)0x4208;

    tft.fillRect(0, kY, (int16_t)scrW, kH, key_bg);

    const char* alphaRows[4] = { ROW0_ALPHA, ROW1_ALPHA, ROW2_ALPHA, ROW3_ALPHA };
    const char* symRows[4]   = { ROW0_SYM,   ROW1_SYM,   ROW2_SYM,   ROW3_SYM   };
    const char** rows = (layout == KB_ALPHA) ? alphaRows : symRows;

    // ── Rows 0-3: character rows ──────────────────────────────────────────
    for (int r = 0; r < 4; r++) {
        const char* row    = rows[r];
        int         rowLen = (int)strlen(row);
        int16_t     rowY   = kY + (int16_t)r * cH;

        // Centre rows except alpha row 3 which is left-aligned for the CAPS key
        int16_t xOff;
        if (layout == KB_ALPHA && r == 3) {
            xOff = 0;
        } else {
            xOff = (int16_t)((KB_COLS - rowLen) * cW / 2);
        }

        for (int i = 0; i < rowLen; i++) {
            int16_t kx = (int16_t)i * cW + xOff;
            tft.drawRect(kx, rowY, cW, cH, bdr);

            char c = row[i];
            if (layout == KB_ALPHA && r >= 1 && c >= 'a' && c <= 'z' && caps)
                c = (char)(c - 'a' + 'A');

            // Use font 2 (16px) for character keys — single character always fits.
            char c_str[2] = { c, '\0' };
            int16_t ty = rowY + (cH - 16) / 2;
            tft.setTextColor(key_fg, key_bg);
            tft.drawCentreString(c_str, kx + cW / 2, ty, 2);
        }

        // CAPS key occupies cols 8-9 in alpha layout row 3
        if (layout == KB_ALPHA && r == 3) {
            int16_t cx  = 8 * cW;
            int16_t cw2 = 2 * cW;
            uint16_t caps_bg = caps ? (uint16_t)0x07E0 : key_bg;
            uint16_t caps_fg = caps ? (uint16_t)TFT_BLACK : key_fg;
            tft.fillRect(cx, rowY, cw2, cH, caps_bg);
            tft.drawRect(cx, rowY, cw2, cH, bdr);
            tft.setTextColor(caps_fg, caps_bg);
            tft.drawCentreString(caps ? "caps" : "CAPS", cx + cW, rowY + (cH - 16) / 2, 2);
        }
    }

    // ── Row 4: control row ────────────────────────────────────────────────
    int16_t rowY = kY + 4 * cH;

    int16_t ctrlY = rowY + (cH - 16) / 2;   // top of font-2 text, vertically centred in row

    // Col 0: CANCEL (red X) — font 2
    tft.drawRect(0, rowY, cW, cH, bdr);
    tft.setTextColor(TFT_RED, key_bg);
    tft.drawCentreString("X", cW / 2, ctrlY, 2);

    // Col 1: SYM / ABC toggle — centered like the other control-row keys.
    tft.drawRect(cW, rowY, cW, cH, bdr);
    tft.setTextColor(key_fg, key_bg);
    tft.drawCentreString(layout == KB_ALPHA ? "SYM" : "ABC", cW + cW/2, ctrlY, 2);

    // Col 2: ä / Ä
    tft.drawRect(2*cW, rowY, cW, cH, bdr);
    drawUmlautLabel(tft, 2*cW, rowY, cW, cH, caps ? 'A' : 'a', key_fg, key_bg);

    // Col 3: ö / Ö
    tft.drawRect(3*cW, rowY, cW, cH, bdr);
    drawUmlautLabel(tft, 3*cW, rowY, cW, cH, caps ? 'O' : 'o', key_fg, key_bg);

    // Col 4: ü / Ü
    tft.drawRect(4*cW, rowY, cW, cH, bdr);
    drawUmlautLabel(tft, 4*cW, rowY, cW, cH, caps ? 'U' : 'u', key_fg, key_bg);

    // Col 5: ß (no uppercase form)
    tft.drawRect(5*cW, rowY, cW, cH, bdr);
    drawSzligLabel(tft, 5*cW, rowY, cW, cH, key_fg, key_bg);

    // Cols 6-7: SPACE (double-wide) — font 2
    tft.fillRect(6*cW, rowY, 2*cW, cH, key_bg);
    tft.drawRect(6*cW, rowY, 2*cW, cH, bdr);
    tft.setTextColor(key_fg, key_bg);
    tft.drawCentreString("SPC", 7*cW, ctrlY, 2);

    // Col 8: BKSP — pixel left-arrow glyph (arrowhead + shaft)
    tft.drawRect(8*cW, rowY, cW, cH, bdr);
    {
        int16_t ax = 8*cW + cW/2 - 5;  // left edge of arrow, centred in cell
        int16_t ay = rowY + cH/2;       // vertical centre of cell
        // Left-pointing arrowhead: tip at ax+0 (centre row), flat base at ax+3 (all rows).
        // Each row starts at ax+3 and grows LEFT toward the tip as rows approach centre.
        tft.fillRect(ax+3, ay-3, 1, 1, key_fg);  // 1px — top tip
        tft.fillRect(ax+2, ay-2, 2, 1, key_fg);  // 2px
        tft.fillRect(ax+1, ay-1, 3, 1, key_fg);  // 3px
        tft.fillRect(ax+0, ay+0, 4, 1, key_fg);  // 4px — includes pointed tip
        tft.fillRect(ax+1, ay+1, 3, 1, key_fg);  // 3px
        tft.fillRect(ax+2, ay+2, 2, 1, key_fg);  // 2px
        tft.fillRect(ax+3, ay+3, 1, 1, key_fg);  // 1px — bottom tip
        // Shaft: 1px tall, from arrowhead base rightward
        tft.fillRect(ax+4, ay+0, 6, 1, key_fg);
    }

    // Col 9: OK (green) — font 2
    tft.fillRect(9*cW, rowY, cW, cH, (uint16_t)0x07E0);
    tft.drawRect(9*cW, rowY, cW, cH, bdr);
    tft.setTextColor((uint16_t)TFT_BLACK, (uint16_t)0x07E0);
    tft.drawCentreString("OK", 9*cW + cW/2, ctrlY, 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Buffer helpers
// ─────────────────────────────────────────────────────────────────────────────
static bool appendByte(char* buf, size_t bufLen, char c) {
    size_t len = strlen(buf);
    if (len + 1 < bufLen) { buf[len] = c; buf[len+1] = '\0'; return true; }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Touch-event handler — returns what action occurred
// ─────────────────────────────────────────────────────────────────────────────
static KbResult handleKbTouch(uint16_t tx, uint16_t ty,
                               uint16_t scrW, uint16_t scrH,
                               char* buffer, size_t bufLen,
                               KbLayout layout, bool caps) {
    int16_t kY = kbY(scrH);
    int16_t kH = kbH(scrH);
    int16_t cW = cellW(scrW);
    int16_t cH = cellH(scrH);

    if ((int16_t)ty < kY || (int16_t)ty >= kY + kH) return KBR_NONE;

    int row = ((int16_t)ty - kY) / cH;
    if (row < 0 || row >= KB_ROWS) return KBR_NONE;

    const char* alphaRows[4] = { ROW0_ALPHA, ROW1_ALPHA, ROW2_ALPHA, ROW3_ALPHA };
    const char* symRows[4]   = { ROW0_SYM,   ROW1_SYM,   ROW2_SYM,   ROW3_SYM   };
    const char** rows = (layout == KB_ALPHA) ? alphaRows : symRows;

    // ── Character rows 0-3 ───────────────────────────────────────────────
    if (row <= 3) {
        const char* rowStr = rows[row];
        int rowLen = (int)strlen(rowStr);

        if (layout == KB_ALPHA && row == 3) {
            // Cols 8-9 = CAPS
            if ((int16_t)tx >= 8 * cW) return KBR_CAPS;
            // Cols 0-7 = chars
            int col = (int16_t)tx / cW;
            if (col < 0 || col >= rowLen) return KBR_NONE;
            char c = rowStr[col];
            if (c >= 'a' && c <= 'z' && caps) c = (char)(c - 'a' + 'A');
            return appendByte(buffer, bufLen, c) ? KBR_CHANGED : KBR_NONE;
        }

        // All other rows: centred
        int16_t xOff = (int16_t)((KB_COLS - rowLen) * cW / 2);
        int col = ((int16_t)tx - xOff) / cW;
        if (col < 0 || col >= rowLen) return KBR_NONE;
        char c = rowStr[col];
        if (layout == KB_ALPHA && row >= 1 && c >= 'a' && c <= 'z' && caps)
            c = (char)(c - 'a' + 'A');
        return appendByte(buffer, bufLen, c) ? KBR_CHANGED : KBR_NONE;
    }

    // ── Control row 4 ────────────────────────────────────────────────────
    int col = (int16_t)tx / cW;
    switch (col) {
        case 0:  return KBR_CANCEL;
        case 1:  return KBR_LAYOUT;
        case 2:  // ä or Ä
            return appendByte(buffer, bufLen, (char)(caps ? 0x80 : 0x81)) ? KBR_CHANGED : KBR_NONE;
        case 3:  // ö or Ö
            return appendByte(buffer, bufLen, (char)(caps ? 0x82 : 0x83)) ? KBR_CHANGED : KBR_NONE;
        case 4:  // ü or Ü
            return appendByte(buffer, bufLen, (char)(caps ? 0x84 : 0x85)) ? KBR_CHANGED : KBR_NONE;
        case 5:  // ß (no uppercase)
            return appendByte(buffer, bufLen, (char)0x86) ? KBR_CHANGED : KBR_NONE;
        case 6:  // SPACE (left cell of double-wide)
        case 7:  // SPACE (right cell)
            return appendByte(buffer, bufLen, ' ') ? KBR_CHANGED : KBR_NONE;
        case 8: {  // BKSP
            size_t len = strlen(buffer);
            if (len > 0) { buffer[len - 1] = '\0'; return KBR_CHANGED; }
            return KBR_NONE;
        }
        case 9:  return KBR_DONE;
        default: return KBR_NONE;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
bool bibleKeyboardInput(TFT_eSPI& tft,
                        uint16_t  fg,
                        uint16_t  bg,
                        char*     buffer,
                        size_t    bufLen,
                        const char* title,
                        bool*       partial_match,
                        bool*       ignore_punct,
                        uint8_t*    scope,
                        const char*        scope_label,
                        const char* const* scope_opts,
                        uint8_t            scope_count,
                        const char* const* dict_names,
                        uint8_t            dict_count,
                        uint8_t*           dict_sel,
                        const char*        dict_label) {
    if (!buffer || bufLen < 2) return false;

    uint16_t scrW      = (uint16_t)tft.width();
    uint16_t scrH      = (uint16_t)tft.height();
    const bool has_scope = (scope != nullptr);
    const bool has_trans = (dict_sel != nullptr && dict_count > 1);  // picker only if >1
    bool     show_opts = (partial_match != nullptr || ignore_punct != nullptr ||
                          has_scope || has_trans);
    g_opt_rows = 2 + (has_scope ? 1 : 0) + (has_trans ? 1 : 0);      // pm + ip + extras

    KbLayout layout = KB_ALPHA;
    bool     caps   = false;

    auto curDictName = [&]() -> const char* {
        if (dict_names && dict_sel && *dict_sel < dict_count) return dict_names[*dict_sel];
        return nullptr;
    };

    drawTextArea(tft, fg, bg, scrW, scrH, title, buffer, show_opts);
    if (show_opts) {
        bool    pm = partial_match ? *partial_match : true;
        bool    ip = ignore_punct  ? *ignore_punct  : true;
        uint8_t sc = scope         ? *scope         : 0;
        drawOptions(tft, fg, bg, scrW, scrH, pm, ip,
                    has_scope, sc, scope_label, scope_opts, scope_count,
                    has_trans, dict_label, curDictName());
    }
    drawKeyboard(tft, fg, bg, scrW, scrH, layout, caps);

    uint32_t lastTouch = 0;
    const uint32_t debounce = 150;

    // Options strip bounds (only meaningful when show_opts is true)
    int16_t optY   = kbY(scrH) - optH();
    int16_t optEnd = kbY(scrH);

    for (;;) {
        uint16_t tx = 0, ty = 0;
        if (kb_rawTouch(tft, &tx, &ty)) {
            uint32_t now = millis();
            if (now - lastTouch < debounce) { delay(5); continue; }
            lastTouch = now;

            // Touch in options strip — toggle/cycle the tapped row.
            if (show_opts && (int16_t)ty >= optY && (int16_t)ty < optEnd) {
                int row = ((int16_t)ty - optY) / OPT_ROW_H;
                if (row == 0 && partial_match) {
                    *partial_match = !(*partial_match);
                } else if (row == 1 && ignore_punct) {
                    *ignore_punct = !(*ignore_punct);
                } else {
                    int r = 2;   // rows after the two checkboxes, in draw order
                    if (has_scope) {
                        if (row == r) {
                            uint8_t cnt = scope_count ? scope_count : 3;
                            *scope = (uint8_t)((*scope + 1) % cnt);
                        }
                        r++;
                    }
                    if (has_trans && row == r) {
                        *dict_sel = (uint8_t)((*dict_sel + 1) % dict_count);
                    }
                }
                bool    pm = partial_match ? *partial_match : true;
                bool    ip = ignore_punct  ? *ignore_punct  : true;
                uint8_t sc = scope         ? *scope         : 0;
                drawOptions(tft, fg, bg, scrW, scrH, pm, ip,
                            has_scope, sc, scope_label, scope_opts, scope_count,
                            has_trans, dict_label, curDictName());
                continue;
            }

            KbResult r = handleKbTouch(tx, ty, scrW, scrH, buffer, bufLen, layout, caps);
            switch (r) {
                case KBR_CHANGED:
                    drawTextArea(tft, fg, bg, scrW, scrH, title, buffer, show_opts);
                    break;
                case KBR_DONE:
                    drawTextArea(tft, fg, bg, scrW, scrH, title, buffer, show_opts);
                    return true;
                case KBR_CANCEL:
                    return false;
                case KBR_LAYOUT:
                    layout = (layout == KB_ALPHA) ? KB_SYMBOLS : KB_ALPHA;
                    drawKeyboard(tft, fg, bg, scrW, scrH, layout, caps);
                    break;
                case KBR_CAPS:
                    caps = !caps;
                    drawKeyboard(tft, fg, bg, scrW, scrH, layout, caps);
                    break;
                default: break;
            }
        }
        delay(5);
        yield();
    }
}

#endif // HAS_TOUCH
