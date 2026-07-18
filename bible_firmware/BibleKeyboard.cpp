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

// Symbol page row 0 holds the German umlauts (they used to sit in the control row;
// moving them here frees that row for wider SPACE / BKSP / OK keys). Bytes are the
// private codes used in verse storage so a search compares directly: 0x81=ä 0x83=ö
// 0x85=ü 0x86=ß. With CAPS on, ä/ö/ü emit 0x80/0x82/0x84 (ß has no uppercase).
// Row 0 keeps the umlauts company with the most-used marks — the row was only half
// full — which frees slots further down so every punctuation character from the
// original symbol page is still here (nothing dropped). Row 3 is capped at 8 keys
// because CAPS sits at cols 8-9 of it, matching its position on the alpha page; the
// two spare slots go to row 2, which is centred.
static const char ROW0_SYM[] = { (char)0x81, (char)0x83, (char)0x85, (char)0x86,
                                 '!', '?', '.', ',', ';', ':', 0 }; // ä ö ü ß ! ? . , ; :
static const char ROW1_SYM[] = "'\"-_^|@#$%";
static const char ROW2_SYM[] = "()[]{}<>";
static const char ROW3_SYM[] = "/\\&*+=`~";

// Fraktur symbol page — used only when a Fraktur songbook is open. Exposes the
// blackletter ligatures and German typographic marks so they can be searched.
// Bytes emitted match verse-storage encoding so the search compares directly:
//   '#'=long-s ſ  '|'=tz  0xA1=ch  0xBF=ck  0xB4=´
//   0x87=„ 0x88=" 0x89=‚ 0x8A=' 0x8B=' 0x8C=– 0x8D=— 0x8E=…
// Row 0 is the umlauts plus the blackletter ligatures; row 1 is the typographic set.
// Row 3 is capped at 8 keys to leave cols 8-9 for CAPS (same position as the alpha
// page), so the slack lands in row 2, which is centred.
static const char FROW0_SYM[] = { (char)0x81, (char)0x83, (char)0x85, (char)0x86,
                                  '#', '|', (char)0xA1, (char)0xBF,
                                  (char)0xB4, '-', 0 };  // ä ö ü ß ſ tz ch ck ´ -
static const char FROW1_SYM[] = { (char)0x87, (char)0x88, (char)0x89, (char)0x8A,
                                  (char)0x8B, (char)0x8C, (char)0x8D, (char)0x8E,
                                  '\'', '"', 0 };        // „ " ‚ ' ' – — … ' "
static const char FROW2_SYM[] = ".,!?:;/";               // 7 keys, centred
static const char FROW3_SYM[] = "()@&*+=_";              // 8 keys, CAPS at cols 8-9

// Control row (row 4) — now only 5 keys (the umlauts moved to the symbol page):
//  CANCEL(1.5)  SYM/ABC(1.5)  SPACE(4)  BKSP(1.5)  OK(1.5) = 10 cells

enum KbLayout { KB_ALPHA = 0, KB_SYMBOLS };

// ─────────────────────────────────────────────────────────────────────────────
// Control row (row 4) geometry — shared by the drawing and hit-test code so they
// can never drift. With the umlauts moved to the symbol page this is the FlipSocial
// layout: SPACE is a 4-cell bar and the four side keys are 1.5 cells each, which
// pushes OK away from BKSP so an OK meant as backspace is far less likely.
//   CANCEL(1.5)  SYM/ABC(1.5)  SPACE(4)  BKSP(1.5)  OK(1.5) = 10 cells
// ─────────────────────────────────────────────────────────────────────────────
enum KeyKind { KK_NONE, KK_CHAR, KK_CAPS, KK_CANCEL, KK_LAYOUT,
               KK_SPACE, KK_BKSP, KK_OK };
struct CtrlKey { KeyKind kind; int16_t x, w; };
static const int KB_CTRL_N = 5;
static void kbCtrlKeys(int16_t cW, CtrlKey out[KB_CTRL_N]) {
    int16_t hw = cW / 2;                                    // half a cell
    out[0] = { KK_CANCEL, 0,                      (int16_t)(cW + hw) };
    out[1] = { KK_LAYOUT, (int16_t)(cW + hw),     (int16_t)(cW + hw) };
    out[2] = { KK_SPACE,  (int16_t)(3 * cW),      (int16_t)(4 * cW)  };
    out[3] = { KK_BKSP,   (int16_t)(7 * cW),      (int16_t)(cW + hw) };
    out[4] = { KK_OK,     (int16_t)(8 * cW + hw), (int16_t)(cW + hw) };
}

// Apply CAPS to a symbol-page umlaut byte (ä ö ü → Ä Ö Ü; ß unchanged).
static inline char kbUmlautCase(char c, bool caps) {
    if (!caps) return c;
    switch ((uint8_t)c) {
        case 0x81: return (char)0x80;   // ä → Ä
        case 0x83: return (char)0x82;   // ö → Ö
        case 0x85: return (char)0x84;   // ü → Ü
        default:   return c;            // ß and everything else
    }
}
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

