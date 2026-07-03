// BibleInterface.cpp
// Marauder Bible Firmware — Bible reader implementation
//
// Streaming OSIS XML parser reads verses directly from the SD card.
// No Python pre-processing step required.

#include "BibleInterface.h"
#include "BibleKeyboard.h"
#include "fonts_vlw.h"          // flash-resident smooth (VLW) fonts (reader + whole UI)
#include "fonts_vlw_fraktur.h"        // Fraktur body set (song text, flagged songbooks)
#include "fonts_vlw_fraktur_title.h"  // Fraktur title set (song/category/book titles)

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
// Reading-text colour palette (Font Color setting). Index 0 = "Default" (follows
// the theme). The rest are white + shades of gray + black (RGB565).
// ─────────────────────────────────────────────────────────────────────────────
// Index 0 = "Default" (each user — font_fg / verse_num_fg — supplies its own
// default for index 0). Indices 1+ are explicit colours shared by both settings.
static const char* const FONT_COLOR_NAMES[] = {
    "Default", "White", "Silver", "Light Gray", "Gray", "Dim Gray",
    "Dark Gray", "Charcoal", "Black",
    "Red", "Orange", "Amber", "Yellow", "Lime", "Green", "Teal",
    "Cyan", "Sky", "Blue", "Indigo", "Purple", "Magenta", "Pink", "Brown"
};
static const uint16_t FONT_COLOR_VAL[] = {
    0x0000,   // [0] Default — unused (caller returns its own default)
    0xFFFF,   // White
    0xC618,   // Silver
    0xAD55,   // Light Gray
    0x8410,   // Gray
    0x6B4D,   // Dim Gray
    0x4208,   // Dark Gray
    0x2104,   // Charcoal
    0x0000,   // Black
    0xF800,   // Red
    0xFC60,   // Orange
    0xFD20,   // Amber
    0xFFE0,   // Yellow
    0xAFE5,   // Lime
    0x07E0,   // Green
    0x0594,   // Teal
    0x07FF,   // Cyan
    0x5D1F,   // Sky
    0x001F,   // Blue
    0x4019,   // Indigo
    0x801F,   // Purple
    0xF81F,   // Magenta
    0xFD9F,   // Pink
    0xA145,   // Brown
};
static const uint8_t FONT_COLOR_COUNT = 24;

// Screen orientation options (global). 0/2 portrait, 1/3 landscape.
static const char* const ORIENT_NAMES[4] = { "Normal", "Landscape", "Flip 180", "Land. Flip" };
static const uint8_t ORIENT_COUNT = 4;

// ─────────────────────────────────────────────────────────────────────────────
// Colour themes (RGB565). `dark` selects the accent (DARK vs LIGHT) palette and
// divider/scrollbar shades. The first two match the original Dark/Light look.
// ─────────────────────────────────────────────────────────────────────────────
struct ThemeDef { uint16_t bg, fg, hdr, dim; bool dark; const char* name; };
static const ThemeDef THEMES[] = {
    { 0x0000, 0xFFFF, 0x1082, 0x7BEF, true,  "Dark"   },  // black / white
    { 0xFFFF, 0x0000, 0x4A69, 0x632C, false, "Light"  },  // white / black
    { 0xF717, 0x51E3, 0x8B26, 0xB4AD, false, "Sepia"  },  // cream / brown
    { 0x0000, 0xAD55, 0x0841, 0x4208, true,  "Night"  },  // black / soft gray
    { 0x2104, 0xE73C, 0x4209, 0x8410, true,  "Gray"   },  // dark gray / near-white
    { 0x0866, 0xFFFF, 0x190C, 0x73D4, true,  "Navy"   },  // deep blue / white
    { 0x0141, 0xCF99, 0x0242, 0x646C, true,  "Forest" },  // dark green / pale green
    { 0x28E2, 0xE6B6, 0x51C5, 0x93CB, true,  "Mocha"  },  // dark brown / cream
    { 0x0105, 0xBF5E, 0x020A, 0x5CB4, true,  "Ocean"  },  // deep teal / pale cyan
    { 0x2085, 0xE65D, 0x420A, 0x9B74, true,  "Plum"   },  // dark purple / lilac
    { 0x0000, 0xFD80, 0x28E0, 0x82C0, true,  "Amber"  },  // black / amber
    { 0x2883, 0xF6BA, 0x5905, 0xAB6F, true,  "Rose"   },  // dark maroon / pink
    { 0xE7BD, 0x21C5, 0x650F, 0x7D51, false, "Mint"   },  // pale mint / dark green
    { 0x30C8, 0xFD4B, 0x718A, 0xD3CC, true,  "Sunset"  }, // dusk purple / warm orange
    { 0x0842, 0x07F9, 0x0249, 0x04B1, true,  "Cyber"   }, // near-black / neon cyan
    { 0x20C2, 0xD5D1, 0x51C4, 0x93CB, true,  "Coffee"  }, // dark brown / tan
    { 0xE79F, 0x1989, 0x6497, 0x8516, false, "Arctic"  }, // icy light / deep blue
    { 0x1801, 0xFE59, 0x6802, 0xBB0D, true,  "Crimson" }, // dark red / pink
    { 0xEF5F, 0x394A, 0x8BD7, 0x9476, false, "Lavender"}, // pale violet / plum
    { 0x0000, 0xFFFF, 0x0841, 0x4208, true,  "Neon"   },  // black / white + rainbow outlines
};
static const uint8_t THEME_COUNT = 20;
static const uint8_t THEME_NEON  = 19;   // index of the Neon theme (last entry)

// Bright saturated colours cycled for neon outlines.
static const uint16_t NEON_HUES[] = {
    0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x07FF, 0x041F, 0x781F, 0xF81F
};
static const uint8_t NEON_COUNT = 8;

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
      mode(MODE_BIBLE), rt_books(nullptr), rt_book_count(0),
      rt_secs(nullptr), rt_sec_count(0), rt_pages(nullptr), rt_page_count(0), rt_pages_book(0xFFFF),
      view(BV_MAIN_MENU), dark_mode(true), theme_idx(0), font_num(3), font_color_idx(0),
      vnum_color_idx(0), orientation(0), menu_font_color_idx(0), accent_def(false), needs_redraw(true),
      settings_scope(0), settings_from_menu(false), set_row_n(0),
      sc_trans_count(0), sc_trans_cur(0),
      menu_sel(0), menu_scroll(0), read_line(0),
      cached_book(0xFFFF), cached_chap(0), cached_count(0),
      line_count(0), trans_count(0), bm_count(0), bm_sel(0), bm_scroll(0),
      bm_confirm_pending(false),
      search_hist_count(0), search_hist_sel(0),
      search_results(nullptr), search_result_count(0), search_res_sel(0),
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
      trans_marq_on(false), trans_marq_winx(0), trans_marq_winy(0), trans_marq_winw(0), trans_marq_texty(0),
      trans_marq_textw(0), trans_marq_off(0), trans_marq_bg(0), trans_marq_fg(0), trans_marq_ms(0),
      about_mcu_y0(0), about_mcu_y1(0),
      book_offsets(nullptr), book_offsets_cap(0),
      book_idx_valid(false), book_idx_trans(0xFF),
      line_spr(&tft), read_font_loaded(-1), read_font_frak(false), ui_font_idx(-1), ui_font_frak(false)
#ifdef HAS_BATTERY
    , batt_ok(false), batt_pct(-1), batt_ms(0)
#endif
{
    memset(vbuf_y,        0, sizeof(vbuf_y));
    memset(vbuf_t,        0, sizeof(vbuf_t));
    trans_marq_str[0] = '\0';
    if (book_offsets) memset(book_offsets, 0, (size_t)book_offsets_cap * sizeof(uint32_t));
    memset(search_query,  0, sizeof(search_query));
    memset(search_hist,   0, sizeof(search_hist));
    // search_results is heap/PSRAM allocated in RunSetup() (kept out of static .bss).
}

// ─────────────────────────────────────────────────────────────────────────────
// RunSetup — called once when Bible firmware starts
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::RunSetup() {
    tft.init();
    setUiFont(2);   // load the UI smooth font (14px) so the whole UI renders with it

    // Search results live in PSRAM/heap, not static .bss (the big verse/line/font
    // buffers already fill internal SRAM). Allocate once at boot.
    {
        size_t sr_sz = sizeof(BibleSearchResult) * BIBLE_MAX_SEARCH_RESULTS;
#ifdef HAS_PSRAM
        search_results = (BibleSearchResult*)ps_malloc(sr_sz);
#endif
        if (!search_results) search_results = (BibleSearchResult*)malloc(sr_sz);
        if (search_results) memset(search_results, 0, sr_sz);
    }

#ifdef HAS_CAP_TOUCH
    ft6336_init();
#endif

    // Menu-level settings live in their own NVS namespace ("menu"): appearance of
    // the main menu, the global screen orientation, plus the hardware touch
    // calibration. Each content mode opens its own namespace in enterMode().
    prefs.begin("menu", false);
    orientation = prefs.getUChar("orient", 0);
    if (orientation > 3) orientation = 0;
    applyOrientation();                 // global 0-3 orientation — set before first draw
    mode       = MODE_BIBLE;            // accessors unused at the menu; harmless default
    theme_idx  = prefs.getUChar("theme", 0);
    if (theme_idx >= THEME_COUNT) theme_idx = 0;
    dark_mode  = THEMES[theme_idx].dark;
    accent_idx = prefs.getUChar("accent", 0);
    if (accent_idx >= ACCENT_COUNT) accent_idx = 0;
    accent_def = (prefs.getUChar("accentdef", 0) != 0);
    font_num   = 3;   // default reading size = Medium (VLW index)
    loadMenuChrome();
    font_color_idx = menu_font_color_idx;   // chrome/menu text follows the menu Font Color
    if (font_color_idx >= FONT_COLOR_COUNT) font_color_idx = 0;   // global chrome text colour
    blInit();   // must run before runTouchCalibration() so the backlight is on

    // Boot splash (SD /splash.raw) — shown for 2.5 s in the current orientation.
    if (drawSplashImage()) delay(2500);

#ifdef MARAUDER_V6_1
    // V6.1: load stored calibration or run first-boot wizard.
    if (prefs.getUChar("tcal2", 0)) {
        uint16_t calData[5];
        prefs.getBytes("tcald2", calData, sizeof(calData));
        tft.setTouch(calData);
        memcpy(cal_data, calData, sizeof(cal_data));   // for manual orientation mapping
    } else {
        runTouchCalibration();
    }
#elif defined(MARAUDER_V8)
    // V8: fixed calibration (portrait, matches Marauder Display.cpp)
    {
        uint16_t calData[5] = { 312, 3431, 191, 3456, 2 };
        tft.setTouch(calData);
        memcpy(cal_data, calData, sizeof(cal_data));   // for manual orientation mapping
    }
#endif

#ifdef HAS_BATTERY
    battInit();
#endif

    goToMainMenu();
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
            case BV_MAIN_MENU:       drawMainMenu();       break;
            case BV_TRANS_SELECT:    drawTransSelect();    break;
            case BV_SECTION_SELECT:  drawSectionSelect();  break;
            case BV_BOOK_SELECT:     drawBookSelect();     break;
            case BV_CHAPTER_SELECT:  drawChapterSelect();  break;
            case BV_READING:         drawReading();        break;
            case BV_SETTINGS:        drawSettings();       break;
            case BV_BOOKMARKS:       drawBookmarks();      break;
            case BV_SEARCH_INPUT:    drawSearchInput();    break;
            case BV_SEARCH_RESULTS:  drawSearchResults();  break;
            case BV_ABOUT:           drawAbout();          break;
        }
        needs_redraw = false;
    }

    switch (view) {
        case BV_MAIN_MENU:       handleMainMenuInput();                 break;
        case BV_TRANS_SELECT:    handleListInput(trans_count);          break;
        case BV_SECTION_SELECT:  handleListInput(numSecs());      break;
        case BV_BOOK_SELECT:     handleListInput(secLen(cur_sec)); break;
        case BV_CHAPTER_SELECT:
            if (mode == MODE_DICT) handleListInput(rt_page_count);  // page list
            else                   handleChapterInput();            // numeric grid
            break;
        case BV_READING:         handleReadingInput();                  break;
        case BV_SETTINGS:        handleSettingsInput();                 break;
        case BV_BOOKMARKS:       handleBookmarksInput();                break;
        case BV_SEARCH_INPUT:    handleSearchInputInput();              break;
        case BV_SEARCH_RESULTS:  handleSearchResultsInput();            break;
        case BV_ABOUT:           handleAboutInput();                    break;
    }

    if (view == BV_SETTINGS) tickTransMarquee();   // slow-scroll a long Translation value
}

// ─────────────────────────────────────────────────────────────────────────────
// Color scheme
// ─────────────────────────────────────────────────────────────────────────────
uint16_t BibleInterface::fg()         const { return THEMES[theme_idx < THEME_COUNT ? theme_idx : 0].fg;  }
uint16_t BibleInterface::bg()         const { return THEMES[theme_idx < THEME_COUNT ? theme_idx : 0].bg;  }
uint16_t BibleInterface::hdr_bg()     const { return THEMES[theme_idx < THEME_COUNT ? theme_idx : 0].hdr; }
uint16_t BibleInterface::sel_bg()     const { return accent_def ? themeHighlight()
                                                   : (dark_mode ? ACCENT_DARK[accent_idx] : ACCENT_LIGHT[accent_idx]); }
uint16_t BibleInterface::dim_fg()     const { return THEMES[theme_idx < THEME_COUNT ? theme_idx : 0].dim; }
uint16_t BibleInterface::verse_num_fg()const{
    if (vnum_color_idx == 0 || vnum_color_idx >= FONT_COLOR_COUNT) return 0x051D; /* muted teal default */
    return FONT_COLOR_VAL[vnum_color_idx];
}
uint16_t BibleInterface::font_fg() const {
    if (font_color_idx == 0 || font_color_idx >= FONT_COLOR_COUNT) return fg();
    return FONT_COLOR_VAL[font_color_idx];
}
// Theme-fitting default Highlight: bg blended ~32% toward fg (subtle, always legible).
uint16_t BibleInterface::themeHighlight() const {
    uint16_t a = bg(), b = fg();
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg5 = (b >> 5) & 0x3F, bb = b & 0x1F;
    const int t = 82;  // ~32% of 255
    int r = ar + (br - ar) * t / 255;
    int g = ag + (bg5 - ag) * t / 255;
    int bl = ab + (bb - ab) * t / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}
