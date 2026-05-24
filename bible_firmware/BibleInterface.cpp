// BibleInterface.cpp
// Marauder Bible Firmware — Bible reader implementation
//
// Streaming OSIS XML parser reads verses directly from the SD card.
// No Python pre-processing step required.

#include "BibleInterface.h"
#include "BibleKeyboard.h"
#include "BibleDrawUTF8.h"

#ifdef HAS_SCREEN

#include <math.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"

#ifdef HAS_CAP_TOUCH
  #include "ft6336.h"
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Static brightness levels (PWM duty, 8-bit, 0-255)
// ─────────────────────────────────────────────────────────────────────────────
const uint8_t BibleInterface::BL_LEVELS[20] = {
      3,  16,  30,  44,  57,  71,  84,  98,
    111, 125, 138, 152, 165, 179, 192, 206,
    219, 233, 246, 255
};

// ─────────────────────────────────────────────────────────────────────────────
// Accent / highlight colour palette (20 options, ordered by hue, cycled from Settings)
// ─────────────────────────────────────────────────────────────────────────────
// 24 jewel-tone darks (contrast with dim_fg ~medium-gray on dark bg)
// Ordered by hue: greens → cyans → blues → purples → pinks → reds → oranges → yellows
static const uint16_t ACCENT_DARK[] = {
    0x0460,  // Green    RGB(  0,140,  0)
    0x0282,  // Forest   RGB(  0, 80, 16)
    0x03CA,  // Mint     RGB(  0,120, 80)
    0x0350,  // Teal     RGB(  0,106,128)
    0x03D9,  // Cyan     RGB(  0,121,200)
    0x047B,  // Sky      RGB(  0,140,216)
    0x0291,  // Steel    RGB(  0, 80,136)
    0x0339,  // Blue     RGB(  0,100,200)
    0x0213,  // Navy     RGB(  0, 64,152)
    0x2813,  // Indigo   RGB( 40,  0,152)
    0x5016,  // Violet   RGB( 80,  0,176)
    0x500F,  // Purple   RGB( 80,  0,120)
    0x6009,  // Magenta  RGB( 96,  0, 72)
    0xA00C,  // Pink     RGB(160,  0, 96)
    0x8803,  // Crimson  RGB(136,  0, 24)
    0x5800,  // Red      RGB( 88,  0,  0)
    0xB1E3,  // Coral    RGB(176, 56, 24)
    0x6181,  // Brown    RGB( 96, 48,  8)
    0x7280,  // Orange   RGB(112, 80,  0)
    0x8240,  // Amber    RGB(128, 72,  0)
    0xA360,  // Gold     RGB(160,108,  0)
    0xA460,  // Yellow   RGB(160,136,  0)
    0x52E0,  // Olive    RGB( 80, 92,  0)
    0x3504,  // Lime     RGB( 48,160, 32)
};
// 24 pastel lights (contrast with dim_fg ~medium-dark-gray on light bg)
static const uint16_t ACCENT_LIGHT[] = {
    0xB696,  // Green    RGB(180,212,176) sage
    0xB737,  // Forest   RGB(176,228,184)
    0xB79A,  // Mint     RGB(176,240,208)
    0xAF3C,  // Teal     RGB(168,232,224)
    0xBF7D,  // Cyan     RGB(188,240,232)
    0xBEFF,  // Sky      RGB(184,220,248)
    0xB69E,  // Steel    RGB(176,212,240)
    0xBEDF,  // Blue     RGB(188,216,248)
    0xC69F,  // Navy     RGB(192,208,248) periwinkle
    0xCDBF,  // Indigo   RGB(204,180,248) lavender
    0xD5DF,  // Violet   RGB(208,184,248)
    0xE61F,  // Purple   RGB(228,192,248)
    0xFDBB,  // Magenta  RGB(248,180,216)
    0xFDDB,  // Pink     RGB(248,184,216)
    0xFE5A,  // Crimson  RGB(248,200,208) light rose
    0xFDB7,  // Red      RGB(248,180,184) rose
    0xFE15,  // Coral    RGB(248,192,168)
    0xDDF4,  // Brown    RGB(216,188,160)
    0xFD40,  // Orange   RGB(248,168,  0)
    0xFEAD,  // Amber    RGB(248,212,104)
    0xFF12,  // Gold     RGB(248,224,144)
    0xFFD5,  // Yellow   RGB(248,248,168)
    0xCEF3,  // Olive    RGB(200,220,152)
    0xCFF3,  // Lime     RGB(200,252,152)
};
static const char* const ACCENT_NAMES[] = {
    "Green","Forest","Mint","Teal","Cyan","Sky",
    "Steel","Blue","Navy","Indigo","Violet","Purple",
    "Magenta","Pink","Crimson","Red","Coral","Brown",
    "Orange","Amber","Gold","Yellow","Olive","Lime"
};
static const uint8_t ACCENT_COUNT = 24;

// ─────────────────────────────────────────────────────────────────────────────
// Section metadata
// ─────────────────────────────────────────────────────────────────────────────
const uint8_t BibleInterface::SEC_BOOK_START[4]  = { 0, 22, 39, 49 };
const uint8_t BibleInterface::SEC_BOOK_COUNT_[4] = { 22, 17, 10, 27 };

const char* const BibleInterface::SEC_NAME_EN[4] = {
    "Old Testament", "Prophets", "Apocrypha", "New Testament"
};

// ─────────────────────────────────────────────────────────────────────────────
// Book table  { display name, OSIS code, chapter count, section }
// ─────────────────────────────────────────────────────────────────────────────
const BibleBook BibleInterface::BOOKS[BIBLE_BOOK_COUNT] = {
    // ── Old Testament (22) ─────────────────────────────────────────────
    { "Genesis",          "Gen",    50, BIBLE_SEC_OT },
    { "Exodus",           "Exod",   40, BIBLE_SEC_OT },
    { "Leviticus",        "Lev",    27, BIBLE_SEC_OT },
    { "Numbers",          "Num",    36, BIBLE_SEC_OT },
    { "Deuteronomy",      "Deut",   34, BIBLE_SEC_OT },
    { "Joshua",           "Josh",   24, BIBLE_SEC_OT },
    { "Judges",           "Judg",   21, BIBLE_SEC_OT },
    { "Ruth",             "Ruth",    4, BIBLE_SEC_OT },
    { "1 Samuel",         "1Sam",   31, BIBLE_SEC_OT },
    { "2 Samuel",         "2Sam",   24, BIBLE_SEC_OT },
    { "1 Kings",          "1Kgs",   22, BIBLE_SEC_OT },
    { "2 Kings",          "2Kgs",   25, BIBLE_SEC_OT },
    { "1 Chronicles",     "1Chr",   29, BIBLE_SEC_OT },
    { "2 Chronicles",     "2Chr",   36, BIBLE_SEC_OT },
    { "Ezra",             "Ezra",   10, BIBLE_SEC_OT },
    { "Nehemiah",         "Neh",    13, BIBLE_SEC_OT },
    { "Esther",           "Esth",   10, BIBLE_SEC_OT },
    { "Job",              "Job",    42, BIBLE_SEC_OT },
    { "Psalms",           "Ps",    150, BIBLE_SEC_OT },
    { "Proverbs",         "Prov",   31, BIBLE_SEC_OT },
    { "Ecclesiastes",     "Eccl",   12, BIBLE_SEC_OT },
    { "Song of Solomon",  "Song",    8, BIBLE_SEC_OT },
    // ── Prophets (17) ──────────────────────────────────────────────────
    { "Isaiah",           "Isa",    66, BIBLE_SEC_PR },
    { "Jeremiah",         "Jer",    52, BIBLE_SEC_PR },
    { "Lamentations",     "Lam",     5, BIBLE_SEC_PR },
    { "Ezekiel",          "Ezek",   48, BIBLE_SEC_PR },
    { "Daniel",           "Dan",    12, BIBLE_SEC_PR },
    { "Hosea",            "Hos",    14, BIBLE_SEC_PR },
    { "Joel",             "Joel",    3, BIBLE_SEC_PR },
    { "Amos",             "Amos",    9, BIBLE_SEC_PR },
    { "Obadiah",          "Obad",    1, BIBLE_SEC_PR },
    { "Jonah",            "Jonah",   4, BIBLE_SEC_PR },
    { "Micah",            "Mic",     7, BIBLE_SEC_PR },
    { "Nahum",            "Nah",     3, BIBLE_SEC_PR },
    { "Habakkuk",         "Hab",     3, BIBLE_SEC_PR },
    { "Zephaniah",        "Zeph",    3, BIBLE_SEC_PR },
    { "Haggai",           "Hag",     2, BIBLE_SEC_PR },
    { "Zechariah",        "Zech",   14, BIBLE_SEC_PR },
    { "Malachi",          "Mal",     4, BIBLE_SEC_PR },
    // ── Apocrypha (10) ─────────────────────────────────────────────────
    { "Judith",           "Jdt",    16, BIBLE_SEC_AP },
    { "Wisdom",           "Wis",    19, BIBLE_SEC_AP },
    { "Tobit",            "Tob",    14, BIBLE_SEC_AP },
    { "Sirach",           "Sir",    51, BIBLE_SEC_AP },
    { "Baruch",           "Bar",     6, BIBLE_SEC_AP },
    { "1 Maccabees",      "1Macc",  16, BIBLE_SEC_AP },
    { "2 Maccabees",      "2Macc",  15, BIBLE_SEC_AP },
    { "Additions to Esther", "AddEsth", 1, BIBLE_SEC_AP },
    { "Additions to Daniel", "AddDan",  1, BIBLE_SEC_AP },
    { "Prayer of Manasseh",  "PrMan",   1, BIBLE_SEC_AP },
    // ── New Testament (27) ─────────────────────────────────────────────
    { "Matthew",          "Matt",   28, BIBLE_SEC_NT },
    { "Mark",             "Mark",   16, BIBLE_SEC_NT },
    { "Luke",             "Luke",   24, BIBLE_SEC_NT },
    { "John",             "John",   21, BIBLE_SEC_NT },
    { "Acts",             "Acts",   28, BIBLE_SEC_NT },
    { "Romans",           "Rom",    16, BIBLE_SEC_NT },
    { "1 Corinthians",    "1Cor",   16, BIBLE_SEC_NT },
    { "2 Corinthians",    "2Cor",   13, BIBLE_SEC_NT },
    { "Galatians",        "Gal",     6, BIBLE_SEC_NT },
    { "Ephesians",        "Eph",     6, BIBLE_SEC_NT },
    { "Philippians",      "Phil",    4, BIBLE_SEC_NT },
    { "Colossians",       "Col",     4, BIBLE_SEC_NT },
    { "1 Thessalonians",  "1Thess",  5, BIBLE_SEC_NT },
    { "2 Thessalonians",  "2Thess",  3, BIBLE_SEC_NT },
    { "1 Timothy",        "1Tim",    6, BIBLE_SEC_NT },
    { "2 Timothy",        "2Tim",    4, BIBLE_SEC_NT },
    { "Titus",            "Titus",   3, BIBLE_SEC_NT },
    { "Philemon",         "Phlm",    1, BIBLE_SEC_NT },
    { "Hebrews",          "Heb",    13, BIBLE_SEC_NT },
    { "James",            "Jas",     5, BIBLE_SEC_NT },
    { "1 Peter",          "1Pet",    5, BIBLE_SEC_NT },
    { "2 Peter",          "2Pet",    3, BIBLE_SEC_NT },
    { "1 John",           "1John",   5, BIBLE_SEC_NT },
    { "2 John",           "2John",   1, BIBLE_SEC_NT },
    { "3 John",           "3John",   1, BIBLE_SEC_NT },
    { "Jude",             "Jude",    1, BIBLE_SEC_NT },
    { "Revelation",       "Rev",    22, BIBLE_SEC_NT },
};

// ─────────────────────────────────────────────────────────────────────────────
// Global instance
// ─────────────────────────────────────────────────────────────────────────────
#ifdef BOARD_HAS_PSRAM
#  ifndef EXT_RAM_BSS_ATTR
#    ifdef EXT_RAM_ATTR
#      define EXT_RAM_BSS_ATTR EXT_RAM_ATTR
#    else
#      define EXT_RAM_BSS_ATTR
#    endif
#  endif
  EXT_RAM_BSS_ATTR BibleInterface bible_obj;
#else
  BibleInterface bible_obj;
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
BibleInterface::BibleInterface()
    : cur_sec(0), cur_book(0), cur_chapter(1), cur_trans(0),
      view(BV_SECTION_SELECT), dark_mode(true), font_num(2), needs_redraw(true),
      menu_sel(0), menu_scroll(0), read_line(0),
      cached_book(255), cached_chap(0), cached_count(0),
      line_count(0), trans_count(0), bm_count(0), bm_sel(0), bm_scroll(0),
      bm_confirm_pending(false),
      search_hist_count(0), search_hist_sel(0),
      search_result_count(0), search_res_sel(0),
      highlight_verse(0), reading_from_search(false),
      search_del_pending(false),
      srch_partial_match(true), srch_ignore_punct(true), srch_scope(0),
      accent_idx(0),
      sel_verse_first(0), sel_verse_last(0),
      last_input_ms(0), last_pressed(false), bl_idx(19),
      touch_was_down(false), touch_down_x(0), touch_down_y(0),
      scroll_dragging(false),
      scroll_px(0.f), fling_vel(0.f), fling_ms(0), fling_active(false),
      drag_origin_px(0.f), vbuf_i(0),
      book_idx_valid(false), book_idx_trans(0xFF),
      line_spr(&tft)
#ifdef HAS_BATTERY
    , batt_ok(false), batt_pct(-1), batt_ms(0)
#endif
{
    memset(vbuf_y,        0, sizeof(vbuf_y));
    memset(vbuf_t,        0, sizeof(vbuf_t));
    memset(book_offsets,  0, sizeof(book_offsets));
    memset(search_query,  0, sizeof(search_query));
    memset(search_hist,   0, sizeof(search_hist));
    memset(search_results,0, sizeof(search_results));
}

// ─────────────────────────────────────────────────────────────────────────────
// RunSetup — called once when Bible firmware starts
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::RunSetup() {
    tft.init();
    tft.setRotation(0);
    tft.fillScreen(bg());

#ifdef HAS_CAP_TOUCH
    ft6336_init();
#endif

    prefs.begin("bible", false);
    blInit();   // must run before runTouchCalibration() so the backlight is on

#ifdef MARAUDER_V6_1
    // V6.1: load stored calibration or run first-boot wizard.
    if (prefs.getUChar("tcal2", 0)) {
        uint16_t calData[5];
        prefs.getBytes("tcald2", calData, sizeof(calData));
        tft.setTouch(calData);
    } else {
        runTouchCalibration();
    }
#elif defined(MARAUDER_V8)
    // V8: fixed calibration (portrait, matches Marauder Display.cpp)
    {
        uint16_t calData[5] = { 312, 3431, 191, 3456, 2 };
        tft.setTouch(calData);
    }
#endif

#ifdef HAS_BATTERY
    battInit();
#endif
    loadState();
    loadBookmarks();
    loadSearchHistory();
    scanTranslations();

    if (trans_count == 0) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawCentreString("No Bible found on SD!", scrW()/2, scrH()/2 - 10, 2);
        tft.drawCentreString("Copy .xml to /bible/",  scrW()/2, scrH()/2 + 14, 2);
        return;
    }

    // Use the nav helpers so menu_sel/scroll_px are set from the restored state
    if (trans_count == 1)
        goToSection();       // highlights cur_sec, scrolls to it
    else
        goToTransSelect();   // highlights cur_trans, scrolls to it
}