// Active "main" font for keys + typed text. Normally the shared UI font, but a
// Fraktur songbook swaps in the blackletter font so keys/text preview correctly.
static const uint8_t* g_kb_main_font = nullptr;
static bool           g_frak_kb      = false;   // Fraktur symbol page + glyph mapping

// Verse-storage private byte → Unicode for rendering, mirroring
// BibleInterface::vlwPrivToUnicode(). ASCII and raw Latin-1 (0xA1 ch, 0xBF ck,
// 0xB4 ´) pass through — the Fraktur font maps them to blackletter ligatures.
static uint16_t kbPrivToUnicode(uint8_t c) {
    switch (c) {
        case 0x80: return 0x00C4; case 0x81: return 0x00E4;
        case 0x82: return 0x00D6; case 0x83: return 0x00F6;
        case 0x84: return 0x00DC; case 0x85: return 0x00FC;
        case 0x86: return 0x00DF;
        case 0x87: return 0x201E; case 0x88: return 0x201C;
        case 0x89: return 0x201A; case 0x8A: return 0x2018;
        case 0x8B: return 0x2019; case 0x8C: return 0x2013;
        case 0x8D: return 0x2014; case 0x8E: return 0x2026;
        case 0x8F: return 0x2030;
        default:   return c;
    }
}

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

// UTF-8 encode a code point (up to U+FFFF → 3 bytes) into a 4-byte buffer.
static void kbUtf8(uint16_t uni, char out[4]) {
    if (uni < 0x80) { out[0] = (char)uni; out[1] = 0; }
    else if (uni < 0x800) {
        out[0] = (char)(0xC0 | (uni >> 6));
        out[1] = (char)(0x80 | (uni & 0x3F)); out[2] = 0;
    } else {
        out[0] = (char)(0xE0 | (uni >> 12));
        out[1] = (char)(0x80 | ((uni >> 6) & 0x3F));
        out[2] = (char)(0x80 | (uni & 0x3F)); out[3] = 0;
    }
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

    tft.loadFont(g_kb_main_font);   // restore the active main font (UI or Fraktur)
}

// Draw the text area (top half of screen minus options strip): title + current buffer.
// German private codes 0x80-0x86 are mapped to their real Unicode glyphs and drawn
// with the loaded smooth UI font (which includes Ä ä Ö ö Ü ü ß).
// ─────────────────────────────────────────────────────────────────────────────
// Input box: cursor, horizontal scrolling and char count
// A header line (title + "used/max" count) sits above a framed box holding the
// typed text. The cursor can be placed anywhere in the text and long entries
// scroll horizontally, so any part of a long query stays reachable.
// ─────────────────────────────────────────────────────────────────────────────
static const int16_t KB_BOX_H = 26;

// Header height: a title line, or a bare strip that still fits the char count.
static inline int16_t kbHeaderH(const char* title) {
    return (title && title[0]) ? 22 : 20;
}
// Rect of the framed input box, so taps inside it can move the cursor / scroll.
static void kbBoxRect(uint16_t scrW, const char* title,
                      int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
    x = 4;
    y = (int16_t)(7 + kbHeaderH(title));
    w = (int16_t)scrW - 8;
    h = KB_BOX_H;
}
// Width of one buffer byte as drawn — private code → real glyph in the ACTIVE font.
// The caller must have the typed-text font loaded.
static int16_t kbCharW(TFT_eSPI& tft, uint8_t c) {
    char u8[4]; kbUtf8(kbPrivToUnicode(c), u8);
    return (int16_t)tft.textWidth(u8);
}
// Width of buffer[a..b) as drawn.
static int16_t kbSubW(TFT_eSPI& tft, const char* buf, size_t a, size_t b) {
    int16_t w = 0;
    for (size_t i = a; i < b && buf[i]; i++) w += kbCharW(tft, (uint8_t)buf[i]);
    return w;
}
// Character index nearest a tap `relx` px from the text's left edge.
static size_t kbIndexAt(TFT_eSPI& tft, const char* buf, size_t viewStart, int16_t relx) {
    size_t n = strlen(buf);
    if (relx <= 0) return viewStart;
    int16_t acc = 0;
    for (size_t i = viewStart; i < n; i++) {
        int16_t cw = kbCharW(tft, (uint8_t)buf[i]);
        if (relx < acc + cw / 2) return i;
        acc += cw;
    }
    return n;
}
// Largest viewStart that still shows the end of the text within maxW — clamps
// manual swipe-scrolling so it can't run off past the last character.
static size_t kbMaxView(TFT_eSPI& tft, const char* buf, int16_t maxW) {
    size_t s = strlen(buf);
    int16_t w = 0;
    while (s > 0) {
        int16_t cw = kbCharW(tft, (uint8_t)buf[s - 1]);
        if (w + cw > maxW) break;
        w += cw; s--;
    }
    return s;
}
// Insert / delete AT THE CURSOR (not just at the end of the buffer).
static bool kbInsertByte(char* buf, size_t bufLen, size_t& cursor, char c) {
    size_t len = strlen(buf);
    if (len + 1 >= bufLen) return false;
    if (cursor > len) cursor = len;
    memmove(buf + cursor + 1, buf + cursor, len - cursor + 1);   // shift incl. NUL
    buf[cursor] = c;
    cursor++;
    return true;
}
static bool kbBackspaceAt(char* buf, size_t& cursor) {
    if (cursor == 0) return false;
    size_t len = strlen(buf);
    memmove(buf + cursor - 1, buf + cursor, len - cursor + 1);
    cursor--;
    return true;
}