// UI chrome text colour (header titles, nav/footer buttons, menu labels). Follows
// the active scope's Font Color setting so changing Font Color recolours the menus
// and buttons too; falls back to white (legible on the button/header backgrounds)
// when Font Color is left on "Default".
uint16_t BibleInterface::chromeFg() const {
    return (font_color_idx == 0 || font_color_idx >= FONT_COLOR_COUNT)
           ? TFT_WHITE : FONT_COLOR_VAL[font_color_idx];
}
void BibleInterface::loadMenuChrome() {
    Preferences p;
    menu_font_color_idx = 0;
    if (p.begin("menu", true)) { menu_font_color_idx = p.getUChar("fontcol", 0); p.end(); }
    if (menu_font_color_idx >= FONT_COLOR_COUNT) menu_font_color_idx = 0;
}
bool BibleInterface::isNeon() const { return theme_idx == THEME_NEON; }
// Outline colour for borders/dividers: a stable rainbow hue (seeded by element
// position) when the Neon theme is active, otherwise the caller's default.
uint16_t BibleInterface::edgeColor(int16_t seed, uint16_t def) const {
    if (!isNeon()) return def;
    return NEON_HUES[(uint16_t)(seed < 0 ? -seed : seed) % NEON_COUNT];
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout helpers
// ─────────────────────────────────────────────────────────────────────────────
// Native portrait panel size; swapped when orientation is landscape (1 or 3) so
// the whole layout (which is driven by scrW()/scrH()) adapts automatically.
uint16_t BibleInterface::scrW() const {
#ifdef MARAUDER_PANCAKE
    const uint16_t pw = 320, ph = 480;
#else
    const uint16_t pw = 240, ph = 320;
#endif
    return (orientation & 1) ? ph : pw;
}
uint16_t BibleInterface::scrH() const {
#ifdef MARAUDER_PANCAKE
    const uint16_t pw = 320, ph = 480;
#else
    const uint16_t pw = 240, ph = 320;
#endif
    return (orientation & 1) ? pw : ph;
}

// Reading-font family flag + accessors (defined with the VLW helpers below).
static bool g_read_fraktur = false;   // set by loadToc; true → Fraktur reading font
static const uint8_t* vlwForSize(uint8_t idx);
static uint8_t         vlwLineH(uint8_t idx);

uint16_t BibleInterface::lineH() const {
    // Reading text is drawn with a smooth VLW font (normal or Fraktur family); the
    // row height is the font's ascent+descent plus a little leading.
    uint8_t idx = (font_num < VLW_FONT_COUNT) ? font_num : 3;
    return (uint16_t)vlwLineH(idx) + 3;
}
uint8_t BibleInterface::visItems() const { return contentH() / itemH(); }
uint8_t BibleInterface::visLines() const { return contentH() / lineH(); }

// ─────────────────────────────────────────────────────────────────────────────
// Drawing — shared elements
// ─────────────────────────────────────────────────────────────────────────────
// Forward decl: UTF-8-aware glyph draw (defined below; used by drawHeader title).
static int16_t tftCharUTF8(TFT_eSPI& tft, uint8_t c, int16_t x, int16_t y,
                           uint8_t font, uint16_t color);

void BibleInterface::drawHeader(const char* title, bool show_back) {
    tft.fillRect(0, 0, scrW(), hdrH(), hdr_bg());
    uint16_t ch_fg = chromeFg();   // global chrome text colour (Main Menu Font Color)
    if (show_back) {
        // Same style as nav bar buttons: hdr_bg fill + dim_fg border
        tft.fillRoundRect(2, 3, 40, 22, 4, hdr_bg());
        tft.drawRoundRect(2, 3, 40, 22, 4, edgeColor(0, dim_fg()));
        drawChevron(2, 3, 40, 22, false, ch_fg);   // crisp centered back arrow
    }
    // Title — rendered char-by-char so umlauts/ß (private codes) show correctly
    // (song titles, dictionary word pairs). Centred, clipped to avoid the back
    // button (left) and the search/battery area (right).
    {
        bool    ft     = titleFraktur();           // Fraktur title for song content
        if (ft) setUiFontEx(2, true);
        int16_t left   = show_back ? 46 : 4;
        bool    full_w = (view == BV_MAIN_MENU) || (view == BV_ABOUT) ||
                         (view == BV_SETTINGS && settings_from_menu);
        int16_t right  = full_w ? (int16_t)scrW() - 4 : (int16_t)scrW() - 66;
        int16_t tw     = textWidthUTF8(title, 2);
        int16_t tx0    = (int16_t)(scrW() / 2) - tw / 2;
        if (tx0 < left)  tx0 = left;
        tft.setTextColor(ch_fg, hdr_bg());
        for (const char* p = title; *p && tx0 < right; p++)
            tx0 += tftCharUTF8(tft, (uint8_t)*p, tx0, 6, 2, ch_fg);
        if (ft) setUiFont(2);                       // restore normal for the rest
    }

    // Search button — same bordered-box style as the back button.
    // Hidden on the main menu and on menu-opened Settings (no content to search).
    // Positioned to the left of the battery % text.
    bool hide_search = (view == BV_MAIN_MENU) || (view == BV_ABOUT) ||
                       (view == BV_SETTINGS && settings_from_menu);
    if (!hide_search) {
        int16_t sb_x = (int16_t)scrW() - 63;
        tft.fillRoundRect(sb_x,     3, 28, 22, 4, hdr_bg());
        tft.drawRoundRect(sb_x,     3, 28, 22, 4, edgeColor(3, dim_fg()));
        // Magnifying glass inside the button (circle + diagonal handle)
        int16_t cx = sb_x + 13;   // horizontal centre of button
        tft.drawCircle(cx,     14, 5, ch_fg);
        tft.drawLine  (cx + 4, 18, cx + 7, 21, ch_fg);
    }

#ifdef HAS_BATTERY
    if (batt_pct >= 0) {
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", (int)batt_pct);
        setUiFont(1);   // X-Small battery %, cleared bg so it never leaves artifacts
        tft.setTextColor(ch_fg, hdr_bg());
        int16_t by = ((int16_t)hdrH() - (int16_t)VLW_FONTS[1].lineH) / 2;
        tft.fillRect((int16_t)scrW() - 33, 3, 30, (int16_t)hdrH() - 6, hdr_bg());  // right of search icon
        tft.drawRightString(pct, (int16_t)(scrW() - 3), by, 1);
        setUiFont(2);
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
    tft.drawFastHLine(0, y, scrW(), edgeColor(5, dark_mode ? 0x2104 : 0xC618));

    // Draw each non-empty label as a rounded button
    const char* labels[3] = { left, mid, right };
    for (uint8_t i = 0; i < 3; i++) {
        if (!labels[i] || !labels[i][0]) continue;
        uint16_t cx = i * third + third / 2;
        uint16_t bx = cx - bw / 2;
        tft.fillRoundRect(bx, by, bw, bh, 4, hdr_bg());
        tft.drawRoundRect(bx, by, bw, bh, 4, edgeColor(i * 2, dim_fg()));
        drawSmallCentered(labels[i], cx, by, bh, chromeFg(), hdr_bg());
    }
}

void BibleInterface::clearContent() {
    tft.fillRect(0, contentY(), scrW(), contentH(), bg());
}

// Forward declarations for the VLW helpers used below (defined after drawListRow).
static inline uint16_t vlwPrivToUnicode(uint8_t c);
static int16_t vlwAdvance(const uint8_t* font, uint16_t uni);
static int16_t vlwSpaceWidth(const uint8_t* font);
// The UI (menus / headers / nav / keyboard) smooth font, kept loaded on tft. All
// TFT_eSPI text calls auto-route to it while it is loaded, so the whole UI renders
// with the anti-aliased VLW font — and the umlaut glyphs come from the font itself.
static const uint8_t* g_ui_vlw = VLW_FONTS[2].data;
// Exposed to BibleKeyboard.cpp (external linkage) so it can render its options strip
// X-Small without duplicating the font arrays in its translation unit.
const uint8_t* g_kb_font_small = VLW_FONTS[1].data;
const uint8_t* g_kb_font_main  = VLW_FONTS[2].data;

// Draw one private-code character on tft at (x, y) using the loaded UI VLW font.
// The bitmap `font`/`color` args are ignored (color comes from setTextColor).
// Returns the advance width.
static int16_t tftCharUTF8(TFT_eSPI& tft, uint8_t c, int16_t x, int16_t y,
                             uint8_t /*font*/, uint16_t /*color*/) {
    uint16_t uni = vlwPrivToUnicode(c);
    tft.setCursor(x, y);
    tft.drawGlyph(uni);                       // uses tft's loaded smooth font
    if (c == ' ') return vlwSpaceWidth(g_ui_vlw);
    int16_t a = vlwAdvance(g_ui_vlw, uni);
    return (a < 0) ? (int16_t)(vlwSpaceWidth(g_ui_vlw) + 1) : a;
}

void BibleInterface::drawListRow(int16_t y_px, const char* text, bool selected, bool has_arrow) {
    uint16_t bg_c  = selected ? sel_bg() : bg();
    uint16_t fg_c  = font_fg();   // list item text follows the Font Color setting
    // Right edge: leave room for scrollbar (6px) + arrow (14px) + small gap
    int16_t  max_x = (int16_t)scrW() - (has_arrow ? 28 : 8);
    int16_t  ty    = y_px + (itemH() - 16) / 2;
    tft.fillRect(0, y_px, scrW(), itemH(), bg_c);
    tft.setTextColor(fg_c, bg_c);
    // Draw text char-by-char with private-code umlaut/ß support
    int16_t tx = 10;
    for (const char* p = text; *p && tx < max_x; p++)
        tx += tftCharUTF8(tft, (uint8_t)*p, tx, ty, 2, fg_c);
    if (has_arrow)
        drawChevron(scrW() - 22, y_px, 16, itemH(), true, font_fg());  // follows Font Color
    // divider
    tft.drawFastHLine(0, y_px + itemH() - 1, scrW(),
                      edgeColor(y_px / (int16_t)itemH(), dark_mode ? 0x2104 : 0xC618));
}

void BibleInterface::drawScrollBar(int16_t total, int16_t vis, int16_t top) {
    if (total <= vis) return;
    uint16_t barX = scrW() - 6;
    uint16_t barY = contentY();
    uint16_t barH = contentH();
    tft.fillRect(barX, barY, 6, barH, dark_mode ? 0x2104 : 0xC618);
    int16_t thumbH = max((int16_t)10, (int16_t)(barH * vis / total));
    int16_t thumbY = barY + (int16_t)((int32_t)top * (barH - thumbH) / max(1, total - vis));
    tft.fillRect(barX, thumbY, 6, thumbH, edgeColor(top, dim_fg()));
}

// Crisp centered "<"/">" selector arrow (solid triangle) — vector, so no font
// reload; safe to call every frame in the settings scroll path.
void BibleInterface::drawChevron(int16_t bx, int16_t by, int16_t bw, int16_t bh, bool right, uint16_t col) {
    int16_t cx = bx + bw / 2, cy = by + bh / 2;
    if (right) tft.fillTriangle(cx - 2, cy - 4, cx - 2, cy + 4, cx + 3, cy, col);
    else       tft.fillTriangle(cx + 2, cy - 4, cx + 2, cy + 4, cx - 3, cy, col);
}
// Centered "+"/"-" for the brightness selector (2px strokes). The +'s right and
// bottom arms are 1px shorter so the cross looks evenly proportioned.
void BibleInterface::drawPlusMinus(int16_t bx, int16_t by, int16_t bw, int16_t bh, bool plus, uint16_t col) {
    int16_t cx = bx + bw / 2, cy = by + bh / 2;
    tft.fillRect(cx - 5, cy - 1, 10, 2, col);              // horizontal bar (right −1px)
    if (plus) tft.fillRect(cx - 1, cy - 5, 2, 10, col);    // vertical bar (bottom −1px)
}
// Draw centered text at the X-Small size, vertically centered in [boxY, boxY+boxH).
// Toggles the loaded UI font; use only off the per-frame scroll path.
void BibleInterface::drawSmallCentered(const char* s, int16_t cx, int16_t boxY, int16_t boxH,
                                       uint16_t fg, uint16_t bg) {
    setUiFont(1);   // X-Small
    int16_t y = boxY + (boxH - (int16_t)VLW_FONTS[1].lineH) / 2;
    tft.setTextColor(fg, bg);
    tft.drawCentreString(s, cx, y, 1);
    setUiFont(2);   // restore normal UI size
}

// ─────────────────────────────────────────────────────────────────────────────
// Smooth (VLW) font helpers for the reading view.
// The reader renders with an anti-aliased VLW font (fonts_vlw.h) instead of the
// TFT_eSPI bitmap fonts. Umlauts are stored as private codes (0x80..0x86); the
// VLW contains the real glyphs, so we map private → UTF-8 for drawString and read
// glyph advances straight from the flash array for text wrapping.
// ─────────────────────────────────────────────────────────────────────────────
static inline uint32_t vlwBE32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
// Map a private code (umlauts 0x80-0x86, typographic marks 0x87-0x8E) to its
// Unicode code point; plain ASCII passes through.
static inline uint16_t vlwPrivToUnicode(uint8_t c) {
    switch (c) {
        case 0x80: return 0xC4; case 0x81: return 0xE4;   // Ä ä
        case 0x82: return 0xD6; case 0x83: return 0xF6;   // Ö ö
        case 0x84: return 0xDC; case 0x85: return 0xFC;   // Ü ü
        case 0x86: return 0xDF;                            // ß
        case 0x87: return 0x201E;                          // „  low double quote
        case 0x88: return 0x201C;                          // "  high double quote
        case 0x89: return 0x201A;                          // ‚  low single quote
        case 0x8A: return 0x2018;                          // '  left single quote
        case 0x8B: return 0x2019;                          // '  right single quote
        case 0x8C: return 0x2013;                          // –  en dash
        case 0x8D: return 0x2014;                          // —  em dash
        case 0x8E: return 0x2026;                          // …  ellipsis
        default:   return c;
    }
}
// xAdvance (px) of a glyph in a VLW flash array, or -1 if the glyph is absent.
static int16_t vlwAdvance(const uint8_t* font, uint16_t uni) {
    uint16_t gCount = (uint16_t)vlwBE32(font);
    const uint8_t* m = font + 24;
    for (uint16_t i = 0; i < gCount; i++, m += 28)
        if ((uint16_t)vlwBE32(m) == uni) return (int16_t)(uint8_t)vlwBE32(m + 12);
    return -1;
}
// TFT_eSPI's guessed space width for a smooth font: (ascent + descent) * 2 / 7.
static int16_t vlwSpaceWidth(const uint8_t* font) {
    int16_t ascent  = (int16_t)vlwBE32(font + 16);
    int16_t descent = (int16_t)vlwBE32(font + 20);
    return (int16_t)(((ascent + descent) * 2) / 7);
}
// Pixel width of a private-code string in a VLW font — matches how TFT_eSPI
// advances the cursor (glyph xAdvance; spaceWidth for ' '; spaceWidth+1 if absent).
static int16_t vlwTextWidth(const uint8_t* font, const char* s) {
    int16_t sw = vlwSpaceWidth(font);
    int16_t w  = 0;
    for (const uint8_t* p = (const uint8_t*)s; *p; p++) {
        if (*p == ' ') { w += sw; continue; }
        int16_t a = vlwAdvance(font, vlwPrivToUnicode(*p));
        w += (a < 0) ? (int16_t)(sw + 1) : a;
    }
    return w;
}
// Convert a private-code string to UTF-8 so the VLW smooth-font drawString renders
// the real glyphs. Umlauts (U+00xx) → 2 bytes; typographic marks (U+20xx) → 3 bytes.
static void vlwPrivToUtf8(const char* in, char* out, size_t n) {
    size_t o = 0;
    for (const uint8_t* p = (const uint8_t*)in; *p && o + 4 < n; p++) {
        uint16_t u = vlwPrivToUnicode(*p);
        if (u < 0x80) {
            out[o++] = (char)u;
        } else if (u < 0x800) {
            out[o++] = (char)(0xC0 | (u >> 6));
            out[o++] = (char)(0x80 | (u & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (u >> 12));
            out[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (u & 0x3F));
        }
    }
    out[o] = 0;
}
// The reading-font array for a size index (font_num), from the active family.
// g_read_fraktur is set by loadToc from a "FONT|fraktur" line; cleared for Bible.
static const uint8_t* vlwForSize(uint8_t idx) {
    if (idx >= VLW_FONT_COUNT) idx = 3;
    return g_read_fraktur ? FRAK_FONTS[idx].data : VLW_FONTS[idx].data;
}
static uint8_t vlwLineH(uint8_t idx) {
    if (idx >= VLW_FONT_COUNT) idx = 3;
    return g_read_fraktur ? FRAK_FONTS[idx].lineH : VLW_FONTS[idx].lineH;
}

// Prettify a filename stem for display: '_' → ' ' and capitalize the first
// letter of each word (e.g. "lutherisches_gesangbuch" → "Lutherisches
// Gesangbuch", "web" → "Web"). The raw stem is still used for SD paths; this is
// display-only. Returns a pointer to a reusable static buffer — draw it before
// the next call (all current callers draw one row at a time).
static const char* prettyName(const char* stem) {
    static char buf[BIBLE_TRANS_LEN + 16];
    size_t n = 0;
    bool   word_start = true;
    for (const char* p = stem; *p && n < sizeof(buf) - 1; p++) {
        char c = (*p == '_') ? ' ' : *p;
        if (c == ' ') {
            word_start = true;
        } else {
            if (word_start && c >= 'a' && c <= 'z') c -= 32;  // capitalize word start
            word_start = false;
        }
        buf[n++] = c;
    }
    buf[n] = 0;
    return buf;
}

// Redraws only the list content area (rows + scrollbar) without touching the
// header or nav bar.  Called directly during scroll drag/fling so there is no
// full-screen repaint — eliminating the white/black flash between frames.
// Uses scroll_px for sub-item-height pixel accuracy.
// setViewport clips rows that extend into the header or nav bar zones.
// startWrite / endWrite batches all SPI transfers in one transaction for speed.
void BibleInterface::redrawListContent(uint16_t item_count) {
    int16_t sub_px      = (int16_t)fmodf(scroll_px, (float)itemH());
    int16_t first       = (int16_t)(scroll_px / (float)itemH());
    int16_t content_top = (int16_t)contentY();
    int16_t content_end = content_top + (int16_t)contentH();

    tft.startWrite();
    // Clip all draws to the content zone — prevents header/nav bleed without a
    // global clear (which would cause flash). vpDatum=false keeps screen-absolute coords.
    tft.setViewport(0, contentY(), scrW(), contentH(), false);

    // Category/song lists of a Fraktur songbook render in the Fraktur title font.
    bool ft = titleFraktur();
    setUiFontEx(2, ft);

    int16_t last_bottom = content_top;

    for (int i = 0; ; i++) {
        int16_t idx = first + i;
        int16_t y   = content_top - sub_px + i * (int16_t)itemH();
        if (y >= content_end || idx >= (int16_t)item_count) break;

        switch (view) {
            case BV_TRANS_SELECT:
                drawListRow(y, trans_names[idx],
                            idx == (int16_t)menu_sel);
                break;
            case BV_SECTION_SELECT:
                if (idx < (int16_t)numSecs())
                    drawListRow(y, secName(idx), idx == (int16_t)menu_sel);
                break;
            case BV_BOOK_SELECT:
                drawListRow(y, bookDisplay(secStart(cur_sec) + idx),
                            idx == (int16_t)menu_sel);
                break;
            case BV_CHAPTER_SELECT:   // Dictionary page list (smooth scroll)
                drawListRow(y, pageLabel(idx), idx == (int16_t)menu_sel, false);
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

    if (ft) setUiFont(2);   // restore the normal UI font

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
    uint16_t  chaps      = bookChapters(cur_book);
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
            uint16_t ch = (uint16_t)((menu_scroll + row) * 5 + col + 1);
            uint16_t x = col * tile_w;
            uint16_t y = contentY() + row * tile_h;
            if (ch > chaps) {
                // Clear unused tile slots to the right in the last row
                if (x < scrW() - 6)
                    tft.fillRect(x, y, scrW() - 6 - x, tile_h, bg());
                break;
            }
            bool     sel     = (ch == cur_chapter) || (ch == (int16_t)menu_sel);
            uint16_t tile_bg = sel ? sel_bg() : bg();
            tft.fillRect(x, y, tile_w, tile_h, tile_bg);
            tft.drawRect(x, y, tile_w, tile_h, edgeColor(menu_scroll + row, dark_mode ? 0x2104 : 0xC618));
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
    const char* title = (mode == MODE_SONGS) ? "Choose Songbook"
                      : (mode == MODE_DICT)  ? "Choose Dictionary"
                                             : "Choose Translation";
    drawHeader(title, true);
    uint8_t vis = visItems();
    for (uint8_t i = 0; i < vis && (menu_scroll + i) < trans_count; i++) {
        bool sel = (menu_scroll + i) == (int16_t)menu_sel;
        drawListRow(contentY() + i * itemH(), trans_names[menu_scroll + i], sel);
    }
    drawScrollBar(trans_count, vis, menu_scroll);
    drawNavBar("Marks", "Settings", "Bright");
}

// ─────────────────────────────────────────────────────────────────────────────
// Section select screen
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::drawSectionSelect() {
    tft.fillScreen(bg());
    drawHeader(trans_count > 0 ? trans_names[cur_trans] : "Bible", true);
    // Scroll-aware: Songs can have many categories (sections), so honor menu_scroll
    // and draw a scrollbar (Bible's 4 sections always fit).
    uint8_t vis = visItems();
    bool ft = titleFraktur();
    setUiFontEx(2, ft);
    for (uint8_t i = 0; i < vis && (menu_scroll + i) < numSecs(); i++) {
        bool sel = (menu_scroll + i) == (int16_t)menu_sel;
        drawListRow(contentY() + i * itemH(), secName(menu_scroll + i), sel);
    }
    if (ft) setUiFont(2);
    drawScrollBar(numSecs(), vis, menu_scroll);
    drawNavBar("Marks", "Settings", "Bright");
}

// ─────────────────────────────────────────────────────────────────────────────
// Book select screen
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::drawBookSelect() {
    tft.fillScreen(bg());
    drawHeader(secName(cur_sec));
    uint8_t vis   = visItems();
    uint16_t start = secStart(cur_sec);
    uint16_t count = secLen(cur_sec);
    bool ft = titleFraktur();
    setUiFontEx(2, ft);
    for (uint8_t i = 0; i < vis && (menu_scroll + i) < count; i++) {
        bool sel = (menu_scroll + i) == (int16_t)menu_sel;
        drawListRow(contentY() + i * itemH(), bookDisplay(start + menu_scroll + i), sel);
    }
    if (ft) setUiFont(2);
    drawScrollBar(count, vis, menu_scroll);
    drawNavBar("Marks", "Settings", "Bright");
}

// ─────────────────────────────────────────────────────────────────────────────
// Chapter select screen
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::drawChapterSelect() {
    tft.fillScreen(bg());
    drawHeader(bookDisplay(cur_book));

    // Dictionary: a scrollable list of pages ("firstword - lastword").
    if (mode == MODE_DICT) {
        uint8_t vis = visItems();
        for (uint8_t i = 0; i < vis && (menu_scroll + i) < (int16_t)rt_page_count; i++) {
            bool sel = (menu_scroll + i) == (int16_t)menu_sel;
            drawListRow(contentY() + i * itemH(), pageLabel(menu_scroll + i), sel, false);
        }
        drawScrollBar((int16_t)rt_page_count, vis, menu_scroll);
        drawNavBar("Marks", "Settings", "Bright");
        return;
    }

    uint16_t  chaps    = bookChapters(cur_book);
    uint16_t tile_w   = scrW() / 5;
    uint16_t tile_h   = 36;
    uint8_t  vis_rows = (uint8_t)(contentH() / tile_h);

    for (uint8_t row = 0; row < vis_rows; row++) {
        for (uint8_t col = 0; col < 5; col++) {
            uint16_t ch = (uint16_t)((menu_scroll + row) * 5 + col + 1);
            if (ch > chaps) break;

            uint16_t x  = col * tile_w;
            uint16_t y  = contentY() + row * tile_h;
            bool     sel = (ch == cur_chapter) || (ch == (int16_t)menu_sel);

            uint16_t tile_bg = sel ? sel_bg() : bg();
            tft.fillRect(x, y, tile_w, tile_h, tile_bg);
            tft.drawRect(x, y, tile_w, tile_h, edgeColor(menu_scroll + row, dark_mode ? 0x2104 : 0xC618));

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
// (Re)load the VLW smooth font for the current size into the line sprite. The
// loaded font persists across createSprite/deleteSprite (only the destructor
// unloads it), so this only does real work when the size actually changes.
void BibleInterface::loadReadingFont() {
    uint8_t idx = (font_num < VLW_FONT_COUNT) ? font_num : 3;
    if (read_font_loaded == (int8_t)idx && read_font_frak == g_read_fraktur) return;
    line_spr.loadFont(vlwForSize(idx));        // normal or Fraktur; unloads previous first
    read_font_loaded = (int8_t)idx;
    read_font_frak   = g_read_fraktur;
}

// Load a smooth VLW size onto tft for the whole UI. While a font is loaded every
// TFT_eSPI text call (drawString/drawCentreString/drawChar/textWidth) renders with
// it, so menus, headers, nav, buttons and the keyboard all use the smooth font and
// its native umlaut glyphs. Big elements bump to a larger size and restore.
void BibleInterface::setUiFont(uint8_t idx) { setUiFontEx(idx, false); }

// As setUiFont, but selects the Fraktur *title* family when frak_title is true —
// used to render song/category/book titles of Fraktur songbooks in blackletter.
void BibleInterface::setUiFontEx(uint8_t idx, bool frak_title) {
    if (idx >= VLW_FONT_COUNT) idx = 2;
    if (ui_font_idx == (int8_t)idx && ui_font_frak == frak_title) return;
    const VlwFont* fam = frak_title ? FRAKT_FONTS : VLW_FONTS;
    tft.loadFont(fam[idx].data);
    g_ui_vlw    = fam[idx].data;
    ui_font_idx = (int8_t)idx;
    ui_font_frak = frak_title;
}

// True when the current view is showing a Fraktur songbook's own content, so its
// titles (category/song names, reading header) should render in the Fraktur title
// font. NOT the songbook picker (which lists mixed books).
bool BibleInterface::titleFraktur() const {
    if (!g_read_fraktur || mode != MODE_SONGS) return false;
    return (view == BV_READING || view == BV_BOOK_SELECT || view == BV_SECTION_SELECT);
}

void BibleInterface::drawReadingLines() {
    loadReadingFont();
    const uint8_t* vfont = vlwForSize(font_num);   // for verse-number width math
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
    line_spr.setTextWrap(false, false);   // one glyph line per sprite — never wrap

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
            char u8[BIBLE_LINE_BUF * 2];               // private codes → UTF-8 for the VLW font
            const int16_t txt_y = 1;                   // small top pad inside the line sprite
            // Smooth fonts render into a sprite via setCursor + printToSprite (the
            // sprite's own drawGlyph); drawString would draw to the physical TFT.
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
                    line_spr.setCursor(4, txt_y);
                    line_spr.printToSprite(num_str, strlen(num_str));
                    int16_t nx = vlwTextWidth(vfont, num_str);   // advance for content x
                    line_spr.setTextColor(font_fg(), line_bg);
                    line_spr.setCursor(4 + nx + 2, txt_y);
                    vlwPrivToUtf8(pipe + 1, u8, sizeof(u8));
                    line_spr.printToSprite(u8, strlen(u8));
                }
            } else {
                line_spr.setTextColor(font_fg(), line_bg);
                line_spr.setCursor(4, txt_y);
                vlwPrivToUtf8(ln, u8, sizeof(u8));
                line_spr.printToSprite(u8, strlen(u8));
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
    char hdr[56];
    if (mode == MODE_DICT) {
        // Dictionary: show the open page's word pair ("firstword - lastword").
        const char* pl = pageLabel(cur_chapter - 1);
        if (pl && pl[0]) snprintf(hdr, sizeof(hdr), "%s", pl);
        else             snprintf(hdr, sizeof(hdr), "%s p%d", bookDisplay(cur_book), cur_chapter);
    } else if (bookChapters(cur_book) <= 1) {
        // Single-chapter books (Songs) show just the title — no redundant " 1".
        snprintf(hdr, sizeof(hdr), "%s", bookDisplay(cur_book));
    } else {
        snprintf(hdr, sizeof(hdr), "%s %d", bookDisplay(cur_book), cur_chapter);
    }
    drawHeader(hdr);
    drawReadingLines();
    drawNavBar("Marks", "Settings", "+Mark");
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings screen
// ─────────────────────────────────────────────────────────────────────────────
// Settings row indices (kept in sync with handleSettingsInput):
//   0 Font Size · 1 Font Color · 2 Verse # Color · 3 Theme · 4 Brightness
//   5 Song Book/Translation/Dictionary · 6 Highlight · 7 Orientation
//   8 About · 9 Boot OTA_1 · 10 Calibrate Touch (resistive only)
static const char* const SCOPE_NAMES[5] = { "Global", "Main Menu", "Bible", "Songs", "Dictionary" };

// Representative NVS namespace used to READ a scope's settings.
static const char* scopeReadNs(uint8_t scope) {
    switch (scope) {
        case 2: return "bible";
        case 3: return "songs";
        case 4: return "dict";
        default: return "menu";   // Global / Main Menu
    }
}

uint8_t BibleInterface::settingsScopeMode() const {
    switch (settings_scope) {
        case 2: return MODE_BIBLE;
        case 3: return MODE_SONGS;
        case 4: return MODE_DICT;
        default: return 0xFF;     // Global / Main Menu have no translation
    }
}

void BibleInterface::buildSettingsRows() {
    uint8_t k = 0;
    set_rows[k++] = SR_SCOPE;
    if (settingsScopeMode() != 0xFF) set_rows[k++] = SR_TRANS;   // Translation near the top
    set_rows[k++] = SR_FONTSIZE;
    set_rows[k++] = SR_FONTCOL;
    set_rows[k++] = SR_VNUMCOL;
    set_rows[k++] = SR_THEME;
    set_rows[k++] = SR_HIGHLIGHT;
    set_rows[k++] = SR_ORIENT;
    set_rows[k++] = SR_BRIGHT;
    set_rows[k++] = SR_ABOUT;                                    // just above Boot
    set_rows[k++] = SR_BOOT;
#ifndef HAS_CAP_TOUCH
    set_rows[k++] = SR_CALIB;
#endif
    set_row_n = k;
}

// Write one "look" setting to every NVS namespace covered by the current scope
// (Global = all modes + menu; otherwise just the scope's namespace).
void BibleInterface::writeScoped(const char* key, uint8_t val) {
    static const char* const ALL[] = { "bible", "songs", "dict", "menu" };
    Preferences p;
    if (settings_scope == 0) {            // Global
        for (uint8_t i = 0; i < 4; i++)
            if (p.begin(ALL[i], false)) { p.putUChar(key, val); p.end(); }
    } else {
        if (p.begin(scopeReadNs(settings_scope), false)) { p.putUChar(key, val); p.end(); }
    }
}

// Load the scope's "look" settings into the live member vars (also previews the
// scope on the settings screen). Brightness/orientation are global (excluded).
void BibleInterface::loadScopeSettings() {
    Preferences p;
    bool ok = p.begin(scopeReadNs(settings_scope), true);
    theme_idx      = ok ? p.getUChar("theme",    0) : 0;
    accent_idx     = ok ? p.getUChar("accent",   0) : 0;
    accent_def     = ok ? (p.getUChar("accentdef", 0) != 0) : false;
    font_num       = ok ? p.getUChar("font",     3) : 3;
    font_color_idx = ok ? p.getUChar("fontcol",  0) : 0;
    vnum_color_idx = ok ? p.getUChar("vnumcol",  0) : 0;
    if (ok) p.end();
    if (theme_idx      >= THEME_COUNT)      theme_idx = 0;
    if (accent_idx     >= ACCENT_COUNT)     accent_idx = 0;
    if (font_num >= VLW_FONT_COUNT) font_num = 3;   // VLW reading-size index; 3 = Medium
    if (font_color_idx >= FONT_COLOR_COUNT) font_color_idx = 0;
    if (vnum_color_idx >= FONT_COLOR_COUNT) vnum_color_idx = 0;
    dark_mode = THEMES[theme_idx].dark;
}

// Scan the scope mode's SD folder for translations into sc_trans[], and set
// sc_trans_cur from that scope's saved "trans" index.
void BibleInterface::scopeScanTrans() {
    sc_trans_count = 0; sc_trans_cur = 0;
    uint8_t m = settingsScopeMode();
    if (m == 0xFF) return;
    const char* base = (m == MODE_SONGS) ? SONGS_SD_BASE
                     : (m == MODE_DICT)  ? DICT_SD_BASE : BIBLE_SD_BASE;
    File root = SD.open(base);
    if (root) {
        while (sc_trans_count < BIBLE_MAX_TRANS) {
            File e = root.openNextFile();
            if (!e) break;
            if (e.isDirectory()) { e.close(); continue; }
            String name = e.name(); e.close();
            name.toLowerCase();
            if (!name.endsWith(".xml")) continue;
            int slash = name.lastIndexOf('/'); if (slash >= 0) name = name.substring(slash + 1);
            String stem = name.substring(0, name.length() - 4);
            strncpy(sc_trans[sc_trans_count], stem.c_str(), BIBLE_TRANS_LEN - 1);
            sc_trans[sc_trans_count][BIBLE_TRANS_LEN - 1] = 0;
            sc_trans_count++;
        }
        root.close();
    }
    for (uint8_t i = 0; i + 1 < sc_trans_count; i++)
        for (uint8_t j = i + 1; j < sc_trans_count; j++)
            if (strcmp(sc_trans[j], sc_trans[i]) < 0) {
                char t[BIBLE_TRANS_LEN];
                strncpy(t, sc_trans[i], BIBLE_TRANS_LEN);
                strncpy(sc_trans[i], sc_trans[j], BIBLE_TRANS_LEN);
                strncpy(sc_trans[j], t, BIBLE_TRANS_LEN);
            }
    for (uint8_t i = 0; i < sc_trans_count; i++)
        transDisplayName(base, sc_trans[i], sc_trans_names[i], BIBLE_TRANS_DISP_LEN);
    Preferences p; uint8_t cur = 0;
    if (p.begin(scopeReadNs(settings_scope), true)) { cur = p.getUChar("trans", 0); p.end(); }
    sc_trans_cur = (cur < sc_trans_count) ? cur : 0;
}

void BibleInterface::redrawSettingsContent() {
    const uint8_t  vis   = visItems();
    const int16_t  btn_w = 28, btn_h = 22, btn_r = 4;

    // Pixel-accurate scroll (same momentum machinery as the other lists).
    int16_t sub_px      = (int16_t)fmodf(scroll_px, (float)itemH());
    int16_t first       = (int16_t)(scroll_px / (float)itemH());
    int16_t content_top = (int16_t)contentY();
    int16_t content_end = content_top + (int16_t)contentH();

    tft.startWrite();
    tft.setViewport(0, contentY(), scrW(), contentH(), false);

    // [<] Name [>] choice row. name_col = colour to draw the value text in
    // (0 = default fg). Font/Verse colour rows pass the actual colour so the user
    // previews it; others pass 0.
    auto choiceRow = [&](int16_t row_y, const char* label, const char* name,
                         bool sel, uint16_t name_col) {
        drawListRow(row_y, label, sel, false);
        uint16_t bg_c  = sel ? sel_bg() : bg();
        int16_t  btn_y = row_y + (itemH() - btn_h) / 2;
        int16_t  txt_y = btn_y + (btn_h - 16) / 2;
        int16_t  nam_y = row_y + (itemH() - 16) / 2;
        int16_t  fwd_bx = (int16_t)scrW() - 9 - btn_w;   // -9 clears the 6px scrollbar
        int16_t  eseed = row_y / (int16_t)itemH();
        tft.fillRoundRect(fwd_bx, btn_y, btn_w, btn_h, btn_r, hdr_bg());
        tft.drawRoundRect(fwd_bx, btn_y, btn_w, btn_h, btn_r, edgeColor(eseed, dim_fg()));
        drawChevron(fwd_bx, btn_y, btn_w, btn_h, true, font_fg());
        int16_t nam_w = (int16_t)tft.textWidth(name, 2);
        int16_t nam_x = fwd_bx - 4 - nam_w;
        tft.setTextColor(name_col ? name_col : fg(), bg_c);
        tft.drawString(name, nam_x, nam_y, 2);
        int16_t bwd_bx = nam_x - 4 - btn_w;
        tft.fillRoundRect(bwd_bx, btn_y, btn_w, btn_h, btn_r, hdr_bg());
        tft.drawRoundRect(bwd_bx, btn_y, btn_w, btn_h, btn_r, edgeColor(eseed + 4, dim_fg()));
        drawChevron(bwd_bx, btn_y, btn_w, btn_h, false, font_fg());
    };
    // Translation row: the value can be long, so instead of letting it push the
    // [<] button onto the "Translation" label, the two selectors bracket a fixed
    // ~11-character window and the value text scrolls (marquee) inside it.
    auto transRow = [&](int16_t row_y, const char* label, const char* name, bool sel) {
        drawListRow(row_y, label, sel, false);
        uint16_t bg_c   = sel ? sel_bg() : bg();
        int16_t  btn_y  = row_y + (itemH() - btn_h) / 2;
        int16_t  txt_y  = btn_y + (btn_h - 16) / 2;
        int16_t  nam_y  = row_y + (itemH() - 16) / 2;
        int16_t  fwd_bx = (int16_t)scrW() - 9 - btn_w;
        int16_t  eseed  = row_y / (int16_t)itemH();
        // [>]
        tft.fillRoundRect(fwd_bx, btn_y, btn_w, btn_h, btn_r, hdr_bg());
        tft.drawRoundRect(fwd_bx, btn_y, btn_w, btn_h, btn_r, edgeColor(eseed, dim_fg()));
        drawChevron(fwd_bx, btn_y, btn_w, btn_h, true, font_fg());
        // Fixed window (~11 chars) for [<], but never let it overlap the label.
        int16_t want_w  = (int16_t)tft.textWidth("Translation", 2);   // 11-char reference
        int16_t label_w = (int16_t)tft.textWidth(label, 2);
        int16_t bwd_des = fwd_bx - 4 - want_w - 4 - btn_w;
        int16_t bwd_min = 10 + label_w + 8;        // sit just right of the label
        int16_t bwd_bx  = bwd_des > bwd_min ? bwd_des : bwd_min;
        // [<]
        tft.fillRoundRect(bwd_bx, btn_y, btn_w, btn_h, btn_r, hdr_bg());
        tft.drawRoundRect(bwd_bx, btn_y, btn_w, btn_h, btn_r, edgeColor(eseed + 4, dim_fg()));
        drawChevron(bwd_bx, btn_y, btn_w, btn_h, false, font_fg());
        // Value window between the inner edges of the two buttons.
        int16_t win_left = bwd_bx + btn_w + 4;
        int16_t win_w    = (fwd_bx - 4) - win_left;
        if (win_w < 8) win_w = 8;
        int16_t textw = (int16_t)tft.textWidth(name, 2);
        if (strncmp(trans_marq_str, name, sizeof(trans_marq_str)) != 0) {
            strncpy(trans_marq_str, name, sizeof(trans_marq_str) - 1);
            trans_marq_str[sizeof(trans_marq_str) - 1] = '\0';
            trans_marq_off = 0;
            trans_marq_ms  = millis();
        }
        trans_marq_winx  = win_left;
        trans_marq_winy  = row_y;
        trans_marq_winw  = win_w;
        trans_marq_texty = nam_y;
        trans_marq_textw = textw;
        trans_marq_bg    = bg_c;
        trans_marq_fg    = fg();
        trans_marq_on    = (textw > win_w);
        drawTransValue(trans_marq_on ? trans_marq_off : 0);
    };
    auto brightRow = [&](int16_t row_y, bool sel) {
        drawListRow(row_y, "Brightness", sel, false);
        uint16_t bg_c  = sel ? sel_bg() : bg();
        int16_t  btn_y = row_y + (itemH() - btn_h) / 2;
        int16_t  txt_y = btn_y + (btn_h - 16) / 2;
        int16_t  num_y = row_y + (itemH() - 16) / 2;
        int16_t  eseed = row_y / (int16_t)itemH();
        int16_t plus_bx = (int16_t)scrW() - 9 - btn_w;
        tft.fillRoundRect(plus_bx, btn_y, btn_w, btn_h, btn_r, hdr_bg());
        tft.drawRoundRect(plus_bx, btn_y, btn_w, btn_h, btn_r, edgeColor(eseed, dim_fg()));
        drawPlusMinus(plus_bx, btn_y, btn_w, btn_h, true, font_fg());
        char nbuf[8];
        snprintf(nbuf, sizeof(nbuf), "%d/20", bl_idx + 1);
        int16_t num_w = (int16_t)tft.textWidth(nbuf, 2);
        int16_t num_x = plus_bx - 4 - num_w;
        tft.setTextColor(fg(), bg_c);
        tft.drawString(nbuf, num_x, num_y, 2);
        int16_t minus_bx = num_x - 4 - btn_w;
        tft.fillRoundRect(minus_bx, btn_y, btn_w, btn_h, btn_r, hdr_bg());
        tft.drawRoundRect(minus_bx, btn_y, btn_w, btn_h, btn_r, edgeColor(eseed + 4, dim_fg()));
        drawPlusMinus(minus_bx, btn_y, btn_w, btn_h, false, font_fg());
    };

    // Cleared each pass; transRow re-asserts it only if the Translation row is
    // actually drawn and overflowing, so a scrolled-off row stops the marquee.
    trans_marq_on = false;

    int16_t last_bottom = content_top;
    for (int vi = 0; ; vi++) {
        int16_t i     = first + vi;
        int16_t row_y = content_top - sub_px + vi * (int16_t)itemH();
        if (row_y >= content_end || i >= (int16_t)set_row_n) break;
        bool sel = (i == (int16_t)menu_sel);

        switch (set_rows[i]) {
            case SR_SCOPE:
                choiceRow(row_y, "Settings Scope", SCOPE_NAMES[settings_scope], sel, 0);
                break;
            case SR_TRANS: {
                uint8_t sm = settingsScopeMode();
                const char* tl = (sm == MODE_SONGS) ? "Song Book"
                               : (sm == MODE_DICT)  ? "Dictionary" : "Translation";
                transRow(row_y, tl, sc_trans_count > 0 ? sc_trans_names[sc_trans_cur] : "-", sel);
                break;
            }
            case SR_FONTSIZE: {
                static const char* const SZ_NAMES[6] = { "Tiny", "X-Small", "Small",
                                                         "Medium", "Large", "X-Large" };
                uint8_t si = (font_num < VLW_FONT_COUNT && font_num < 6) ? font_num : 3;
                choiceRow(row_y, "Font Size", SZ_NAMES[si], sel, 0);
                break;
            }
            case SR_FONTCOL:   choiceRow(row_y, "Font Color",    FONT_COLOR_NAMES[font_color_idx], sel, font_fg());       break;
            case SR_VNUMCOL:   choiceRow(row_y, "Verse # Color", FONT_COLOR_NAMES[vnum_color_idx], sel, verse_num_fg()); break;
            case SR_THEME:     choiceRow(row_y, "Theme",         THEMES[theme_idx].name,           sel, 0);              break;
            case SR_HIGHLIGHT: choiceRow(row_y, "Highlight", accent_def ? "Default" : ACCENT_NAMES[accent_idx], sel, 0); break;
            case SR_ORIENT:    choiceRow(row_y, "Orientation",   ORIENT_NAMES[orientation & 3],    sel, 0);              break;
            case SR_BRIGHT:    brightRow(row_y, sel); break;
            case SR_ABOUT:     drawListRow(row_y, "About",           sel, false); break;
            case SR_BOOT:      drawListRow(row_y, "Boot OTA_1",      sel, false); break;
            case SR_CALIB:     drawListRow(row_y, "Calibrate Touch", sel, false); break;
        }
        int16_t bot = row_y + (int16_t)itemH();
        if (bot > last_bottom) last_bottom = bot;
    }

    if (last_bottom < content_end)
        tft.fillRect(0, last_bottom, scrW(), content_end - last_bottom, bg());

    tft.resetViewport();
    drawScrollBar((int16_t)set_row_n, vis, first);
    tft.endWrite();
}

// Draws the Translation value text into its window at horizontal scroll `off`,
// clipped to the window via a temporary viewport. Used for both the static draw
// and the marquee tick. Must be called inside a startWrite()/endWrite() pair.
void BibleInterface::drawTransValue(int16_t off) {
    const int16_t GAP = 24;
    // Clip to just this row's band (intersected with the content area) — NOT the
    // whole content height, or it paints a tall bar over the other rows' column.
    // Stop 3 pixels short of the bottom so the row's divider line is preserved.
    int16_t top = trans_marq_winy;
    int16_t bot = trans_marq_winy + (int16_t)itemH() - 3;
    int16_t cy0 = (int16_t)contentY();
    int16_t cy1 = cy0 + (int16_t)contentH();
    if (top < cy0) top = cy0;
    if (bot > cy1) bot = cy1;
    int16_t h = bot - top;
    if (h <= 0) return;
    tft.setViewport(trans_marq_winx, top, trans_marq_winw, h, false);
    tft.fillRect(trans_marq_winx, top, trans_marq_winw, h, trans_marq_bg);
    // tftCharUTF8 fills each glyph cell with the current text bg colour, so set it
    // to the row background here or the text sits on a stale (button) colour.
    tft.setTextColor(trans_marq_fg, trans_marq_bg);
    int16_t x0 = trans_marq_winx - off;
    int16_t tx = x0;
    for (const char* p = trans_marq_str; *p; p++)
        tx += tftCharUTF8(tft, (uint8_t)*p, tx, trans_marq_texty, 2, trans_marq_fg);
    if (trans_marq_on) {                       // second copy for a seamless wrap
        int16_t tx2 = x0 + trans_marq_textw + GAP;
        int16_t end = trans_marq_winx + trans_marq_winw;
        for (const char* p = trans_marq_str; *p && tx2 < end; p++)
            tx2 += tftCharUTF8(tft, (uint8_t)*p, tx2, trans_marq_texty, 2, trans_marq_fg);
    }
    tft.setViewport(0, contentY(), scrW(), contentH(), false);  // restore content clip
}

// Advances the Translation marquee slowly and redraws just its window. Called
// every loop while the Settings view is idle (no drag/fling/press in progress).
void BibleInterface::tickTransMarquee() {
    if (!trans_marq_on) return;
    if (scroll_dragging || fling_active || touch_was_down) return;
    uint32_t now = millis();
    uint32_t interval = (trans_marq_off == 0) ? 900 : 55;   // dwell at start, then ~18 px/s
    if (now - trans_marq_ms < interval) return;
    trans_marq_ms = now;
    trans_marq_off += 1;
    if (trans_marq_off >= trans_marq_textw + 24) trans_marq_off = 0;  // wrap (GAP)
    tft.startWrite();
    drawTransValue(trans_marq_off);
    tft.resetViewport();   // don't leave the content clip set for the next view's redraw
    tft.endWrite();
}

void BibleInterface::drawSettings() {
    tft.fillScreen(bg());
    drawHeader("Settings");
    redrawSettingsContent();
    // Footer: show a "Menu" button (bottom-right) only when opened from a mode.
    drawNavBar("Back", "", settings_from_menu ? "" : "Menu");
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
    drawSmallCentered("Cancel", pop_x + 4 + half_w / 2, btn_y, btn_h, TFT_WHITE, hdr_bg());

    // Delete (right) — red border + red text to signal destructive action
    tft.fillRoundRect(del_x, btn_y, half_w, btn_h, 4, hdr_bg());
    tft.drawRoundRect(del_x, btn_y, half_w, btn_h, 4, TFT_RED);
    drawSmallCentered("Delete", del_x + half_w / 2, btn_y, btn_h, TFT_RED, hdr_bg());
}

// ─────────────────────────────────────────────────────────────────────────────
// Touch input
// ─────────────────────────────────────────────────────────────────────────────
bool BibleInterface::getTouch(uint16_t* tx, uint16_t* ty) {
#ifdef HAS_CAP_TOUCH
    uint16_t raw_x, raw_y;
    uint8_t touches = ft6336_update(&raw_x, &raw_y);
    if (touches == 0) { last_pressed = false; return false; }
    // FT6336 returns panel-native coords; map to the active orientation.
    *tx = raw_x;
    *ty = raw_y;
    orientTouch(*tx, *ty);
#else
    if (!resistiveTouch(tx, ty)) { last_pressed = false; return false; }
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
    orientTouch(*tx, *ty);   // map panel-native coords to the active orientation
#else
    if (!resistiveTouch(tx, ty)) { last_pressed = false; return false; }
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
    // Always calibrate in PORTRAIT (rotation 0) so the stored calibration is
    // orientation-independent; touch is then rotated in software (resistiveTouch).
    uint8_t saved_ori = orientation;
    orientation = 0;
    applyOrientation();

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
        memcpy(cal_data, calData, sizeof(cal_data));  // for manual orientation mapping

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

    // Restore orientation and the user's saved brightness level.
    orientation = saved_ori;
    applyOrientation();
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
void BibleInterface::handleListInput(uint16_t item_count) {
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
                selectTranslation((uint16_t)menu_sel);
                break;
            case BV_SECTION_SELECT:
                goToBook((int16_t)menu_sel);
                break;
            case BV_BOOK_SELECT:
                goToChapter(secStart(cur_sec) + (int16_t)menu_sel);
                break;
            case BV_CHAPTER_SELECT:
                goToReading((int16_t)menu_sel + 1);
                break;
            default: break;
        }
    }
}

void BibleInterface::handleChapterInput() {
    uint16_t  chaps      = bookChapters(cur_book);
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
            uint16_t ch_p  = (uint16_t)((menu_scroll + row_p) * 5 + col_p + 1);
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
        goToReading((int16_t)menu_sel);
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
            } else if (cur_chapter < bookChapters(cur_book)) {
                goToReading(cur_chapter + 1);
            }
        }
    }
}

void BibleInterface::handleSettingsInput() {
    const uint8_t SETTINGS_N = set_row_n;
    uint16_t tx, ty;
    bool down = pollTouch(&tx, &ty);

    // ── Finger just touched down ──────────────────────────────────────────────
    if (down && !touch_was_down) {
        touch_was_down  = true;
        touch_down_x    = tx;
        touch_down_y    = ty;
        scroll_dragging = false;
        stopFling();
        drag_origin_px  = scroll_px;

        // Header and nav fire immediately on press (no drag ambiguity)
        if (touchInHeader(tx, ty)) {
            touch_was_down = false;
            if (!settings_from_menu && touchInSearchIcon(tx, ty)) { goToSearchInput(); return; }
            if (tx < 48) goBack();
            return;
        }
        if (touchInNav(tx, ty)) {
            touch_was_down = false;
            if (tx < scrW()/3) goBack();
            else if (tx > 2 * (scrW()/3) && !settings_from_menu) goToMainMenu();  // footer Menu
            return;
        }
        // Highlight touched (absolute) row immediately for press feedback.
        int16_t row = (int16_t)((scroll_px + (float)((int16_t)ty - (int16_t)contentY())) / (float)itemH());
        if (row >= 0 && row < (int16_t)SETTINGS_N) {
            menu_sel = row;
            redrawSettingsContent();
        }
        return;
    }

    // ── Finger held — pixel-accurate drag (momentum like the other lists) ─────
    if (down && touch_was_down) {
        int16_t dy = (int16_t)ty - (int16_t)touch_down_y;
        if (!scroll_dragging && abs(dy) > 8) {
            scroll_dragging = true;
            menu_sel = -1;
        }
        if (scroll_dragging) {
            float max_px = (float)max(0, (int)SETTINGS_N - (int)visItems()) * (float)itemH();
            float new_px = drag_origin_px + (float)((int16_t)touch_down_y - (int16_t)ty);
            if (new_px < 0.f) new_px = 0.f;
            if (new_px > max_px) new_px = max_px;
            scroll_px   = new_px;
            menu_scroll = (int16_t)(scroll_px / (float)itemH());
            recordVel((int16_t)ty, millis());
            redrawSettingsContent();
        }
        return;
    }

    // ── Finger lifted — fling, or activate the item highlighted on press ──────
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
        if (menu_sel < 0 || menu_sel >= (int16_t)SETTINGS_N) return;

        bool fwd = ((int16_t)touch_down_x >= (int16_t)scrW() - 32);  // [>] vs [<]
        switch ((SettingRow)set_rows[menu_sel]) {
            case SR_SCOPE:
                settings_scope = fwd ? (uint8_t)((settings_scope + 1) % 5)
                                     : (settings_scope == 0 ? 4 : settings_scope - 1);
                buildSettingsRows();
                loadScopeSettings();   // preview + edit the chosen scope's look
                scopeScanTrans();
                menu_sel = 0; menu_scroll = 0; scroll_px = 0.f;
                drawSettings();        // theme may differ — full repaint
                return;
            case SR_TRANS:
                if (sc_trans_count > 1) {
                    sc_trans_cur = fwd ? (uint8_t)((sc_trans_cur + 1) % sc_trans_count)
                                       : (sc_trans_cur == 0 ? sc_trans_count - 1 : sc_trans_cur - 1);
                    { Preferences p;
                      if (p.begin(scopeReadNs(settings_scope), false)) {
                          p.putUChar("trans", sc_trans_cur); p.end(); } }
                    // If editing the active mode's own scope, switch it live too.
                    if (!settings_from_menu && settingsScopeMode() == mode) {
                        cur_trans = sc_trans_cur;
                        cached_book = 0xFFFF; cached_chap = 0; cached_count = 0;
                        book_idx_valid = false;
                        if (mode != MODE_BIBLE) {
                            if (loadToc(trans_stems[cur_trans])) {
                                if (cur_book >= numBooks()) cur_book = 0;
                                cur_sec     = (numBooks() > 0) ? bookSection(cur_book) : 0;
                                cur_chapter = 1;
                            }
                        } else if (cur_book >= numBooks()) cur_book = 0;
                    }
                }
                redrawSettingsContent();
                break;
            case SR_FONTSIZE:
                if (font_num >= VLW_FONT_COUNT) font_num = 3;
                font_num = fwd ? (uint8_t)((font_num + 1) % VLW_FONT_COUNT)
                               : (uint8_t)(font_num == 0 ? VLW_FONT_COUNT - 1 : font_num - 1);
                writeScoped("font", font_num);
                if (cached_count > 0) buildWrappedLines();   // re-wrap at the new size
                redrawSettingsContent();
                break;
            case SR_FONTCOL:
                font_color_idx = fwd ? (uint8_t)((font_color_idx + 1) % FONT_COLOR_COUNT)
                                     : (font_color_idx == 0 ? FONT_COLOR_COUNT - 1 : font_color_idx - 1);
                writeScoped("fontcol", font_color_idx);
                loadMenuChrome();          // chrome text follows the Main Menu font colour
                drawSettings();            // repaint so chrome (header/footer) updates live
                return;
            case SR_VNUMCOL:
                vnum_color_idx = fwd ? (uint8_t)((vnum_color_idx + 1) % FONT_COLOR_COUNT)
                                     : (vnum_color_idx == 0 ? FONT_COLOR_COUNT - 1 : vnum_color_idx - 1);
                writeScoped("vnumcol", vnum_color_idx);
                redrawSettingsContent();
                break;
            case SR_THEME:
                theme_idx = fwd ? (uint8_t)((theme_idx + 1) % THEME_COUNT)
                                : (theme_idx == 0 ? THEME_COUNT - 1 : theme_idx - 1);
                dark_mode = THEMES[theme_idx].dark;
                writeScoped("theme", theme_idx);
                drawSettings();        // bg/fg all change — full repaint
                return;
            case SR_HIGHLIGHT: {
                // Option 0 = Default (theme colour); 1..ACCENT_COUNT = accents.
                uint8_t cur   = accent_def ? 0 : (uint8_t)(accent_idx + 1);
                uint8_t total = ACCENT_COUNT + 1;
                cur = fwd ? (uint8_t)((cur + 1) % total) : (cur == 0 ? total - 1 : cur - 1);
                accent_def = (cur == 0);
                if (!accent_def) accent_idx = cur - 1;
                writeScoped("accent",    accent_idx);
                writeScoped("accentdef", accent_def ? 1 : 0);
                redrawSettingsContent();
                break;
            }
            case SR_ORIENT:   // global (not scoped)
                orientation = fwd ? (uint8_t)((orientation + 1) % ORIENT_COUNT)
                                  : (orientation == 0 ? ORIENT_COUNT - 1 : orientation - 1);
                { Preferences mp;
                  if (mp.begin("menu", false)) { mp.putUChar("orient", orientation); mp.end(); } }
                applyOrientation();
                if (cached_count > 0) buildWrappedLines();
                menu_scroll = 0; scroll_px = 0.f;
                drawSettings();
                return;
            case SR_BRIGHT:   // global (not scoped)
                if (fwd) { if (bl_idx < 19) blSet(bl_idx + 1); }
                else     { if (bl_idx > 0)  blSet(bl_idx - 1); }
                redrawSettingsContent();
                break;
            case SR_ABOUT:
                goToAbout();
                return;
            case SR_BOOT:
                bootMarauder();
                return;
            case SR_CALIB:
#ifndef HAS_CAP_TOUCH
                runTouchCalibration();
                drawSettings();
#endif
                return;
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
    // Modes with a single section (e.g. Dictionary) skip the section list.
    if (numSecs() <= 1) { goToBook(0); return; }
    view = BV_SECTION_SELECT;
    // Scroll the current section into view (Songs can have many categories).
    menu_sel    = (int16_t)cur_sec;
    menu_scroll = 0;
    if (cur_sec >= visItems()) {
        int16_t mid   = (int16_t)cur_sec - (int16_t)(visItems() / 2);
        int16_t max_s = (int16_t)numSecs() - (int16_t)visItems();
        menu_scroll   = (mid > max_s) ? max_s : mid;
        if (menu_scroll < 0) menu_scroll = 0;
    }
    scroll_px   = (float)menu_scroll * (float)itemH();
    needs_redraw = true;
}
void BibleInterface::goToBook(uint8_t sec) {
    stopFling();
    cur_sec = sec;
    view    = BV_BOOK_SELECT;
    // Scroll to show cur_book within this section
    uint16_t local_idx = 0;
    if (sec == bookSection(cur_book)) {
        local_idx = cur_book - secStart(sec);
    }
    menu_sel    = (int16_t)local_idx;
    uint16_t cnt = secLen(sec);
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
void BibleInterface::goToChapter(uint16_t book) {
    stopFling();
    cur_book = book;
    // Single-chapter books (each Song is one chapter) skip the chapter grid and
    // open the reader directly.
    if (bookChapters(book) <= 1) { goToReading(1); return; }

    // Dictionary: show a scrollable list of pages ("firstword - lastword") instead
    // of the numeric grid — reads more like a dictionary.
    if (mode == MODE_DICT) {
        loadDictPages(book);
        view = BV_CHAPTER_SELECT;
        int16_t sel = (int16_t)cur_chapter - 1;       // page index (0-based)
        if (sel < 0 || sel >= (int16_t)rt_page_count) sel = 0;
        menu_sel    = sel;
        menu_scroll = 0;
        if (sel >= (int16_t)visItems()) {
            int16_t mid   = sel - (int16_t)(visItems() / 2);
            int16_t max_s = (int16_t)rt_page_count - (int16_t)visItems();
            menu_scroll   = (mid > max_s) ? max_s : mid;
            if (menu_scroll < 0) menu_scroll = 0;
        }
        scroll_px    = (float)menu_scroll * (float)itemH();
        needs_redraw = true;
        return;
    }

    view     = BV_CHAPTER_SELECT;
    // Scroll grid to show cur_chapter
    const uint16_t tile_h   = 36;
    uint8_t        vis_rows = (uint8_t)(contentH() / tile_h);
    int16_t        row      = (int16_t)((cur_chapter - 1) / 5);
    int16_t        total_rows = ((int16_t)bookChapters(book) + 4) / 5;
    int16_t        max_s    = total_rows - (int16_t)vis_rows;
    if (max_s < 0) max_s = 0;
    menu_scroll = row - (int16_t)(vis_rows / 2);
    if (menu_scroll > max_s)  menu_scroll = max_s;
    if (menu_scroll < 0)      menu_scroll = 0;
    menu_sel  = (int16_t)cur_chapter;   // 1-based; used for tile highlight
    scroll_px = (float)menu_scroll * (float)tile_h;
    needs_redraw = true;
}
// Boot/easter-egg splash: stream /splash.raw (RGB565, big-endian) from SD row-by-row.
// Drawn in the CURRENT orientation — the file must be sized to scrW()×scrH() for the
// active rotation (portrait file works for 0°/180°; make a landscape file for 90°/270°
// with make_splash.py --landscape). Returns false if absent or size mismatched.
bool BibleInterface::drawSplashImage() {
#ifdef MARAUDER_PANCAKE
    const int16_t MAXW = 480;
#else
    const int16_t MAXW = 320;
#endif
    // Try both splash files and use whichever one's embedded size matches the
    // current orientation — so a portrait splash.raw and a landscape
    // splash_land.raw can both live in the library folder and the right one is
    // auto-picked. 8-byte header: "SPL1" + uint16 width + uint16 height (LE).
    static const char* const PATHS[2] = { SD_LIB_ROOT "/splash.raw",
                                          SD_LIB_ROOT "/splash_land.raw" };
    for (uint8_t i = 0; i < 2; i++) {
        File f = SD.open(PATHS[i]);
        if (!f) continue;
        uint8_t hdr[8];
        if (f.read(hdr, 8) != 8 || memcmp(hdr, "SPL1", 4) != 0) { f.close(); continue; }
        int16_t fw = (int16_t)(hdr[4] | (hdr[5] << 8));
        int16_t fh = (int16_t)(hdr[6] | (hdr[7] << 8));
        if (fw != (int16_t)scrW() || fh != (int16_t)scrH() || fw > MAXW) { f.close(); continue; }
        tft.setSwapBytes(true);
        uint16_t row[MAXW];
        for (int16_t y = 0; y < fh; y++) {
            if (f.read((uint8_t*)row, fw * 2) != (int)(fw * 2)) break;
            tft.pushImage(0, y, fw, 1, row);
        }
        tft.setSwapBytes(false);
        f.close();
        return true;
    }
    return false;
}

void BibleInterface::showSplashUntilTap() {
    if (!drawSplashImage()) return;
    delay(350);                       // ignore the triggering tap's release
    for (;;) {
        uint16_t x, y;
        if (pollTouch(&x, &y)) break; // any tap dismisses
        delay(20);
    }
    needs_redraw = true;              // repaint the menu
}

void BibleInterface::drawLoading() {
    tft.fillScreen(bg());
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "%s %d", bookDisplay(cur_book), cur_chapter);
    drawHeader(hdr);
    tft.setTextColor(dim_fg(), bg());
    tft.drawCentreString("Loading...", scrW() / 2, contentY() + contentH() / 2 - 8, 2);
    drawMemUsage((int16_t)(contentY() + contentH() / 2 + 16));
}

void BibleInterface::goToReading(uint16_t chapter, int16_t start_line) {
    stopFling();
    sel_verse_first = sel_verse_last = 0;  // clear verse selection on chapter change
    cur_chapter = chapter;
    // Dictionary: ensure the page labels for this letter are loaded (so the header
    // and page bookmarks can show "firstword - lastword"). Needed when arriving via
    // search/bookmark rather than the page list.
    if (mode == MODE_DICT && rt_pages_book != cur_book) loadDictPages(cur_book);
    read_line   = start_line;
    scroll_px   = (float)start_line * (float)lineH();
    drawLoading();
    view        = BV_READING;
    cacheChapter(cur_book, cur_chapter);
    buildWrappedLines();
    saveState();
    needs_redraw = true;
}
void BibleInterface::goToSettings(bool from_menu) {
    stopFling();
    settings_from_menu = from_menu;
    // Default scope = the context settings was opened from.
    settings_scope = from_menu ? 1 /*Main Menu*/ : (uint8_t)(mode + 2);
    buildSettingsRows();
    loadScopeSettings();   // load (and preview) the scope's look settings
    scopeScanTrans();      // populate the Translation row for the scope
    view = BV_SETTINGS;
    menu_sel = 0;
    menu_scroll = 0;
    scroll_px = 0.f;
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
void BibleInterface::goToAbout() {
    stopFling();
    view = BV_ABOUT;
    needs_redraw = true;
}

// Firmware / hardware information screen. Identity strings come from configs.h
// (BIBLE_FW_* and per-board BOARD_*), so bumping the version is a one-line edit.
void BibleInterface::drawAbout() {
    tft.fillScreen(bg());
    drawHeader("About");

    int16_t cx = (int16_t)scrW() / 2;
    int16_t y  = (int16_t)contentY() + 10;

    // App name + version + author (centred, prominent).
    tft.setTextColor(fg(), bg());
    setUiFont(4);                             // larger smooth font for the app name
    tft.drawCentreString(BIBLE_FW_NAME, cx, y, 4);
    setUiFont(2);
    y += 34;
    char vbuf[40];
    snprintf(vbuf, sizeof(vbuf), "Version %s", BIBLE_FW_VERSION);
    tft.setTextColor(fg(), bg());
    tft.drawCentreString(vbuf, cx, y, 2);
    y += 22;
    tft.setTextColor(dim_fg(), bg());
    tft.drawCentreString("by " BIBLE_FW_AUTHOR, cx, y, 2);
    y += 26;

    tft.drawFastHLine(16, y, (int16_t)scrW() - 32, dim_fg());
    y += 10;

    // Label : value detail rows.
    auto row = [&](const char* label, const char* value) {
        tft.setTextColor(dim_fg(), bg());
        tft.drawString(label, 14, y, 2);
        tft.setTextColor(fg(), bg());
        tft.drawString(value, 92, y, 2);
        y += 22;
    };
    row("Board",   BOARD_NAME);
    about_mcu_y0 = y;                 // remember the MCU row band for the splash easter egg
    row("MCU",     BOARD_MCU);
    about_mcu_y1 = y;
    row("Display", BOARD_DISPLAY);
    row("Touch",   BOARD_TOUCH);
#ifdef HAS_PSRAM
    row("PSRAM",   "Yes");
#else
    row("PSRAM",   "None");
#endif
    row("Built",   __DATE__);
    row("Commit",  BIBLE_FW_COMMIT);

    drawNavBar("Back", "", "");
}

void BibleInterface::handleAboutInput() {
    uint16_t tx, ty;
    bool down = pollTouch(&tx, &ty);
    if (down && !touch_was_down) {
        touch_was_down = true;
        touch_down_x = tx; touch_down_y = ty;
        return;
    }
    if (!down && touch_was_down) {
        touch_was_down = false;
        // Easter egg: tapping the MCU row shows the boot splash until tapped.
        if ((int16_t)touch_down_y >= about_mcu_y0 && (int16_t)touch_down_y < about_mcu_y1) {
            showSplashUntilTap();
            return;
        }
        // Header back arrow or the "Back" nav button returns to Settings.
        bool hdr_back = ((int16_t)touch_down_y < (int16_t)hdrH() && touch_down_x < 48);
        bool nav_back = (touchInNav(touch_down_x, touch_down_y) &&
                         touch_down_x < (int16_t)scrW() / 3);
        if (hdr_back || nav_back) { view = BV_SETTINGS; needs_redraw = true; }
    }
}
// ─────────────────────────────────────────────────────────────────────────────
// Mode-parameterized SD paths and NVS namespace
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::applyOrientation() {
    // 0 = Normal · 1 = Landscape · 2 = Flip 180 · 3 = Landscape flipped.
    // scrW()/scrH() track this so all layout adapts; setRotation maps 1:1.
    tft.setRotation(orientation & 3);
}

// Map a raw cap-touch point (native portrait frame) to the active orientation.
#ifndef HAS_CAP_TOUCH
// Resistive (XPT2046): read raw ADC, map to native portrait using the stored
// calibration (rotation 0), then rotate to the active orientation. This avoids
// TFT_eSPI's per-rotation getTouch() calibration, which breaks when the screen
// is rotated after a single (portrait) calibration.
bool BibleInterface::resistiveTouch(uint16_t* x, uint16_t* y) {
    if (tft.getTouchRawZ() < 600) return false;        // pressure threshold
    // The XPT2046 is noisy: a single raw read jitters by tens of pixels, which the
    // shared scroll logic misreads as a drag + fling — that made the settings list
    // flicker, scroll on its own, and swallow the Back tap. So take several samples
    // and average the middle ones (drop the min and max on each axis). Require a
    // few stable reads, otherwise treat it as no-touch (rejects pressure bounce and
    // phantom spikes that produced ghost taps).
    const uint8_t N = 7;
    uint16_t bx[N], by[N];
    uint8_t got = 0;
    for (uint8_t i = 0; i < N; i++) {
        if (tft.getTouchRawZ() < 600) break;
        uint16_t srx = 0, sry = 0;
        tft.getTouchRaw(&srx, &sry);
        bx[got] = srx; by[got] = sry; got++;
    }
    if (got < 4) return false;                          // not a stable press
    uint32_t sumx = 0, sumy = 0;
    uint16_t minx = 0xFFFF, maxx = 0, miny = 0xFFFF, maxy = 0;
    for (uint8_t i = 0; i < got; i++) {
        sumx += bx[i]; sumy += by[i];
        if (bx[i] < minx) minx = bx[i];
        if (bx[i] > maxx) maxx = bx[i];
        if (by[i] < miny) miny = by[i];
        if (by[i] > maxy) maxy = by[i];
    }
    sumx -= (uint32_t)minx + maxx;                      // drop the extremes
    sumy -= (uint32_t)miny + maxy;
    uint16_t rx = (uint16_t)(sumx / (got - 2));
    uint16_t ry = (uint16_t)(sumy / (got - 2));
    const int32_t NW = 240, NH = 320;                  // native portrait (ILI9341)
    int32_t x0 = cal_data[0], x1 = cal_data[1] ? cal_data[1] : 1;
    int32_t y0 = cal_data[2], y1 = cal_data[3] ? cal_data[3] : 1;
    int32_t px, py;
    if (!(cal_data[4] & 0x01)) { px = ((int32_t)rx - x0) * NW / x1; py = ((int32_t)ry - y0) * NH / y1; }
    else                       { px = ((int32_t)ry - x0) * NW / x1; py = ((int32_t)rx - y0) * NH / y1; }
    if (cal_data[4] & 0x02) px = NW - px;
    if (cal_data[4] & 0x04) py = NH - py;
    if (px < 0) px = 0; if (px >= NW) px = NW - 1;
    if (py < 0) py = 0; if (py >= NH) py = NH - 1;
    uint16_t fx = (uint16_t)px, fy = (uint16_t)py;
    orientTouch(fx, fy);                               // portrait → active orientation
    *x = fx; *y = fy;
    return true;
}
#endif

void BibleInterface::orientTouch(uint16_t& x, uint16_t& y) const {
#ifdef MARAUDER_PANCAKE
    const uint16_t NW = 320, NH = 480;
#else
    const uint16_t NW = 240, NH = 320;
#endif
    uint16_t rx = x, ry = y;
    switch (orientation & 3) {
        case 0: x = rx;                       y = ry;                       break;
        case 1: x = ry;                       y = (uint16_t)(NW - 1 - rx);  break;
        case 2: x = (uint16_t)(NW - 1 - rx);  y = (uint16_t)(NH - 1 - ry);  break;
        case 3: x = (uint16_t)(NH - 1 - ry);  y = rx;                       break;
    }
}

const char* BibleInterface::basePath() const {
    switch (mode) {
        case MODE_SONGS: return SONGS_SD_BASE;
        case MODE_DICT:  return DICT_SD_BASE;
        default:         return BIBLE_SD_BASE;
    }
}
const char* BibleInterface::nvsNamespace() const {
    switch (mode) {
        case MODE_SONGS: return "songs";
        case MODE_DICT:  return "dict";
        default:         return "bible";
    }
}
void BibleInterface::bmPath(char* out, size_t n) const {
    snprintf(out, n, "%s/bookmarks.txt", basePath());
}
void BibleInterface::srchHistPath(char* out, size_t n) const {
    snprintf(out, n, "%s/srch_hist.txt", basePath());
}

// ─────────────────────────────────────────────────────────────────────────────
// Runtime structure tables (Songs / Dictionary) — allocated from PSRAM if present
// ─────────────────────────────────────────────────────────────────────────────
// Allocate from PSRAM when available, but ALWAYS fall back to internal SRAM if
// PSRAM is absent or not enabled in the build — these tables are small (a few KB,
// up to ~50 KB for the largest songbook) and must never fail just because PSRAM
// isn't there. A NULL here used to crash Bible reads and bounce Songs/Dict to the menu.
static inline void* rt_alloc(size_t sz) {
#ifdef HAS_PSRAM
    void* p = ps_malloc(sz);
    if (p) return p;
#endif
    return malloc(sz);
}
#define RT_MALLOC(sz) rt_alloc(sz)

// Map the first character of a dictionary query to its letter-bucket book code
// (must match generate_dict_xml.py: a-z -> "A".."Z", digit -> "NUM", else "SYM";
// German umlauts fold to their base letter, matching the generator's bucketing).
static void dictBucketCode(char first, char* out) {
    unsigned char c = (unsigned char)first;
    char base = 0;
    if      (c >= 'A' && c <= 'Z') base = (char)(c - 'A' + 'a');
    else if (c >= 'a' && c <= 'z') base = (char)c;
    else if (c == 0x80 || c == 0x81) base = 'a';   // Ä ä
    else if (c == 0x82 || c == 0x83) base = 'o';   // Ö ö
    else if (c == 0x84 || c == 0x85) base = 'u';   // Ü ü
    else if (c == 0x86)              base = 's';    // ß
    if (base >= 'a' && base <= 'z') { out[0] = (char)(base - 'a' + 'A'); out[1] = 0; return; }
    if (c >= '0' && c <= '9') { strcpy(out, "NUM"); return; }
    strcpy(out, "SYM");
}

// Last loadToc() failure reason (shown on screen by selectTranslation).
static const char* g_toc_err = "";

void BibleInterface::freeRuntime() {
    if (rt_books)     { free(rt_books);     rt_books     = nullptr; }
    if (rt_secs)      { free(rt_secs);      rt_secs      = nullptr; }
    if (book_offsets) { free(book_offsets); book_offsets = nullptr; }
    if (rt_pages)     { free(rt_pages);     rt_pages     = nullptr; }
    rt_book_count = 0; rt_sec_count = 0; book_offsets_cap = 0;
    rt_page_count = 0; rt_pages_book = 0xFFFF;
}

// Dictionary: load the "firstword - lastword" labels for `book`'s pages from the
// generated <stem>.pgx. Falls back to "Page N" labels if the file is absent.
const char* BibleInterface::pageLabel(uint16_t i) const {
    if (rt_pages && i < rt_page_count) return rt_pages + (size_t)i * DICT_PAGE_LABEL_LEN;
    return "";
}

bool BibleInterface::loadDictPages(uint16_t book) {
    if (rt_pages) { free(rt_pages); rt_pages = nullptr; }
    rt_page_count = 0;
    rt_pages_book = 0xFFFF;
    if (book >= numBooks()) return false;
    uint16_t npages = bookChapters(book);
    if (npages == 0) npages = 1;

    rt_pages = (char*)RT_MALLOC((size_t)npages * DICT_PAGE_LABEL_LEN);
    if (!rt_pages) return false;
    rt_page_count = npages;
    rt_pages_book = book;
    for (uint16_t p = 0; p < npages; p++)              // default labels
        snprintf(rt_pages + (size_t)p * DICT_PAGE_LABEL_LEN, DICT_PAGE_LABEL_LEN,
                 "Page %d", p + 1);

    char path[80];
    snprintf(path, sizeof(path), "%s/%s.pgx", basePath(), trans_stems[cur_trans]);
    File f = SD.open(path);
    if (!f) return true;                              // fall back to "Page N"

    const char* code = bookCode(book);
    size_t clen = strlen(code);
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        // Match "<code>|..." exactly (code is a single letter / NUM / SYM).
        if ((size_t)line.length() < clen + 1) continue;
        if (strncmp(line.c_str(), code, clen) != 0 || line[clen] != '|') continue;
        int p1 = clen;                                // the '|' after code
        int p2 = line.indexOf('|', p1 + 1);
        int p3 = (p2 >= 0) ? line.indexOf('|', p2 + 1) : -1;
        if (p2 < 0 || p3 < 0) continue;
        uint16_t pg = (uint16_t)line.substring(p1 + 1, p2).toInt();
        if (pg < 1 || pg > npages) continue;
        String first = line.substring(p2 + 1, p3);
        String last  = line.substring(p3 + 1);
        char* dst = rt_pages + (size_t)(pg - 1) * DICT_PAGE_LABEL_LEN;
        snprintf(dst, DICT_PAGE_LABEL_LEN, "%s - %s", first.c_str(), last.c_str());
        utf8Encode(dst);                              // render any umlauts
    }
    f.close();
    return true;
}

// Load <base>/<stem>.toc into rt_books[], rt_secs[] and book_offsets[].
//   S|<section display name>
//   B|<code>|<display>|<chapterCount>|<sectionIndex>|<byteOffset>
// Books must be grouped by section and contiguous (the generator guarantees this).
bool BibleInterface::loadToc(const char* stem) {
    freeRuntime();
    g_read_fraktur = false;   // default; a "F|fraktur" line turns it on for this file
    char path[80];
    snprintf(path, sizeof(path), "%s/%s.toc", basePath(), stem);

    // Open ONCE and read the whole file into a buffer, then parse from memory.
    // (Avoids reopening the same SD file twice, which is unreliable on some builds.)
    g_toc_err = "";
    File f = SD.open(path);
    if (!f) { g_toc_err = "file not found"; Serial.printf("[%s] TOC missing: %s\n", nvsNamespace(), path); return false; }
    if (f.isDirectory()) { f.close(); g_toc_err = "is a directory"; return false; }
    size_t sz = f.size();
    if (sz == 0) { f.close(); g_toc_err = "file is empty"; Serial.printf("[%s] TOC empty: %s\n", nvsNamespace(), path); return false; }
    char* buf = (char*)RT_MALLOC(sz + 1);
    if (!buf) { f.close(); g_toc_err = "low memory (enable PSRAM)"; Serial.println(F("[toc] buf alloc fail")); return false; }
    size_t got = f.read((uint8_t*)buf, sz);
    f.close();
    buf[got] = 0;

    // Pass 1: count S| and B| lines.
    uint16_t nsec = 0, nbook = 0;
    for (char* ln = buf; *ln; ) {
        char* nl = strchr(ln, '\n');
        size_t len = nl ? (size_t)(nl - ln) : strlen(ln);
        if (len >= 2 && ln[1] == '|') {
            if      (ln[0] == 'S') nsec++;
            else if (ln[0] == 'B') nbook++;
        }
        if (!nl) break;
        ln = nl + 1;
    }
    if (nbook == 0) { free(buf); g_toc_err = "no B| rows (bad TOC)"; return false; }

    rt_secs      = (RtSec*)   RT_MALLOC(sizeof(RtSec)   * (nsec ? nsec : 1));
    rt_books     = (RtBook*)  RT_MALLOC(sizeof(RtBook)  * nbook);
    book_offsets = (uint32_t*)RT_MALLOC(sizeof(uint32_t)* nbook);
    if (!rt_secs || !rt_books || !book_offsets) { free(buf); freeRuntime(); g_toc_err = "low memory (enable PSRAM)"; return false; }
    book_offsets_cap = nbook;

    // Pass 2: fill (parse each line in place; field separator is '|').
    uint16_t si = 0, bi = 0;
    for (char* ln = buf; *ln; ) {
        char* nl = strchr(ln, '\n');
        if (nl) *nl = 0;                          // terminate this line
        // strip a trailing '\r' (in case the file has CRLF endings)
        size_t len = strlen(ln);
        while (len && (ln[len - 1] == '\r' || ln[len - 1] == ' ')) ln[--len] = 0;

        if (len >= 2 && ln[1] == '|') {
            if (ln[0] == 'F') {                 // F|fraktur → use the Fraktur reading font
                g_read_fraktur = (strcmp(ln + 2, "fraktur") == 0);
            } else if (ln[0] == 'S' && si < nsec) {
                strncpy(rt_secs[si].name, ln + 2, RT_SEC_NAME_LEN - 1);
                rt_secs[si].name[RT_SEC_NAME_LEN - 1] = 0;
                utf8Encode(rt_secs[si].name);   // UTF-8 umlauts → private codes for rendering
                rt_secs[si].start = 0; rt_secs[si].len = 0;
                si++;
            } else if (ln[0] == 'B' && bi < nbook) {
                // B|<code>|<display>|<chapters>|<sectionIndex>|<byteOffset>
                char* f1 = strchr(ln + 2, '|');
                char* f2 = f1 ? strchr(f1 + 1, '|') : nullptr;
                char* f3 = f2 ? strchr(f2 + 1, '|') : nullptr;
                char* f4 = f3 ? strchr(f3 + 1, '|') : nullptr;
                if (f1 && f2 && f3 && f4) {
                    *f1 = *f2 = *f3 = *f4 = 0;       // split into fields
                    strncpy(rt_books[bi].code, ln + 2, RT_CODE_LEN - 1);
                    rt_books[bi].code[RT_CODE_LEN - 1] = 0;
                    strncpy(rt_books[bi].display, f1 + 1, RT_DISP_LEN - 1);
                    rt_books[bi].display[RT_DISP_LEN - 1] = 0;
                    utf8Encode(rt_books[bi].display);  // UTF-8 umlauts → private codes
                    uint16_t chaps = (uint16_t)atoi(f2 + 1);
                    rt_books[bi].chapters = chaps ? chaps : 1;
                    rt_books[bi].section  = (uint16_t)atoi(f3 + 1);
                    book_offsets[bi]      = (uint32_t)strtoul(f4 + 1, nullptr, 10);
                    bi++;
                }
            }
        }
        if (!nl) break;
        ln = nl + 1;
    }
    free(buf);
    rt_book_count = bi;
    rt_sec_count  = si;

    // Derive section start/len from the (contiguous, section-grouped) book list.
    for (uint16_t s = 0; s < rt_sec_count; s++) { rt_secs[s].start = 0; rt_secs[s].len = 0; }
    for (uint16_t b = 0; b < rt_book_count; b++) {
        uint16_t s = rt_books[b].section;
        if (s >= rt_sec_count) continue;
        if (rt_secs[s].len == 0) rt_secs[s].start = b;
        rt_secs[s].len++;
    }
    book_idx_valid = true;   // offsets came from the TOC; no .idx scan needed
    book_idx_trans = cur_trans;
    Serial.printf("[%s] TOC %s: %u sections, %u books\n",
                  nvsNamespace(), stem, rt_sec_count, rt_book_count);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main menu (root) — pick Bible / Songs / Dictionary
// ─────────────────────────────────────────────────────────────────────────────
static const char* const MENU_LABELS[3] = { "Bible", "Songs", "Dictionary" };

void BibleInterface::goToMainMenu() {
    stopFling();
    // Restore the menu's own appearance/namespace (board-global brightness lives here).
    prefs.end();
    prefs.begin("menu", false);
    mode       = MODE_BIBLE;            // accessors unused at the menu
    theme_idx  = prefs.getUChar("theme", 0);
    if (theme_idx >= THEME_COUNT) theme_idx = 0;
    dark_mode  = THEMES[theme_idx].dark;
    accent_idx = prefs.getUChar("accent", 0);
    if (accent_idx >= ACCENT_COUNT) accent_idx = 0;
    accent_def = (prefs.getUChar("accentdef", 0) != 0);
    font_num   = 3;   // default reading size = Medium (VLW index)
    loadMenuChrome();
    font_color_idx = menu_font_color_idx;   // chrome/menu text follows the menu Font Color
    if (font_color_idx >= FONT_COLOR_COUNT) font_color_idx = 0;
    // Keep the current backlight level (avoids a brightness flash when returning
    // from a mode); each mode still applies its own brightness on entry.
    freeRuntime();
    cached_book = 0xFFFF; cached_chap = 0; cached_count = 0;
    book_idx_valid = false;
    view = BV_MAIN_MENU;
    menu_sel = 0; menu_scroll = 0; scroll_px = 0.f;
    needs_redraw = true;
}

void BibleInterface::drawMainMenu() {
    tft.fillScreen(bg());
    drawHeader("ESP-32 Library", false);

    // All buttons use the menu theme's button background — the same colour the
    // Settings button and every header/nav button use — so the menu reads as one
    // consistent themed surface (driven by the theme set for the Main Menu scope).
    uint16_t btn_bg = hdr_bg();

    const int16_t margin = 16;
    const int16_t gap    = 14;
    const int16_t sh     = 34;                 // small Settings button height
    int16_t top   = (int16_t)contentY() + 12;
    int16_t avail = (int16_t)scrH() - top - 12;
    int16_t bh    = (avail - sh - gap * 3) / 3;
    setUiFont(4);                              // larger smooth font for the mode buttons
    int16_t lblH = (int16_t)VLW_FONTS[4].lineH;
    for (uint8_t i = 0; i < 3; i++) {
        int16_t y = top + i * (bh + gap);
        tft.fillRoundRect(margin, y, scrW() - 2 * margin, bh, 12, btn_bg);
        tft.drawRoundRect(margin, y, scrW() - 2 * margin, bh, 12, edgeColor(i * 3, dim_fg()));
        tft.setTextColor(chromeFg(), btn_bg);
        tft.drawCentreString(MENU_LABELS[i], scrW() / 2, y + (bh - lblH) / 2, 4);
    }
    setUiFont(2);                              // restore the normal UI size
    // Smaller Settings button under Dictionary.
    int16_t sy = top + 3 * (bh + gap);
    int16_t sw = (int16_t)((scrW() - 2 * margin) * 7 / 10);
    int16_t sx = ((int16_t)scrW() - sw) / 2;
    int16_t uiH = (int16_t)VLW_FONTS[2].lineH;
    tft.fillRoundRect(sx, sy, sw, sh, 10, hdr_bg());
    tft.drawRoundRect(sx, sy, sw, sh, 10, edgeColor(9, dim_fg()));
    tft.setTextColor(chromeFg(), hdr_bg());
    tft.drawCentreString("Settings", scrW() / 2, sy + (sh - uiH) / 2, 2);
}

void BibleInterface::handleMainMenuInput() {
    uint16_t tx, ty;
    bool down = pollTouch(&tx, &ty);
    if (down && !touch_was_down) {
        touch_was_down = true;
        touch_down_x = tx; touch_down_y = ty;
        return;
    }
    if (!down && touch_was_down) {
        touch_was_down = false;
        // Easter egg: tap the header to show the boot splash again until tapped.
        if ((int16_t)touch_down_y < (int16_t)hdrH()) { showSplashUntilTap(); return; }
        const int16_t margin = 16;
        const int16_t gap    = 14;
        const int16_t sh     = 34;
        int16_t top   = (int16_t)contentY() + 12;
        int16_t avail = (int16_t)scrH() - top - 12;
        int16_t bh    = (avail - sh - gap * 3) / 3;
        for (uint8_t i = 0; i < 3; i++) {
            int16_t y = top + i * (bh + gap);
            if ((int16_t)touch_down_y >= y && (int16_t)touch_down_y < y + bh &&
                (int16_t)touch_down_x >= margin && (int16_t)touch_down_x < (int16_t)scrW() - margin) {
                enterMode((ContentMode)i);
                return;
            }
        }
        // Settings button (smaller, under Dictionary).
        int16_t sy = top + 3 * (bh + gap);
        int16_t sw = (int16_t)((scrW() - 2 * margin) * 7 / 10);
        int16_t sx = ((int16_t)scrW() - sw) / 2;
        if ((int16_t)touch_down_y >= sy && (int16_t)touch_down_y < sy + sh &&
            (int16_t)touch_down_x >= sx && (int16_t)touch_down_x < sx + sw) {
            goToSettings(true);   // from main menu
            return;
        }
        // Tap on empty menu background: do nothing (no flash / repaint).
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Mode switching
// ─────────────────────────────────────────────────────────────────────────────
void BibleInterface::enterMode(ContentMode m) {
    prefs.end();
    mode = m;
    freeRuntime();
    cached_book = 0xFFFF; cached_chap = 0; cached_count = 0;
    book_idx_valid = false;

    prefs.begin(nvsNamespace(), false);
    loadState();                          // per-mode font/dark/accent/position/search
    // Brightness is global (set once at boot in blInit) — no per-mode re-apply.
    loadBookmarks();
    loadSearchHistory();
    scanTranslations();

    if (trans_count == 0) {
        tft.fillScreen(bg());
        drawHeader("ESP-32 Library", false);
        tft.setTextColor(TFT_RED, bg());
        char msg[48];
        snprintf(msg, sizeof(msg), "No %s files on SD", MENU_LABELS[m]);
        tft.drawCentreString(msg, scrW() / 2, scrH() / 2 - 18, 2);
        char where[48];
        snprintf(where, sizeof(where), "Copy .xml to %s/", basePath());
        tft.setTextColor(dim_fg(), bg());
        tft.drawCentreString(where, scrW() / 2, scrH() / 2 + 6, 2);
        tft.drawCentreString("Tap to go back", scrW() / 2, scrH() / 2 + 30, 2);
        // Stay on the menu state so a tap returns; do not auto-redraw over the message.
        mode = MODE_BIBLE;
        view = BV_MAIN_MENU;
        needs_redraw = false;
        return;
    }
    if (cur_trans >= trans_count) cur_trans = 0;

    // Multiple translations → show the picker; otherwise open the single one.
    if (trans_count > 1) {
        if (mode != MODE_BIBLE) loadToc(trans_stems[cur_trans]);  // so cur_book clamps correctly
        goToTransSelect();
    } else {
        selectTranslation(cur_trans);
    }
}

void BibleInterface::selectTranslation(uint16_t idx) {
    if (idx >= trans_count) idx = 0;
    cur_trans = (uint8_t)idx;
    cached_book = 0xFFFF; cached_chap = 0; cached_count = 0;
    book_idx_valid = false;

    if (mode != MODE_BIBLE) {
        if (!loadToc(trans_stems[cur_trans])) {
            // Structure file missing/unreadable — tell the user which file, then
            // wait for a tap (rather than silently bouncing to the menu).
            tft.fillScreen(bg());
            drawHeader("ESP-32 Library", false);
            tft.setTextColor(TFT_RED, bg());
            char msg[72];
            snprintf(msg, sizeof(msg), "%s/%s.toc", basePath(), trans_stems[cur_trans]);
            tft.drawCentreString(msg, scrW() / 2, scrH() / 2 - 24, 2);
            tft.setTextColor(dim_fg(), bg());
            char reason[64];
            snprintf(reason, sizeof(reason), "TOC error: %s", g_toc_err[0] ? g_toc_err : "unknown");
            tft.drawCentreString(reason, scrW() / 2, scrH() / 2 - 2, 2);
            tft.drawCentreString("Copy the .toc next to the .xml", scrW() / 2, scrH() / 2 + 20, 2);
            tft.drawCentreString("Tap to go back", scrW() / 2, scrH() / 2 + 42, 2);
            mode = MODE_BIBLE;
            view = BV_MAIN_MENU;
            needs_redraw = false;
            return;
        }
        if (cur_book >= numBooks()) cur_book = 0;
        cur_sec = (numBooks() > 0) ? bookSection(cur_book) : 0;
        if (cur_chapter == 0 || cur_chapter > bookChapters(cur_book)) cur_chapter = 1;
    } else {
        // Bible: ensure the byte-offset buffer exists (filled lazily from the .idx
        // file inside cacheChapter). loadToc() handles this for Songs/Dict.
        g_read_fraktur = false;   // Bible always uses the normal reading font
        if (!book_offsets || book_offsets_cap < BIBLE_BOOK_COUNT) {
            if (book_offsets) free(book_offsets);
            book_offsets     = (uint32_t*)RT_MALLOC(sizeof(uint32_t) * BIBLE_BOOK_COUNT);
            book_offsets_cap = book_offsets ? BIBLE_BOOK_COUNT : 0;
        }
        if (book_offsets) memset(book_offsets, 0, sizeof(uint32_t) * BIBLE_BOOK_COUNT);
        if (cur_book >= numBooks()) cur_book = 0;
        cur_sec = bookSection(cur_book);
    }
    saveState();
    goToSection();
}

void BibleInterface::goBack() {
    stopFling();
    switch (view) {
        case BV_MAIN_MENU:       /* already root */ break;
        case BV_TRANS_SELECT:    goToMainMenu(); break;
        case BV_SECTION_SELECT:
            if (trans_count > 1) goToTransSelect();
            else                 goToMainMenu();
            break;
        case BV_BOOK_SELECT:
            if      (numSecs() > 1)    goToSection();
            else if (trans_count > 1)  goToTransSelect();
            else                       goToMainMenu();
            break;
        case BV_CHAPTER_SELECT:  goToBook(bookSection(cur_book)); break;
        case BV_READING:
            if (reading_from_search) {
                reading_from_search = false;
                highlight_verse     = 0;
                view      = BV_SEARCH_RESULTS;
                scroll_px = (float)menu_scroll * (float)srchH();
                needs_redraw = true;
            } else if (bookChapters(cur_book) <= 1) {
                // Single-chapter book (Song): chapter grid is skipped, so go to
                // the book (song) list rather than back into the reader.
                goToBook(bookSection(cur_book));
            } else {
                goToChapter(cur_book);
            }
            break;
        case BV_SETTINGS:
            // Restore the active context's look (settings may have previewed another scope).
            settings_scope = settings_from_menu ? 1 : (uint8_t)(mode + 2);
            loadScopeSettings();
            if (settings_from_menu) { goToMainMenu(); break; }
            if (cached_count > 0) {
                buildWrappedLines();   // re-wrap in case Font Size changed
                view = BV_READING;
                scroll_px = (float)read_line * (float)lineH();
                needs_redraw = true;
            } else {
                goToSection();
            }
            break;
        case BV_BOOKMARKS:
            bm_confirm_pending = false;
            if (cached_count > 0) {
                view = BV_READING;
                scroll_px = (float)read_line * (float)lineH();
                needs_redraw = true;
            } else {
                goToSection();
            }
            break;
        case BV_SEARCH_RESULTS:
            goToSearchInput();
            break;
        case BV_SEARCH_INPUT:
            search_del_pending = false;
            if (cached_count > 0) {
                view = BV_READING;
                scroll_px = (float)read_line * (float)lineH();
                needs_redraw = true;
            } else {
                goToSection();
            }
            break;
    }
}
void BibleInterface::addBookmarkCurrent() {
    BibleBookmark bm;
    bm.book        = cur_book;
    bm.chapter     = cur_chapter;
    bm.verse_first = sel_verse_first;
    bm.verse_last  = sel_verse_last;
    bm.trans       = cur_trans;   // used by Songs/Dict (book index is per-file)

    if (sel_verse_first > 0) {
        // Verse / range bookmark. Songs are single-chapter, so drop the chapter
        // number (e.g. "Title  v3" instead of "Title 1:3").
        if (mode == MODE_DICT) {
            // Label with the entry's headword (text before " - ").
            char w[40]; w[0] = 0;
            if (cached_book == cur_book && cached_chap == cur_chapter &&
                sel_verse_first >= 1 && sel_verse_first <= cached_count) {
                const char* s    = verse_buf[sel_verse_first - 1];
                const char* dash = strstr(s, " - ");
                size_t len = dash ? (size_t)(dash - s) : strlen(s);
                if (len > sizeof(w) - 1) len = sizeof(w) - 1;
                memcpy(w, s, len); w[len] = 0;
            }
            if (w[0]) snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s", w);
            else      snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s p%d:%d",
                               bookDisplay(cur_book), cur_chapter, sel_verse_first);
        } else if (mode == MODE_SONGS) {
            if (sel_verse_first == sel_verse_last)
                snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s  v%d",
                         bookDisplay(cur_book), sel_verse_first);
            else
                snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s  v%d-%d",
                         bookDisplay(cur_book), sel_verse_first, sel_verse_last);
        } else if (sel_verse_first == sel_verse_last)
            snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s %d:%d",
                     bookDisplay(cur_book), cur_chapter, sel_verse_first);
        else
            snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s %d:%d-%d",
                     bookDisplay(cur_book), cur_chapter,
                     sel_verse_first, sel_verse_last);
        // Reject exact duplicates (same book/chapter/verse range)
        for (uint8_t i = 0; i < bm_count; i++) {
            if (bookmarks[i].book == cur_book && bookmarks[i].chapter == cur_chapter
                    && bookmarks[i].verse_first == sel_verse_first
                    && bookmarks[i].verse_last  == sel_verse_last
                    && (mode == MODE_BIBLE || bookmarks[i].trans == cur_trans)) {
                tft.fillRect(scrW()/2, scrH() - navH() - 18, scrW()/2, 16, (uint16_t)0xFD20);
                tft.setTextColor(TFT_BLACK, (uint16_t)0xFD20);
                tft.drawCentreString("Already saved", 3*scrW()/4, scrH() - navH() - 18, 1);
                delay(600);
                needs_redraw = true;
                return;
            }
        }
    } else {
        // Whole-chapter bookmark (songs: just the title; dict: the page word pair).
        if (mode == MODE_SONGS) {
            snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s", bookDisplay(cur_book));
        } else if (mode == MODE_DICT) {
            const char* pl = pageLabel(cur_chapter - 1);
            if (pl && pl[0]) snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s", pl);
            else             snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s p%d",
                                      bookDisplay(cur_book), cur_chapter);
        } else {
            snprintf(bm.label, BIBLE_BM_LABEL_LEN, "%s %d",
                     bookDisplay(cur_book), cur_chapter);
        }
        for (uint8_t i = 0; i < bm_count; i++) {
            if (bookmarks[i].book == cur_book && bookmarks[i].chapter == cur_chapter
                    && bookmarks[i].verse_first == 0
                    && (mode == MODE_BIBLE || bookmarks[i].trans == cur_trans)) {
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
    // Songs/Dict bookmarks are translation-scoped — switch file and reload its
    // structure if the bookmark belongs to a different translation than the one open.
    if (mode != MODE_BIBLE && bookmarks[bm_idx].trans != cur_trans &&
        bookmarks[bm_idx].trans < trans_count) {
        cur_trans = bookmarks[bm_idx].trans;
        if (!loadToc(trans_stems[cur_trans])) { goToMainMenu(); return; }
    }
    cur_book    = bookmarks[bm_idx].book;
    if (cur_book >= numBooks()) { goToSection(); return; }
    cur_chapter = bookmarks[bm_idx].chapter;
    cur_sec     = bookSection(cur_book);
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
            max_px = (float)max(0, (int)secLen(cur_sec) - (int)visItems()) * (float)itemH();
            break;
        case BV_CHAPTER_SELECT: {
            uint16_t  chaps      = bookChapters(cur_book);
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
        case BV_SETTINGS:
            max_px = (float)max(0, (int)set_row_n - (int)visItems()) * (float)itemH();
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
                view == BV_SECTION_SELECT ? numSecs() :
                                            secLen(cur_sec));
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
        case BV_SETTINGS:
            menu_scroll = (int16_t)(scroll_px / (float)itemH());
            redrawSettingsContent();
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

// Compress multi-byte UTF-8 to private single-byte codes the VLW fonts render:
//   umlauts/ß  0x80=Ä 0x81=ä 0x82=Ö 0x83=ö 0x84=Ü 0x85=ü 0x86=ß
//   typographic 0x87=„ 0x88=" 0x89=‚ 0x8A=' 0x8B=' 0x8C=– 0x8D=— 0x8E=…
// Other unrecognised multi-byte sequences: lead byte kept, trail dropped.
// Uses read/write pointers so no additional buffer is needed.
void BibleInterface::utf8Encode(char* buf) {
    char* r = buf;
    char* w = buf;
    while (*r) {
        uint8_t b  = (uint8_t)*r;
        uint8_t b1 = (b >= 0x80) ? (uint8_t)r[1] : 0;   // 0 if string ends early
        uint8_t b2 = (b >= 0xE0 && b1) ? (uint8_t)r[2] : 0;

        if (b < 0x80) { *w++ = *r++; continue; }         // plain ASCII

        // ── 2-byte: Latin-1 supplement (0xC2/0xC3) ────────────────────────────
        if (b == 0xC3 && b1) {
            switch (b1) {
                case 0x84: *w++ = (char)0x80; break;     // Ä
                case 0xA4: *w++ = (char)0x81; break;     // ä
                case 0x96: *w++ = (char)0x82; break;     // Ö
                case 0xB6: *w++ = (char)0x83; break;     // ö
                case 0x9C: *w++ = (char)0x84; break;     // Ü
                case 0xBC: *w++ = (char)0x85; break;     // ü
                case 0x9F: *w++ = (char)0x86; break;     // ß
                default:   /* other accented Latin — drop */ break;
            }
            r += 2; continue;
        }
        if (b == 0xC2 && b1) {
            if      (b1 == 0xA0) *w++ = ' ';             // non-breaking space
            else if (b1 == 0xB4) *w++ = '\'';            // ´ acute accent
            /* else (©, °, …) drop */
            r += 2; continue;
        }

        // ── 3-byte: typographic marks (0xE2 …) → private codes 0x87-0x8E so they
        //    render literally in the font (variants fold onto the baked glyphs). ──
        if (b == 0xE2 && b1 && b2) {
            if (b1 == 0x80) {
                switch (b2) {
                    case 0x9E:            *w++ = (char)0x87; break;  // „ low double
                    case 0x9C: case 0x9D: case 0x9F:
                                          *w++ = (char)0x88; break;  // “ ” ‟ → " high double
                    case 0x9A:            *w++ = (char)0x89; break;  // ‚ low single
                    case 0x98:            *w++ = (char)0x8A; break;  // ‘ left single
                    case 0x99: case 0x9B: *w++ = (char)0x8B; break;  // ’ ‛ → ' right single
                    case 0x90: case 0x91: case 0x93:
                                          *w++ = (char)0x8C; break;  // ‐ ‑ – → en dash
                    case 0x94: case 0x95: *w++ = (char)0x8D; break;  // — ― → em dash
                    case 0xA6:            *w++ = (char)0x8E; break;  // … ellipsis
                    case 0xA2: *w++ = '*';  break;                   // • bullet (no glyph)
                    case 0xAF: *w++ = ' ';  break;                   // narrow nbsp
                    default: break;                                  // zwsp etc. drop
                }
            } else if (b1 == 0x88 && b2 == 0x92) {
                *w++ = '-';                                          // − minus sign
            }
            r += 3; continue;
        }

        // ── Any other multibyte sequence: drop it whole (never leak a byte) ────
        if (b >= 0xF0)      r += (r[1] && r[2] && r[3]) ? 4 : 1;  // 4-byte
        else if (b >= 0xE0) r += (b1 && b2) ? 3 : 1;             // 3-byte
        else if (b >= 0xC0) r += b1 ? 2 : 1;                     // 2-byte
        else                r += 1;                              // stray continuation
    }
    *w = 0;
}

// Measure pixel width of a private-code string in the loaded UI VLW font.
int16_t BibleInterface::textWidthUTF8(const char* str, uint8_t /*font*/) {
    return vlwTextWidth(g_ui_vlw, str);
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

bool BibleInterface::cacheChapter(uint16_t book, uint16_t chapter) {
    if (cached_book == book && cached_chap == chapter) return (cached_count > 0);
    cached_count = 0;
    cached_book  = book;
    cached_chap  = chapter;

    if (trans_count == 0 || book >= numBooks()) return false;

    // ── Book byte-offset index ─────────────────────────────────────────────
    // Bible: load (or build+save) the .idx so we can seek directly to a book.
    // Songs/Dict: book_offsets[] is already filled by loadToc() (no .idx scan).
    if (mode == MODE_BIBLE) {
        // Defensive: guarantee the offset buffer exists and is large enough before
        // any index load/build/seek. A prior Songs/Dict session can free or resize
        // it, and the original alloc in selectTranslation can fail under memory
        // pressure — either left book_offsets NULL/undersized, and loadBookIndex/
        // buildBookIndex/seek then wrote through it and crashed intermittently.
        if (!book_offsets || book_offsets_cap < BIBLE_BOOK_COUNT) {
            if (book_offsets) free(book_offsets);
            book_offsets     = (uint32_t*)RT_MALLOC(sizeof(uint32_t) * BIBLE_BOOK_COUNT);
            book_offsets_cap = book_offsets ? BIBLE_BOOK_COUNT : 0;
            if (book_offsets) memset(book_offsets, 0, sizeof(uint32_t) * BIBLE_BOOK_COUNT);
            book_idx_valid = false;   // must (re)load into the fresh buffer
        }
        if (!book_offsets) return false;   // out of memory — fail, don't crash

        if (!book_idx_valid || book_idx_trans != cur_trans) {
            if (!loadBookIndex(trans_stems[cur_trans])) {
                buildBookIndex(trans_stems[cur_trans]);
                saveBookIndex(trans_stems[cur_trans]);
            }
            book_idx_valid = true;
            book_idx_trans = cur_trans;
        }
    }

    // Build file path: /bible/<stem>.xml
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.xml", basePath(), trans_stems[cur_trans]);

    File f = SD.open(path);
    if (!f) {
        Serial.print(F("[Bible] Cannot open ")); Serial.println(path);
        return false;
    }

    // Seek to the book's starting byte — skips all preceding books.
    // (Bounds-checked: a missing/short index just means scan from offset 0.)
    if (book_offsets && book < book_offsets_cap && book_offsets[book] > 0)
        f.seek(book_offsets[book]);

    // Build the osisID prefix we are looking for: "Book.Chapter."
    // e.g. "Gen.1." for Genesis chapter 1
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%s.%d.", bookCode(book), chapter);
    size_t prefix_len = strlen(prefix);

    // Build the osisID prefix for the NEXT chapter so we know when to stop
    char next_prefix[32];
    snprintf(next_prefix, sizeof(next_prefix), "%s.%d.", bookCode(book), chapter + 1);

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
                            } else if (cached_count > 0) {
                                // A verse from a different book/chapter appeared after
                                // we already collected ours — we're done. (Verses are
                                // contiguous, so this is the safe stop for the last
                                // chapter of a book and for single-chapter songs.)
                                done = true;
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
                  cached_count, bookDisplay(book), chapter);
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
        // Songs: blank line between stanzas for readability (not after the last).
        if (mode == MODE_SONGS && v + 1 < cached_count && line_count < BIBLE_MAX_LINES) {
            lines[line_count][0] = 0;
            line_count++;
        }
    }
}

void BibleInterface::addWrappedLine(uint8_t verse_num, const char* text,
                                     uint16_t max_px, uint8_t fnt, uint16_t& idx) {
    const char* p            = text;
    bool        is_first_line = true;   // true only for the verse's first screen line
    const uint8_t* vfont     = vlwForSize(fnt);   // reading uses the smooth VLW font

    // For the verse-first line the verse number prefix is drawn to the left of the
    // content text, so the available content width is narrowed by its pixel width.
    uint16_t first_max_px = max_px;
    if (verse_num > 0) {
        char num_str[12];
        snprintf(num_str, sizeof(num_str), "%d.", verse_num);
        int16_t num_w = vlwTextWidth(vfont, num_str) + 2;  // +2px gap
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

        // Fill the line word by word.  A '\n' in the text forces a line break
        // (used by Songs so stanza line structure is preserved).
        bool forced_nl = false;
        char tmp[BIBLE_LINE_BUF];
        while (*p) {
            // Advance to end of next word (stop at space or newline)
            const char* wp = p;
            while (*wp && *wp != ' ' && *wp != '\n') wp++;
            // Include trailing space if present (newline is not stored)
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

            int16_t px = vlwTextWidth(vfont, measure);
            if (px > (int16_t)line_max && out_len > 0) break; // word doesn't fit

            memcpy(out, tmp, tmp_len + 1);
            out_len = tmp_len;
            p = wp;
            if (*p == ' ') { p++; }
            else if (*p == '\n') { p++; forced_nl = true; break; }  // explicit break
        }

        // Nothing fit on this line (single word wider than line_max).
        // Detect: continuation line with nothing added (out_len==0), OR verse-first
        // line with only the "^N|" prefix and no content added.
        // Skipped after a forced newline break (an intentionally short/blank line).
        int prefix_len = (out[0] == '^') ? (int)(strchr(out, '|') - out + 1) : 0;
        if (!forced_nl && out_len <= prefix_len) {
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
                if (vlwTextWidth(vfont, measure2) > (int16_t)line_max) {
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
    prefs.putUShort("book",     cur_book);
    prefs.putUShort("chap",     cur_chapter);
    prefs.putUChar("trans",     cur_trans);
    prefs.putUChar("font",      font_num);
    prefs.putUChar("fontcol",   font_color_idx);
    prefs.putUChar("vnumcol",   vnum_color_idx);
    prefs.putUChar("theme",     theme_idx);
    prefs.putUChar("accentdef", accent_def ? 1 : 0);
    prefs.putBool ("dark",      dark_mode);   // kept for backward compatibility
    prefs.putUChar("accent",    accent_idx);
    prefs.putBool ("srch_part", srch_partial_match);
    prefs.putBool ("srch_pnct", srch_ignore_punct);
    prefs.putUChar("srch_scp",  srch_scope);
}

void BibleInterface::loadState() {
    cur_book          = prefs.getUShort("book",     0);
    cur_chapter       = prefs.getUShort("chap",     1);
    cur_trans         = prefs.getUChar("trans",     0);
    font_num          = prefs.getUChar("font",      3);
    font_color_idx    = prefs.getUChar("fontcol",   0);
    if (font_color_idx >= FONT_COLOR_COUNT) font_color_idx = 0;
    vnum_color_idx    = prefs.getUChar("vnumcol",   0);
    if (vnum_color_idx >= FONT_COLOR_COUNT) vnum_color_idx = 0;
    // Theme: prefer "theme" index; fall back to the legacy "dark" bool (0=Dark,1=Light).
    theme_idx         = prefs.getUChar("theme", 0xFF);
    if (theme_idx == 0xFF) theme_idx = prefs.getBool("dark", true) ? 0 : 1;
    if (theme_idx >= THEME_COUNT) theme_idx = 0;
    dark_mode         = THEMES[theme_idx].dark;
    accent_idx        = prefs.getUChar("accent",    0);
    accent_def        = (prefs.getUChar("accentdef", 0) != 0);
    srch_partial_match = prefs.getBool ("srch_part", true);
    srch_ignore_punct  = prefs.getBool ("srch_pnct", true);
    srch_scope         = prefs.getUChar("srch_scp",  0);
    if (mode == MODE_BIBLE && cur_book >= BIBLE_BOOK_COUNT) cur_book = 0;
    if (cur_chapter == 0)               cur_chapter = 1;
    if (cur_trans  >= BIBLE_MAX_TRANS)  cur_trans  = 0;
    if (font_num >= VLW_FONT_COUNT) font_num = 3;   // VLW reading-size index; 3 = Medium
    if (accent_idx >= ACCENT_COUNT)     accent_idx = 0;
    if (srch_scope >= 3)                srch_scope  = 0;
    // Derive section from book so navigation back shows correct highlight.
    // For Songs/Dict the runtime table is not loaded yet here — selectTranslation()
    // recomputes cur_sec after loadToc().
    cur_sec = (mode == MODE_BIBLE) ? bookSection(cur_book) : 0;
}

void BibleInterface::saveBookmarks() {
    char path[64];
    bmPath(path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    // Bible:        "book chapter verse_first verse_last label"   (canon-global book)
    // Songs/Dict:   "T<trans> book chapter verse_first verse_last label"
    //   The leading "T<n>" token marks a translation-scoped bookmark.
    for (uint8_t i = 0; i < bm_count; i++) {
        if (mode == MODE_BIBLE) {
            f.printf("%d %d %d %d %s\n",
                     bookmarks[i].book, bookmarks[i].chapter,
                     bookmarks[i].verse_first, bookmarks[i].verse_last,
                     bookmarks[i].label);
        } else {
            f.printf("T%d %d %d %d %d %s\n",
                     bookmarks[i].trans, bookmarks[i].book, bookmarks[i].chapter,
                     bookmarks[i].verse_first, bookmarks[i].verse_last,
                     bookmarks[i].label);
        }
    }
    f.close();
}

void BibleInterface::loadBookmarks() {
    bm_count = 0;
    char path[64];
    bmPath(path, sizeof(path));
    File f = SD.open(path);
    if (!f) return;
    while (f.available() && bm_count < BIBLE_MAX_BM) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        // Songs/Dict lines start with a "T<trans>" token; strip and remember it.
        uint8_t bm_trans = 0;
        if (line[0] == 'T' && line.length() > 1 && isdigit((unsigned char)line[1])) {
            int sp = line.indexOf(' ');
            if (sp < 0) continue;
            bm_trans = (uint8_t)line.substring(1, sp).toInt();
            line = line.substring(sp + 1);
            line.trim();
        }

        int s1 = line.indexOf(' ');
        int s2 = (s1 >= 0) ? line.indexOf(' ', s1 + 1) : -1;
        if (s1 < 0 || s2 < 0) continue;
        uint16_t book = (uint16_t)line.substring(0, s1).toInt();
        uint16_t chap = (uint16_t)line.substring(s1 + 1, s2).toInt();
        if (chap == 0) continue;

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
        bookmarks[bm_count].trans       = bm_trans;
        strncpy(bookmarks[bm_count].label, label.c_str(), BIBLE_BM_LABEL_LEN - 1);
        bookmarks[bm_count].label[BIBLE_BM_LABEL_LEN - 1] = 0;
        bm_count++;
    }
    f.close();
}

void BibleInterface::transDisplayName(const char* base, const char* stem, char* out, size_t n) {
    if (n == 0) return;
    out[0] = 0;
    char path[80];
    snprintf(path, sizeof(path), "%s/%s.toc", base, stem);
    File f = SD.open(path);
    if (f && !f.isDirectory()) {
        char hdr[160];                       // the T| line is written first in the .toc
        int len = (int)f.read((uint8_t*)hdr, sizeof(hdr) - 1);
        f.close();
        if (len > 0) {
            hdr[len] = 0;
            for (char* ln = hdr; *ln; ) {
                char* nl = strchr(ln, '\n');
                if (nl) *nl = 0;
                size_t l = strlen(ln);
                while (l && (ln[l - 1] == '\r' || ln[l - 1] == ' ')) ln[--l] = 0;
                if (ln[0] == 'T' && ln[1] == '|') {
                    strncpy(out, ln + 2, n - 1);
                    out[n - 1] = 0;
                    utf8Encode(out);          // UTF-8 umlauts → private codes for rendering
                    return;
                }
                if (!nl) break;
                ln = nl + 1;
            }
        }
    } else if (f) {
        f.close();
    }
    // No .toc / no T| line (e.g. Bible): prettify the stem (ASCII, utf8Encode no-op).
    strncpy(out, prettyName(stem), n - 1);
    out[n - 1] = 0;
}

void BibleInterface::scanTranslations() {
    trans_count = 0;
    File root = SD.open(basePath());
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
    // Sort the list alphabetically so the picker (songbooks/dictionaries/Bibles)
    // is in a predictable order.
    for (uint8_t i = 0; i + 1 < trans_count; i++)
        for (uint8_t j = i + 1; j < trans_count; j++)
            if (strcmp(trans_stems[j], trans_stems[i]) < 0) {
                char tmp[BIBLE_TRANS_LEN];
                strncpy(tmp, trans_stems[i], BIBLE_TRANS_LEN);
                strncpy(trans_stems[i], trans_stems[j], BIBLE_TRANS_LEN);
                strncpy(trans_stems[j], tmp, BIBLE_TRANS_LEN);
            }
    // Cache each translation's display name (umlauts) for the menus.
    for (uint8_t i = 0; i < trans_count; i++)
        transDisplayName(basePath(), trans_stems[i], trans_names[i], BIBLE_TRANS_DISP_LEN);
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
    if (!book_offsets || book_offsets_cap < BIBLE_BOOK_COUNT) return false;
    char idx_path[64];
    snprintf(idx_path, sizeof(idx_path), "%s/%s.idx", basePath(), stem);
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
    snprintf(xml_path, sizeof(xml_path), "%s/%s.xml", basePath(), stem);
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
    if (!book_offsets || book_offsets_cap < BIBLE_BOOK_COUNT) return false;
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.xml", basePath(), stem);
    File f = SD.open(path);
    if (!f) return false;

    // Inform the user — this is a one-time scan that can take a few seconds
    tft.fillScreen(bg());
    tft.setTextColor(fg(), bg());
    tft.drawCentreString("Building index...", scrW() / 2, scrH() / 2 - 8, 2);
    tft.setTextColor(dim_fg(), bg());
    tft.drawCentreString("(first run only)", scrW() / 2, scrH() / 2 + 14, 2);

    if (book_offsets) memset(book_offsets, 0, (size_t)book_offsets_cap * sizeof(uint32_t));

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
                                    (int)strlen(bookCode(b)) == code_len &&
                                    strncmp(osis_id, bookCode(b), code_len) == 0) {
                                    book_offsets[b] = tag_start;
                                    found++;
                                    Serial.printf("[Bible] idx: %s @ %u\n",
                                                  bookCode(b), tag_start);
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
    snprintf(xml_path, sizeof(xml_path), "%s/%s.xml", basePath(), stem);
    File fx = SD.open(xml_path);
    if (!fx) return;
    uint32_t xml_size = (uint32_t)fx.size();
    fx.close();

    char idx_path[64];
    snprintf(idx_path, sizeof(idx_path), "%s/%s.idx", basePath(), stem);
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
    // Brightness is GLOBAL (one value for the whole firmware), stored in the "menu"
    // namespace regardless of which mode adjusts it.
    bl_idx = 10;
    { Preferences mp; if (mp.begin("menu", true)) { bl_idx = mp.getUChar("bright", 10); mp.end(); } }
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
    { Preferences mp; if (mp.begin("menu", false)) { mp.putUChar("bright", bl_idx); mp.end(); } }
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
    if (mode == MODE_DICT && trans_count > 0) {
        // Header shows the active dictionary; tapping it (centre) cycles to the next.
        String nm = trans_stems[cur_trans]; nm.toUpperCase();
        char title[40];
        snprintf(title, sizeof(title), "Search: %s", nm.c_str());
        drawHeader(title, true);
    } else {
        drawHeader("Search", true);
    }

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
    drawSmallCentered("Cancel", pop_x + 4 + half_w / 2, btn_y, btn_h, TFT_WHITE, hdr_bg());

    tft.fillRoundRect(del_x, btn_y, half_w, btn_h, 4, hdr_bg());
    tft.drawRoundRect(del_x, btn_y, half_w, btn_h, 4, TFT_RED);
    drawSmallCentered("Delete", del_x + half_w / 2, btn_y, btn_h, TFT_RED, hdr_bg());
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
    tft.drawFastHLine(0, y_px + row_h - 1, row_w, edgeColor(y_px / (int16_t)srchH(), dim_fg()));

    // Reference line in font 2 (16px). Rendered char-by-char so song titles with
    // umlauts (private codes) display correctly.
    {
        BibleSearchResult& r = search_results[idx];
        char ref[48];
        if (mode == MODE_DICT) {
            // Show the entry headword (text before " - " in the "word - definition" pair).
            const char* dash = strstr(r.snippet, " - ");
            size_t len = dash ? (size_t)(dash - r.snippet) : strlen(r.snippet);
            if (len > sizeof(ref) - 1) len = sizeof(ref) - 1;
            memcpy(ref, r.snippet, len); ref[len] = 0;
        } else if (mode == MODE_SONGS) {
            if (r.trans == cur_trans) {
                snprintf(ref, sizeof(ref), "%s", bookDisplay(r.book));  // song title
            } else {
                // "All" hit in another songbook — show the songbook name (uppercased).
                char nm[BIBLE_TRANS_LEN];
                strncpy(nm, (r.trans < trans_count) ? trans_stems[r.trans] : "?", sizeof(nm) - 1);
                nm[sizeof(nm) - 1] = 0;
                for (char* p = nm; *p; ++p) *p = (char)toupper((unsigned char)*p);
                snprintf(ref, sizeof(ref), "%s", nm);
            }
        } else {
            snprintf(ref, sizeof(ref), "%s %d:%d",
                     bookDisplay(r.book), (int)r.chapter, (int)r.verse);
        }
        tft.setTextColor(fg(), bg_col);
        int16_t rx = PADDING;
        for (const char* p = ref; *p && rx < row_w - 4; p++)
            rx += tftCharUTF8(tft, (uint8_t)*p, rx, y_px + 3, 2, fg());
    }

    // Snippet in font 1 (8px) with highlighted query segments.
    // disp/qdisp hold ASCII base letters (for case-insensitive match); snip holds
    // the original private codes so rendering shows proper umlauts/ß.
    // Private codes are 1-byte just like ASCII, so indices are identical between
    // snip and disp — we match on disp, render from snip.
    int16_t snip_y = y_px + 20;
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
    setUiFont(0);                          // Tiny for the snippet (one size smaller)
    const int16_t tiny_lh = (int16_t)VLW_FONTS[0].lineH;
    const size_t  slen    = strlen(disp);  // disp and snip share indices (1 byte each)

    // Pass 1: mark which snippet characters fall inside a matched token.
    bool hl[BIBLE_SRCH_SNIPPET_LEN];
    memset(hl, 0, sizeof(hl));
    for (size_t si = 0; si < slen; ) {
        bool   found     = false;
        size_t match_end = 0;
        for (int t = 0; t < ntokens && !found; t++) {
            const char* tok = tokens[t].s;
            size_t      tl  = tokens[t].len;
            if (tl == 0) continue;
            if (srch_ignore_punct) {
                size_t qi = 0, di = si;
                while (qi < tl) {
                    while (disp[di] && (uint8_t)disp[di] < 0x80 && ispunct((int)(uint8_t)disp[di])) di++;
                    if (!disp[di]) break;
                    if (tolower((uint8_t)disp[di]) != tolower((uint8_t)tok[qi])) break;
                    di++; qi++;
                }
                if (qi == tl) { found = true; match_end = di; }
            } else {
                bool m = true;
                for (size_t j = 0; j < tl && m; j++)
                    if (!disp[si + j] || tolower((uint8_t)disp[si + j]) != tolower((uint8_t)tok[j])) m = false;
                if (m) { found = true; match_end = si + tl; }
            }
        }
        if (found) { for (size_t j = si; j < match_end && j < slen; j++) hl[j] = true; si = match_end; }
        else si++;
    }

    // Character advance in the loaded Tiny font.
    auto charAdv = [&](uint8_t c) -> int16_t {
        if (c == ' ') return vlwSpaceWidth(g_ui_vlw);
        int16_t a = vlwAdvance(g_ui_vlw, vlwPrivToUnicode(c));
        return (a < 0) ? (int16_t)(vlwSpaceWidth(g_ui_vlw) + 1) : a;
    };

    // Pass 2: word-wrap over two lines. max_sx already excludes the scrollbar, so
    // words break before it. A word wider than a line falls back to char breaks.
    int16_t sx     = PADDING;
    int16_t max_sx = row_w - PADDING;
    int16_t cur_y  = snip_y;
    uint8_t sline  = 0;
    const uint16_t norm_col = sel ? fg() : dim_fg();
    for (size_t i = 0; i < slen; ) {
        size_t ws = i;
        while (i < slen && snip[i] != ' ') i++;   // word
        size_t we = i;
        while (i < slen && snip[i] == ' ') i++;   // trailing spaces
        int16_t word_w = 0;
        for (size_t k = ws; k < we; k++) word_w += charAdv((uint8_t)snip[k]);
        if (sx > PADDING && sx + word_w > max_sx) {           // whole word won't fit → wrap
            if (sline == 0) { sline = 1; sx = PADDING; cur_y = snip_y + tiny_lh; }
            else break;
        }
        bool stop = false;
        for (size_t k = ws; k < we && !stop; k++) {           // draw word (char-wrap if huge)
            int16_t cw = charAdv((uint8_t)snip[k]);
            if (sx + cw > max_sx) {
                if (sline == 0) { sline = 1; sx = PADDING; cur_y = snip_y + tiny_lh; }
                else { stop = true; break; }
            }
            uint16_t col = hl[k] ? (uint16_t)0xFD20 : norm_col;
            tft.setTextColor(col, bg_col);
            sx += tftCharUTF8(tft, (uint8_t)snip[k], sx, cur_y, 1, col);
        }
        if (stop) break;
        for (size_t k = we; k < i && sx > PADDING; k++)       // advance past spaces (no draw)
            sx += charAdv((uint8_t)snip[k]);
    }
    setUiFont(1);   // restore X-Small for the next row's reference line
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
    setUiFont(1);   // X-Small for result rows (one switch for the whole loop)

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
    setUiFont(2);
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
    setUiFont(1);   // X-Small for result rows
    for (uint8_t i = 0; i < vis && (menu_scroll + i) < search_result_count; i++) {
        uint16_t idx = menu_scroll + i;
        drawSearchResultRow((int16_t)(contentY() + i * srchH()), idx,
                            idx == (uint16_t)search_res_sel);
    }
    setUiFont(2);
    drawScrollBar(search_result_count, vis, menu_scroll);
    drawNavBar("Back", "", "View");
}

// Progress bar drawn during searchBible() — called ~every 8 KB of XML read.
// The border is drawn only on the first call (done==0) to avoid repainting gray
// over the bar interior on every update, which caused a strobing gray flash.
// Small live "DRAM: NN%  PSRAM: NN%" readout (used% of each pool), centred at y.
void BibleInterface::drawMemUsage(int16_t y) {
    uint32_t dtot = ESP.getHeapSize();
    uint32_t dfree = ESP.getFreeHeap();
    int dpct = (dtot > 0) ? (int)(100UL * (dtot - dfree) / dtot) : 0;

    char buf[40];
#ifdef HAS_PSRAM
    uint32_t ptot = ESP.getPsramSize();
    uint32_t pfree = ESP.getFreePsram();
    int ppct = (ptot > 0) ? (int)(100UL * (ptot - pfree) / ptot) : 0;
    if (ptot > 0) snprintf(buf, sizeof(buf), "D-RAM: %d%%   PSRAM: %d%%", dpct, ppct);
    else          snprintf(buf, sizeof(buf), "D-RAM: %d%%   PSRAM: n/a", dpct);
#else
    snprintf(buf, sizeof(buf), "D-RAM: %d%%", dpct);
#endif
    // Repaint a clean strip (X-Small height) so the value updates without ghosting.
    setUiFont(1);
    tft.fillRect(0, y, scrW(), (int16_t)VLW_FONTS[1].lineH + 2, bg());
    tft.setTextColor(dim_fg(), bg());
    tft.drawCentreString(buf, scrW() / 2, y, 1);
    setUiFont(2);
}

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
    // Clear the previous number's band first (smooth text doesn't fill its bg, so a
    // shorter value would otherwise leave the old digits behind).
    int16_t pct_y = bar_y + bar_h + 2;
    tft.fillRect(0, pct_y, scrW(), (int16_t)VLW_FONTS[1].lineH + 2, bg());
    drawSmallCentered(pct, scrW() / 2, pct_y, (int16_t)VLW_FONTS[1].lineH, fg(), bg());

    // Live memory usage near the top of the content area (updates every call).
    drawMemUsage((int16_t)contentY() + 4);
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
            if (tx < 48) { goBack(); return; }
            // Dictionary: tap the header title to switch which dictionary is searched.
            if (mode == MODE_DICT && trans_count > 1) {
                cur_trans = (cur_trans + 1) % trans_count;
                prefs.putUChar("trans", cur_trans);
                loadToc(trans_stems[cur_trans]);
                cached_book = 0xFFFF; cached_chap = 0; cached_count = 0;
                if (cur_book >= numBooks()) cur_book = 0;
                cur_sec = (numBooks() > 0) ? bookSection(cur_book) : 0;
                needs_redraw = true;
            }
            return;
        }
        if (touchInNav(tx, ty)) {
            touch_was_down = false;
            uint16_t third = scrW() / 3;
            if (tx < third) {
                // New — open a fresh keyboard, run search if confirmed
                search_query[0] = 0;
                if (openSearchKeyboard() && search_query[0]) {
                    addToSearchHistory(search_query);
                    if (searchBible(search_query)) goToSearchResults();
                    else                           needs_redraw = true;
                } else {
                    needs_redraw = true;
                }
                return;
            } else if (tx < 2 * third) {
                // View — open the keyboard pre-filled with the selected history
                // item so the user can edit the query/options before searching.
                if (search_hist_sel >= 0 &&
                    search_hist_sel < (int16_t)search_hist_count) {
                    strncpy(search_query, search_hist[search_hist_sel],
                            BIBLE_SEARCH_QUERY_LEN - 1);
                    search_query[BIBLE_SEARCH_QUERY_LEN - 1] = 0;
                    if (openSearchKeyboard() && search_query[0]) {
                        addToSearchHistory(search_query);
                        if (searchBible(search_query)) goToSearchResults();
                        else                           needs_redraw = true;
                    } else {
                        needs_redraw = true;
                    }
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

    // Songs "All" results may live in another songbook — switch to it first.
    if (mode != MODE_BIBLE && r.trans != cur_trans && r.trans < trans_count) {
        cur_trans = r.trans;
        prefs.putUChar("trans", cur_trans);
        if (!loadToc(trans_stems[cur_trans])) { goToMainMenu(); return; }
    }

    highlight_verse     = r.verse;
    reading_from_search = true;
    cur_book    = r.book;
    if (cur_book >= numBooks()) { goToSection(); return; }
    cur_chapter = r.chapter;
    cur_sec     = bookSection(cur_book);

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
                                  uint16_t& book_out,
                                  uint16_t& chap_out,
                                  uint8_t& verse_out) {
    const char* dot1 = strchr(osisID, '.');
    if (!dot1) return false;
    const char* dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return false;
    int code_len = (int)(dot1 - osisID);
    for (uint16_t b = 0; b < numBooks(); b++) {
        if ((int)strlen(bookCode(b)) == code_len &&
            strncmp(osisID, bookCode(b), code_len) == 0) {
            book_out  = b;
            chap_out  = (uint16_t)atoi(dot1 + 1);
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
            if ((c < 0x80 && ispunct((int)c)) || (c >= 0x87 && c <= 0x8E)) continue;  // + typographic marks
            t2[ti++] = *p;
        }
        t2[ti] = 0;
        t = t2;

        size_t qi = 0;
        for (const char* p = query; *p && qi < BIBLE_SEARCH_QUERY_LEN - 1; p++) {
            uint8_t c = (uint8_t)*p;
            if ((c < 0x80 && ispunct((int)c)) || (c >= 0x87 && c <= 0x8E)) continue;  // + typographic marks
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

bool BibleInterface::openSearchKeyboard() {
#ifdef HAS_TOUCH
    const char* kb_title = (mode == MODE_SONGS) ? "Search Songs:"
                         : (mode == MODE_DICT)  ? "Search word:"
                                                : "Search Bible:";
    // Translation / songbook / dictionary picker (shown when there are 2+ files):
    // tap it to switch which one is searched. Use each file's real display name
    // (from the .toc, umlauts included) converted from private codes to UTF-8 so
    // the keyboard's smooth-font drawString renders the umlaut glyphs.
    char names_buf[BIBLE_MAX_TRANS][BIBLE_TRANS_DISP_LEN * 2];  // UTF-8 ≈ up to 2×
    const char* names[BIBLE_MAX_TRANS];
    for (uint8_t i = 0; i < trans_count; i++) {
        vlwPrivToUtf8(trans_names[i], names_buf[i], sizeof(names_buf[i]));
        names[i] = names_buf[i];
    }
    const char* pick_label = (mode == MODE_SONGS) ? "Book:"
                           : (mode == MODE_DICT)  ? "Dict:" : "Trans:";
    uint8_t dsel = cur_trans;

    bool ok;
    if (mode == MODE_DICT) {
        // Dictionary auto-scopes to the query's letter bucket (no scope row).
        ok = bibleKeyboardInput(tft, fg(), bg(), search_query, BIBLE_SEARCH_QUERY_LEN,
                                kb_title, &srch_partial_match, &srch_ignore_punct,
                                nullptr, nullptr, nullptr, 0,
                                names, trans_count, &dsel, pick_label);
    } else if (mode == MODE_SONGS) {
        static const char* const FIND[3] = { "All", "Title", "Body" };
        if (srch_scope > 2) srch_scope = 0;
        ok = bibleKeyboardInput(tft, fg(), bg(), search_query, BIBLE_SEARCH_QUERY_LEN,
                                kb_title, &srch_partial_match, &srch_ignore_punct,
                                &srch_scope, "Find:", FIND, 3,
                                names, trans_count, &dsel, pick_label);
    } else {  // Bible — default scope row (Bible / Section / Book) + translation picker
        ok = bibleKeyboardInput(tft, fg(), bg(), search_query, BIBLE_SEARCH_QUERY_LEN,
                                kb_title, &srch_partial_match, &srch_ignore_punct,
                                &srch_scope, nullptr, nullptr, 0,
                                names, trans_count, &dsel, pick_label);
    }

    // Apply a translation switch made in the picker (any mode).
    if (dsel != cur_trans && dsel < trans_count) {
        cur_trans = dsel;
        prefs.putUChar("trans", cur_trans);
        cached_book = 0xFFFF; cached_chap = 0; cached_count = 0;
        book_idx_valid = false;
        if (mode != MODE_BIBLE) {
            loadToc(trans_stems[cur_trans]);
            if (cur_book >= numBooks()) cur_book = 0;
            cur_sec = (numBooks() > 0) ? bookSection(cur_book) : 0;
        } else if (cur_book >= numBooks()) {
            cur_book = 0;
            cur_sec  = bookSection(cur_book);
        }
    }
    // Persist option changes the user made inside the keyboard.
    prefs.putBool ("srch_part", srch_partial_match);
    prefs.putBool ("srch_pnct", srch_ignore_punct);
    prefs.putUChar("srch_scp",  srch_scope);
    return ok;
#else
    return false;
#endif
}

bool BibleInterface::searchBible(const char* query) {
    search_result_count = 0;
    search_res_sel      = 0;
    if (!search_results) return false;   // allocation failed at boot — no search
    if (trans_count == 0 || !query || !query[0]) return false;

    // Songs "All": body scan across every songbook (handled in its own method).
    if (mode == MODE_SONGS && srch_scope == 0)
        return searchSongsAll(query);   // false = user cancelled

    // Songs "Title" search: match song titles directly (instant, no XML scan).
    if (mode == MODE_SONGS && srch_scope == 1) {
        for (uint16_t b = 0; b < numBooks() &&
                             search_result_count < BIBLE_MAX_SEARCH_RESULTS; b++) {
            if (searchContains(bookDisplay(b), query)) {
                BibleSearchResult r;
                r.book = b; r.chapter = 1; r.verse = 1; r.trans = cur_trans;
                strncpy(r.snippet, bookDisplay(b), BIBLE_SRCH_SNIPPET_LEN - 1);
                r.snippet[BIBLE_SRCH_SNIPPET_LEN - 1] = 0;
                search_results[search_result_count++] = r;
            }
        }
        return true;
    }

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
    const int16_t cbtn_y     = cbtn_bar_y + 14 + 18 + 20;  // below bar + pct-text, + 20px gap
    tft.fillRoundRect(cbtn_x, cbtn_y, CBTN_W, CBTN_H, 4, hdr_bg());
    tft.drawRoundRect(cbtn_x, cbtn_y, CBTN_W, CBTN_H, 4, dim_fg());
    drawSmallCentered("Cancel", (int16_t)(scrW() / 2), cbtn_y, CBTN_H, fg(), hdr_bg());

    char path[64];
    snprintf(path, sizeof(path), "%s/%s.xml", basePath(), trans_stems[cur_trans]);
    File f = SD.open(path);
    if (!f) return false;

    uint32_t file_size = (uint32_t)f.size();
    uint32_t last_upd  = 0;

    // Dictionary: only scan the letter-bucket book whose code matches the first
    // character of the query — seek straight to it and stop at the next book.
    // (Turns a 100+ MB full-file scan into a quick few-MB range scan.)
    uint16_t dict_bucket = 0xFFFF;
    uint32_t prog_base = 0, prog_total = file_size;
    if (mode == MODE_DICT) {
        char code[12];
        dictBucketCode(query[0], code);
        for (uint16_t b = 0; b < numBooks(); b++) {
            if (strcmp(bookCode(b), code) == 0) { dict_bucket = b; break; }
        }
        if (dict_bucket == 0xFFFF || dict_bucket >= book_offsets_cap) {
            f.close();
            drawSearchProgress(1, 1);   // no bucket for this letter → zero results
            return true;
        }
        uint32_t start = book_offsets[dict_bucket];
        uint32_t next  = (dict_bucket + 1 < numBooks()) ? book_offsets[dict_bucket + 1] : file_size;
        f.seek(start);
        last_upd   = start;
        prog_base  = start;
        prog_total = (next > start) ? (next - start) : 1;
    }

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
    uint16_t vs_book  = 0, vs_chap = 0;
    uint8_t  vs_verse = 0;
    char c;

    while (xmlNextByte(s, c)) {
        uint32_t pos = (uint32_t)f.position();
        if (pos - last_upd >= 8192) {
            last_upd = pos;
            drawSearchProgress(pos - prog_base, prog_total);
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
                            uint16_t bk = 0, ch = 0;
                            uint8_t  vs = 0;
                            if (parseOsisID(osis_id, bk, ch, vs)) {
                                // Dictionary: we seeked to one letter-bucket book.
                                // Once a verse from a later book appears, we're done.
                                if (mode == MODE_DICT && bk != dict_bucket) break;
                                // Scope filter applies to Bible only (Section/Book).
                                // Dict scopes by letter bucket; Songs Body scans all
                                // stanzas (Title search is handled separately above).
                                bool in_scope = true;
                                if (mode == MODE_BIBLE) {
                                    if (srch_scope == 1)
                                        in_scope = (bookSection(bk) == cur_sec);
                                    else if (srch_scope == 2)
                                        in_scope = (bk == cur_book);
                                }
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
                            r.trans   = (mode == MODE_BIBLE) ? 0 : cur_trans;
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
                                // Dictionary entries ("word - definition") read best from the
                                // start so the word pair is visible; others centre on the match.
                                size_t snip_start = (mode == MODE_DICT)
                                    ? 0
                                    : ((match_pos > half) ? match_pos - half : 0);
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
    drawSearchProgress(prog_total, prog_total);
    return true;
}

// Songs "All" — body-scan every songbook. Results carry their songbook index in
// .trans so jumpToSearchResult() can switch files. Reloads each songbook's TOC so
// parseOsisID resolves its codes, then restores the originally-open songbook.
bool BibleInterface::searchSongsAll(const char* query) {
    if (!query || !query[0] || !search_results) return false;
    uint8_t saved_trans = cur_trans;

    tft.fillScreen(bg());
    drawHeader("Searching all...", false);
    drawSearchProgress(0, 1);

    // Cancel button (same geometry/hit-test as searchBible's).
    const int16_t CBTN_W = 80, CBTN_H = 26;
    const int16_t cbtn_bar_y = (int16_t)(contentY() + contentH() / 2 + 8);
    const int16_t cbtn_x     = (int16_t)(scrW() / 2) - CBTN_W / 2;
    const int16_t cbtn_y     = cbtn_bar_y + 14 + 18 + 20;
    tft.fillRoundRect(cbtn_x, cbtn_y, CBTN_W, CBTN_H, 4, hdr_bg());
    tft.drawRoundRect(cbtn_x, cbtn_y, CBTN_W, CBTN_H, 4, dim_fg());
    drawSmallCentered("Cancel", (int16_t)(scrW() / 2), cbtn_y, CBTN_H, fg(), hdr_bg());

    bool cancelled = false;
    for (uint8_t t = 0; t < trans_count && !cancelled &&
                        search_result_count < BIBLE_MAX_SEARCH_RESULTS; t++) {
        if (!loadToc(trans_stems[t])) continue;   // need this file's codes
        char path[64];
        snprintf(path, sizeof(path), "%s/%s.xml", basePath(), trans_stems[t]);
        File f = SD.open(path);
        if (!f) continue;
        uint32_t fsize = (uint32_t)f.size();
        uint32_t lastu = 0;

        XmlState s; memset(&s, 0, sizeof(s)); s.f = f;
        enum { ST_TEXT, ST_TAG } st = ST_TEXT;
        char tag_buf[XML_TAG_BUF]; int tag_len = 0;
        bool in_verse = false, in_note = false;
        char vtext[BIBLE_VERSE_BUF]; int vtext_len = 0;
        uint16_t vb = 0, vc = 0; uint8_t vv = 0;
        char c;
        while (xmlNextByte(s, c)) {
            uint32_t pos = (uint32_t)f.position();
            if (pos - lastu >= 16384) {
                lastu = pos;
                drawSearchProgress((uint32_t)t * 100 + (fsize ? 100UL * pos / fsize : 0),
                                   (uint32_t)trans_count * 100);
                yield();
                uint16_t cx, cy;
                if (pollTouch(&cx, &cy) &&
                    (int16_t)cx >= cbtn_x && (int16_t)cx < cbtn_x + CBTN_W &&
                    (int16_t)cy >= cbtn_y && (int16_t)cy < cbtn_y + CBTN_H) {
                    cancelled = true; break;   // exit this file's scan
                }
            }
            if (st == ST_TEXT) {
                if (c == '<') { st = ST_TAG; tag_len = 0; continue; }
                if (in_verse && !in_note && vtext_len < BIBLE_VERSE_BUF - 2) vtext[vtext_len++] = c;
            } else {
                if (c == '>') {
                    st = ST_TEXT; tag_buf[tag_len] = 0;
                    bool closing = (tag_buf[0] == '/');
                    const char* tn = closing ? tag_buf + 1 : tag_buf;
                    if (tag_buf[0] == '?' || tag_buf[0] == '!') { tag_len = 0; continue; }
                    if (!closing) {
                        bool sc = (tag_len > 0 && tag_buf[tag_len - 1] == '/');
                        if (sc) tag_buf[--tag_len] = 0;
                        if (strncmp(tn, "verse", 5) == 0 &&
                            (tn[5] == ' ' || tn[5] == '\t' || tn[5] == 0)) {
                            char oid[64] = {0};
                            if (xmlGetAttr(tag_buf, "osisID", oid, sizeof(oid))) {
                                uint16_t bk = 0, ch = 0; uint8_t vs = 0;
                                if (parseOsisID(oid, bk, ch, vs)) {
                                    vb = bk; vc = ch; vv = vs;
                                    in_verse = !sc; in_note = false; vtext_len = 0;
                                }
                            }
                        } else if (in_verse && strncmp(tn, "note", 4) == 0) {
                            in_note = true;
                        }
                    } else {
                        if (strncmp(tn, "verse", 5) == 0 && in_verse) {
                            vtext[vtext_len] = 0;
                            xmlDecodeEntities(vtext, BIBLE_VERSE_BUF);
                            utf8Encode(vtext);
                            if (searchContains(vtext, query) &&
                                search_result_count < BIBLE_MAX_SEARCH_RESULTS) {
                                BibleSearchResult r;
                                r.book = vb; r.chapter = vc; r.verse = vv; r.trans = t;
                                strncpy(r.snippet, vtext, BIBLE_SRCH_SNIPPET_LEN - 1);
                                r.snippet[BIBLE_SRCH_SNIPPET_LEN - 1] = 0;
                                search_results[search_result_count++] = r;
                            }
                            in_verse = false; in_note = false; vtext_len = 0;
                        } else if (in_verse && strncmp(tn, "note", 4) == 0) {
                            in_note = false;
                        }
                    }
                    tag_len = 0;
                } else if (tag_len < XML_TAG_BUF - 1) {
                    tag_buf[tag_len++] = c;
                }
            }
        }
        f.close();
    }

    // Restore the songbook that was open before the search.
    cur_trans = saved_trans;
    loadToc(trans_stems[cur_trans]);
    if (cancelled) {
        search_result_count = 0;
        needs_redraw = true;
        return false;
    }
    drawSearchProgress((uint32_t)trans_count * 100, (uint32_t)trans_count * 100);
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
    char path[64];
    srchHistPath(path, sizeof(path));
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    for (uint8_t i = 0; i < search_hist_count; i++) {
        f.print(search_hist[i]);
        f.write('\n');
    }
    f.close();
}

void BibleInterface::loadSearchHistory() {
    search_hist_count = 0;
    char path[64];
    srchHistPath(path, sizeof(path));
    File f = SD.open(path);
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