// ─────────────────────────────────────────────────────────────────────────────
// main — called every loop()
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::main(uint32_t currentTime) {
#ifdef HAS_BATTERY
    if (batt_ok && (currentTime - batt_ms >= 3000)) battUpdate();
#endif
    if (fling_active) updateFling(currentTime);

    if (needs_redraw) {
        switch (view) {
            case BV_TRANS_SELECT:    drawTransSelect();    break;
            case BV_SECTION_SELECT:  drawSectionSelect();  break;
            case BV_BOOK_SELECT:     drawBookSelect();     break;
            case BV_CHAPTER_SELECT:  drawChapterSelect();  break;
            case BV_READING:         drawReading();        break;
            case BV_SETTINGS:        drawSettings();       break;
            case BV_BOOKMARKS:       drawBookmarks();      break;
            case BV_SEARCH_INPUT:    drawSearchInput();    break;
            case BV_SEARCH_RESULTS:  drawSearchResults();  break;
        }
        needs_redraw = false;
    }

    switch (view) {
        case BV_TRANS_SELECT:    handleListInput(trans_count);          break;
        case BV_SECTION_SELECT:  handleListInput(BIBLE_SEC_COUNT);      break;
        case BV_BOOK_SELECT:     handleListInput(SEC_BOOK_COUNT_[cur_sec]); break;
        case BV_CHAPTER_SELECT:  handleChapterInput(); break;
        case BV_READING:         handleReadingInput();                  break;
        case BV_SETTINGS:        handleSettingsInput();                 break;
        case BV_BOOKMARKS:       handleBookmarksInput();                break;
        case BV_SEARCH_INPUT:    handleSearchInputInput();              break;
        case BV_SEARCH_RESULTS:  handleSearchResultsInput();            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Color scheme
// ─────────────────────────────────────────────────────────────────────────────
uint16_t BibleInterface::fg()         const { return dark_mode ? TFT_WHITE    : TFT_BLACK; }
uint16_t BibleInterface::bg()         const { return dark_mode ? TFT_BLACK    : TFT_WHITE; }
uint16_t BibleInterface::hdr_bg()     const { return dark_mode ? 0x1082       : 0x4A69;   }
uint16_t BibleInterface::sel_bg()     const { return dark_mode ? ACCENT_DARK[accent_idx] : ACCENT_LIGHT[accent_idx]; }
uint16_t BibleInterface::dim_fg()     const { return dark_mode ? 0x7BEF       : 0x632C;   }
uint16_t BibleInterface::verse_num_fg()const{ return 0x051D; /* muted teal */ }

// ─────────────────────────────────────────────────────────────────────────────
// Layout helpers
// ─────────────────────────────────────────────────────────────────────────────
uint16_t BibleInterface::scrW() const {
#ifdef MARAUDER_PANCAKE
    return 320;
#else
    return 240;
#endif
}
uint16_t BibleInterface::scrH() const {
#ifdef MARAUDER_PANCAKE
    return 480;
#else
    return 320;
#endif
}

uint16_t BibleInterface::lineH() const {
    switch (font_num) {
        case 1: return 12;
        case 4: return 30;
        default: return 18; // font 2
    }
}
uint8_t BibleInterface::visItems() const { return contentH() / itemH(); }
uint8_t BibleInterface::visLines() const { return contentH() / lineH(); }

// ─────────────────────────────────────────────────────────────────────────────
// Drawing — shared elements
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::drawHeader(const char* title, bool show_back) {
    tft.fillRect(0, 0, scrW(), hdrH(), hdr_bg());
    if (show_back) {
        // Same style as nav bar buttons: hdr_bg fill + dim_fg border
        tft.fillRoundRect(2, 3, 40, 22, 4, hdr_bg());
        tft.drawRoundRect(2, 3, 40, 22, 4, dim_fg());
        tft.setTextColor(TFT_WHITE, hdr_bg());
        tft.drawCentreString("<", 22, 10, 1);
    }
    tft.setTextColor(TFT_WHITE, hdr_bg());
    tft.drawCentreString(title, scrW()/2, 6, 2);

    // Search button — same bordered-box style as the back button.
    // Positioned to the left of the battery % text.
    // Battery: right-aligned at scrW()-3, max ~28px wide → left edge ~scrW()-31.
    // Button: 28px wide, 4px gap → right edge scrW()-35, left edge scrW()-63.
    int16_t sb_x = (int16_t)scrW() - 63;
    tft.fillRoundRect(sb_x,     3, 28, 22, 4, hdr_bg());
    tft.drawRoundRect(sb_x,     3, 28, 22, 4, dim_fg());
    // Magnifying glass inside the button (circle + diagonal handle)
    int16_t cx = sb_x + 13;   // horizontal centre of button
    tft.drawCircle(cx,     14, 5, TFT_WHITE);
    tft.drawLine  (cx + 4, 18, cx + 7, 21, TFT_WHITE);

#ifdef HAS_BATTERY
    if (batt_pct >= 0) {
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", (int)batt_pct);
        tft.drawRightString(pct, (int16_t)(scrW() - 3), 10, 1);
    }
#endif
}

void BibleInterface::drawNavBar(const char* left, const char* mid, const char* right) {
    uint16_t y     = scrH() - navH();
    uint16_t third = scrW() / 3;
    uint16_t bh    = navH() - 8;   // button height (4 px margin top + bottom)
    uint16_t by    = y + 4;
    uint16_t bw    = third - 8;    // button width  (4 px margin each side)

    // Nav area background + divider line
    tft.fillRect(0, y, scrW(), navH(), bg());
    tft.drawFastHLine(0, y, scrW(), dark_mode ? 0x2104 : 0xC618);

    // Draw each non-empty label as a rounded button
    const char* labels[3] = { left, mid, right };
    for (uint8_t i = 0; i < 3; i++) {
        if (!labels[i] || !labels[i][0]) continue;
        uint16_t cx = i * third + third / 2;
        uint16_t bx = cx - bw / 2;
        tft.fillRoundRect(bx, by, bw, bh, 4, hdr_bg());
        tft.drawRoundRect(bx, by, bw, bh, 4, dim_fg());
        tft.setTextColor(TFT_WHITE, hdr_bg());
        tft.drawCentreString(labels[i], cx, by + (bh - 8) / 2, 1);
    }
}

void BibleInterface::clearContent() {
    tft.fillRect(0, contentY(), scrW(), contentH(), bg());
}

// Draw one private-code character on tft at (x, y) in the given font.
// setTextColor must be set by the caller before use.  Returns advance width.
static int16_t tftCharUTF8(TFT_eSPI& tft, uint8_t c, int16_t x, int16_t y,
                             uint8_t font, uint16_t color) {
    static const char ubases[] = {'A','a','O','o','U','u'};
    if (c == 0x86)
        return bibleDrawSzlig(tft, x, y, font, color);
    if (c >= 0x80 && c <= 0x85) {
        bool    is_lower = ((c - 0x80) & 1) != 0;
        int16_t adv      = tft.drawChar((uint16_t)(uint8_t)ubases[c - 0x80], x, y, font);
        bibleDrawUmlautDots(tft, x, y, adv, is_lower, font, color);
        return adv;
    }
    return tft.drawChar((uint16_t)c, x, y, font);
}

void BibleInterface::drawListRow(int16_t y_px, const char* text, bool selected, bool has_arrow) {
    uint16_t bg_c  = selected ? sel_bg() : bg();
    uint16_t fg_c  = fg();
    // Right edge: leave room for scrollbar (6px) + arrow (14px) + small gap
    int16_t  max_x = (int16_t)scrW() - (has_arrow ? 28 : 8);
    int16_t  ty    = y_px + (itemH() - 16) / 2;
    tft.fillRect(0, y_px, scrW(), itemH(), bg_c);
    tft.setTextColor(fg_c, bg_c);
    // Draw text char-by-char with private-code umlaut/ß support
    int16_t tx = 10;
    for (const char* p = text; *p && tx < max_x; p++)
        tx += tftCharUTF8(tft, (uint8_t)*p, tx, ty, 2, fg_c);
    if (has_arrow) {
        tft.setTextColor(dim_fg(), bg_c);
        tft.drawString(">", scrW() - 20, ty, 2);
    }
    // divider
    tft.drawFastHLine(0, y_px + itemH() - 1, scrW(), dark_mode ? 0x2104 : 0xC618);
}

void BibleInterface::drawScrollBar(int16_t total, int16_t vis, int16_t top) {
    if (total <= vis) return;
    uint16_t barX = scrW() - 6;
    uint16_t barY = contentY();
    uint16_t barH = contentH();
    tft.fillRect(barX, barY, 6, barH, dark_mode ? 0x2104 : 0xC618);
    int16_t thumbH = max((int16_t)10, (int16_t)(barH * vis / total));
    int16_t thumbY = barY + (int16_t)((int32_t)top * (barH - thumbH) / max(1, total - vis));
    tft.fillRect(barX, thumbY, 6, thumbH, dim_fg());
}

// Redraws only the list content area (rows + scrollbar) without touching the
// header or nav bar.  Called directly during scroll drag/fling so there is no
// full-screen repaint — eliminating the white/black flash between frames.
// Uses scroll_px for sub-item-height pixel accuracy.
// setViewport clips rows that extend into the header or nav bar zones.
// startWrite / endWrite batches all SPI transfers in one transaction for speed.
void BibleInterface::redrawListContent(uint8_t item_count) {
    int16_t sub_px      = (int16_t)fmodf(scroll_px, (float)itemH());
    int16_t first       = (int16_t)(scroll_px / (float)itemH());
    int16_t content_top = (int16_t)contentY();
    int16_t content_end = content_top + (int16_t)contentH();

    tft.startWrite();
    // Clip all draws to the content zone — prevents header/nav bleed without a
    // global clear (which would cause flash). vpDatum=false keeps screen-absolute coords.
    tft.setViewport(0, contentY(), scrW(), contentH(), false);

    int16_t last_bottom = content_top;

    for (int i = 0; ; i++) {
        int16_t idx = first + i;
        int16_t y   = content_top - sub_px + i * (int16_t)itemH();
        if (y >= content_end || idx >= (int16_t)item_count) break;

        switch (view) {
            case BV_TRANS_SELECT:
                drawListRow(y, trans_stems[idx],
                            idx == (int16_t)menu_sel);
                break;
            case BV_SECTION_SELECT:
                if (idx < (int16_t)BIBLE_SEC_COUNT)
                    drawListRow(y, SEC_NAME_EN[idx], idx == (int16_t)menu_sel);
                break;
            case BV_BOOK_SELECT:
                drawListRow(y, BOOKS[SEC_BOOK_START[cur_sec] + idx].display,
                            idx == (int16_t)menu_sel);
                break;
            case BV_BOOKMARKS:
                drawListRow(y, bookmarks[idx].label,
                            idx == (int16_t)bm_sel, false);
                break;
            case BV_SEARCH_INPUT:
                drawListRow(y, search_hist[idx], idx == (int16_t)search_hist_sel, false);
                break;
            default: break;
        }
        int16_t bot = y + (int16_t)itemH();
        if (bot > last_bottom) last_bottom = bot;
    }

    // Clear any unused space below the last row (list shorter than content zone).
    // Fill full width — the scrollbar will repaint its 6px column when needed.
    if (last_bottom < content_end)
        tft.fillRect(0, last_bottom, scrW(), content_end - last_bottom, bg());

    tft.resetViewport();
    drawScrollBar(item_count, visItems(), first);
    tft.endWrite();
}

// Redraws only the chapter tile grid and scrollbar without touching the header
// or nav bar.  Called directly during scroll drag instead of setting needs_redraw,
// which would trigger a full drawChapterSelect() → fillScreen() → flash next loop.
// startWrite / endWrite batches all SPI transfers in one transaction for speed.
void BibleInterface::redrawChapterContent() {
    uint8_t  chaps      = BOOKS[cur_book].chapters;
    uint16_t tile_w     = scrW() / 5;
    uint16_t tile_h     = 36;
    uint8_t  vis_rows   = (uint8_t)(contentH() / tile_h);
    int16_t  total_rows = ((int16_t)chaps + 4) / 5;

    tft.startWrite();
    // Each tile fills its own background; no global clear (prevents flash).
    // Clip to content zone so tiles never bleed into header or nav.
    tft.setViewport(0, contentY(), scrW(), contentH(), false);

    for (uint8_t row = 0; row < vis_rows; row++) {
        for (uint8_t col = 0; col < 5; col++) {
            uint8_t ch = (uint8_t)((menu_scroll + row) * 5 + col + 1);
            uint16_t x = col * tile_w;
            uint16_t y = contentY() + row * tile_h;
            if (ch > chaps) {
                // Clear unused tile slots to the right in the last row
                if (x < scrW() - 6)
                    tft.fillRect(x, y, scrW() - 6 - x, tile_h, bg());
                break;
            }
            bool     sel     = (ch == cur_chapter) || (ch == (uint8_t)menu_sel);
            uint16_t tile_bg = sel ? sel_bg() : bg();
            tft.fillRect(x, y, tile_w, tile_h, tile_bg);
            tft.drawRect(x, y, tile_w, tile_h, dark_mode ? 0x2104 : 0xC618);
            char buf[5];
            snprintf(buf, sizeof(buf), "%d", ch);
            tft.setTextColor(fg(), tile_bg);
            tft.drawCentreString(buf, x + tile_w / 2, y + (tile_h - 16) / 2, 2);
        }
    }
    // Clear any gap below the tile rows
    uint16_t used_h = (uint16_t)vis_rows * tile_h;
    if (used_h < contentH())
        tft.fillRect(0, contentY() + used_h, scrW() - 6, contentH() - used_h, bg());

    tft.resetViewport();
    drawScrollBar(total_rows, vis_rows, menu_scroll);
    tft.endWrite();
}

// ─────────────────────────────────────────────────────────────────────────────
// Translation select screen
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::drawTransSelect() {
    tft.fillScreen(bg());
    drawHeader("Choose Translation", false);
    uint8_t vis = visItems();
    for (uint8_t i = 0; i < vis && (menu_scroll + i) < trans_count; i++) {
        bool sel = (menu_scroll + i) == (uint8_t)menu_sel;
        drawListRow(contentY() + i * itemH(), trans_stems[menu_scroll + i], sel);
    }
    drawScrollBar(trans_count, vis, menu_scroll);
    drawNavBar("Marks", "Settings", "Bright");
}

// ─────────────────────────────────────────────────────────────────────────────
// Section select screen
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::drawSectionSelect() {
    tft.fillScreen(bg());
    drawHeader(trans_count > 0 ? trans_stems[cur_trans] : "Bible", false);
    for (uint8_t i = 0; i < BIBLE_SEC_COUNT; i++) {
        bool sel = (i == (uint8_t)menu_sel);
        drawListRow(contentY() + i * itemH(), SEC_NAME_EN[i], sel);
    }
    drawNavBar("Marks", "Settings", "Bright");
}

// ─────────────────────────────────────────────────────────────────────────────
// Book select screen
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::drawBookSelect() {
    tft.fillScreen(bg());
    drawHeader(SEC_NAME_EN[cur_sec]);
    uint8_t vis   = visItems();
    uint8_t start = SEC_BOOK_START[cur_sec];
    uint8_t count = SEC_BOOK_COUNT_[cur_sec];
    for (uint8_t i = 0; i < vis && (menu_scroll + i) < count; i++) {
        bool sel = (menu_scroll + i) == (uint8_t)menu_sel;
        drawListRow(contentY() + i * itemH(), BOOKS[start + menu_scroll + i].display, sel);
    }
    drawScrollBar(count, vis, menu_scroll);
    drawNavBar("Marks", "Settings", "Bright");
}

// ─────────────────────────────────────────────────────────────────────────────
// Chapter select screen
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::drawChapterSelect() {
    tft.fillScreen(bg());
    drawHeader(BOOKS[cur_book].display);

    uint8_t  chaps    = BOOKS[cur_book].chapters;
    uint16_t tile_w   = scrW() / 5;
    uint16_t tile_h   = 36;
    uint8_t  vis_rows = (uint8_t)(contentH() / tile_h);

    for (uint8_t row = 0; row < vis_rows; row++) {
        for (uint8_t col = 0; col < 5; col++) {
            uint8_t ch = (uint8_t)((menu_scroll + row) * 5 + col + 1);
            if (ch > chaps) break;

            uint16_t x  = col * tile_w;
            uint16_t y  = contentY() + row * tile_h;
            bool     sel = (ch == cur_chapter) || (ch == (uint8_t)menu_sel);

            uint16_t tile_bg = sel ? sel_bg() : bg();
            tft.fillRect(x, y, tile_w, tile_h, tile_bg);
            tft.drawRect(x, y, tile_w, tile_h, dark_mode ? 0x2104 : 0xC618);

            char buf[5];
            snprintf(buf, sizeof(buf), "%d", ch);
            tft.setTextColor(fg(), tile_bg);
            tft.drawCentreString(buf, x + tile_w / 2, y + (tile_h - 16) / 2, 2);
        }
    }

    int16_t total_rows = ((int16_t)chaps + 4) / 5;
    drawScrollBar(total_rows, vis_rows, menu_scroll);
    drawNavBar("Marks", "Settings", "Bright");
}

// ─────────────────────────────────────────────────────────────────────────────
// Reading view — draws the paged verse text
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::drawReadingLines() {
    int16_t  sub_px = (int16_t)fmodf(scroll_px, (float)lineH());
    int16_t  first  = (int16_t)(scroll_px / (float)lineH());
    uint8_t  vis    = visLines() + 1;   // +1 covers partial bottom row
    int16_t  cy     = (int16_t)contentY();
    int16_t  ce     = cy + (int16_t)contentH();
    uint16_t lh     = lineH();

    read_line = first;

    // Allocate one line-height sprite for atomic pushes — eliminates both the
    // per-line ripple (interleaved fill+draw) and the full-zone flash (single
    // big clear).  Each pushSprite() is a single SPI burst; the TFT viewport
    // clips any partial line at the header/nav boundaries.
    if (!line_spr.createSprite(scrW(), lh)) {
        // Allocation failed (very low heap) — fall back to direct draw.
        tft.startWrite();
        tft.fillRect(0, cy, scrW(), contentH(), bg());
        tft.endWrite();
        return;
    }

    tft.setViewport(0, cy, scrW(), contentH(), false);

    // Track current verse number for highlight support.
    // Scan backward from 'first' to find which verse that line belongs to.
    uint8_t cur_verse_num = 0;
    for (int16_t k = first; k >= 0; k--) {
        if (lines[k][0] == '^') {
            const char* pipe = strchr(lines[k] + 1, '|');
            if (pipe) { cur_verse_num = (uint8_t)atoi(lines[k] + 1); break; }
        }
    }
    // Highlight background — accent colour from Settings
    uint16_t hi_bg = sel_bg();

    // Precompute bookmark verse-range for this chapter (first matching verse bookmark).
    // Bookmark verse ranges are highlighted the same as search/selection highlights.
    uint8_t bm_v1 = 0, bm_v2 = 0;
    for (uint8_t k = 0; k < bm_count; k++) {
        if (bookmarks[k].book == cur_book && bookmarks[k].chapter == cur_chapter
                && bookmarks[k].verse_first > 0) {
            bm_v1 = bookmarks[k].verse_first;
            bm_v2 = bookmarks[k].verse_last;
            break;  // show first matching range only
        }
    }

    for (uint8_t i = 0; i < vis; i++) {
        int16_t row_y    = cy - sub_px + (int16_t)i * (int16_t)lh;
        if (row_y >= ce) break;

        int16_t line_idx = first + (int16_t)i;

        // Update verse tracking before drawing
        if (line_idx >= 0 && line_idx < (int16_t)line_count) {
            const char* ln = lines[line_idx];
            if (ln[0] == '^') {
                const char* pipe = strchr(ln + 1, '|');
                if (pipe) cur_verse_num = (uint8_t)atoi(ln + 1);
            }
        }

        uint16_t line_bg = bg();
        if (highlight_verse > 0 && cur_verse_num == highlight_verse)
            line_bg = hi_bg;
        else if (sel_verse_first > 0
                 && cur_verse_num >= sel_verse_first
                 && cur_verse_num <= sel_verse_last)
            line_bg = hi_bg;
        else if (bm_v1 > 0
                 && cur_verse_num >= bm_v1
                 && cur_verse_num <= bm_v2)
            line_bg = hi_bg;

        line_spr.fillSprite(line_bg);

        if (line_idx >= 0 && line_idx < (int16_t)line_count) {
            const char* ln = lines[line_idx];
            if (ln[0] == '^') {
                const char* pipe = strchr(ln + 1, '|');
                if (pipe) {
                    char num_str[12];
                    int  n_len = (int)(pipe - (ln + 1));
                    if (n_len > 6) n_len = 6;
                    memcpy(num_str, ln + 1, n_len);
                    num_str[n_len]     = '.';
                    num_str[n_len + 1] = 0;
                    line_spr.setTextColor(verse_num_fg(), line_bg);
                    int16_t nx = line_spr.drawString(num_str, 4, 2, font_num);
                    line_spr.setTextColor(fg(), line_bg);
                    drawStringUTF8(line_spr, pipe + 1, 4 + nx + 2, 2, font_num, fg());
                }
            } else {
                line_spr.setTextColor(fg(), line_bg);
                drawStringUTF8(line_spr, ln, 4, 2, font_num, fg());
            }
        }

        // pushSprite respects the viewport set above, clipping partial lines
        line_spr.pushSprite(0, row_y);
    }

    tft.resetViewport();
    line_spr.deleteSprite();
}

void BibleInterface::drawReading() {
    tft.fillScreen(bg());
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "%s %d", BOOKS[cur_book].display, cur_chapter);
    drawHeader(hdr);
    drawReadingLines();
    drawNavBar("Marks", "Settings", "+Mark");
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings screen
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::redrawSettingsContent() {
#ifdef HAS_CAP_TOUCH
    const uint8_t  n = 6;
#else
    const uint8_t  n = 7;
#endif
    const int16_t  btn_w = 28, btn_h = 22, btn_r = 4;

    for (uint8_t i = 0; i < n; i++) {
        int16_t row_y = contentY() + (int16_t)i * itemH();
        bool    sel   = (i == (uint8_t)menu_sel);

        if (i == 2) {
            // Brightness row: "Brightness" label + [-] X/20 [+] buttons.
            // Buttons are styled like the search button: 28×22, radius 4,
            // hdr_bg() fill, dim_fg() border, white centred text.
            drawListRow(row_y, "Brightness", sel, false);

            uint16_t bg_c  = sel ? sel_bg() : bg();
            int16_t  btn_y = row_y + (itemH() - btn_h) / 2;   // centre vertically
            int16_t  txt_y = btn_y + (btn_h - 16) / 2;         // font-2 text top
            int16_t  num_y = row_y + (itemH() - 16) / 2;        // number text top

            // [+] button — right-aligned with 4 px margin
            int16_t plus_bx = (int16_t)scrW() - 4 - btn_w;
            tft.fillRoundRect(plus_bx, btn_y, btn_w, btn_h, btn_r, hdr_bg());
            tft.drawRoundRect(plus_bx, btn_y, btn_w, btn_h, btn_r, dim_fg());
            tft.setTextColor(TFT_WHITE, hdr_bg());
            tft.drawCentreString("+", plus_bx + btn_w / 2, txt_y, 2);

            // X/20 number — right-aligned 4 px left of [+]
            char nbuf[8];
            snprintf(nbuf, sizeof(nbuf), "%d/20", bl_idx + 1);
            int16_t num_w = (int16_t)tft.textWidth(nbuf, 2);
            int16_t num_x = plus_bx - 4 - num_w;
            tft.setTextColor(fg(), bg_c);
            tft.drawString(nbuf, num_x, num_y, 2);

            // [-] button — 4 px left of the number
            int16_t minus_bx = num_x - 4 - btn_w;
            tft.fillRoundRect(minus_bx, btn_y, btn_w, btn_h, btn_r, hdr_bg());
            tft.drawRoundRect(minus_bx, btn_y, btn_w, btn_h, btn_r, dim_fg());
            tft.setTextColor(TFT_WHITE, hdr_bg());
            tft.drawCentreString("-", minus_bx + btn_w / 2, txt_y, 2);

        } else {
            char buf[40];
            if (i == 0) {
                const char* sz = (font_num == 1) ? "Small" : (font_num == 2) ? "Medium" : "Large";
                snprintf(buf, sizeof(buf), "Font: %s", sz);
            } else if (i == 1) {
                snprintf(buf, sizeof(buf), "Theme: %s", dark_mode ? "Dark" : "Light");
            } else if (i == 3) {
                snprintf(buf, sizeof(buf), "Translation: %s",
                         trans_count > 0 ? trans_stems[cur_trans] : "-");
            } else if (i == 4) {
                // Highlight row drawn separately below — fall through to drawListRow
                // for the label, then overlay [<] Name [>] controls.
                drawListRow(row_y, "Highlight", sel, false);

                uint16_t bg_c  = sel ? sel_bg() : bg();
                int16_t  btn_y = row_y + (itemH() - btn_h) / 2;
                int16_t  txt_y = btn_y + (btn_h - 16) / 2;
                int16_t  nam_y = row_y + (itemH() - 16) / 2;

                // [>] button — right-aligned with 4 px margin (same position as [+])
                int16_t fwd_bx = (int16_t)scrW() - 4 - btn_w;
                tft.fillRoundRect(fwd_bx, btn_y, btn_w, btn_h, btn_r, hdr_bg());
                tft.drawRoundRect(fwd_bx, btn_y, btn_w, btn_h, btn_r, dim_fg());
                tft.setTextColor(TFT_WHITE, hdr_bg());
                tft.drawCentreString(">", fwd_bx + btn_w / 2, txt_y, 2);

                // Color name — right-aligned 4 px left of [>]
                const char* cname  = ACCENT_NAMES[accent_idx];
                int16_t     nam_w  = (int16_t)tft.textWidth(cname, 2);
                int16_t     nam_x  = fwd_bx - 4 - nam_w;
                tft.setTextColor(fg(), bg_c);
                tft.drawString(cname, nam_x, nam_y, 2);

                // [<] button — 4 px left of the name
                int16_t bwd_bx = nam_x - 4 - btn_w;
                tft.fillRoundRect(bwd_bx, btn_y, btn_w, btn_h, btn_r, hdr_bg());
                tft.drawRoundRect(bwd_bx, btn_y, btn_w, btn_h, btn_r, dim_fg());
                tft.setTextColor(TFT_WHITE, hdr_bg());
                tft.drawCentreString("<", bwd_bx + btn_w / 2, txt_y, 2);
                continue;  // skip the drawListRow call at the bottom of the else block
            } else if (i == 5) {
                strncpy(buf, "Boot OTA_1", sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = 0;
#ifndef HAS_CAP_TOUCH
            } else {  // i == 6
                strncpy(buf, "Calibrate Touch", sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = 0;
#endif
            }
            drawListRow(row_y, buf, sel, false);
        }
    }
}

void BibleInterface::drawSettings() {
    tft.fillScreen(bg());
    drawHeader("Settings");
    redrawSettingsContent();
    drawNavBar("Back", "", "");
}

// ─────────────────────────────────────────────────────────────────────────────
// Bookmarks screen
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::drawBookmarks() {
    tft.fillScreen(bg());
    drawHeader("Bookmarks");
    if (bm_count == 0) {
        tft.setTextColor(dim_fg(), bg());
        tft.drawCentreString("No bookmarks yet", scrW()/2, contentY() + 20, 2);
        drawNavBar("Back", "", "");
        return;
    }
    uint8_t vis = visItems();
    for (uint8_t i = 0; i < vis && (bm_scroll + i) < bm_count; i++) {
        bool sel = (bm_scroll + i) == (uint8_t)bm_sel;
        drawListRow(contentY() + i * itemH(), bookmarks[bm_scroll + i].label, sel, false);
    }
    drawScrollBar(bm_count, vis, bm_scroll);
    drawNavBar("Back", "View", "Del");
    if (bm_confirm_pending) drawConfirmDelete();
}

// Delete-confirmation popup — drawn on top of the bookmark list.
// Geometry is computed from scrW()/scrH() so it scales for both boards.
//
//  ┌─────────────────────────────────┐
//  │       Delete bookmark?          │
//  │                                 │
//  │  [ Cancel ]       [ Delete ]    │
//  └─────────────────────────────────┘
//
// Touch detection: lift inside the Delete button confirms; anywhere else cancels.
void BibleInterface::drawConfirmDelete() {
    int16_t pop_w = (int16_t)scrW() - 40;   // 20 px margin each side
    int16_t pop_h = 80;
    int16_t pop_x = 20;
    int16_t pop_y = (int16_t)(scrH() / 2) - 40;

    // Panel — filled with bg() so it covers content behind it
    tft.fillRoundRect(pop_x,     pop_y,     pop_w,     pop_h,     6, bg());
    tft.drawRoundRect(pop_x,     pop_y,     pop_w,     pop_h,     6, dim_fg());
    tft.drawRoundRect(pop_x + 1, pop_y + 1, pop_w - 2, pop_h - 2, 6, dim_fg());

    // Message
    tft.setTextColor(fg(), bg());
    tft.drawCentreString("Delete bookmark?", scrW() / 2, pop_y + 10, 2);

    // Button row — two equal-width buttons separated by a small gap
    int16_t btn_y  = pop_y + 44;
    int16_t btn_h  = 28;
    int16_t half_w = pop_w / 2 - 6;    // each button width
    int16_t del_x  = pop_x + pop_w / 2 + 2;

    // Cancel (left)
    tft.fillRoundRect(pop_x + 4, btn_y, half_w, btn_h, 4, hdr_bg());
    tft.drawRoundRect(pop_x + 4, btn_y, half_w, btn_h, 4, dim_fg());
    tft.setTextColor(TFT_WHITE, hdr_bg());
    tft.drawCentreString("Cancel", pop_x + 4 + half_w / 2, btn_y + (btn_h - 8) / 2, 1);

    // Delete (right) — red border + red text to signal destructive action
    tft.fillRoundRect(del_x, btn_y, half_w, btn_h, 4, hdr_bg());
    tft.drawRoundRect(del_x, btn_y, half_w, btn_h, 4, TFT_RED);
    tft.setTextColor(TFT_RED, hdr_bg());
    tft.drawCentreString("Delete", del_x + half_w / 2, btn_y + (btn_h - 8) / 2, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Touch input
// ─────────────────────────────────────────────────────────────────────────────
bool BibleInterface::getTouch(uint16_t* tx, uint16_t* ty) {
#ifdef HAS_CAP_TOUCH
    uint16_t raw_x, raw_y;
    uint8_t touches = ft6336_update(&raw_x, &raw_y);
    if (touches == 0) { last_pressed = false; return false; }
    // FT6336 returns coordinates in panel orientation; rotation=0 portrait
    *tx = raw_x;
    *ty = raw_y;
#else
    uint16_t cal[5] = {0,0,0,0,0}; // use default calibration
    (void)cal;
    if (!tft.getTouch(tx, ty, 600)) { last_pressed = false; return false; }
#endif
    uint32_t now = millis();
    if (last_pressed && (now - last_input_ms) < 200) return false;
    last_pressed   = true;
    last_input_ms  = now;
    return true;
}

// Raw touch poll — no debounce, used for gesture detection in list views.
bool BibleInterface::pollTouch(uint16_t* tx, uint16_t* ty) {
#ifdef HAS_CAP_TOUCH
    uint16_t raw_x, raw_y;
    if (!ft6336_update(&raw_x, &raw_y)) { last_pressed = false; return false; }
    *tx = raw_x; *ty = raw_y;
#else
    if (!tft.getTouch(tx, ty, 600)) { last_pressed = false; return false; }
#endif
    last_pressed  = true;
    last_input_ms = millis();
    return true;
}

bool BibleInterface::touchInHeader(uint16_t /*x*/, uint16_t y) {
    return y < hdrH();
}

#ifndef HAS_CAP_TOUCH
// Run the TFT_eSPI resistive-touch calibration wizard.
// Loops until the user correctly taps a verification circle, then saves
// the resulting 5-word calibration array to Preferences ("tcal2"/"tcald2").
// blInit() must have been called before this so the LEDC channel is attached.
void BibleInterface::runTouchCalibration() {
    // Force full brightness — stored level may be very low.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(TFT_BL, 255);
#else
    ledcWrite(0, 255);
#endif

    bool success = false;
    while (!success) {
        // ── Instructions screen ──────────────────────────────────────────
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawCentreString("Touch Calibration", scrW() / 2, scrH() / 2 - 20, 2);
        tft.drawCentreString("Tap each + marker", scrW() / 2, scrH() / 2 +  5, 2);
        delay(2000);

        // ── 4-corner calibration ─────────────────────────────────────────
        uint16_t calData[5];
        tft.calibrateTouch(calData, TFT_WHITE, TFT_BLACK, 15);
        tft.setTouch(calData);  // apply immediately for verification

        // ── Verification: tap the green circle ───────────────────────────
        int16_t cx = scrW() / 2;
        int16_t cy = scrH() / 2 + 20;
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawCentreString("Tap the circle", scrW() / 2, cy - 55, 2);
        tft.drawCircle(cx, cy, 22, TFT_GREEN);
        tft.fillCircle(cx, cy,  8, TFT_GREEN);

        uint16_t vx = 0, vy = 0;
        bool tapped = false;
        uint32_t t0 = millis();
        while (millis() - t0 < 6000) {
            if (tft.getTouch(&vx, &vy, 600)) { tapped = true; break; }
            delay(20);
        }

        if (tapped && abs((int16_t)vx - cx) < 35 && abs((int16_t)vy - cy) < 35) {
            // ── Good — save ───────────────────────────────────────────────
            prefs.putBytes("tcald2", calData, sizeof(calData));
            prefs.putUChar("tcal2", 1);
            success = true;
        } else {
            // ── Bad or timeout — try again ────────────────────────────────
            tft.fillScreen(TFT_BLACK);
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawCentreString("Calibration failed", scrW() / 2, scrH() / 2 - 8, 2);
            tft.drawCentreString("Try again...",       scrW() / 2, scrH() / 2 + 16, 2);
            delay(2000);
        }
    }

    // Restore user's saved brightness level.
    blSet(bl_idx);
    tft.fillScreen(bg());
}
#endif
bool BibleInterface::touchInNav(uint16_t /*x*/, uint16_t y) {
    return y >= (scrH() - navH());
}
int16_t BibleInterface::touchItem(uint16_t /*x*/, uint16_t y) {
    if (y < contentY() || y >= (scrH() - navH())) return -1;
    return (int16_t)((y - contentY()) / itemH());
}

// ─────────────────────────────────────────────────────────────────────────────
// Input handlers
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::handleListInput(uint8_t item_count) {
    uint16_t tx, ty;
    bool down = pollTouch(&tx, &ty);

    // ── Finger just touched down ─────────────────────────────────────────────
    if (down && !touch_was_down) {
        touch_was_down  = true;
        touch_down_x    = tx;
        touch_down_y    = ty;
        scroll_dragging = false;
        stopFling();
        drag_origin_px  = scroll_px;

        // Header and nav bar fire immediately on press — no drag ambiguity there
        if (touchInHeader(tx, ty)) {
            touch_was_down = false;
            if (touchInSearchIcon(tx, ty)) { goToSearchInput(); return; }
            if (tx < 48) goBack();
            return;
        }
        if (touchInNav(tx, ty)) {
            touch_was_down = false;
            if      (tx > scrW() - scrW()/3) { blCycle(); needs_redraw = true; }
            else if (tx < scrW()/3)           { goToBookmarks(); }
            else                               { goToSettings(); }
            return;
        }
        // Highlight the touched item immediately for press feedback
        int16_t hi = touchItem(tx, ty);
        if (hi >= 0 && (menu_scroll + hi) < (int16_t)item_count) {
            menu_sel = menu_scroll + hi;
            redrawListContent(item_count);
        }
        return;
    }

    // ── Finger held — pixel-accurate drag ────────────────────────────────────
    if (down && touch_was_down) {
        int16_t dy = (int16_t)ty - (int16_t)touch_down_y;
        if (!scroll_dragging && abs(dy) > 8) {
            scroll_dragging = true;
            menu_sel = -1;  // clear press highlight when drag begins
        }
        if (scroll_dragging) {
            float max_px = (float)max(0, (int)item_count - (int)visItems()) * (float)itemH();
            float new_px = drag_origin_px + (float)((int16_t)touch_down_y - (int16_t)ty);
            if (new_px < 0.f) new_px = 0.f;
            if (new_px > max_px) new_px = max_px;
            scroll_px   = new_px;
            menu_scroll = (int16_t)(scroll_px / (float)itemH());
            recordVel((int16_t)ty, millis());
            redrawListContent(item_count);
        }
        return;
    }

    // ── Finger lifted ────────────────────────────────────────────────────────
    if (!down && touch_was_down) {
        touch_was_down = false;
        if (scroll_dragging) {
            scroll_dragging = false;
            // Kick fling if lift velocity is fast enough
            float v = computeFlingVel();
            if (fabsf(v) > 50.f) {
                fling_vel    = v < -4000.f ? -4000.f : v > 4000.f ? 4000.f : v;
                fling_active = true;
                fling_ms     = millis();
            } else {
                stopFling();
            }
            return;
        }

        // It was a tap — navigate to the item highlighted on press
        if (menu_sel < 0 || menu_sel >= (int16_t)item_count) return;

        switch (view) {
            case BV_TRANS_SELECT:
                cur_trans = (uint8_t)menu_sel;
                goToSection();
                break;
            case BV_SECTION_SELECT:
                goToBook((uint8_t)menu_sel);
                break;
            case BV_BOOK_SELECT:
                goToChapter(SEC_BOOK_START[cur_sec] + (uint8_t)menu_sel);
                break;
            case BV_CHAPTER_SELECT:
                goToReading((uint8_t)menu_sel + 1);
                break;
            default: break;
        }
    }
}

void BibleInterface::handleChapterInput() {
    uint8_t  chaps      = BOOKS[cur_book].chapters;
    uint16_t tile_w     = scrW() / 5;
    uint16_t tile_h     = 36;
    uint8_t  vis_rows   = (uint8_t)(contentH() / tile_h);
    int16_t  total_rows = ((int16_t)chaps + 4) / 5;
    int16_t  max_scroll = max((int16_t)0, (int16_t)(total_rows - vis_rows));

    uint16_t tx, ty;
    bool down = pollTouch(&tx, &ty);

    if (down && !touch_was_down) {
        touch_was_down  = true;
        touch_down_x    = tx;
        touch_down_y    = ty;
        scroll_dragging = false;
        stopFling();
        drag_origin_px  = (float)menu_scroll * (float)tile_h;

        if (touchInHeader(tx, ty)) {
            touch_was_down = false;
            if (touchInSearchIcon(tx, ty)) { goToSearchInput(); return; }
            if (tx < 48) goBack();
            return;
        }
        if (touchInNav(tx, ty)) {
            touch_was_down = false;
            if      (tx > scrW() - scrW()/3) { blCycle(); needs_redraw = true; }
            else if (tx < scrW()/3)           { goToBookmarks(); }
            else                               { goToSettings(); }
            return;
        }
        // Highlight the touched chapter tile immediately for press feedback
        if (ty >= (uint16_t)contentY() && ty < (uint16_t)(scrH() - navH())) {
            uint8_t col_p = (uint8_t)(tx / tile_w);
            uint8_t row_p = (uint8_t)((ty - contentY()) / tile_h);
            uint8_t ch_p  = (uint8_t)((menu_scroll + row_p) * 5 + col_p + 1);
            if (ch_p >= 1 && ch_p <= chaps) {
                menu_sel = (int16_t)ch_p;
                redrawChapterContent();
            }
        }
        return;
    }

    if (down && touch_was_down) {
        int16_t dy = (int16_t)ty - (int16_t)touch_down_y;
        if (!scroll_dragging && abs(dy) > 12) {
            scroll_dragging = true;
            menu_sel = -1;  // clear tile highlight when drag begins
        }
        if (scroll_dragging) {
            // Chapter grid stays quantized during drag
            float raw_px   = drag_origin_px + (float)((int16_t)touch_down_y - (int16_t)ty);
            float max_px   = (float)max_scroll * (float)tile_h;
            if (raw_px < 0.f) raw_px = 0.f;
            if (raw_px > max_px) raw_px = max_px;
            scroll_px = raw_px;
            int16_t new_scroll = (int16_t)(scroll_px / (float)tile_h);
            recordVel((int16_t)ty, millis());
            if (new_scroll != menu_scroll) {
                menu_scroll = new_scroll;
                redrawChapterContent();
            }
        }
        return;
    }

    if (!down && touch_was_down) {
        touch_was_down = false;
        if (scroll_dragging) {
            scroll_dragging = false;
            float v = computeFlingVel();
            if (fabsf(v) > 50.f) {
                fling_vel    = v < -4000.f ? -4000.f : v > 4000.f ? 4000.f : v;
                fling_active = true;
                fling_ms     = millis();
            } else {
                stopFling();
            }
            return;
        }

        // Navigate to the chapter highlighted on press
        if (menu_sel < 1 || menu_sel > (int16_t)chaps) return;
        goToReading((uint8_t)menu_sel);
    }
}

void BibleInterface::handleReadingInput() {
    uint16_t tx, ty;
    bool down = pollTouch(&tx, &ty);

    // ── Finger just touched ────────────────────────────────────────────────
    if (down && !touch_was_down) {
        touch_was_down  = true;
        touch_down_x    = tx;
        touch_down_y    = ty;
        scroll_dragging = false;
        stopFling();
        drag_origin_px  = scroll_px;
        if (touchInHeader(tx, ty)) {
            touch_was_down = false;
            if (touchInSearchIcon(tx, ty)) { goToSearchInput(); return; }
            if (tx < 48) goBack();
            return;
        }
        if (touchInNav(tx, ty)) {
            touch_was_down = false;
            uint16_t third = scrW() / 3;
            if (tx < third)      { goToBookmarks();      return; }
            if (tx < 2 * third)  { goToSettings();       return; }
            addBookmarkCurrent(); return;
        }
        return;
    }

    // ── Finger held down — pixel-accurate drag ────────────────────────────
    if (down && touch_was_down) {
        int16_t dy = (int16_t)ty - (int16_t)touch_down_y;
        if (!scroll_dragging && abs(dy) > 8) scroll_dragging = true;
        if (scroll_dragging) {
            float max_px = (float)max(0, (int)line_count - (int)visLines()) * (float)lineH();
            float new_px = drag_origin_px + (float)((int16_t)touch_down_y - (int16_t)ty);
            if (new_px < 0.f) new_px = 0.f;
            if (new_px > max_px) new_px = max_px;
            scroll_px = new_px;
            recordVel((int16_t)ty, millis());
            drawReadingLines();  // updates read_line internally from scroll_px
        }
        return;
    }

    // ── Finger lifted ─────────────────────────────────────────────────────
    if (!down && touch_was_down) {
        touch_was_down = false;
        if (scroll_dragging) {
            scroll_dragging = false;
            float v = computeFlingVel();
            if (fabsf(v) > 50.f) {
                fling_vel    = v < -4000.f ? -4000.f : v > 4000.f ? 4000.f : v;
                fling_active = true;
                fling_ms     = millis();
            } else {
                stopFling();
            }
            return;
        }
        // Tap without drag ─────────────────────────────────────────────────
        // If tap is in the verse-number column (x < 55) and in the content
        // area, toggle verse selection instead of doing page navigation.
        if ((int16_t)touch_down_x < 55
                && (int16_t)touch_down_y >= (int16_t)contentY()
                && (int16_t)touch_down_y < (int16_t)(contentY() + contentH())) {
            // Determine which line was tapped
            int16_t sub_px   = (int16_t)fmodf(scroll_px, (float)lineH());
            int16_t first_ln = (int16_t)(scroll_px / (float)lineH());
            int16_t row_off  = ((int16_t)touch_down_y - (int16_t)contentY() + sub_px) / (int16_t)lineH();
            int16_t line_idx = first_ln + row_off;

            // Scan backward from tapped line to find the owning verse number
            uint8_t v = 0;
            for (int16_t k = line_idx; k >= 0; k--) {
                if (k < (int16_t)line_count && lines[k][0] == '^') {
                    const char* pipe = strchr(lines[k] + 1, '|');
                    if (pipe) { v = (uint8_t)atoi(lines[k] + 1); break; }
                }
            }

            if (v > 0) {
                if (sel_verse_first == 0) {
                    // Nothing selected → select this verse
                    sel_verse_first = sel_verse_last = v;
                } else if (v >= sel_verse_first && v <= sel_verse_last) {
                    // Tap on already-selected verse
                    if (sel_verse_first == sel_verse_last) {
                        sel_verse_first = sel_verse_last = 0;  // deselect single
                    } else if (v == sel_verse_first) {
                        sel_verse_first++;                      // shrink from start
                    } else if (v == sel_verse_last) {
                        sel_verse_last--;                       // shrink from end
                    } else {
                        sel_verse_first = sel_verse_last = v;  // middle → reset to this
                    }
                } else if (v == sel_verse_last + 1) {
                    sel_verse_last = v;                         // extend end
                } else if (v + 1 == sel_verse_first) {
                    sel_verse_first = v;                        // extend start
                } else {
                    sel_verse_first = sel_verse_last = v;       // non-adjacent → reset
                }
                drawReadingLines();
                return;
            }
        }

        // Upper half = previous page/chapter, lower = next
        uint16_t mid = scrH() / 2;
        if (touch_down_y < mid) {
            int16_t prev = read_line - (int16_t)visLines();
            if (prev >= 0) {
                scroll_px = (float)prev * (float)lineH();
                drawReadingLines();
            } else if (scroll_px > 0.f) {
                scroll_px = 0.f;
                drawReadingLines();
            } else if (cur_chapter > 1) {
                goToReading(cur_chapter - 1);
            }
        } else {
            int16_t next = read_line + (int16_t)visLines();
            if (next < (int16_t)line_count) {
                scroll_px = (float)next * (float)lineH();
                drawReadingLines();
            } else if (cur_chapter < BOOKS[cur_book].chapters) {
                goToReading(cur_chapter + 1);
            }
        }
    }
}

void BibleInterface::handleSettingsInput() {
#ifdef HAS_CAP_TOUCH
    const uint8_t SETTINGS_N = 6;
#else
    const uint8_t SETTINGS_N = 7;
#endif
    uint16_t tx, ty;
    bool down = pollTouch(&tx, &ty);

    // ── Finger just touched down ──────────────────────────────────────────────
    if (down && !touch_was_down) {
        touch_was_down = true;
        touch_down_x   = tx;
        touch_down_y   = ty;

        // Header and nav fire immediately on press (no drag ambiguity)
        if (touchInHeader(tx, ty)) {
            touch_was_down = false;
            if (touchInSearchIcon(tx, ty)) { goToSearchInput(); return; }
            if (tx < 48) goBack();
            return;
        }
        if (touchInNav(tx, ty)) {
            touch_was_down = false;
            if (tx < scrW()/3) goBack();
            return;
        }
        // Highlight touched item immediately for press feedback (partial redraw, no fillScreen)
        int16_t hi = touchItem(tx, ty);
        if (hi >= 0 && hi < (int16_t)SETTINGS_N) {
            menu_sel = hi;
            redrawSettingsContent();
        }
        return;
    }

    // ── Finger held — no drag in settings ────────────────────────────────────
    if (down && touch_was_down) return;

    // ── Finger lifted — activate the item highlighted on press ───────────────
    if (!down && touch_was_down) {
        touch_was_down = false;
        if (menu_sel < 0 || menu_sel >= (int16_t)SETTINGS_N) return;

        switch (menu_sel) {
            case 0: // Font size
                font_num = (font_num == 1) ? 2 : (font_num == 2) ? 4 : 1;
                prefs.putUChar("font", font_num);
                redrawSettingsContent();
                break;
            case 1: // Theme — bg/header/nav all change, repaint each zone without fillScreen
                dark_mode = !dark_mode;
                prefs.putBool("dark", dark_mode);
                drawHeader("Settings");
                redrawSettingsContent();
                // Clear any empty content area below the settings rows
                {
                    int16_t empty_y = contentY() + (int16_t)SETTINGS_N * (int16_t)itemH();
                    int16_t empty_h = (int16_t)contentH() - (int16_t)SETTINGS_N * (int16_t)itemH();
                    if (empty_h > 0) tft.fillRect(0, empty_y, scrW(), empty_h, bg());
                }
                drawNavBar("Back", "", "");
                break;
            case 2: // Brightness — [+] button starts at scrW()-32; everything left = [-]
                if ((int16_t)touch_down_x >= (int16_t)scrW() - 32) {
                    if (bl_idx < 19) blSet(bl_idx + 1);
                } else {
                    if (bl_idx > 0)  blSet(bl_idx - 1);
                }
                redrawSettingsContent();
                break;
            case 3: // Translation cycle
                if (trans_count > 1) {
                    cur_trans = (cur_trans + 1) % trans_count;
                    prefs.putUChar("trans", cur_trans);
                    cached_book = 255;
                }
                redrawSettingsContent();
                break;
            case 4: // Highlight — [>] forward, [<] backward (boundary at scrW()-32)
                if ((int16_t)touch_down_x >= (int16_t)scrW() - 32)
                    accent_idx = (uint8_t)((accent_idx + 1) % ACCENT_COUNT);
                else
                    accent_idx = (accent_idx == 0) ? ACCENT_COUNT - 1 : accent_idx - 1;
                prefs.putUChar("accent", accent_idx);
                redrawSettingsContent();
                break;
            case 5: // Boot Marauder
                bootMarauder();
                return;
#ifndef HAS_CAP_TOUCH
            case 6: // Calibrate Touch
                runTouchCalibration();
                drawSettings();
                return;
#endif
        }
    }
}

void BibleInterface::handleBookmarksInput() {
    uint16_t tx, ty;
    bool down = pollTouch(&tx, &ty);

    // ── Delete confirmation popup ─────────────────────────────────────────────
    // While the popup is visible, block all normal list input and only act on
    // a tap inside the Delete button (anywhere else = Cancel).
    if (bm_confirm_pending) {
        if (down && !touch_was_down) {
            touch_was_down = true;
            touch_down_x = tx;   // save position — pollTouch doesn't write tx/ty on lift
            touch_down_y = ty;
        } else if (!down && touch_was_down) {
            touch_was_down = false;
            // Recompute Delete button bounds (must match drawConfirmDelete)
            int16_t pop_w  = (int16_t)scrW() - 40;
            int16_t pop_y  = (int16_t)(scrH() / 2) - 40;
            int16_t btn_y  = pop_y + 44;
            int16_t btn_h  = 28;
            int16_t half_w = pop_w / 2 - 6;
            int16_t del_x  = 20 + pop_w / 2 + 2;
            // Use saved down-position — tx/ty are stale on lift (pollTouch returns false)
            bool in_del = ((int16_t)touch_down_x >= del_x && (int16_t)touch_down_x < del_x + half_w &&
                           (int16_t)touch_down_y >= btn_y  && (int16_t)touch_down_y < btn_y + btn_h);
            if (in_del && bm_sel >= 0 && bm_sel < (int16_t)bm_count) {
                for (uint8_t i = (uint8_t)bm_sel; i < bm_count - 1; i++)
                    bookmarks[i] = bookmarks[i + 1];
                bm_count--;
                if (bm_sel >= (int16_t)bm_count && bm_sel > 0) bm_sel--;
                saveBookmarks();
            }
            bm_confirm_pending = false;
            needs_redraw = true;
        }
        return;
    }

    // ── Finger just touched down ─────────────────────────────────────────────
    if (down && !touch_was_down) {
        touch_was_down  = true;
        touch_down_x    = tx;
        touch_down_y    = ty;
        scroll_dragging = false;
        stopFling();
        drag_origin_px  = scroll_px;

        if (touchInHeader(tx, ty)) {
            touch_was_down = false;
            if (touchInSearchIcon(tx, ty)) { goToSearchInput(); return; }
            if (tx < 48) goBack();
            return;
        }
        if (touchInNav(tx, ty)) {
            touch_was_down = false;
            if (tx < scrW()/3) {
                goBack(); return;
            } else if (tx > 2*(scrW()/3)) {
                // "Del" — show confirmation popup
                if (bm_count > 0 && bm_sel >= 0 && bm_sel < (int16_t)bm_count) {
                    bm_confirm_pending = true;
                    needs_redraw = true;
                }
            } else {
                // "View" — navigate to the selected bookmark
                if (bm_count > 0 && bm_sel >= 0 && bm_sel < (int16_t)bm_count) {
                    jumpToBookmark((uint8_t)bm_sel);
                }
            }
            return;
        }
        // Highlight the touched bookmark immediately for press feedback
        int16_t hi = touchItem(tx, ty);
        if (hi >= 0 && (bm_scroll + hi) < (int16_t)bm_count) {
            bm_sel = bm_scroll + hi;
            redrawListContent(bm_count);
        }
        return;
    }

    // ── Finger held — pixel-accurate drag ────────────────────────────────────
    if (down && touch_was_down) {
        int16_t dy = (int16_t)ty - (int16_t)touch_down_y;
        if (!scroll_dragging && abs(dy) > 8) {
            scroll_dragging = true;
            bm_sel = -1;  // clear press highlight when drag begins
        }
        if (scroll_dragging) {
            float max_px = (float)max(0, (int)bm_count - (int)visItems()) * (float)itemH();
            float new_px = drag_origin_px + (float)((int16_t)touch_down_y - (int16_t)ty);
            if (new_px < 0.f) new_px = 0.f;
            if (new_px > max_px) new_px = max_px;
            scroll_px = new_px;
            bm_scroll = (int16_t)(scroll_px / (float)itemH());
            recordVel((int16_t)ty, millis());
            redrawListContent(bm_count);
        }
        return;
    }

    // ── Finger lifted ────────────────────────────────────────────────────────
    if (!down && touch_was_down) {
        touch_was_down = false;
        if (scroll_dragging) {
            scroll_dragging = false;
            float v = computeFlingVel();
            if (fabsf(v) > 50.f) {
                fling_vel    = v < -4000.f ? -4000.f : v > 4000.f ? 4000.f : v;
                fling_active = true;
                fling_ms     = millis();
            } else {
                stopFling();
            }
            return;
        }

        // Tap selects the bookmark; navigation is via the "View" button.
        // Nothing else to do on lift — selection was set on press.
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Navigation helpers
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::goToTransSelect() {
    stopFling();
    view     = BV_TRANS_SELECT;
    menu_sel = (int16_t)cur_trans;
    // Scroll to show selected translation
    menu_scroll = 0;
    if (cur_trans >= visItems()) {
        int16_t mid   = (int16_t)cur_trans - (int16_t)(visItems() / 2);
        int16_t max_s = (int16_t)trans_count - (int16_t)visItems();
        menu_scroll   = (mid > max_s) ? max_s : mid;
        if (menu_scroll < 0) menu_scroll = 0;
    }
    scroll_px    = (float)menu_scroll * (float)itemH();
    needs_redraw = true;
}
void BibleInterface::goToSection() {
    stopFling();
    view = BV_SECTION_SELECT;
    // Restore scroll to show current section (4 items always fit on screen)
    menu_sel    = (int16_t)cur_sec;
    menu_scroll = 0;
    scroll_px   = 0.f;
    needs_redraw = true;
}
void BibleInterface::goToBook(uint8_t sec) {
    stopFling();
    cur_sec = sec;
    view    = BV_BOOK_SELECT;
    // Scroll to show cur_book within this section
    uint8_t local_idx = 0;
    if (sec == BOOKS[cur_book].section) {
        local_idx = cur_book - SEC_BOOK_START[sec];
    }
    menu_sel    = (int16_t)local_idx;
    uint8_t cnt = SEC_BOOK_COUNT_[sec];
    menu_scroll = 0;
    if (local_idx >= visItems()) {
        int16_t mid = (int16_t)local_idx - (int16_t)(visItems() / 2);
        int16_t max_s = (int16_t)cnt - (int16_t)visItems();
        menu_scroll = (mid > max_s) ? max_s : mid;
        if (menu_scroll < 0) menu_scroll = 0;
    }
    scroll_px = (float)menu_scroll * (float)itemH();
    needs_redraw = true;
}
void BibleInterface::goToChapter(uint8_t book) {
    stopFling();
    cur_book = book;
    view     = BV_CHAPTER_SELECT;
    // Scroll grid to show cur_chapter
    const uint16_t tile_h   = 36;
    uint8_t        vis_rows = (uint8_t)(contentH() / tile_h);
    int16_t        row      = (int16_t)((cur_chapter - 1) / 5);
    int16_t        total_rows = ((int16_t)BOOKS[book].chapters + 4) / 5;
    int16_t        max_s    = total_rows - (int16_t)vis_rows;
    if (max_s < 0) max_s = 0;
    menu_scroll = row - (int16_t)(vis_rows / 2);
    if (menu_scroll > max_s)  menu_scroll = max_s;
    if (menu_scroll < 0)      menu_scroll = 0;
    menu_sel  = (int16_t)cur_chapter;   // 1-based; used for tile highlight
    scroll_px = (float)menu_scroll * (float)tile_h;
    needs_redraw = true;
}
void BibleInterface::drawLoading() {
    tft.fillScreen(bg());
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "%s %d", BOOKS[cur_book].display, cur_chapter);
    drawHeader(hdr);
    tft.setTextColor(dim_fg(), bg());
    tft.drawCentreString("Loading...", scrW() / 2, contentY() + contentH() / 2 - 8, 2);
}

void BibleInterface::goToReading(uint8_t chapter, int16_t start_line) {
    stopFling();
    sel_verse_first = sel_verse_last = 0;  // clear verse selection on chapter change
    cur_chapter = chapter;
    read_line   = start_line;
    scroll_px   = (float)start_line * (float)lineH();
    drawLoading();
    view        = BV_READING;
    cacheChapter(cur_book, cur_chapter);
    buildWrappedLines();
    saveState();
    needs_redraw = true;
}
void BibleInterface::goToSettings() {
    stopFling();
    view = BV_SETTINGS;
    menu_sel = 0;
    needs_redraw = true;
}
void BibleInterface::goToBookmarks() {
    stopFling();
    bm_confirm_pending = false;
    view = BV_BOOKMARKS;
    bm_sel = 0; bm_scroll = 0;
    scroll_px = 0.f;
    needs_redraw = true;
}
void BibleInterface::goBack() {
    stopFling();
    switch (view) {
        case BV_TRANS_SELECT:    /* already top */ break;
        case BV_SECTION_SELECT:
            if (trans_count > 1) goToTransSelect();
            break;
        case BV_BOOK_SELECT:     goToSection(); break;
        case BV_CHAPTER_SELECT:  goToBook(BOOKS[cur_book].section); break;
        case BV_READING:
            if (reading_from_search) {
                reading_from_search = false;
                highlight_verse     = 0;
                view      = BV_SEARCH_RESULTS;
                scroll_px = (float)menu_scroll * (float)srchH();
                needs_redraw = true;
            } else {
                goToChapter(cur_book);
            }
            break;
        case BV_SETTINGS:
        case BV_BOOKMARKS:
            bm_confirm_pending = false;
            if (cached_count > 0) {
                view = BV_READING;
                scroll_px = (float)read_line * (float)lineH();
            } else {
                view = BV_SECTION_SELECT;
                scroll_px = 0.f;
            }
            needs_redraw = true;
            break;
        case BV_SEARCH_RESULTS:
            goToSearchInput();
            break;
        case BV_SEARCH_INPUT:
            search_del_pending = false;
            if (cached_count > 0) {
                view = BV_READING;
                scroll_px = (float)read_line * (float)lineH();
            } else {
                view = BV_SECTION_SELECT;
                scroll_px = 0.f;
            }
            needs_redraw = true;
            break;
    }
}
void BibleInterface::addBookmarkCurrent() {
    BibleBookmark bm;
    bm.book        = cur_book;
    bm.chapter     = cur_chapter;
    bm.verse_first = sel_verse_first;
    bm.verse_last  = sel_verse_last;

    if (sel_verse_first > 0) {
        // Verse / range bookmark
        if (sel_verse_first == sel_verse_last)
            snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s %d:%d",
                     BOOKS[cur_book].display, cur_chapter, sel_verse_first);
        else
            snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s %d:%d-%d",
                     BOOKS[cur_book].display, cur_chapter,
                     sel_verse_first, sel_verse_last);
        // Reject exact duplicates (same book/chapter/verse range)
        for (uint8_t i = 0; i < bm_count; i++) {
            if (bookmarks[i].book == cur_book && bookmarks[i].chapter == cur_chapter
                    && bookmarks[i].verse_first == sel_verse_first
                    && bookmarks[i].verse_last  == sel_verse_last) {
                tft.fillRect(scrW()/2, scrH() - navH() - 18, scrW()/2, 16, (uint16_t)0xFD20);
                tft.setTextColor(TFT_BLACK, (uint16_t)0xFD20);
                tft.drawCentreString("Already saved", 3*scrW()/4, scrH() - navH() - 18, 1);
                delay(600);
                needs_redraw = true;
                return;
            }
        }
    } else {
        // Whole-chapter bookmark
        snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s %d",
                 BOOKS[cur_book].display, cur_chapter);
        for (uint8_t i = 0; i < bm_count; i++) {
            if (bookmarks[i].book == cur_book && bookmarks[i].chapter == cur_chapter
                    && bookmarks[i].verse_first == 0) {
                tft.fillRect(scrW()/2, scrH() - navH() - 18, scrW()/2, 16, (uint16_t)0xFD20);
                tft.setTextColor(TFT_BLACK, (uint16_t)0xFD20);
                tft.drawCentreString("Already saved", 3*scrW()/4, scrH() - navH() - 18, 1);
                delay(600);
                needs_redraw = true;
                return;
            }
        }
    }

    if (bm_count >= BIBLE_MAX_BM) return;
    bookmarks[bm_count++] = bm;
    saveBookmarks();

    // Clear verse selection after saving
    sel_verse_first = sel_verse_last = 0;

    // Brief visual feedback
    tft.fillRect(scrW()/2, scrH() - navH() - 18, scrW()/2, 16, TFT_GREEN);
    tft.setTextColor(TFT_BLACK, TFT_GREEN);
    tft.drawCentreString("Saved", 3*scrW()/4, scrH() - navH() - 18, 1);
    delay(600);
    needs_redraw = true;
}
void BibleInterface::jumpToBookmark(uint8_t bm_idx) {
    cur_book    = bookmarks[bm_idx].book;
    cur_chapter = bookmarks[bm_idx].chapter;
    cur_sec     = BOOKS[cur_book].section;
    goToReading(cur_chapter, 0);  // builds lines[], sets scroll_px=0

    // If it's a verse bookmark, scroll to the first selected verse
    uint8_t v = bookmarks[bm_idx].verse_first;
    if (v > 0) {
        for (int16_t k = 0; k < (int16_t)line_count; k++) {
            if (lines[k][0] == '^') {
                const char* pipe = strchr(lines[k] + 1, '|');
                if (pipe && (uint8_t)atoi(lines[k] + 1) == v) {
                    scroll_px = (float)k * (float)lineH();
                    read_line = k;
                    break;
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Momentum scroll helpers
// ─────────────────────────────────────────────────────────────────────────────

void BibleInterface::stopFling() {
    fling_active = false;
    fling_vel    = 0.f;
    memset(vbuf_y, 0, sizeof(vbuf_y));
    memset(vbuf_t, 0, sizeof(vbuf_t));
    vbuf_i = 0;
}

void BibleInterface::recordVel(int16_t y, uint32_t t) {
    vbuf_i          = (vbuf_i + 1) & 3;
    vbuf_y[vbuf_i]  = y;
    vbuf_t[vbuf_i]  = t;
}

float BibleInterface::computeFlingVel() const {
    uint8_t newest = vbuf_i;
    for (int back = 3; back >= 1; back--) {
        uint8_t old  = (vbuf_i - (uint8_t)back) & 3;
        uint32_t dt  = vbuf_t[newest] - vbuf_t[old];
        if (dt >= 8 && dt <= 120) {
            float vel = (float)(vbuf_y[newest] - vbuf_y[old]) / (float)dt;
            return -vel * 1000.f;   // px/s; negated: finger-up → positive fling
        }
    }
    return 0.f;
}

void BibleInterface::updateFling(uint32_t now) {
    float dt = (float)(now - fling_ms) * 0.001f;
    fling_ms = now;
    if (dt <= 0.f || dt > 0.5f) return;

    // Determine max_px and unit for the current view
    float max_px = 0.f;
    switch (view) {
        case BV_TRANS_SELECT:
            max_px = (float)max(0, (int)trans_count - (int)visItems()) * (float)itemH();
            break;
        case BV_SECTION_SELECT:
            max_px = 0.f;   // section list always fits, no fling needed
            break;
        case BV_BOOK_SELECT:
            max_px = (float)max(0, (int)SEC_BOOK_COUNT_[cur_sec] - (int)visItems()) * (float)itemH();
            break;
        case BV_CHAPTER_SELECT: {
            uint8_t  chaps      = BOOKS[cur_book].chapters;
            uint8_t  tile_h     = 36;
            uint8_t  vis_rows   = (uint8_t)(contentH() / tile_h);
            int16_t  total_rows = ((int16_t)chaps + 4) / 5;
            max_px = (float)max(0, (int)total_rows - (int)vis_rows) * (float)tile_h;
            break;
        }
        case BV_READING:
            max_px = (float)max(0, (int)line_count - (int)visLines()) * (float)lineH();
            break;
        case BV_BOOKMARKS:
            max_px = (float)max(0, (int)bm_count - (int)visItems()) * (float)itemH();
            break;
        case BV_SEARCH_INPUT:
            max_px = (float)max(0, (int)search_hist_count - (int)visItems()) * (float)itemH();
            break;
        case BV_SEARCH_RESULTS:
            max_px = (float)max(0, (int)search_result_count - (int)visSearchItems()) * (float)srchH();
            break;
        default:
            fling_active = false;
            return;
    }

    scroll_px += fling_vel * dt;
    if (scroll_px < 0.f)     scroll_px = 0.f;
    if (scroll_px > max_px)  scroll_px = max_px;

    // Exponential decay — velocity halves every ~350ms
    fling_vel *= expf(-2.0f * dt);

    if (fabsf(fling_vel) < 30.f || scroll_px <= 0.f || scroll_px >= max_px)
        fling_active = false;

    // Sync integer scroll vars and do a partial redraw
    switch (view) {
        case BV_TRANS_SELECT:
        case BV_SECTION_SELECT:
        case BV_BOOK_SELECT:
            menu_scroll = (int16_t)(scroll_px / (float)itemH());
            redrawListContent(
                view == BV_TRANS_SELECT   ? trans_count :
                view == BV_SECTION_SELECT ? BIBLE_SEC_COUNT :
                                            SEC_BOOK_COUNT_[cur_sec]);
            break;
        case BV_CHAPTER_SELECT:
            menu_scroll = (int16_t)(scroll_px / 36.f);
            redrawChapterContent();
            break;
        case BV_READING:
            // read_line is updated inside drawReadingLines via scroll_px
            drawReadingLines();
            break;
        case BV_BOOKMARKS:
            bm_scroll = (int16_t)(scroll_px / (float)itemH());
            redrawListContent(bm_count);
            break;
        case BV_SEARCH_INPUT:
            menu_scroll = (int16_t)(scroll_px / (float)itemH());
            redrawListContent(search_hist_count);
            break;
        case BV_SEARCH_RESULTS:
            menu_scroll = (int16_t)(scroll_px / (float)srchH());
            redrawSearchResultsContent();
            break;
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// XML streaming parser — caches all verses for one chapter
// ─────────────────────────────────────────────────────────────────────────────

// Read one byte from the XML file (buffered in 512-byte chunks)
bool BibleInterface::xmlNextByte(XmlState& s, char& c) {
    if (s.chunk_pos >= s.chunk_len) {
        s.chunk_len = s.f.read(s.chunk, XML_CHUNK_SIZE);
        s.chunk_pos = 0;
        if (s.chunk_len <= 0) return false;
    }
    c = (char)s.chunk[s.chunk_pos++];
    return true;
}

// Decode common XML entities in-place
void BibleInterface::xmlDecodeEntities(char* buf, size_t len) {
    struct { const char* ent; char ch; } map[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
        { "&apos;", '\'' }, { "&quot;", '"' }, { "&nbsp;", ' ' },
        { nullptr, 0 }
    };
    for (size_t i = 0; i < len && buf[i]; i++) {
        if (buf[i] != '&') continue;
        for (int m = 0; map[m].ent; m++) {
            size_t elen = strlen(map[m].ent);
            if (strncmp(buf + i, map[m].ent, elen) == 0) {
                buf[i] = map[m].ch;
                memmove(buf + i + 1, buf + i + elen, len - i - elen);
                break;
            }
        }
    }
}

// Compress 2-byte UTF-8 German sequences to private single-byte codes:
//   0x80=Ä  0x81=ä  0x82=Ö  0x83=ö  0x84=Ü  0x85=ü  0x86=ß (pixel-drawn)
// All other unrecognised multi-byte sequences: lead byte is kept, trail byte dropped.
// Uses read/write pointers so no additional buffer is needed.
void BibleInterface::utf8Encode(char* buf) {
    static const struct { uint8_t b1, b2, code; } map[] = {
        { 0xC3, 0x84, 0x80 },  // Ä
        { 0xC3, 0xA4, 0x81 },  // ä
        { 0xC3, 0x96, 0x82 },  // Ö
        { 0xC3, 0xB6, 0x83 },  // ö
        { 0xC3, 0x9C, 0x84 },  // Ü
        { 0xC3, 0xBC, 0x85 },  // ü
    };
    char* r = buf;
    char* w = buf;
    while (*r) {
        uint8_t b = (uint8_t)*r;
        if (b >= 0x80 && r[1]) {
            // ß → private code 0x86 (2 UTF-8 bytes → 1 private byte)
            if (b == 0xC3 && (uint8_t)r[1] == 0x9F) {
                *w++ = (char)0x86; r += 2; continue;
            }
            bool found = false;
            for (uint8_t i = 0; i < 6; i++) {
                if (b == map[i].b1 && (uint8_t)r[1] == map[i].b2) {
                    *w++ = (char)map[i].code; r += 2; found = true; break;
                }
            }
            if (!found) *w++ = *r++;   // unknown sequence: keep lead byte
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

// Measure pixel width of a string that may contain private codes 0x80-0x86.
// Substitutes the base letter for measurement (widths are nearly identical).
int16_t BibleInterface::textWidthUTF8(const char* str, uint8_t font) {
    static const char bases[] = {'A','a','O','o','U','u'};
    char tmp[BIBLE_LINE_BUF];
    size_t j = 0;
    for (const char* p = str; *p && j < BIBLE_LINE_BUF - 1; p++) {
        uint8_t c = (uint8_t)*p;
        if      (c >= 0x80 && c <= 0x85) tmp[j++] = bases[c - 0x80];
        else if (c == 0x86)               tmp[j++] = 'B';  // ß ≈ B width
        else                              tmp[j++] = *p;
    }
    tmp[j] = 0;
    return tft.textWidth(tmp, font);
}

// Draw a string that may contain private codes 0x80-0x86.
// Umlauts (0x80-0x85): base letter + two diaeresis dots via bibleDrawUmlautDots().
// ß (0x86): pixel-drawn glyph via bibleDrawSzlig().
void BibleInterface::drawStringUTF8(TFT_eSprite& spr, const char* str, int16_t x, int16_t y,
                                     uint8_t font, uint16_t color) {
    static const char bases[] = {'A','a','O','o','U','u'};

    for (const char* p = str; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c == 0x86) {
            x += bibleDrawSzlig(spr, x, y, font, color);
        } else if (c >= 0x80 && c <= 0x85) {
            int16_t w    = spr.drawChar((uint16_t)bases[c - 0x80], x, y, font);
            bool is_lower = ((c - 0x80) & 1) != 0;  // 0x81/0x83/0x85 = ä/ö/ü
            bibleDrawUmlautDots(spr, x, y, w, is_lower, font, color);
            x += w;
        } else {
            x += spr.drawChar((uint16_t)(uint8_t)*p, x, y, font);
        }
    }
}

// Render ß (private code 0x86) as a pixel-drawn glyph matching each font size.
// Returns the advance width (pixels to next character).
//
// Glyph designs (y = top of character cell, same coordinate as drawChar):
//
//   Font 1 (GLCD 8px) — 6-col canvas, advance 6
//     .##...   row 0  top arch
//     #..#..   row 1  open counters
//     #..#..   row 2
//     #.#...   row 3  diagonal mid-junction
//     #..#..   row 4
//     #...#.   row 5  lower lobe widens
//     #...#.   row 6
//     ..##..   row 7  bottom tail
//
//   Font 2 (Font16 16px) — 8-col canvas, advance 8
//     .###....  row 3  top arch (3px)
//     #...#...  row 4  open counters
//     ...
//     #.##....  row 7  mid junction (cols 2-3)
//     #...#...  row 8
//     #....#..  rows 9-11  lower lobe widens to col 5
//     #...#...  row 12
//     ..##....  row 13 bottom tail
//
//   Font 4 (Font32 26px) — 13-col canvas, advance 13
//     Smooth curved ß: 2px-wide left stem (cols 1-2, rows 5-17),
//     rounded upper loop, rounded lower lobe, curved bottom tail.
int16_t BibleInterface::drawSzlig(TFT_eSprite& spr, int16_t x, int16_t y,
                                   uint8_t font, uint16_t color) {
    return bibleDrawSzlig(spr, x, y, font, color);
}

// Extract attribute value from a tag string like:  verse osisID="Gen.1.1"
bool BibleInterface::xmlGetAttr(const char* tag, const char* attr, char* out, size_t out_len) {
    const char* p = strstr(tag, attr);
    if (!p) return false;
    p += strlen(attr);
    while (*p == ' ') p++;
    if (*p != '=') return false;
    p++;
    while (*p == ' ') p++;
    char delim = (*p == '"' || *p == '\'') ? *p++ : ' ';
    size_t n = 0;
    while (*p && *p != delim && n < out_len - 1)
        out[n++] = *p++;
    out[n] = 0;
    return (n > 0);
}

bool BibleInterface::cacheChapter(uint8_t book, uint8_t chapter) {
    if (cached_book == book && cached_chap == chapter) return (cached_count > 0);
    cached_count = 0;
    cached_book  = book;
    cached_chap  = chapter;

    if (trans_count == 0) return false;

    // ── Book byte-offset index ─────────────────────────────────────────────
    // On first access or when the translation changes, load (or build+save)
    // the .idx file so we can seek directly to this book instead of scanning
    // the whole XML from byte 0 every time.
    if (!book_idx_valid || book_idx_trans != cur_trans) {
        if (!loadBookIndex(trans_stems[cur_trans])) {
            buildBookIndex(trans_stems[cur_trans]);
            saveBookIndex(trans_stems[cur_trans]);
        }
        book_idx_valid = true;
        book_idx_trans = cur_trans;
    }

    // Build file path: /bible/<stem>.xml
    char path[64];
    snprintf(path, sizeof(path), "/bible/%s.xml", trans_stems[cur_trans]);

    File f = SD.open(path);
    if (!f) {
        Serial.print(F("[Bible] Cannot open ")); Serial.println(path);
        return false;
    }

    // Seek to the book's starting byte — skips all preceding books
    if (book_offsets[book] > 0)
        f.seek(book_offsets[book]);

    // Build the osisID prefix we are looking for: "Book.Chapter."
    // e.g. "Gen.1." for Genesis chapter 1
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%s.%d.", BOOKS[book].osis_code, chapter);
    size_t prefix_len = strlen(prefix);

    // Build the osisID prefix for the NEXT chapter so we know when to stop
    char next_prefix[32];
    snprintf(next_prefix, sizeof(next_prefix), "%s.%d.", BOOKS[book].osis_code, chapter + 1);

    XmlState s;
    memset(&s, 0, sizeof(s));
    s.f          = f;
    s.chunk_len  = 0;
    s.chunk_pos  = 0;
    s.in_verse   = false;
    s.in_note    = false;
    s.nest_level = 0;
    s.collecting = false;
    s.verse_text_len = 0;

    // State machine
    // States: outside tag, inside tag, inside verse text, inside note
    enum { ST_TEXT, ST_TAG } st = ST_TEXT;
    char c;
    char tag_buf[XML_TAG_BUF];
    int  tag_len = 0;
    bool done    = false;

    while (!done && xmlNextByte(s, c)) {
        if (st == ST_TEXT) {
            if (c == '<') {
                st = ST_TAG;
                tag_len = 0;
                continue;
            }
            // Collect text if we are inside a target verse and not in a note
            if (s.in_verse && !s.in_note && s.collecting) {
                if (s.verse_text_len < BIBLE_VERSE_BUF - 2) {
                    s.verse_text[s.verse_text_len++] = c;
                }
            }
        } else { // ST_TAG
            if (c == '>') {
                st = ST_TEXT;
                tag_buf[tag_len] = 0;

                bool closing = (tag_buf[0] == '/');
                const char* tname = closing ? tag_buf + 1 : tag_buf;

                // Skip XML declaration, comments, DOCTYPE
                if (tag_buf[0] == '?' || tag_buf[0] == '!') {
                    tag_len = 0; continue;
                }

                if (!closing) {
                    // Opening or self-closing tag
                    bool self_close = (tag_len > 0 && tag_buf[tag_len-1] == '/');
                    if (self_close) { tag_buf[tag_len-1] = 0; }

                    // Is this a <verse ...> tag?
                    if (strncmp(tname, "verse", 5) == 0 &&
                        (tname[5] == ' ' || tname[5] == '\t' || tname[5] == 0)) {
                        char osis_id[64] = {0};
                        if (xmlGetAttr(tag_buf, "osisID", osis_id, sizeof(osis_id))) {
                            if (strncmp(osis_id, prefix, prefix_len) == 0) {
                                // This is a verse we want
                                s.in_verse = true;
                                s.nest_level = 1;
                                s.verse_text_len = 0;
                                s.in_note = false;
                                s.collecting = !self_close;
                                if (self_close) {
                                    // Self-closing verse with no text (rare)
                                    s.in_verse = false;
                                }
                            } else if (strncmp(osis_id, next_prefix, strlen(next_prefix)) == 0) {
                                // We've gone past our chapter — stop
                                done = true;
                            } else if (s.in_verse) {
                                // Different verse — shouldn't happen inside our verse
                            }
                        }
                    } else if (s.in_verse) {
                        // Tag inside a verse
                        if (strncmp(tname, "note", 4) == 0) {
                            s.in_note = true;
                        }
                        if (!self_close) s.nest_level++;
                    }
                } else {
                    // Closing tag
                    if (strncmp(tname, "verse", 5) == 0 && s.in_verse) {
                        // End of verse — save it
                        s.verse_text[s.verse_text_len] = 0;
                        xmlDecodeEntities(s.verse_text, BIBLE_VERSE_BUF);
                        utf8Encode(s.verse_text);
                        // Trim trailing whitespace
                        int vlen = strlen(s.verse_text);
                        while (vlen > 0 && (s.verse_text[vlen-1] == ' ' ||
                               s.verse_text[vlen-1] == '\n' ||
                               s.verse_text[vlen-1] == '\r')) {
                            s.verse_text[--vlen] = 0;
                        }
                        if (cached_count < BIBLE_MAX_VERSES_CACHED) {
                            strncpy(verse_buf[cached_count], s.verse_text, BIBLE_VERSE_BUF - 1);
                            verse_buf[cached_count][BIBLE_VERSE_BUF - 1] = 0;
                            cached_count++;
                        }
                        s.in_verse = false;
                        s.in_note  = false;
                        s.nest_level = 0;
                        s.collecting = false;
                    } else if (s.in_verse) {
                        if (strncmp(tname, "note", 4) == 0) s.in_note = false;
                        if (s.nest_level > 1) s.nest_level--;
                    }
                }
                tag_len = 0;
            } else {
                if (tag_len < XML_TAG_BUF - 1)
                    tag_buf[tag_len++] = c;
            }
        }
    }

    f.close();
    Serial.printf("[Bible] Cached %d verses for %s ch %d\n",
                  cached_count, BOOKS[book].display, chapter);
    return (cached_count > 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Text wrapping — builds lines[] from verse_buf[]
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::buildWrappedLines() {
    line_count = 0;
    uint16_t max_px = scrW() - 14;  // 4px left pad + 10px right pad

    for (uint8_t v = 0; v < cached_count && line_count < BIBLE_MAX_LINES; v++) {
        addWrappedLine(v + 1, verse_buf[v], max_px, font_num, line_count);
    }
}

void BibleInterface::addWrappedLine(uint8_t verse_num, const char* text,
                                     uint16_t max_px, uint8_t fnt, uint16_t& idx) {
    const char* p            = text;
    bool        is_first_line = true;   // true only for the verse's first screen line

    // For the verse-first line the verse number prefix is drawn to the left of the
    // content text, so the available content width is narrowed by its pixel width.
    uint16_t first_max_px = max_px;
    if (verse_num > 0) {
        char num_str[12];
        snprintf(num_str, sizeof(num_str), "%d.", verse_num);
        int16_t num_w = textWidthUTF8(num_str, fnt) + 2;  // +2px gap
        first_max_px  = (num_w < (int16_t)max_px) ? (uint16_t)(max_px - num_w) : 0;
    }

    while (*p && idx < BIBLE_MAX_LINES) {
        char* out     = lines[idx];
        int   out_len = 0;

        uint16_t line_max = is_first_line ? first_max_px : max_px;

        if (is_first_line) {
            // Prefix: "^N|" where N is the verse number
            out_len = snprintf(out, BIBLE_LINE_BUF, "^%d|", verse_num);
        } else {
            out[0] = 0;
        }
        is_first_line = false;

        // Fill the line word by word
        char tmp[BIBLE_LINE_BUF];
        while (*p) {
            // Advance to end of next word
            const char* wp = p;
            while (*wp && *wp != ' ') wp++;
            // Include trailing space if present
            int wlen = (int)(wp - p) + (*wp == ' ' ? 1 : 0);

            // Test fit
            int tmp_len = out_len + wlen;
            if (tmp_len >= BIBLE_LINE_BUF) break; // line buffer full

            memcpy(tmp, out, out_len);
            memcpy(tmp + out_len, p, wlen);
            tmp[tmp_len] = 0;

            // Strip the "^N|" prefix for pixel-width measurement
            const char* measure = tmp;
            if (measure[0] == '^') {
                measure = strchr(measure, '|');
                if (measure) measure++; else measure = tmp;
            }

            int16_t px = textWidthUTF8(measure, fnt);
            if (px > (int16_t)line_max && out_len > 0) break; // word doesn't fit

            memcpy(out, tmp, tmp_len + 1);
            out_len = tmp_len;
            p = wp;
            if (*p == ' ') p++;
        }

        // Nothing fit on this line (single word wider than line_max).
        // Detect: continuation line with nothing added (out_len==0), OR verse-first
        // line with only the "^N|" prefix and no content added.
        int prefix_len = (out[0] == '^') ? (int)(strchr(out, '|') - out + 1) : 0;
        if (out_len <= prefix_len) {
            // Force-add characters until the pixel budget is exhausted.
            // Preserve any existing prefix in out[0..prefix_len-1].
            char tmp2[BIBLE_LINE_BUF];
            memcpy(tmp2, out, prefix_len);  // copy prefix (or nothing)
            int n = prefix_len;
            while (*p && n < BIBLE_LINE_BUF - 1) {
                tmp2[n++] = *p++;
                tmp2[n]   = 0;
                // Measure only the content part (after prefix)
                const char* measure2 = tmp2 + prefix_len;
                if (textWidthUTF8(measure2, fnt) > (int16_t)line_max) {
                    // Back off the last character
                    if (n > prefix_len) { n--; tmp2[n] = 0; p--; }
                    break;
                }
            }
            memcpy(out, tmp2, n + 1);
            out_len = n;
            if (out_len <= prefix_len) { p++; }  // unprintably wide single char — skip
        }

        idx++;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Persistence
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::saveState() {
    prefs.putUChar("book",      cur_book);
    prefs.putUChar("chap",      cur_chapter);
    prefs.putUChar("trans",     cur_trans);
    prefs.putUChar("font",      font_num);
    prefs.putBool ("dark",      dark_mode);
    prefs.putUChar("accent",    accent_idx);
    prefs.putBool ("srch_part", srch_partial_match);
    prefs.putBool ("srch_pnct", srch_ignore_punct);
    prefs.putUChar("srch_scp",  srch_scope);
}

void BibleInterface::loadState() {
    cur_book          = prefs.getUChar("book",      0);
    cur_chapter       = prefs.getUChar("chap",      1);
    cur_trans         = prefs.getUChar("trans",     0);
    font_num          = prefs.getUChar("font",      2);
    dark_mode         = prefs.getBool ("dark",      true);
    accent_idx        = prefs.getUChar("accent",    0);
    srch_partial_match = prefs.getBool ("srch_part", true);
    srch_ignore_punct  = prefs.getBool ("srch_pnct", true);
    srch_scope         = prefs.getUChar("srch_scp",  0);
    if (cur_book   >= BIBLE_BOOK_COUNT) cur_book   = 0;
    if (cur_chapter == 0)               cur_chapter = 1;
    if (cur_trans  >= BIBLE_MAX_TRANS)  cur_trans  = 0;
    if (font_num != 1 && font_num != 2 && font_num != 4) font_num = 2;
    if (accent_idx >= ACCENT_COUNT)     accent_idx = 0;
    if (srch_scope >= 3)                srch_scope  = 0;
    // Derive section from book so navigation back shows correct highlight
    cur_sec = BOOKS[cur_book].section;
}

void BibleInterface::saveBookmarks() {
    // Format: "bookIdx chapter verse_first verse_last label\n"
    File f = SD.open(BIBLE_BM_FILE, FILE_WRITE);
    if (!f) return;
    for (uint8_t i = 0; i < bm_count; i++) {
        f.printf("%d %d %d %d %s\n",
                 bookmarks[i].book, bookmarks[i].chapter,
                 bookmarks[i].verse_first, bookmarks[i].verse_last,
                 bookmarks[i].label);
    }
    f.close();
}

void BibleInterface::loadBookmarks() {
    bm_count = 0;
    File f = SD.open(BIBLE_BM_FILE);
    if (!f) return;
    while (f.available() && bm_count < BIBLE_MAX_BM) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        int s1 = line.indexOf(' ');
        int s2 = (s1 >= 0) ? line.indexOf(' ', s1 + 1) : -1;
        if (s1 < 0 || s2 < 0) continue;
        uint8_t book = (uint8_t)line.substring(0, s1).toInt();
        uint8_t chap = (uint8_t)line.substring(s1 + 1, s2).toInt();
        if (book >= BIBLE_BOOK_COUNT || chap == 0) continue;

        // Detect format: new has two more integer tokens before the label.
        // Check tokens 3 and 4 — if both are all-digit, it is new format.
        // This handles book names starting with a digit (1Ki, 2Co, 1John, etc.)
        // because those names contain letters and will fail the all-digit check.
        int s3 = line.indexOf(' ', s2 + 1);
        int s4 = (s3 >= 0) ? line.indexOf(' ', s3 + 1) : -1;
        bool new_fmt = false;
        if (s3 > s2 + 1 && s4 > s3 + 1) {
            new_fmt = true;
            for (int k = s2 + 1; k < s3 && new_fmt; k++)
                if (!isdigit((unsigned char)line[k])) new_fmt = false;
            for (int k = s3 + 1; k < s4 && new_fmt; k++)
                if (!isdigit((unsigned char)line[k])) new_fmt = false;
        }

        uint8_t v1 = 0, v2 = 0;
        String  label;
        if (new_fmt) {
            v1    = (uint8_t)line.substring(s2 + 1, s3).toInt();
            v2    = (uint8_t)line.substring(s3 + 1, s4).toInt();
            label = line.substring(s4 + 1);
        } else {
            label = line.substring(s2 + 1);
        }

        bookmarks[bm_count].book        = book;
        bookmarks[bm_count].chapter     = chap;
        bookmarks[bm_count].verse_first = v1;
        bookmarks[bm_count].verse_last  = v2;
        strncpy(bookmarks[bm_count].label, label.c_str(), BIBLE_BM_LABEL_LEN - 1);
        bookmarks[bm_count].label[BIBLE_BM_LABEL_LEN - 1] = 0;
        bm_count++;
    }
    f.close();
}

void BibleInterface::scanTranslations() {
    trans_count = 0;
    File root = SD.open(BIBLE_SD_BASE);
    if (!root) {
        Serial.println(F("[Bible] /bible/ not found"));
        return;
    }
    while (trans_count < BIBLE_MAX_TRANS) {
        File entry = root.openNextFile();
        if (!entry) break;
        if (entry.isDirectory()) { entry.close(); continue; }
        String name = entry.name();
        entry.close();
        // Accept only .xml files
        name.toLowerCase();
        if (!name.endsWith(".xml")) continue;
        // Strip path prefix if present
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        // Remove .xml extension to get the stem
        String stem = name.substring(0, name.length() - 4);
        stem.toUpperCase();  // display as "ASV", "WEB", etc.
        strncpy(trans_stems[trans_count], stem.c_str(), BIBLE_TRANS_LEN - 1);
        trans_stems[trans_count][BIBLE_TRANS_LEN - 1] = 0;
        trans_count++;
        Serial.printf("[Bible] Found translation: %s\n", trans_stems[trans_count - 1]);
    }
    root.close();
    // Restore lowercase stems (paths need lowercase for SD.open)
    for (uint8_t i = 0; i < trans_count; i++) {
        String s = trans_stems[i];
        s.toLowerCase();
        strncpy(trans_stems[i], s.c_str(), BIBLE_TRANS_LEN - 1);
        trans_stems[i][BIBLE_TRANS_LEN - 1] = 0;
    }
    if (cur_trans >= trans_count) cur_trans = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Book byte-offset index
//
// Index file: /bible/<stem>.idx  (binary, 316 bytes)
//   uint32_t magic     = 0x42494458  ('B','I','D','X')
//   uint32_t version   = 1
//   uint32_t xml_size               (size of the .xml — detects replacement)
//   uint32_t offsets[BIBLE_BOOK_COUNT]  (byte offset of each book's first verse)
//
// offset == 0 means "not found in this translation" → fall back to full scan.
// ─────────────────────────────────────────────────────────────────────────────

bool BibleInterface::loadBookIndex(const char* stem) {
    char idx_path[64];
    snprintf(idx_path, sizeof(idx_path), "/bible/%s.idx", stem);
    File fi = SD.open(idx_path);
    if (!fi) return false;

    // Validate file size: header(12) + offsets(76*4) = 316 bytes
    const uint32_t expected = 12 + (uint32_t)BIBLE_BOOK_COUNT * sizeof(uint32_t);
    if ((uint32_t)fi.size() != expected) { fi.close(); return false; }

    uint32_t magic, ver, xml_size;
    fi.read((uint8_t*)&magic,    4);
    fi.read((uint8_t*)&ver,      4);
    fi.read((uint8_t*)&xml_size, 4);
    if (magic != 0x42494458UL || ver != 1) { fi.close(); return false; }

    // Compare stored XML size against the actual file — detects replaced translations
    char xml_path[64];
    snprintf(xml_path, sizeof(xml_path), "/bible/%s.xml", stem);
    File fx = SD.open(xml_path);
    if (!fx) { fi.close(); return false; }
    uint32_t actual_xml = (uint32_t)fx.size();
    fx.close();
    if (actual_xml != xml_size) { fi.close(); return false; }

    fi.read((uint8_t*)book_offsets, BIBLE_BOOK_COUNT * sizeof(uint32_t));
    fi.close();
    Serial.printf("[Bible] Loaded index: %s\n", idx_path);
    return true;
}

// Scan the XML file and record the byte offset of the first <verse> tag for
// each book.  Displays a "Building index..." screen while working.
bool BibleInterface::buildBookIndex(const char* stem) {
    char path[64];
    snprintf(path, sizeof(path), "/bible/%s.xml", stem);
    File f = SD.open(path);
    if (!f) return false;

    // Inform the user — this is a one-time scan that can take a few seconds
    tft.fillScreen(bg());
    tft.setTextColor(fg(), bg());
    tft.drawCentreString("Building index...", scrW() / 2, scrH() / 2 - 8, 2);
    tft.setTextColor(dim_fg(), bg());
    tft.drawCentreString("(first run only)", scrW() / 2, scrH() / 2 + 14, 2);

    memset(book_offsets, 0, sizeof(book_offsets));

    XmlState s;
    memset(&s, 0, sizeof(s));
    s.f = f;

    uint8_t  found     = 0;
    uint32_t tag_start = 0;
    bool     in_tag    = false;
    char     tag_buf[XML_TAG_BUF];
    int      tag_len   = 0;
    char     c;

    while (found < BIBLE_BOOK_COUNT && xmlNextByte(s, c)) {
        if (!in_tag) {
            if (c == '<') {
                // Record the absolute byte position of this '<'
                tag_start = (uint32_t)((int32_t)s.f.position()
                                       - s.chunk_len + s.chunk_pos - 1);
                in_tag  = true;
                tag_len = 0;
            }
        } else {
            if (c == '>') {
                in_tag = false;
                tag_buf[tag_len] = 0;

                // Skip closing / declaration / comment tags
                if (tag_buf[0] == '/' || tag_buf[0] == '?' || tag_buf[0] == '!') {
                    tag_len = 0;
                    continue;
                }

                // Only <verse ...> tags carry book information
                if (strncmp(tag_buf, "verse", 5) == 0 &&
                    (tag_buf[5] == ' ' || tag_buf[5] == '\t' || tag_buf[5] == 0)) {
                    char osis_id[64] = {0};
                    if (xmlGetAttr(tag_buf, "osisID", osis_id, sizeof(osis_id))) {
                        // osisID format: "BookCode.chapter.verse"
                        char* dot = strchr(osis_id, '.');
                        if (dot) {
                            int code_len = (int)(dot - osis_id);
                            for (uint8_t b = 0; b < BIBLE_BOOK_COUNT; b++) {
                                if (book_offsets[b] == 0 &&
                                    (int)strlen(BOOKS[b].osis_code) == code_len &&
                                    strncmp(osis_id, BOOKS[b].osis_code, code_len) == 0) {
                                    book_offsets[b] = tag_start;
                                    found++;
                                    Serial.printf("[Bible] idx: %s @ %u\n",
                                                  BOOKS[b].osis_code, tag_start);
                                    break;
                                }
                            }
                        }
                    }
                }
                tag_len = 0;
            } else {
                if (tag_len < XML_TAG_BUF - 1)
                    tag_buf[tag_len++] = c;
            }
        }
    }

    f.close();
    Serial.printf("[Bible] Built index: %u/%u books found\n", found, BIBLE_BOOK_COUNT);
    return (found > 0);
}

void BibleInterface::saveBookIndex(const char* stem) {
    // Embed the current XML file size so we can detect if it gets replaced
    char xml_path[64];
    snprintf(xml_path, sizeof(xml_path), "/bible/%s.xml", stem);
    File fx = SD.open(xml_path);
    if (!fx) return;
    uint32_t xml_size = (uint32_t)fx.size();
    fx.close();

    char idx_path[64];
    snprintf(idx_path, sizeof(idx_path), "/bible/%s.idx", stem);
    SD.remove(idx_path);   // overwrite any stale index
    File fo = SD.open(idx_path, FILE_WRITE);
    if (!fo) return;

    uint32_t magic = 0x42494458UL;
    uint32_t ver   = 1;
    fo.write((uint8_t*)&magic,       4);
    fo.write((uint8_t*)&ver,         4);
    fo.write((uint8_t*)&xml_size,    4);
    fo.write((uint8_t*)book_offsets, BIBLE_BOOK_COUNT * sizeof(uint32_t));
    fo.close();
    Serial.printf("[Bible] Saved index: %s\n", idx_path);
}

// ─────────────────────────────────────────────────────────────────────────────
// Brightness
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::blInit() {
    bl_idx = prefs.getUChar("bright", 19);
    if (bl_idx >= 20) bl_idx = 19;
#ifndef HAS_MINI_SCREEN
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(TFT_BL, 5000, 8);
    ledcWrite(TFT_BL, BL_LEVELS[bl_idx]);
  #else
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_BL, 0);
    ledcWrite(0, BL_LEVELS[bl_idx]);
  #endif
#else
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
#endif
}

void BibleInterface::blSet(uint8_t idx) {
    if (idx >= 20) idx = 19;
    bl_idx = idx;
    prefs.putUChar("bright", bl_idx);
#ifndef HAS_MINI_SCREEN
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(TFT_BL, BL_LEVELS[bl_idx]);
  #else
    ledcWrite(0, BL_LEVELS[bl_idx]);
  #endif
#endif
}

void BibleInterface::blCycle() {
    blSet((bl_idx + 1) % 20);
}

// ─────────────────────────────────────────────────────────────────────────────
// Battery gauge — MAX17048 direct I2C register access (no external library)
// Register 0x04 = SOC: high byte = integer %, low byte = 1/256 % (discarded)
// ─────────────────────────────────────────────────────────────────────────────
#ifdef HAS_BATTERY
void BibleInterface::battInit() {
#ifndef HAS_CAP_TOUCH
    // Cap-touch boards already called Wire.begin() in ft6336_init()
    Wire.begin(I2C_SDA, I2C_SCL);
#endif
    Wire.beginTransmission(0x36);
    batt_ok  = (Wire.endTransmission() == 0);
    batt_pct = -1;
    if (batt_ok) {
        battUpdate();
        Serial.println(F("[Battery] MAX17048 OK"));
    } else {
        Serial.println(F("[Battery] MAX17048 not found"));
    }
}
void BibleInterface::battUpdate() {
    Wire.beginTransmission(0x36);
    Wire.write(0x04);   // SOC register
    if (Wire.endTransmission(false) != 0) { batt_ok = false; return; }
    Wire.requestFrom((uint8_t)0x36, (uint8_t)2);
    if (Wire.available() < 2) return;
    uint8_t hi = Wire.read();
    Wire.read();  // fractional byte — discard
    batt_pct = (int8_t)((hi > 100) ? 100 : hi);
    batt_ms  = millis();
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Search — touch helper
// ─────────────────────────────────────────────────────────────────────────────
bool BibleInterface::touchInSearchIcon(uint16_t x, uint16_t y) {
    if (y >= hdrH()) return false;
    // Button spans scrW()-63 to scrW()-35 (28px wide, matches drawHeader)
    int16_t sb_x = (int16_t)scrW() - 63;
    return ((int16_t)x >= sb_x && (int16_t)x < sb_x + 28);
}

// ─────────────────────────────────────────────────────────────────────────────
// Search — navigation helpers
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::goToSearchInput() {
    stopFling();
    search_del_pending = false;
    view            = BV_SEARCH_INPUT;
    search_hist_sel = 0;
    menu_scroll     = 0;
    scroll_px       = 0.f;
    needs_redraw    = true;
}

void BibleInterface::goToSearchResults() {
    stopFling();
    view           = BV_SEARCH_RESULTS;
    search_res_sel = 0;
    menu_scroll    = 0;
    scroll_px      = 0.f;
    needs_redraw   = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Search — draw views
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::drawSearchInput() {
    tft.fillScreen(bg());
    drawHeader("Search", true);

    if (search_hist_count == 0) {
        tft.setTextColor(dim_fg(), bg());
        tft.drawCentreString("No search history", scrW() / 2, contentY() + 30, 2);
        tft.setTextColor(dim_fg(), bg());
        tft.drawCentreString("Tap  New  to search", scrW() / 2, contentY() + 56, 1);
    } else {
        uint8_t vis = visItems();
        for (uint8_t i = 0; i < vis && (menu_scroll + i) < search_hist_count; i++) {
            bool sel = ((menu_scroll + i) == (uint8_t)search_hist_sel);
            drawListRow(contentY() + i * itemH(), search_hist[menu_scroll + i], sel, false);
        }
        drawScrollBar(search_hist_count, vis, menu_scroll);
    }
    drawNavBar("New", "View", "Del");
    if (search_del_pending) drawSearchDelConfirm();
}

void BibleInterface::drawSearchDelConfirm() {
    int16_t pop_w = (int16_t)scrW() - 40;
    int16_t pop_h = 80;
    int16_t pop_x = 20;
    int16_t pop_y = (int16_t)(scrH() / 2) - 40;

    tft.fillRoundRect(pop_x,     pop_y,     pop_w,     pop_h,     6, bg());
    tft.drawRoundRect(pop_x,     pop_y,     pop_w,     pop_h,     6, dim_fg());
    tft.drawRoundRect(pop_x + 1, pop_y + 1, pop_w - 2, pop_h - 2, 6, dim_fg());

    tft.setTextColor(fg(), bg());
    tft.drawCentreString("Delete from history?", scrW() / 2, pop_y + 10, 2);

    int16_t btn_y  = pop_y + 44;
    int16_t btn_h  = 28;
    int16_t half_w = pop_w / 2 - 6;
    int16_t del_x  = pop_x + pop_w / 2 + 2;

    tft.fillRoundRect(pop_x + 4, btn_y, half_w, btn_h, 4, hdr_bg());
    tft.drawRoundRect(pop_x + 4, btn_y, half_w, btn_h, 4, dim_fg());
    tft.setTextColor(TFT_WHITE, hdr_bg());
    tft.drawCentreString("Cancel", pop_x + 4 + half_w / 2, btn_y + (btn_h - 8) / 2, 1);

    tft.fillRoundRect(del_x, btn_y, half_w, btn_h, 4, hdr_bg());
    tft.drawRoundRect(del_x, btn_y, half_w, btn_h, 4, TFT_RED);
    tft.setTextColor(TFT_RED, hdr_bg());
    tft.drawCentreString("Delete", del_x + half_w / 2, btn_y + (btn_h - 8) / 2, 1);
}

// Draws one search result row: reference on top line, snippet (with highlighted
// query text) on second line.  y_px is the absolute screen Y of the row top.
// Drawing is clipped by any active TFT viewport (set by redrawSearchResultsContent).
void BibleInterface::drawSearchResultRow(int16_t y_px, uint16_t idx, bool sel) {
    const int16_t SBAR_W  = 6;
    const int16_t PADDING = 4;
    int16_t row_w = (int16_t)scrW() - SBAR_W;
    int16_t row_h = (int16_t)srchH();

    uint16_t bg_col = sel ? sel_bg() : bg();
    tft.fillRect(0, y_px, row_w, row_h, bg_col);
    // Divider line at bottom of row
    tft.drawFastHLine(0, y_px + row_h - 1, row_w, dim_fg());

    // Reference "Book Ch:Vs" in font 2 (16px)
    {
        BibleSearchResult& r = search_results[idx];
        char ref[40];
        snprintf(ref, sizeof(ref), "%s %d:%d",
                 BOOKS[r.book].display, (int)r.chapter, (int)r.verse);
        tft.setTextColor(fg(), bg_col);
        tft.setTextDatum(TL_DATUM);
        tft.drawString(ref, PADDING, y_px + 3, 2);
    }

    // Snippet in font 1 (8px) with highlighted query segments.
    // disp/qdisp hold ASCII base letters (for case-insensitive match); snip holds
    // the original private codes so rendering shows proper umlauts/ß.
    // Private codes are 1-byte just like ASCII, so indices are identical between
    // snip and disp — we match on disp, render from snip.
    int16_t snip_y = y_px + 23;
    const char* snip = search_results[idx].snippet;

    static const char bases[] = {'A','a','O','o','U','u','B'};
    char disp[BIBLE_SRCH_SNIPPET_LEN];
    {
        const char* src = snip;
        size_t di = 0;
        for (; *src && di < BIBLE_SRCH_SNIPPET_LEN - 1; src++) {
            uint8_t bc = (uint8_t)*src;
            disp[di++] = (bc >= 0x80 && bc <= 0x86) ? bases[bc - 0x80] : *src;
        }
        disp[di] = 0;
    }
    char qdisp[BIBLE_SEARCH_QUERY_LEN];
    {
        const char* qs = search_query;
        size_t qi = 0;
        for (; *qs && qi < BIBLE_SEARCH_QUERY_LEN - 1; qs++) {
            uint8_t bc = (uint8_t)*qs;
            qdisp[qi++] = (bc >= 0x80 && bc <= 0x86) ? bases[bc - 0x80] : *qs;
        }
        qdisp[qi] = 0;
    }
    size_t qlen = strlen(qdisp);

    // Build the set of tokens to highlight.
    // Phrase mode: one token = the full (possibly punct-stripped) query.
    // Partial mode: one token per space-delimited word.
    // Each token is punct-stripped when srch_ignore_punct is on.
    struct Token { char s[BIBLE_SEARCH_QUERY_LEN]; size_t len; };
    Token  tokens[16];
    int    ntokens = 0;

    // Source string for splitting: qdisp (private-codes mapped to ASCII bases)
    // with punctuation stripped if needed.
    char qbase[BIBLE_SEARCH_QUERY_LEN];  // punct-stripped qdisp
    {
        size_t qi = 0;
        for (size_t i = 0; qdisp[i] && qi < BIBLE_SEARCH_QUERY_LEN - 1; i++) {
            uint8_t c = (uint8_t)qdisp[i];
            if (srch_ignore_punct && c < 0x80 && ispunct((int)c)) continue;
            qbase[qi++] = qdisp[i];
        }
        qbase[qi] = 0;
    }

    if (srch_partial_match) {
        const char* p = qbase;
        while (*p && ntokens < 16) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char* ws = p;
            while (*p && *p != ' ') p++;
            size_t wl = (size_t)(p - ws);
            if (wl > 0 && wl < BIBLE_SEARCH_QUERY_LEN) {
                memcpy(tokens[ntokens].s, ws, wl);
                tokens[ntokens].s[wl] = 0;
                tokens[ntokens].len   = wl;
                ntokens++;
            }
        }
    } else {
        size_t bl = strlen(qbase);
        if (bl > 0) {
            memcpy(tokens[0].s, qbase, bl + 1);
            tokens[0].len = bl;
            ntokens = 1;
        }
    }

    tft.setTextDatum(TL_DATUM);
    int16_t sx     = PADDING;
    int16_t max_sx = row_w - PADDING;
    size_t  si     = 0;

    while (disp[si] && sx < max_sx) {
        // Try each token at position si; take the first match.
        bool   found     = false;
        size_t match_end = 0;

        for (int t = 0; t < ntokens && !found; t++) {
            const char* tok = tokens[t].s;
            size_t      tl  = tokens[t].len;
            if (tl == 0) continue;

            if (srch_ignore_punct) {
                // Skip punct in disp while consuming token chars
                size_t qi = 0, di = si;
                while (qi < tl) {
                    while (disp[di] && (uint8_t)disp[di] < 0x80 && ispunct((int)(uint8_t)disp[di]))
                        di++;
                    if (!disp[di]) break;
                    if (tolower((uint8_t)disp[di]) != tolower((uint8_t)tok[qi])) break;
                    di++; qi++;
                }
                if (qi == tl) { found = true; match_end = di; }
            } else {
                bool m = true;
                for (size_t j = 0; j < tl && m; j++) {
                    if (!disp[si + j] ||
                        tolower((uint8_t)disp[si + j]) != tolower((uint8_t)tok[j]))
                        m = false;
                }
                if (m) { found = true; match_end = si + tl; }
            }
        }

        if (found) {
            tft.setTextColor((uint16_t)0xFD20, bg_col);
            for (size_t j = si; j < match_end && sx < max_sx; j++)
                sx += tftCharUTF8(tft, (uint8_t)snip[j], sx, snip_y, 1, (uint16_t)0xFD20);
            si = match_end;
        } else {
            uint16_t text_color = sel ? fg() : dim_fg();
            tft.setTextColor(text_color, bg_col);
            sx += tftCharUTF8(tft, (uint8_t)snip[si], sx, snip_y, 1, text_color);
            si++;
        }
    }
}

// Partial redraw of search result list (no header/nav repaint).
// Uses scroll_px for sub-row pixel accuracy and a viewport to clip rows.
void BibleInterface::redrawSearchResultsContent() {
    int16_t sub_px    = (int16_t)fmodf(scroll_px, (float)srchH());
    int16_t first     = (int16_t)(scroll_px / (float)srchH());
    int16_t cTop      = (int16_t)contentY();
    int16_t cEnd      = cTop + (int16_t)contentH();

    tft.startWrite();
    tft.setViewport(0, cTop, scrW(), contentH(), false);

    // Draw each visible row (each fills its own background — no upfront clear,
    // which would cause a white/black flash between frames).
    int16_t last_bottom = cTop;
    for (int i = 0; ; i++) {
        int16_t idx = first + i;
        int16_t y   = cTop - sub_px + i * (int16_t)srchH();
        if (y >= cEnd || idx >= (int16_t)search_result_count) break;
        drawSearchResultRow(y, (uint16_t)idx, idx == search_res_sel);
        int16_t bot = y + (int16_t)srchH();
        if (bot > last_bottom) last_bottom = bot;
    }
    // Clear any unused space below the last row (list shorter than content zone)
    if (last_bottom < cEnd)
        tft.fillRect(0, last_bottom, (int16_t)scrW() - 6, cEnd - last_bottom, bg());

    drawScrollBar(search_result_count, visSearchItems(), first);
    tft.resetViewport();
    tft.endWrite();
}

void BibleInterface::drawSearchResults() {
    tft.fillScreen(bg());
    char hdr[32];
    if (search_result_count >= BIBLE_MAX_SEARCH_RESULTS)
        snprintf(hdr, sizeof(hdr), "%d+ Results", BIBLE_MAX_SEARCH_RESULTS);
    else
        snprintf(hdr, sizeof(hdr), "%d Results", (int)search_result_count);
    drawHeader(hdr, true);

    if (search_result_count == 0) {
        tft.setTextColor(dim_fg(), bg());
        tft.drawCentreString("No matches found", scrW() / 2, contentY() + 30, 2);
        drawNavBar("Back", "", "");
        return;
    }

    uint8_t vis = visSearchItems();
    for (uint8_t i = 0; i < vis && (menu_scroll + i) < search_result_count; i++) {
        uint16_t idx = menu_scroll + i;
        drawSearchResultRow((int16_t)(contentY() + i * srchH()), idx,
                            idx == (uint16_t)search_res_sel);
    }
    drawScrollBar(search_result_count, vis, menu_scroll);
    drawNavBar("Back", "", "View");
}

// Progress bar drawn during searchBible() — called ~every 8 KB of XML read.
// The border is drawn only on the first call (done==0) to avoid repainting gray
// over the bar interior on every update, which caused a strobing gray flash.
void BibleInterface::drawSearchProgress(uint32_t done, uint32_t total) {
    int16_t bar_x = 10;
    int16_t bar_y = (int16_t)(contentY() + contentH() / 2 + 8);
    int16_t bar_w = (int16_t)scrW() - 20;
    int16_t bar_h = 14;

    // Draw border + empty interior only once at the start
    if (done == 0) {
        tft.fillRect(bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, dim_fg());
        tft.fillRect(bar_x, bar_y, bar_w, bar_h, bg());
    }

    // Fill from left: green up to `filled`, then bg() for the remainder.
    // Drawing in this order avoids a full-bar clear that would flash the background.
    int16_t filled = 0;
    if (total > 0) {
        filled = (int16_t)((uint32_t)bar_w * done / total);
        if (filled > bar_w) filled = bar_w;
        if (filled > 0)
            tft.fillRect(bar_x, bar_y, filled, bar_h, (uint16_t)0x07E0);
    }
    if (filled < bar_w)
        tft.fillRect(bar_x + filled, bar_y, bar_w - filled, bar_h, bg());

    char pct[12];
    if (total > 0)
        snprintf(pct, sizeof(pct), "%d%%", (int)(100UL * done / total));
    else
        snprintf(pct, sizeof(pct), "...");
    tft.setTextColor(fg(), bg());
    tft.drawCentreString(pct, scrW() / 2, bar_y + bar_h + 2, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Search — input handlers
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::handleSearchInputInput() {
    uint16_t tx, ty;
    bool down = pollTouch(&tx, &ty);

    // ── Delete confirmation popup ─────────────────────────────────────────────
    if (search_del_pending) {
        if (down && !touch_was_down) {
            touch_was_down = true;
            touch_down_x = tx;
            touch_down_y = ty;
        } else if (!down && touch_was_down) {
            touch_was_down = false;
            // Recompute Delete button bounds (must match drawSearchDelConfirm)
            int16_t pop_w  = (int16_t)scrW() - 40;
            int16_t pop_x  = 20;
            int16_t pop_y  = (int16_t)(scrH() / 2) - 40;
            int16_t btn_y  = pop_y + 44;
            int16_t btn_h  = 28;
            int16_t half_w = pop_w / 2 - 6;
            int16_t del_x  = pop_x + pop_w / 2 + 2;
            bool in_del = ((int16_t)touch_down_x >= del_x &&
                           (int16_t)touch_down_x <  del_x + half_w &&
                           (int16_t)touch_down_y >= btn_y &&
                           (int16_t)touch_down_y <  btn_y + btn_h);
            if (in_del && search_hist_count > 0 &&
                search_hist_sel >= 0 &&
                search_hist_sel < (int16_t)search_hist_count) {
                for (uint8_t i = (uint8_t)search_hist_sel;
                     i < search_hist_count - 1; i++)
                    memcpy(search_hist[i], search_hist[i + 1],
                           BIBLE_SEARCH_QUERY_LEN);
                search_hist_count--;
                if (search_hist_sel >= (int16_t)search_hist_count &&
                    search_hist_sel > 0)
                    search_hist_sel--;
                saveSearchHistory();
            }
            search_del_pending = false;
            needs_redraw = true;
        }
        return;
    }

    if (down && !touch_was_down) {
        touch_was_down  = true;
        touch_down_x    = tx;
        touch_down_y    = ty;
        scroll_dragging = false;
        stopFling();
        drag_origin_px  = scroll_px;

        if (touchInHeader(tx, ty)) {
            touch_was_down = false;
            if (touchInSearchIcon(tx, ty)) { goToSearchInput(); return; }
            if (tx < 48) goBack();
            return;
        }
        if (touchInNav(tx, ty)) {
            touch_was_down = false;
            uint16_t third = scrW() / 3;
            if (tx < third) {
                // New — open keyboard, run search if confirmed
                search_query[0] = 0;
#ifdef HAS_TOUCH
                bool ok = bibleKeyboardInput(tft, fg(), bg(),
                                             search_query, BIBLE_SEARCH_QUERY_LEN,
                                             "Search Bible:",
                                             &srch_partial_match,
                                             &srch_ignore_punct, &srch_scope);
                // Persist any option changes the user made inside the keyboard
                prefs.putBool ("srch_part", srch_partial_match);
                prefs.putBool ("srch_pnct", srch_ignore_punct);
                prefs.putUChar("srch_scp",  srch_scope);
                if (ok && search_query[0]) {
                    addToSearchHistory(search_query);
                    if (searchBible(search_query))
                        goToSearchResults();
                    else
                        needs_redraw = true;
                } else {
                    needs_redraw = true;
                }
#endif
                return;
            } else if (tx < 2 * third) {
                // View — run search for the selected history item
                if (search_hist_sel >= 0 &&
                    search_hist_sel < (int16_t)search_hist_count) {
                    strncpy(search_query, search_hist[search_hist_sel],
                            BIBLE_SEARCH_QUERY_LEN - 1);
                    search_query[BIBLE_SEARCH_QUERY_LEN - 1] = 0;
                    addToSearchHistory(search_query);
                    if (searchBible(search_query))
                        goToSearchResults();
                    else
                        needs_redraw = true;
                }
                return;
            } else {
                // Del — show confirmation popup
                if (search_hist_count > 0 &&
                    search_hist_sel >= 0 &&
                    search_hist_sel < (int16_t)search_hist_count) {
                    search_del_pending = true;
                    needs_redraw = true;
                }
                return;
            }
        }
        // Highlight the touched history item
        int16_t hi = touchItem(tx, ty);
        if (hi >= 0 && (menu_scroll + hi) < (int16_t)search_hist_count) {
            search_hist_sel = menu_scroll + hi;
            redrawListContent(search_hist_count);
        }
        return;
    }

    if (down && touch_was_down) {
        int16_t dy = (int16_t)ty - (int16_t)touch_down_y;
        if (!scroll_dragging && abs(dy) > 8) {
            scroll_dragging = true;
            search_hist_sel = -1;
        }
        if (scroll_dragging) {
            float max_px = (float)max(0, (int)search_hist_count - (int)visItems())
                           * (float)itemH();
            float new_px = drag_origin_px + (float)((int16_t)touch_down_y - (int16_t)ty);
            if (new_px < 0.f) new_px = 0.f;
            if (new_px > max_px) new_px = max_px;
            scroll_px   = new_px;
            menu_scroll = (int16_t)(scroll_px / (float)itemH());
            recordVel((int16_t)ty, millis());
            redrawListContent(search_hist_count);
        }
        return;
    }

    if (!down && touch_was_down) {
        touch_was_down = false;
        if (scroll_dragging) {
            scroll_dragging = false;
            float v = computeFlingVel();
            if (fabsf(v) > 50.f) {
                fling_vel    = v < -4000.f ? -4000.f : v > 4000.f ? 4000.f : v;
                fling_active = true;
                fling_ms     = millis();
            } else {
                stopFling();
            }
        }
        // Tap selects item only — use View button to run the search
    }
}

void BibleInterface::handleSearchResultsInput() {
    uint16_t tx, ty;
    bool down = pollTouch(&tx, &ty);

    if (down && !touch_was_down) {
        touch_was_down  = true;
        touch_down_x    = tx;
        touch_down_y    = ty;
        scroll_dragging = false;
        stopFling();
        drag_origin_px  = scroll_px;

        if (touchInHeader(tx, ty)) {
            touch_was_down = false;
            if (touchInSearchIcon(tx, ty)) { goToSearchInput(); return; }
            if (tx < 48) goBack();
            return;
        }
        if (touchInNav(tx, ty)) {
            touch_was_down = false;
            if (tx < scrW() / 3) {
                goBack();
            } else if (tx > 2 * (scrW() / 3)) {
                if (search_result_count > 0 &&
                    search_res_sel >= 0 &&
                    search_res_sel < (int16_t)search_result_count)
                    jumpToSearchResult((uint16_t)search_res_sel);
            }
            return;
        }
        // Hit-test: which search result row was tapped?
        if ((int16_t)ty >= (int16_t)contentY() &&
            (int16_t)ty <  (int16_t)(contentY() + contentH())) {
            int16_t hi = (int16_t)((ty - contentY()) / srchH());
            int16_t abs_idx = menu_scroll + hi;
            if (abs_idx < (int16_t)search_result_count) {
                search_res_sel = abs_idx;
                redrawSearchResultsContent();
            }
        }
        return;
    }

    if (down && touch_was_down) {
        int16_t dy = (int16_t)ty - (int16_t)touch_down_y;
        if (!scroll_dragging && abs(dy) > 8) {
            scroll_dragging = true;
        }
        if (scroll_dragging) {
            float max_px = (float)max(0, (int)search_result_count - (int)visSearchItems())
                           * (float)srchH();
            float new_px = drag_origin_px + (float)((int16_t)touch_down_y - (int16_t)ty);
            if (new_px < 0.f) new_px = 0.f;
            if (new_px > max_px) new_px = max_px;
            scroll_px   = new_px;
            menu_scroll = (int16_t)(scroll_px / (float)srchH());
            recordVel((int16_t)ty, millis());
            redrawSearchResultsContent();
        }
        return;
    }

    if (!down && touch_was_down) {
        touch_was_down = false;
        if (scroll_dragging) {
            scroll_dragging = false;
            float v = computeFlingVel();
            if (fabsf(v) > 50.f) {
                fling_vel    = v < -4000.f ? -4000.f : v > 4000.f ? 4000.f : v;
                fling_active = true;
                fling_ms     = millis();
            } else {
                stopFling();
            }
            return;
        }
        // Tap selects; navigation is via the View button
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Search — jump to a result in the reading view
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::jumpToSearchResult(uint16_t idx) {
    if (idx >= search_result_count) return;
    BibleSearchResult& r = search_results[idx];

    highlight_verse     = r.verse;
    reading_from_search = true;
    cur_book    = r.book;
    cur_chapter = r.chapter;
    cur_sec     = BOOKS[cur_book].section;

    drawLoading();
    view = BV_READING;
    cacheChapter(cur_book, cur_chapter);
    buildWrappedLines();

    // Find the first wrapped line for this verse
    int16_t start = 0;
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "^%d|", (int)r.verse);
    size_t plen = strlen(prefix);
    for (uint16_t i = 0; i < line_count; i++) {
        if (strncmp(lines[i], prefix, plen) == 0) {
            start = (int16_t)i;
            break;
        }
    }
    read_line = start;
    scroll_px = (float)start * (float)lineH();
    saveState();
    needs_redraw = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Search — streaming XML full-Bible search with progress bar
// ─────────────────────────────────────────────────────────────────────────────
bool BibleInterface::parseOsisID(const char* osisID,
                                  uint8_t& book_out,
                                  uint8_t& chap_out,
                                  uint8_t& verse_out) {
    const char* dot1 = strchr(osisID, '.');
    if (!dot1) return false;
    const char* dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return false;
    int code_len = (int)(dot1 - osisID);
    for (uint8_t b = 0; b < BIBLE_BOOK_COUNT; b++) {
        if ((int)strlen(BOOKS[b].osis_code) == code_len &&
            strncmp(osisID, BOOKS[b].osis_code, code_len) == 0) {
            book_out  = b;
            chap_out  = (uint8_t)atoi(dot1 + 1);
            verse_out = (uint8_t)atoi(dot2 + 1);
            return true;
        }
    }
    return false;
}

bool BibleInterface::searchContains(const char* text, const char* query) {
    if (!query || !query[0]) return false;

    // When ignoring punctuation, strip it from both inputs into static buffers
    // (single-threaded — no re-entrancy concern).
    static char t2[BIBLE_VERSE_BUF];
    static char q2[BIBLE_SEARCH_QUERY_LEN];
    const char* t = text;
    const char* q = query;
    if (srch_ignore_punct) {
        size_t ti = 0;
        for (const char* p = text; *p && ti < BIBLE_VERSE_BUF - 1; p++) {
            uint8_t c = (uint8_t)*p;
            if (c < 0x80 && ispunct((int)c)) continue;
            t2[ti++] = *p;
        }
        t2[ti] = 0;
        t = t2;

        size_t qi = 0;
        for (const char* p = query; *p && qi < BIBLE_SEARCH_QUERY_LEN - 1; p++) {
            uint8_t c = (uint8_t)*p;
            if (c < 0x80 && ispunct((int)c)) continue;
            q2[qi++] = *p;
        }
        q2[qi] = 0;
        q = q2;
        if (!q[0]) return false;
    }

    // Inline helper: does word w[0..wl) appear anywhere in t?
    auto wordIn = [](const char* t_, const char* w, size_t wl) -> bool {
        for (size_t i = 0; t_[i]; i++) {
            bool m = true;
            for (size_t j = 0; j < wl && m; j++) {
                if (!t_[i + j]) { m = false; break; }
                uint8_t tc = (uint8_t)t_[i + j];
                uint8_t qc = (uint8_t)w[j];
                if (tc >= 0x80 || qc >= 0x80) { if (tc != qc) m = false; }
                else if (tolower((int)tc) != tolower((int)qc)) m = false;
            }
            if (m) return true;
        }
        return false;
    };

    if (srch_partial_match) {
        // Every space-delimited word in q must appear somewhere in t (any order/position)
        const char* p = q;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char* ws = p;
            while (*p && *p != ' ') p++;
            size_t wl = (size_t)(p - ws);
            if (wl == 0) continue;
            if (!wordIn(t, ws, wl)) return false;
        }
        return true;
    }

    // Exact phrase match
    size_t qlen = strlen(q);
    for (size_t i = 0; t[i]; i++) {
        bool match = true;
        for (size_t j = 0; j < qlen; j++) {
            if (!t[i + j]) { match = false; break; }
            uint8_t tc = (uint8_t)t[i + j];
            uint8_t qc = (uint8_t)q[j];
            if (tc >= 0x80 || qc >= 0x80) {
                if (tc != qc) { match = false; break; }
            } else {
                if (tolower((int)tc) != tolower((int)qc)) { match = false; break; }
            }
        }
        if (match) return true;
    }
    return false;
}

bool BibleInterface::searchBible(const char* query) {
    search_result_count = 0;
    search_res_sel      = 0;
    if (trans_count == 0 || !query || !query[0]) return false;

    // Show search progress screen
    tft.fillScreen(bg());
    drawHeader("Searching...", false);
    // Show query centred, rendering umlauts/ß properly
    {
        int16_t qw = textWidthUTF8(query, 2);
        int16_t qx = (int16_t)(scrW() / 2) - qw / 2;
        int16_t qy = (int16_t)(contentY() + contentH() / 2 - 20);
        tft.setTextColor(fg(), bg());
        for (const char* p = query; *p; p++)
            qx += tftCharUTF8(tft, (uint8_t)*p, qx, qy, 2, fg());
    }
    drawSearchProgress(0, 1);

    // Draw Cancel button once — stays visible; progress updates don't overlap it.
    // Geometry must match the hit-test inside the parse loop below.
    const int16_t CBTN_W = 80, CBTN_H = 26;
    const int16_t cbtn_bar_y = (int16_t)(contentY() + contentH() / 2 + 8);
    const int16_t cbtn_x     = (int16_t)(scrW() / 2) - CBTN_W / 2;
    const int16_t cbtn_y     = cbtn_bar_y + 14 + 18;  // below bar (14px) + pct-text (18px)
    tft.fillRoundRect(cbtn_x, cbtn_y, CBTN_W, CBTN_H, 4, hdr_bg());
    tft.drawRoundRect(cbtn_x, cbtn_y, CBTN_W, CBTN_H, 4, dim_fg());
    tft.setTextColor(fg(), hdr_bg());
    tft.drawCentreString("Cancel", (int16_t)(scrW() / 2), cbtn_y + (CBTN_H - 8) / 2, 1);

    char path[64];
    snprintf(path, sizeof(path), "/bible/%s.xml", trans_stems[cur_trans]);
    File f = SD.open(path);
    if (!f) return false;

    uint32_t file_size = (uint32_t)f.size();
    uint32_t last_upd  = 0;

    XmlState s;
    memset(&s, 0, sizeof(s));
    s.f = f;

    enum { ST_TEXT, ST_TAG } st = ST_TEXT;
    char tag_buf[XML_TAG_BUF];
    int  tag_len      = 0;
    bool in_verse     = false;
    bool in_note      = false;
    char vtext[BIBLE_VERSE_BUF];
    int  vtext_len    = 0;
    uint8_t vs_book   = 0, vs_chap = 0, vs_verse = 0;
    char c;

    while (xmlNextByte(s, c)) {
        uint32_t pos = (uint32_t)f.position();
        if (pos - last_upd >= 8192) {
            last_upd = pos;
            drawSearchProgress(pos, file_size);
            yield();
            // Cancel only if user taps the Cancel button
            {
                uint16_t cx, cy;
                if (pollTouch(&cx, &cy) &&
                    (int16_t)cx >= cbtn_x && (int16_t)cx < cbtn_x + CBTN_W &&
                    (int16_t)cy >= cbtn_y && (int16_t)cy < cbtn_y + CBTN_H) {
                    f.close();
                    search_result_count = 0;
                    needs_redraw = true;
                    return false;
                }
            }
        }

        if (st == ST_TEXT) {
            if (c == '<') { st = ST_TAG; tag_len = 0; continue; }
            if (in_verse && !in_note && vtext_len < BIBLE_VERSE_BUF - 2)
                vtext[vtext_len++] = c;
        } else {
            if (c == '>') {
                st = ST_TEXT;
                tag_buf[tag_len] = 0;

                bool closing = (tag_buf[0] == '/');
                const char* tname = closing ? tag_buf + 1 : tag_buf;

                if (tag_buf[0] == '?' || tag_buf[0] == '!') {
                    tag_len = 0; continue;
                }

                if (!closing) {
                    bool self_close = (tag_len > 0 && tag_buf[tag_len - 1] == '/');
                    if (self_close) tag_buf[--tag_len] = 0;

                    if (strncmp(tname, "verse", 5) == 0 &&
                        (tname[5] == ' ' || tname[5] == '\t' || tname[5] == 0)) {
                        char osis_id[64] = {0};
                        if (xmlGetAttr(tag_buf, "osisID", osis_id, sizeof(osis_id))) {
                            uint8_t bk = 0, ch = 0, vs = 0;
                            if (parseOsisID(osis_id, bk, ch, vs)) {
                                // Scope filter: skip verses outside selected scope
                                bool in_scope = true;
                                if (srch_scope == 1)
                                    in_scope = (BOOKS[bk].section == cur_sec);
                                else if (srch_scope == 2)
                                    in_scope = (bk == cur_book);
                                if (in_scope) {
                                    vs_book   = bk;
                                    vs_chap   = ch;
                                    vs_verse  = vs;
                                    in_verse  = !self_close;
                                    in_note   = false;
                                    vtext_len = 0;
                                } else {
                                    in_verse  = false;
                                }
                            }
                        }
                    } else if (in_verse) {
                        if (strncmp(tname, "note", 4) == 0) in_note = true;
                    }
                } else {
                    if (strncmp(tname, "verse", 5) == 0 && in_verse) {
                        vtext[vtext_len] = 0;
                        xmlDecodeEntities(vtext, BIBLE_VERSE_BUF);
                        utf8Encode(vtext);
                        if (searchContains(vtext, query) &&
                            search_result_count < BIBLE_MAX_SEARCH_RESULTS) {
                            BibleSearchResult r;
                            r.book    = vs_book;
                            r.chapter = vs_chap;
                            r.verse   = vs_verse;
                            // Store snippet centered on first match occurrence
                            {
                                size_t qlen2 = strlen(query);
                                size_t tlen2 = (size_t)vtext_len;
                                size_t match_pos = 0;
                                for (size_t si = 0; si + qlen2 <= tlen2; si++) {
                                    bool m = true;
                                    for (size_t sj = 0; sj < qlen2 && m; sj++) {
                                        uint8_t tc = (uint8_t)vtext[si + sj];
                                        uint8_t qc = (uint8_t)query[sj];
                                        if (tc >= 0x80 || qc >= 0x80) { if (tc != qc) m = false; }
                                        else if (tolower((int)tc) != tolower((int)qc)) m = false;
                                    }
                                    if (m) { match_pos = si; break; }
                                }
                                size_t half = (BIBLE_SRCH_SNIPPET_LEN - 1) / 2;
                                size_t snip_start = (match_pos > half) ? match_pos - half : 0;
                                strncpy(r.snippet, vtext + snip_start, BIBLE_SRCH_SNIPPET_LEN - 1);
                                r.snippet[BIBLE_SRCH_SNIPPET_LEN - 1] = 0;
                            }
                            search_results[search_result_count++] = r;
                        }
                        in_verse  = false;
                        in_note   = false;
                        vtext_len = 0;
                    } else if (in_verse) {
                        if (strncmp(tname, "note", 4) == 0) in_note = false;
                    }
                }
                tag_len = 0;
            } else {
                if (tag_len < XML_TAG_BUF - 1)
                    tag_buf[tag_len++] = c;
            }
        }
    }
    f.close();
    // Final progress bar fill
    drawSearchProgress(file_size, file_size);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Search — history persistence
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::addToSearchHistory(const char* query) {
    if (!query || !query[0]) return;
    // Deduplicate: if already present, move to front
    for (uint8_t i = 0; i < search_hist_count; i++) {
        if (strcmp(search_hist[i], query) == 0) {
            for (uint8_t j = i; j > 0; j--)
                memcpy(search_hist[j], search_hist[j - 1], BIBLE_SEARCH_QUERY_LEN);
            strncpy(search_hist[0], query, BIBLE_SEARCH_QUERY_LEN - 1);
            search_hist[0][BIBLE_SEARCH_QUERY_LEN - 1] = 0;
            saveSearchHistory();
            return;
        }
    }
    // Insert at front, shifting existing entries (oldest drops off if full)
    uint8_t new_count = (search_hist_count < BIBLE_SEARCH_HIST_MAX)
                        ? search_hist_count + 1
                        : BIBLE_SEARCH_HIST_MAX;
    for (uint8_t i = new_count - 1; i > 0; i--)
        memcpy(search_hist[i], search_hist[i - 1], BIBLE_SEARCH_QUERY_LEN);
    strncpy(search_hist[0], query, BIBLE_SEARCH_QUERY_LEN - 1);
    search_hist[0][BIBLE_SEARCH_QUERY_LEN - 1] = 0;
    search_hist_count = new_count;
    saveSearchHistory();
}

void BibleInterface::saveSearchHistory() {
    SD.remove(BIBLE_SRCH_HIST_FILE);
    File f = SD.open(BIBLE_SRCH_HIST_FILE, FILE_WRITE);
    if (!f) return;
    for (uint8_t i = 0; i < search_hist_count; i++) {
        f.print(search_hist[i]);
        f.write('\n');
    }
    f.close();
}

void BibleInterface::loadSearchHistory() {
    search_hist_count = 0;
    File f = SD.open(BIBLE_SRCH_HIST_FILE);
    if (!f) return;
    while (f.available() && search_hist_count < BIBLE_SEARCH_HIST_MAX) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.length() >= BIBLE_SEARCH_QUERY_LEN) continue;
        strncpy(search_hist[search_hist_count], line.c_str(), BIBLE_SEARCH_QUERY_LEN - 1);
        search_hist[search_hist_count][BIBLE_SEARCH_QUERY_LEN - 1] = 0;
        search_hist_count++;
    }
    f.close();
}

// ─────────────────────────────────────────────────────────────────────────────
// OTA — switch boot partition to Marauder (ota_1) and restart
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::bootMarauder() {
    const esp_partition_t* ota1 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (ota1) {
        Serial.println(F("[Bible] Switching to Marauder (ota_1)..."));
        esp_ota_set_boot_partition(ota1);
        delay(200);
        esp_restart();
    } else {
        Serial.println(F("[Bible] ota_1 partition not found!"));
        tft.fillRect(0, contentY() + 40, scrW(), 30, TFT_RED);
        tft.setTextColor(TFT_WHITE, TFT_RED);
        tft.drawCentreString("ota_1 not found!", scrW()/2, contentY() + 48, 2);
        delay(2000);
        needs_redraw = true;
    }
}

#endif  // HAS_SCREEN