// Partial redraw for typing / cursor moves: only the box interior (text + caret)
// and the char count. No full-area clear, so the top doesn't flash.
static void drawTextLine(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                         uint16_t scrW, const char* title, const char* buffer,
                         size_t cursor, size_t& viewStart, size_t bufLen,
                         bool followCursor = true) {
    int16_t bx, by, bw, bh; kbBoxRect(scrW, title, bx, by, bw, bh);
    int16_t tx   = bx + 4;
    int16_t ty   = by + (bh - 18) / 2;
    int16_t maxW = bw - 10;
    const char* p = buffer ? buffer : "";
    size_t n = strlen(p);
    if (cursor > n) cursor = n;

    // Typed text uses the active font (Fraktur for a Fraktur songbook).
    tft.loadFont(g_kb_main_font);
    tft.fillRect(bx + 1, by + 1, bw - 2, bh - 2, bg);      // clear box interior only

    // Follow mode keeps the cursor inside the window; manual (swipe) mode honours
    // the caller's viewStart as-is.
    if (followCursor) {
        if (cursor < viewStart) viewStart = cursor;
        while (viewStart < cursor && kbSubW(tft, p, viewStart, cursor) > maxW) viewStart++;
    }

    // Draw the visible glyphs, taking the caret position from the REAL pen position
    // rather than re-measuring. drawGlyph() advances the cursor by the font's own
    // advance; summing textWidth() per character can differ from that (side bearings,
    // multi-byte codepoints), and in the Fraktur font the error accumulated so the
    // caret drifted further from the text with every keystroke.
    tft.setTextColor(fg, bg);
    tft.setCursor(tx, ty);
    int16_t caretX = -1;
    for (size_t i = viewStart; i <= n; i++) {
        int16_t penX = tft.getCursorX();
        if (i == cursor) caretX = penX;              // exact: same advance as drawn
        if (i >= n) break;
        if (penX + kbCharW(tft, (uint8_t)p[i]) > tx + maxW) break;   // would overflow
        tft.drawGlyph(kbPrivToUnicode((uint8_t)p[i]));
    }

    // Caret — only when the cursor fell inside the drawn (visible) window.
    if (caretX >= 0 && caretX <= bx + bw - 3)
        tft.fillRect(caretX, ty, 2, 18, fg);

    // Char count "used/max" at the top-right of the header (normal UI font).
    tft.loadFont(g_kb_font_main);
    char cnt[20];
    snprintf(cnt, sizeof(cnt), "%u/%u", (unsigned)n, (unsigned)(bufLen ? bufLen - 1 : 0));
    int16_t cwid = (int16_t)tft.textWidth(cnt, 2);
    tft.fillRect((int16_t)scrW - 8 - cwid, 4, cwid + 6, 18, bg);
    tft.setTextColor((uint16_t)0x8410, bg);                // gray: legible either theme
    tft.setTextDatum(TR_DATUM);
    tft.drawString(cnt, (int16_t)scrW - 6, 6, 2);
    tft.setTextDatum(TL_DATUM);
    tft.loadFont(g_kb_main_font);
}

// Full redraw of the top area: title, box frame, typed text + caret + count.
static void drawTextArea(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                          uint16_t scrW, uint16_t scrH,
                          const char* title, const char* buffer,
                          size_t cursor, size_t& viewStart, size_t bufLen,
                          bool show_opts = false) {
    int16_t areaH = kbY(scrH) - (show_opts ? optH() : 0);
    tft.fillRect(0, 0, (int16_t)scrW, areaH, bg);

    // Leave 7px top margin so umlaut dots (drawn 3px above the character top)
    // have room even without a title line.
    if (title && title[0]) {
        // Title is a fixed UI prompt ("Search Songs:") — always the normal font,
        // never Fraktur, even for a Fraktur songbook.
        tft.loadFont(g_kb_font_main);
        tft.setTextColor(TFT_GREEN, bg);
        tft.drawString(title, 4, 7, 2);
    }

    int16_t bx, by, bw, bh; kbBoxRect(scrW, title, bx, by, bw, bh);
    tft.drawRect(bx, by, bw, bh, fg);
    drawTextLine(tft, fg, bg, scrW, title, buffer, cursor, viewStart, bufLen);
}

// Draw the full keyboard.
//   upper    — render letters uppercase (caps-lock OR a pending one-shot shift)
//   capsMode — CAPS key look: 0 off, 1 shift-once, 2 caps-lock
static void drawKeyboard(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                          uint16_t scrW, uint16_t scrH,
                          KbLayout layout, bool caps, int capsMode) {
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
    const char* symRows[4];
    if (g_frak_kb) { symRows[0]=FROW0_SYM; symRows[1]=FROW1_SYM; symRows[2]=FROW2_SYM; symRows[3]=FROW3_SYM; }
    else           { symRows[0]=ROW0_SYM;  symRows[1]=ROW1_SYM;  symRows[2]=ROW2_SYM;  symRows[3]=ROW3_SYM;  }
    const char** rows = (layout == KB_ALPHA) ? alphaRows : symRows;

    // ── Rows 0-3: character rows ──────────────────────────────────────────
    for (int r = 0; r < 4; r++) {
        const char* row    = rows[r];
        int         rowLen = (int)strlen(row);
        int16_t     rowY   = kY + (int16_t)r * cH;

        // CAPS lives at cols 8-9 of row 3 on BOTH pages, so it never moves under your
        // thumb when you switch between abc and sym. That row is left-aligned to make
        // space for it; every other row is centred.
        bool rowHasCaps = (r == 3);
        int16_t xOff = rowHasCaps ? 0
                                  : (int16_t)((KB_COLS - rowLen) * cW / 2);

        for (int i = 0; i < rowLen; i++) {
            int16_t kx = (int16_t)i * cW + xOff;
            tft.drawRect(kx, rowY, cW, cH, bdr);

            char c = row[i];
            if (layout == KB_ALPHA && r >= 1 && c >= 'a' && c <= 'z' && caps)
                c = (char)(c - 'a' + 'A');
            else if (layout == KB_SYMBOLS && r == 0)
                c = kbUmlautCase(c, caps);      // symbol row 0 = ä ö ü ß

            int16_t ty = rowY + (cH - 16) / 2;
            tft.setTextColor(key_fg, key_bg);
            if ((uint8_t)c >= 0x80) {
                // Private/Latin-1 byte (umlaut, ligature, typographic mark) → real glyph.
                char u8[4]; kbUtf8(kbPrivToUnicode((uint8_t)c), u8);
                tft.drawCentreString(u8, kx + cW / 2, ty, 2);
            } else {
                // Font 2 (16px) for character keys — single character always fits.
                char c_str[2] = { c, '\0' };
                tft.drawCentreString(c_str, kx + cW / 2, ty, 2);
            }
        }

        // CAPS key occupies cols 8-9 of the left-aligned row
        if (rowHasCaps) {
            int16_t cx  = 8 * cW;
            int16_t cw2 = 2 * cW;
            // Three states: off = plain "CAPS", shift-once = yellow "Caps"
            // (uppercases only the next letter), caps-lock = green "CAPS".
            uint16_t caps_bg = (capsMode == 2) ? (uint16_t)0x07E0
                             : (capsMode == 1) ? (uint16_t)0xFFE0 : key_bg;
            uint16_t caps_fg = (capsMode == 0) ? key_fg : (uint16_t)TFT_BLACK;
            const char* caps_lbl = (capsMode == 1) ? "Caps" : "CAPS";
            tft.fillRect(cx, rowY, cw2, cH, caps_bg);
            tft.drawRect(cx, rowY, cw2, cH, bdr);
            tft.loadFont(g_kb_font_main);   // CAPS is a function key — normal font
            tft.setTextColor(caps_fg, caps_bg);
            tft.drawCentreString(caps_lbl, cx + cW, rowY + (cH - 16) / 2, 2);
        }
    }

    // ── Row 4: control row ────────────────────────────────────────────────
    // Function keys (X, SYM/ABC, SPC, OK) stay in the normal font even for a
    // Fraktur songbook; the umlaut keys (ä ö ü ß) are letters, so they render in
    // the active (Fraktur) font like the other letter keys.
    tft.loadFont(g_kb_font_main);
    int16_t rowY  = kY + 4 * cH;
    int16_t ctrlY = rowY + (cH - 16) / 2;   // top of font-2 text, vertically centred

    CtrlKey ck[KB_CTRL_N];
    kbCtrlKeys(cW, ck);

    for (int i = 0; i < KB_CTRL_N; i++) {
        const CtrlKey& k = ck[i];
        int16_t cx = k.x + k.w / 2;
        switch (k.kind) {
            case KK_CANCEL:                                  // red X
                tft.drawRect(k.x, rowY, k.w, cH, bdr);
                tft.setTextColor(TFT_RED, key_bg);
                tft.drawCentreString("X", cx, ctrlY, 2);
                break;
            case KK_LAYOUT:                                  // SYM / ABC toggle
                tft.drawRect(k.x, rowY, k.w, cH, bdr);
                tft.setTextColor(key_fg, key_bg);
                tft.drawCentreString(layout == KB_ALPHA ? "SYM" : "ABC", cx, ctrlY, 2);
                break;
            case KK_SPACE:
                tft.fillRect(k.x, rowY, k.w, cH, key_bg);
                tft.drawRect(k.x, rowY, k.w, cH, bdr);
                tft.setTextColor(key_fg, key_bg);
                tft.drawCentreString("SPC", cx, ctrlY, 2);
                break;
            case KK_BKSP: {                                  // pixel left-arrow glyph
                tft.drawRect(k.x, rowY, k.w, cH, bdr);
                int16_t ax = cx - 5, ay = rowY + cH / 2;
                tft.fillRect(ax+3, ay-3, 1, 1, key_fg);
                tft.fillRect(ax+2, ay-2, 2, 1, key_fg);
                tft.fillRect(ax+1, ay-1, 3, 1, key_fg);
                tft.fillRect(ax+0, ay+0, 4, 1, key_fg);
                tft.fillRect(ax+1, ay+1, 3, 1, key_fg);
                tft.fillRect(ax+2, ay+2, 2, 1, key_fg);
                tft.fillRect(ax+3, ay+3, 1, 1, key_fg);
                tft.fillRect(ax+4, ay+0, 6, 1, key_fg);
                break;
            }
            case KK_OK:                                      // green OK
                tft.fillRect(k.x, rowY, k.w, cH, (uint16_t)0x07E0);
                tft.drawRect(k.x, rowY, k.w, cH, bdr);
                tft.setTextColor((uint16_t)TFT_BLACK, (uint16_t)0x07E0);
                tft.drawCentreString("OK", cx, ctrlY, 2);
                break;
            default: break;
        }
    }

    tft.loadFont(g_kb_main_font);   // restore active font for the typed-text preview
}

// Repaint ONE key in its normal state — clears the momentary press highlight without
// redrawing the whole board (a full redraw is what made the keyboard flash on every
// keystroke). CAPS and SYM/ABC change every key's label, so those still do a full
// drawKeyboard(); everything else only ever repaints the key that was pressed.
static void drawOneKey(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                       KeyKind kind, const int16_t r[4], char ch) {
    if (kind == KK_NONE || r[2] <= 0) return;
    uint16_t key_bg = (bg == TFT_WHITE) ? (uint16_t)0xBDF7 : (uint16_t)0x1082;
    uint16_t key_fg = fg;
    uint16_t bdr    = (bg == TFT_WHITE) ? (uint16_t)0x8430 : (uint16_t)0x4208;
    int16_t x = r[0], y = r[1], w = r[2], h = r[3];
    int16_t cx = x + w / 2, ty = y + (h - 16) / 2;

    tft.fillRect(x, y, w, h, (kind == KK_OK) ? (uint16_t)0x07E0 : key_bg);
    tft.drawRect(x, y, w, h, bdr);

    switch (kind) {
        case KK_CHAR:
            tft.loadFont(g_kb_main_font);          // active font (umlauts / Fraktur)
            tft.setTextColor(key_fg, key_bg);
            if ((uint8_t)ch >= 0x80) {             // private/Latin-1 → real glyph
                char u8[4]; kbUtf8(kbPrivToUnicode((uint8_t)ch), u8);
                tft.drawCentreString(u8, cx, ty, 2);
            } else {
                char s[2] = { ch, '\0' };
                tft.drawCentreString(s, cx, ty, 2);
            }
            break;
        case KK_SPACE:
            tft.loadFont(g_kb_font_main);
            tft.setTextColor(key_fg, key_bg);
            tft.drawCentreString("SPC", cx, ty, 2);
            break;
        case KK_CANCEL:
            tft.loadFont(g_kb_font_main);
            tft.setTextColor(TFT_RED, key_bg);
            tft.drawCentreString("X", cx, ty, 2);
            break;
        case KK_OK:
            tft.loadFont(g_kb_font_main);
            tft.setTextColor((uint16_t)TFT_BLACK, (uint16_t)0x07E0);
            tft.drawCentreString("OK", cx, ty, 2);
            break;
        case KK_BKSP: {                            // pixel left-arrow glyph
            int16_t ax = cx - 5, ay = y + h / 2;
            tft.fillRect(ax+3, ay-3, 1, 1, key_fg);
            tft.fillRect(ax+2, ay-2, 2, 1, key_fg);
            tft.fillRect(ax+1, ay-1, 3, 1, key_fg);
            tft.fillRect(ax+0, ay+0, 4, 1, key_fg);
            tft.fillRect(ax+1, ay+1, 3, 1, key_fg);
            tft.fillRect(ax+2, ay+2, 2, 1, key_fg);
            tft.fillRect(ax+3, ay+3, 1, 1, key_fg);
            tft.fillRect(ax+4, ay+0, 6, 1, key_fg);
            break;
        }
        default: break;
    }
    tft.loadFont(g_kb_main_font);                  // restore the typed-text font
}

// ─────────────────────────────────────────────────────────────────────────────
// Touch-event handler — returns what action occurred
// ─────────────────────────────────────────────────────────────────────────────
// `cursor` is the insertion point (edits happen there, not at the end). When given,
// hitRect[4] receives the pressed key's rect, *hitKind its kind and *hitChar the
// character it produced — enough for the caller to flash the key and then repaint
// just that one key (no full-keyboard redraw, so the board doesn't flash).
static KbResult handleKbTouch(uint16_t tx, uint16_t ty,
                               uint16_t scrW, uint16_t scrH,
                               char* buffer, size_t bufLen, size_t& cursor,
                               KbLayout layout, bool caps,
                               int16_t* hitRect = nullptr, KeyKind* hitKind = nullptr,
                               char* hitChar = nullptr) {
    int16_t kY = kbY(scrH);
    int16_t kH = kbH(scrH);
    int16_t cW = cellW(scrW);
    int16_t cH = cellH(scrH);

    if (hitKind) *hitKind = KK_NONE;
    if ((int16_t)ty < kY || (int16_t)ty >= kY + kH) return KBR_NONE;

    int row = ((int16_t)ty - kY) / cH;
    if (row < 0 || row >= KB_ROWS) return KBR_NONE;
    int16_t rowY = kY + (int16_t)row * cH;

    const char* alphaRows[4] = { ROW0_ALPHA, ROW1_ALPHA, ROW2_ALPHA, ROW3_ALPHA };
    const char* symRows[4];
    if (g_frak_kb) { symRows[0]=FROW0_SYM; symRows[1]=FROW1_SYM; symRows[2]=FROW2_SYM; symRows[3]=FROW3_SYM; }
    else           { symRows[0]=ROW0_SYM;  symRows[1]=ROW1_SYM;  symRows[2]=ROW2_SYM;  symRows[3]=ROW3_SYM;  }
    const char** rows = (layout == KB_ALPHA) ? alphaRows : symRows;

    // ── Character rows 0-3 ───────────────────────────────────────────────
    if (row <= 3) {
        const char* rowStr = rows[row];
        int rowLen = (int)strlen(rowStr);

        // Row 3 carries the CAPS key on both pages: left-aligned with CAPS at
        // cols 8-9; geometry must match drawKeyboard().
        bool rowHasCaps = (row == 3);
        if (rowHasCaps && (int16_t)tx >= 8 * cW) {
            if (hitRect) { hitRect[0] = 8 * cW; hitRect[1] = rowY; hitRect[2] = 2 * cW; hitRect[3] = cH; }
            if (hitKind) *hitKind = KK_CAPS;
            return KBR_CAPS;
        }

        int16_t xOff = rowHasCaps ? 0
                                  : (int16_t)((KB_COLS - rowLen) * cW / 2);
        int col = ((int16_t)tx - xOff) / cW;
        if (col < 0 || col >= rowLen) return KBR_NONE;
        char c = rowStr[col];
        if (layout == KB_ALPHA && row >= 1 && c >= 'a' && c <= 'z' && caps)
            c = (char)(c - 'a' + 'A');
        else if (layout == KB_SYMBOLS && row == 0)
            c = kbUmlautCase(c, caps);      // symbol row 0 = ä ö ü ß
        if (hitRect) { hitRect[0] = (int16_t)col * cW + xOff; hitRect[1] = rowY; hitRect[2] = cW; hitRect[3] = cH; }
        if (hitKind) *hitKind = KK_CHAR;
        if (hitChar) *hitChar = c;
        return kbInsertByte(buffer, bufLen, cursor, c) ? KBR_CHANGED : KBR_NONE;
    }

    // ── Control row 4 ────────────────────────────────────────────────────
    // Geometry comes from the shared kbCtrlKeys() table (keys are no longer a
    // uniform cell wide), and edits happen AT THE CURSOR rather than at the end.
    {
        CtrlKey ck[KB_CTRL_N];
        kbCtrlKeys(cW, ck);
        for (int i = 0; i < KB_CTRL_N; i++) {
            const CtrlKey& k = ck[i];
            if ((int16_t)tx < k.x || (int16_t)tx >= k.x + k.w) continue;
            if (hitRect) { hitRect[0] = k.x; hitRect[1] = rowY; hitRect[2] = k.w; hitRect[3] = cH; }
            if (hitKind) *hitKind = k.kind;
            switch (k.kind) {
                case KK_CANCEL: return KBR_CANCEL;
                case KK_LAYOUT: return KBR_LAYOUT;
                case KK_OK:     return KBR_DONE;
                case KK_SPACE:
                    return kbInsertByte(buffer, bufLen, cursor, ' ') ? KBR_CHANGED : KBR_NONE;
                case KK_BKSP:
                    return kbBackspaceAt(buffer, cursor) ? KBR_CHANGED : KBR_NONE;
                default: return KBR_NONE;
            }
        }
    }
    return KBR_NONE;
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
                        const char*        dict_label,
                        const uint8_t*     frak_font,
                        const bool*        dict_frak) {
    if (!buffer || bufLen < 2) return false;

    // Fraktur songbook: draw keys/typed text in the blackletter font and expose the
    // ligature/typographic symbol page. When a per-option Fraktur table is supplied,
    // the initial state follows the currently-selected picker option (and cycling the
    // picker below switches it live). Otherwise it's simply on iff frak_font is set.
    bool init_frak = (frak_font != nullptr) &&
                     (dict_frak && dict_sel ? dict_frak[*dict_sel] : true);
    g_frak_kb      = init_frak;
    g_kb_main_font = init_frak ? frak_font : g_kb_font_main;
    tft.loadFont(g_kb_main_font);

    uint16_t scrW      = (uint16_t)tft.width();
    uint16_t scrH      = (uint16_t)tft.height();
    const bool has_scope = (scope != nullptr);
    const bool has_trans = (dict_sel != nullptr && dict_count > 1);  // picker only if >1
    bool     show_opts = (partial_match != nullptr || ignore_punct != nullptr ||
                          has_scope || has_trans);
    g_opt_rows = 2 + (has_scope ? 1 : 0) + (has_trans ? 1 : 0);      // pm + ip + extras

    KbLayout layout = KB_ALPHA;
    // CAPS has three states, like the FlipSocial keyboard:
    //   tap        → shift-once (uppercase the next letter only, yellow "Caps")
    //   hold 450ms → caps-lock  (stays uppercase, green "CAPS")
    //   tap while locked → off
    bool     capsLock  = false;
    bool     shiftOnce = false;
    auto upperNow = [&]() { return capsLock || shiftOnce; };
    auto capsMode = [&]() { return capsLock ? 2 : (shiftOnce ? 1 : 0); };
    const uint32_t CAPS_HOLD_MS = 450;

    auto curDictName = [&]() -> const char* {
        if (dict_names && dict_sel && *dict_sel < dict_count) return dict_names[*dict_sel];
        return nullptr;
    };

    size_t cursor    = strlen(buffer);   // insertion point within the text
    size_t viewStart = 0;                // first visible char (horizontal scroll)
    const uint32_t MIN_FLASH_MS = 55;    // keep the press highlight visible this long

    drawTextArea(tft, fg, bg, scrW, scrH, title, buffer, cursor, viewStart, bufLen, show_opts);
    if (show_opts) {
        bool    pm = partial_match ? *partial_match : true;
        bool    ip = ignore_punct  ? *ignore_punct  : true;
        uint8_t sc = scope         ? *scope         : 0;
        drawOptions(tft, fg, bg, scrW, scrH, pm, ip,
                    has_scope, sc, scope_label, scope_opts, scope_count,
                    has_trans, dict_label, curDictName());
    }
    drawKeyboard(tft, fg, bg, scrW, scrH, layout, upperNow(), capsMode());

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
                        // Songs: switch the keyboard font to match the picked songbook.
                        if (frak_font && dict_frak) {
                            bool nf = dict_frak[*dict_sel];
                            if (nf != g_frak_kb) {
                                g_frak_kb      = nf;
                                g_kb_main_font = nf ? frak_font : g_kb_font_main;
                                tft.loadFont(g_kb_main_font);
                                drawKeyboard(tft, fg, bg, scrW, scrH, layout, upperNow(), capsMode());
                                drawTextArea(tft, fg, bg, scrW, scrH, title, buffer,
                                             cursor, viewStart, bufLen, show_opts);
                            }
                        }
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

            // Input box: a tap moves the cursor to that character; a horizontal swipe
            // scrolls the text so any edit point in a long entry stays reachable.
            {
                int16_t bx, by, bw, bh; kbBoxRect(scrW, title, bx, by, bw, bh);
                if ((int16_t)tx >= bx && (int16_t)tx < bx + bw &&
                    (int16_t)ty >= by && (int16_t)ty < by + bh) {
                    tft.loadFont(g_kb_main_font);
                    int16_t maxW    = bw - 10;
                    size_t  maxView = kbMaxView(tft, buffer, maxW);
                    int16_t downX   = (int16_t)tx, lastX = downX;
                    int16_t travel = 0, accum = 0;
                    bool    swiping = false;
                    uint16_t rx = tx, ry = ty;
                    for (;;) {
                        if (!kb_rawTouch(tft, &rx, &ry)) break;      // released
                        int16_t cx = (int16_t)rx;
                        int16_t ad = cx - downX; if (ad < 0) ad = -ad;
                        if (ad > travel) travel = ad;
                        if (travel > 8) swiping = true;              // past jitter → swipe
                        if (swiping) {
                            accum += lastX - cx;                     // >0 : finger went left
                            while (accum > 0 && viewStart < maxView) {
                                int16_t cw = kbCharW(tft, (uint8_t)buffer[viewStart]);
                                if (accum < cw) break;
                                accum -= cw; viewStart++;
                            }
                            while (accum < 0 && viewStart > 0) {
                                int16_t cw = kbCharW(tft, (uint8_t)buffer[viewStart - 1]);
                                if (-accum < cw) break;
                                accum += cw; viewStart--;
                            }
                            lastX = cx;
                            drawTextLine(tft, fg, bg, scrW, title, buffer,
                                         cursor, viewStart, bufLen, false);
                        }
                        delay(8); yield();
                    }
                    if (!swiping) {                                  // tap → place cursor
                        cursor = kbIndexAt(tft, buffer, viewStart, downX - (bx + 4));
                        drawTextLine(tft, fg, bg, scrW, title, buffer,
                                     cursor, viewStart, bufLen);
                    }
                    continue;
                }
            }

            int16_t hit[4] = { 0, 0, 0, 0 };
            KeyKind hk = KK_NONE;
            char    hc = 0;
            KbResult r = handleKbTouch(tx, ty, scrW, scrH, buffer, bufLen, cursor,
                                       layout, upperNow(), hit, &hk, &hc);

            // Momentary press highlight: invert the key, hold it briefly / until
            // release, then restore the board. Holding CAPS past CAPS_HOLD_MS turns
            // it into a caps-lock instead of a one-shot shift.
            bool flashed = false;
            bool held    = false;
            if (hk != KK_NONE && hit[2] > 0) {
                tft.fillRect(hit[0], hit[1], hit[2], hit[3], fg);
                flashed = true;
                uint32_t t0 = millis();
                for (;;) {
                    uint16_t rx2, ry2;
                    bool     down = kb_rawTouch(tft, &rx2, &ry2);
                    uint32_t el   = millis() - t0;
                    if (hk == KK_CAPS && down && el >= CAPS_HOLD_MS) { held = true; break; }
                    if (!down && el >= MIN_FLASH_MS) break;
                    if (el > 1200) break;                 // safety: stuck touch
                    delay(8); yield();
                }
                // A recognised hold: wait for the finger to actually come up so the
                // release isn't re-read as a fresh tap.
                if (held) { uint16_t rx2, ry2; while (kb_rawTouch(tft, &rx2, &ry2)) { delay(8); yield(); } }
            }

            switch (r) {
                case KBR_CHANGED: {
                    // A one-shot shift is consumed by the letter just typed. That flips
                    // every letter's case, so it's the one edit needing a full redraw;
                    // otherwise only the pressed key is repainted (no board flash).
                    bool oneShot = (hk == KK_CHAR) && shiftOnce && !capsLock;
                    if (oneShot) shiftOnce = false;
                    drawTextLine(tft, fg, bg, scrW, title, buffer, cursor, viewStart, bufLen);
                    if (oneShot)      drawKeyboard(tft, fg, bg, scrW, scrH, layout, upperNow(), capsMode());
                    else if (flashed) drawOneKey(tft, fg, bg, hk, hit, hc);
                    break;
                }
                case KBR_DONE:
                    drawTextLine(tft, fg, bg, scrW, title, buffer, cursor, viewStart, bufLen);
                    tft.loadFont(g_kb_font_main);   // restore UI font for later draws
                    return true;
                case KBR_CANCEL:
                    tft.loadFont(g_kb_font_main);   // restore UI font for later draws
                    return false;
                case KBR_LAYOUT:
                    layout = (layout == KB_ALPHA) ? KB_SYMBOLS : KB_ALPHA;
                    drawKeyboard(tft, fg, bg, scrW, scrH, layout, upperNow(), capsMode());
                    break;
                case KBR_CAPS:
                    if (held)          { capsLock = !capsLock; shiftOnce = false; }  // hold → lock
                    else if (capsLock) { capsLock = false; }                         // tap while locked → off
                    else               { shiftOnce = !shiftOnce; }                   // tap → one-shot shift
                    drawKeyboard(tft, fg, bg, scrW, scrH, layout, upperNow(), capsMode());
                    break;
                default:
                    if (flashed) drawOneKey(tft, fg, bg, hk, hit, hc);
                    break;
            }
        }
        delay(5);
        yield();
    }
}

#endif // HAS_TOUCH
