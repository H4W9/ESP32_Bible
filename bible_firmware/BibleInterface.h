// BibleInterface.h
// Marauder Bible Firmware — standalone Bible reader for ESP32-C5 boards
//
// Reads OSIS XML Bible files directly from SD card via a streaming parser.
// No pre-processing step needed — copy the raw .xml file to /bible/ on the SD card.
//
// Supported boards (set one #define in configs.h):
//   MARAUDER_PANCAKE  — ST7796 320x480, FT6336 cap touch, PSRAM
//   MARAUDER_V8       — ILI9341 240x320, XPT2046 resistive touch, PSRAM
//   MARAUDER_V6_1     — ILI9341 240x320, XPT2046 resistive touch, no PSRAM
//
// SD card layout (place XML files here):
//   /bible/asv.xml
//   /bible/web.xml
//   /bible/luth1912ap.xml
//   ... (any OSIS XML file)
//
// OTA layout:
//   ota_0 = Bible firmware (this)
//   ota_1 = Marauder
//   Settings page has "Boot Marauder" which calls esp_ota_set_boot_partition(ota_1)
//   and restarts into Marauder.

#pragma once
#ifndef BibleInterface_h
#define BibleInterface_h

#include "configs.h"

#ifdef HAS_SCREEN

#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <SD.h>
#include <SPI.h>
#ifdef HAS_BATTERY
#  include <Wire.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// SD card paths
// ─────────────────────────────────────────────────────────────────────────────
// All firmware files live under one SD folder so the card root stays tidy.
#define SD_LIB_ROOT        "/esp32_library"             // parent folder on the SD card
#define BIBLE_SD_BASE      SD_LIB_ROOT "/bible"         // directory scanned for .xml files
#define SONGS_SD_BASE      SD_LIB_ROOT "/songs"         // Songs mode XML directory
#define DICT_SD_BASE       SD_LIB_ROOT "/dictionary"    // Dictionary mode XML directory
#define BIBLE_BM_FILE      SD_LIB_ROOT "/bible/bookmarks.txt"
#define BIBLE_SRCH_HIST_FILE    SD_LIB_ROOT "/bible/srch_hist.txt"
// Runtime structure tables (Songs / Dictionary — loaded from a .toc beside the .xml)
#define RT_DISP_LEN        48     // book display name buffer (e.g. song title)
#define RT_CODE_LEN        12     // osis code buffer (e.g. "S730", "SYM")
#define RT_SEC_NAME_LEN    48     // section display name buffer (e.g. category)
#define DICT_PAGE_LABEL_LEN 56    // "firstword - lastword" page label (Dictionary)
#define BIBLE_SEARCH_QUERY_LEN  48    // max query length incl null
#define BIBLE_SEARCH_HIST_MAX   10    // max history entries
#define BIBLE_SRCH_SNIPPET_LEN   140  // max snippet bytes per result (fills ~2 lines)
#ifdef HAS_PSRAM
#  define BIBLE_MAX_SEARCH_RESULTS 200
#else
#  define BIBLE_MAX_SEARCH_RESULTS  50
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Limits and capacities
// ─────────────────────────────────────────────────────────────────────────────
#define BIBLE_MAX_TRANS          10     // max detected translations
#define BIBLE_TRANS_LEN          32     // max chars in translation filename stem
#define BIBLE_TRANS_DISP_LEN     48     // max chars in a translation's display name
#define BIBLE_MAX_BM             50     // max stored bookmarks
#define BIBLE_BM_LABEL_LEN       48     // max chars in bookmark label (fits song titles)
#define BIBLE_VERSE_BUF         512     // max chars per verse (with null)
#define BIBLE_LINE_BUF          160     // max chars per wrapped display line
#ifdef HAS_PSRAM
#  define BIBLE_MAX_VERSES_CACHED  200  // enough for Psalm 119 (176 verses) + headroom
#  define BIBLE_MAX_LINES          700  // enough for long chapters at all font sizes
#else
#  define BIBLE_MAX_VERSES_CACHED   30  // fits in SRAM (no PSRAM board)
#  define BIBLE_MAX_LINES          300
#endif

// ─────────────────────────────────────────────────────────────────────────────
// XML streaming parser limits
// ─────────────────────────────────────────────────────────────────────────────
#define XML_CHUNK_SIZE    512   // bytes per SD read
#define XML_TAG_BUF       256   // max tag content buffer
#define XML_ATTR_BUF       64   // max attribute value buffer

// ─────────────────────────────────────────────────────────────────────────────
// Section indices
// ─────────────────────────────────────────────────────────────────────────────
#define BIBLE_SEC_OT    0   // Old Testament  (22 books, idx 0-21)
#define BIBLE_SEC_PR    1   // Prophets       (17 books, idx 22-38)
#define BIBLE_SEC_AP    2   // Apocrypha      (10 books, idx 39-48)
#define BIBLE_SEC_NT    3   // New Testament  (27 books, idx 49-75)
#define BIBLE_SEC_COUNT 4
#define BIBLE_BOOK_COUNT 76

// ─────────────────────────────────────────────────────────────────────────────
// View state machine
// ─────────────────────────────────────────────────────────────────────────────
// Content modes — Bible (static 76-book table) / Songs / Dictionary
// (structure loaded at runtime from a .toc file). Each mode has its own NVS
// namespace, SD directory, bookmarks file and search history.
enum ContentMode {
    MODE_BIBLE = 0,
    MODE_SONGS = 1,
    MODE_DICT  = 2,
};

enum BibleView {
    BV_MAIN_MENU,       // root: pick Bible / Songs / Dictionary
    BV_TRANS_SELECT,    // pick translation (skipped if only one exists)
    BV_SECTION_SELECT,  // pick OT / Prophets / Apocrypha / NT
    BV_BOOK_SELECT,     // pick book within selected section
    BV_CHAPTER_SELECT,  // pick chapter number
    BV_READING,         // full-screen verse reader with page scroll
    BV_SETTINGS,        // font size, dark mode, brightness, "Boot Marauder"
    BV_BOOKMARKS,       // saved bookmarks list
    BV_SEARCH_INPUT,    // search history list + "New" keyboard entry
    BV_SEARCH_RESULTS,  // scrollable list of matching verse references
    BV_ABOUT,           // firmware/hardware info screen (from Settings)
};

// ─────────────────────────────────────────────────────────────────────────────
// Structs
// ─────────────────────────────────────────────────────────────────────────────
struct BibleBookmark {
    uint16_t book;                      // index into active book table
    uint16_t chapter;                   // 1-based
    uint8_t verse_first;                // 0 = whole chapter; else 1-based verse start
    uint8_t verse_last;                 // 0 = whole chapter; else >= verse_first
    uint8_t trans;                      // translation index (Songs/Dict: which file);
                                        //   unused for Bible (book index is canon-global)
    char    label[BIBLE_BM_LABEL_LEN];  // e.g. "Gen 1" or "Gen 1:5" or "Gen 1:5-8"
};

struct BibleSearchResult {
    uint16_t book;      // index into active book table (valid for `trans`)
    uint16_t chapter;   // 1-based
    uint8_t verse;      // 1-based
    uint8_t trans;      // translation index (Songs "All" spans songbooks; else cur_trans)
    char    snippet[BIBLE_SRCH_SNIPPET_LEN]; // verse text excerpt (private codes)
};

struct BibleBook {
    const char* display;    // "Genesis"     shown in UI
    const char* osis_code;  // "Gen"         OSIS book identifier used in XML osisID
    uint8_t     chapters;   // canonical chapter count
    uint8_t     section;    // BIBLE_SEC_OT / PR / AP / NT
};

// Runtime book/section tables for Songs & Dictionary modes (loaded from .toc).
struct RtBook {
    char     display[RT_DISP_LEN];  // song title / dictionary letter label
    char     code[RT_CODE_LEN];     // osisID code prefix (".-free", unique in file)
    uint16_t chapters;              // chapter count (1 for songs; pages for dict)
    uint16_t section;               // index into rt_secs[]
};
struct RtSec {
    char     name[RT_SEC_NAME_LEN]; // category / direction label
    uint16_t start;                 // first book index in this section
    uint16_t len;                   // number of books in this section
};

// ─────────────────────────────────────────────────────────────────────────────
// BibleInterface
// ─────────────────────────────────────────────────────────────────────────────
class BibleInterface {
private:
    // ── Display ───────────────────────────────────────────────────────────
    TFT_eSPI    tft;
    TFT_eSprite line_spr;   // per-line off-screen buffer for flicker-free reading scroll
    int8_t      read_font_loaded;  // VLW size index currently loaded in line_spr (-1 = none)
    bool        read_font_frak;    // true if the loaded reading font is the Fraktur family
    int8_t      ui_font_idx;       // VLW size index currently loaded on tft for the UI (-1 = none)
    uint8_t     ui_font_fam;       // UI font family on tft: 0=normal 1=Fraktur-title 2=Fraktur-body

    // ── Navigation position ───────────────────────────────────────────────
    uint8_t  cur_sec;
    uint16_t cur_book;      // index into active book table
    uint16_t cur_chapter;   // 1-based
    uint8_t  cur_trans;     // index into trans_stems[]

    // ── Content mode + runtime structure (Songs / Dictionary) ─────────────
    ContentMode mode;       // MODE_BIBLE uses static BOOKS[]; others use rt_*
    RtBook*  rt_books;      // PSRAM/heap; valid for Songs/Dict
    uint16_t rt_book_count;
    RtSec*   rt_secs;
    uint16_t rt_sec_count;
    // Dictionary page index (labels "firstword - lastword") for the active letter.
    char*    rt_pages;        // flat array of rt_page_count × DICT_PAGE_LABEL_LEN
    uint16_t rt_page_count;
    uint16_t rt_pages_book;   // which book index rt_pages was loaded for (0xFFFF = none)

    // ── View state ────────────────────────────────────────────────────────
    BibleView view;
    bool      dark_mode;    // derived from theme_idx (THEMES[].dark) — drives accent/divider choices
    uint8_t   theme_idx;    // index into THEMES[] (colour scheme)
    uint8_t   font_num;     // 1=small(8px), 2=medium(16px), 4=large(26px)
    uint8_t   font_color_idx; // 0 = Default (theme fg); else index into FONT_COLOR_VAL[]
    uint8_t   vnum_color_idx; // 0 = Default (teal); else index into FONT_COLOR_VAL[]
    uint8_t   orientation;    // 0-3 screen rotation (global, "menu" NVS)
    uint8_t   menu_font_color_idx; // global chrome text colour (Main Menu scope Font Color)
    bool      accent_def;     // Highlight uses the theme-default colour (vs accent_idx)
    bool      needs_redraw;

    // ── Settings screen state ─────────────────────────────────────────────
    uint8_t   settings_scope;      // 0 Global · 1 Main Menu · 2 Bible · 3 Songs · 4 Dictionary
    bool      settings_from_menu;  // opened from the main menu (vs a content mode)
    uint8_t   set_rows[16];        // dynamic list of SettingRow kinds currently shown
    uint8_t   set_row_n;
    char      sc_trans[BIBLE_MAX_TRANS][BIBLE_TRANS_LEN]; // scope's translation stems
    char      sc_trans_names[BIBLE_MAX_TRANS][BIBLE_TRANS_DISP_LEN]; // display names (umlaut codes)
    uint8_t   sc_trans_count;
    uint8_t   sc_trans_cur;

    // ── List scroll/select ────────────────────────────────────────────────
    int16_t   menu_sel;     // highlighted item index (absolute)
    int16_t   menu_scroll;  // first visible item index

    // ── Reading view ──────────────────────────────────────────────────────
    int16_t   read_line;    // index of first visible wrapped line

    // ── Verse cache (one chapter at a time) ───────────────────────────────
    // Big buffers live in PSRAM (allocated in RunSetup) — kept out of internal
    // DRAM so WiFi/BLE (VerseBroadcast) and the SDK have room. Indexing is
    // unchanged: verse_buf[i][j] / lines[i][j] work exactly as 2-D arrays.
    char      (*verse_buf)[BIBLE_VERSE_BUF];   // [BIBLE_MAX_VERSES_CACHED] rows
    uint16_t  cached_book;
    uint16_t  cached_chap;
    uint8_t   cached_count;  // number of verses actually loaded

    // ── Wrapped text lines ────────────────────────────────────────────────
    // Format: "^N|text" where N is 1-based verse number (0 = continuation)
    char      (*lines)[BIBLE_LINE_BUF];        // [BIBLE_MAX_LINES] rows
    uint16_t  line_count;

    // ── Translations ──────────────────────────────────────────────────────
    char      trans_stems[BIBLE_MAX_TRANS][BIBLE_TRANS_LEN]; // e.g. "asv", "web"
    char      trans_names[BIBLE_MAX_TRANS][BIBLE_TRANS_DISP_LEN]; // display names (umlaut codes)
    uint8_t   trans_count;

    // ── Bookmarks ─────────────────────────────────────────────────────────
    BibleBookmark bookmarks[BIBLE_MAX_BM];
    uint8_t   bm_count;
    int16_t   bm_sel;
    int16_t   bm_scroll;
    bool      bm_confirm_pending;   // true while delete-confirmation popup is shown
    bool      bcast_pending;        // true while the verse-broadcast popup is shown

    // ── Search ────────────────────────────────────────────────────────────
    char             search_query[BIBLE_SEARCH_QUERY_LEN];
    char             search_hist[BIBLE_SEARCH_HIST_MAX][BIBLE_SEARCH_QUERY_LEN];
    bool             search_hist_frak[BIBLE_SEARCH_HIST_MAX];  // entry was a Fraktur search
    uint8_t          search_hist_count;
    int16_t          search_hist_sel;
    BibleSearchResult* search_results;   // heap/PSRAM (kept out of static .bss)
    uint16_t         search_result_count;
    int16_t          search_res_sel;
    uint8_t          highlight_verse;       // 1-based; 0 = no highlight (from search jump)
    bool             reading_from_search;   // true → goBack() returns to BV_SEARCH_RESULTS
    bool             search_del_pending;    // true while delete-confirmation popup is shown
    bool             srch_partial_match;    // true = all query words must appear (any order/pos)
    bool             srch_ignore_punct;     // strip punctuation from text and query before matching
    uint8_t          srch_scope;            // 0=Bible, 1=Section, 2=Book
    uint8_t          accent_idx;            // 0-19, index into ACCENT_DARK/LIGHT arrays
    uint8_t          sel_verse_first;       // 0 = no selection; else 1-based verse start
    uint8_t          sel_verse_last;        // >= sel_verse_first when selection active

    // ── Touch debounce ────────────────────────────────────────────────────
    uint32_t  last_input_ms;
    bool      last_pressed;

    // ── Scroll gesture state ───────────────────────────────────────────────
    bool      touch_was_down;
    uint16_t  touch_down_x, touch_down_y;
    bool      scroll_dragging;

    // ── Momentum scrolling ────────────────────────────────────────────────
    float     scroll_px;        // continuous pixel scroll position (0 = top)
    float     fling_vel;        // px/s (+ve = content scrolling toward later items)
    uint32_t  fling_ms;         // timestamp of last fling tick
    bool      fling_active;     // true while coasting after finger lift
    float     drag_origin_px;   // scroll_px at start of current drag
    int16_t   vbuf_y[4];        // touch-y ring buffer for velocity
    uint32_t  vbuf_t[4];        // timestamp ring buffer
    uint8_t   vbuf_i;           // ring buffer write index (0-3)

    // ── Settings "Translation" row marquee (value text scrolls in a fixed gap) ─
    bool      trans_marq_on;    // true when the value overflows its window & is visible
    int16_t   trans_marq_winx;  // absolute X of the value window
    int16_t   trans_marq_winy;  // absolute Y (top) of the row band
    int16_t   trans_marq_winw;  // window width (px)
    int16_t   trans_marq_texty; // absolute baseline Y of the value text
    int16_t   trans_marq_textw; // pixel width of the value string
    int16_t   trans_marq_off;   // current scroll offset (px)
    uint16_t  trans_marq_bg;    // window background colour
    uint16_t  trans_marq_fg;    // value text colour
    uint32_t  trans_marq_ms;    // timestamp of last marquee advance
    char      trans_marq_str[64];

    // ── About screen: tap band of the MCU row (hidden splash easter egg) ──────
    int16_t   about_mcu_y0, about_mcu_y1;

    // ── Book byte-offset index (fast chapter loads) ───────────────────────
    // Dynamically allocated, sized to numBooks() for the active mode. For Bible
    // it is filled from the .idx file; for Songs/Dict it is filled by loadToc().
    uint32_t* book_offsets;                   // byte offset of first verse per book
    uint16_t  book_offsets_cap;               // allocated entry count
    bool      book_idx_valid;                 // true when book_offsets matches cur_trans
    uint8_t   book_idx_trans;                 // cur_trans value when index was built/loaded

    // ── Battery gauge (MAX17048 via I2C) ─────────────────────────────────
#ifdef HAS_BATTERY
    bool     batt_ok;       // true when MAX17048 detected at startup
    int8_t   batt_pct;      // 0–100, or -1 if not yet read
    uint32_t batt_ms;       // millis() of last battUpdate()
    void     battInit();
    void     battUpdate();
#endif

    // ── Brightness (20 levels via PWM) ────────────────────────────────────
    static const uint8_t BL_LEVELS[20];
    uint8_t   bl_idx;       // 0-19
    Preferences prefs;

    // ── Drawing helpers ───────────────────────────────────────────────────
    void drawHeader(const char* title, bool show_back = true);
    void drawNavBar(const char* left, const char* mid, const char* right);
    // Small-chrome helpers: vector selector symbols (no font reload — safe in the
    // per-frame scroll path) and X-Small centered text for buttons/labels.
    void drawChevron(int16_t bx, int16_t by, int16_t bw, int16_t bh, bool right, uint16_t col);
    void drawPlusMinus(int16_t bx, int16_t by, int16_t bw, int16_t bh, bool plus, uint16_t col);
    void drawSmallCentered(const char* s, int16_t cx, int16_t boxY, int16_t boxH,
                           uint16_t fg, uint16_t bg);
    void clearContent();

    void drawLoading();
    bool drawSplashImage();      // blit /splash.raw (native-portrait RGB565) — returns true if shown
    void showSplashUntilTap();   // easter egg: show splash until the screen is tapped
    void drawMainMenu();
    void drawTransSelect();
    void drawSectionSelect();
    void drawBookSelect();
    void drawChapterSelect();
    void drawReading();
    void drawSettings();
    void redrawSettingsContent(); // partial redraw — rows only, no fillScreen/header/nav
    void drawTransValue(int16_t off); // draw the Translation value text at scroll offset
    void tickTransMarquee();          // advance + redraw the marquee (called from main)
    // Settings rows are dynamic (depend on Settings Scope + board), addressed by kind.
    enum SettingRow : uint8_t {
        SR_SCOPE, SR_TRANS, SR_FONTSIZE, SR_FONTCOL, SR_VNUMCOL,
        SR_THEME, SR_HIGHLIGHT, SR_ORIENT, SR_BRIGHT, SR_ABOUT, SR_BOOT, SR_CALIB
    };
    void     buildSettingsRows();        // fill set_rows[] for the current scope/board
    uint8_t  settingsScopeMode() const;  // MODE_* for a content-mode scope, else 0xFF
    uint16_t themeHighlight() const;     // theme-fitting default Highlight colour
    void     loadScopeSettings();        // load "look" settings from the scope's NVS
    void     writeScoped(const char* key, uint8_t val); // write to every NVS ns in the scope
    void     scopeScanTrans();           // scan the scope mode's SD folder into sc_trans[]
#ifndef HAS_CAP_TOUCH
    void runTouchCalibration();   // show TFT_eSPI calibration wizard, save result to Prefs
#endif
    void drawBookmarks();
    void drawAbout();           // firmware/hardware info screen
    void drawConfirmDelete();   // overlay popup drawn on top of bookmark list
    void drawBroadcastMenu();   // "Broadcast verse" popup (WiFi / Bluetooth / Cancel)
    void runVerseBroadcast(bool use_wifi);  // blocking broadcast screen until Stop
    void drawSearchInput();
    void drawSearchDelConfirm(); // overlay popup drawn on top of search history
    void drawSearchResults();
    void drawSearchResultRow(TFT_eSprite& spr, int16_t y_px, uint16_t idx, bool sel); // one row → sprite
    void drawSearchProgress(uint32_t done, uint32_t total);
    void drawMemUsage(int16_t y);   // live DRAM/PSRAM usage on loading/search screens

    void drawListRow(int16_t y_px, const char* text, bool selected, bool has_arrow = true);
    void drawListRowSprite(TFT_eSprite& spr, int16_t y_px, const char* text,
                           bool selected, bool has_arrow);   // one row → sprite (smooth scroll)
    void redrawListContent(uint16_t item_count); // partial redraw during scroll (no header/nav)
    void redrawChapterContent(); // partial redraw during scroll (no header/nav)
    void redrawSearchResultsContent(); // partial redraw of search result list
    void drawScrollBar(int16_t total, int16_t vis, int16_t top);
    void drawReadingLines();
    void loadReadingFont();   // (re)load the VLW font for font_num into line_spr
    void setUiFont(uint8_t idx);  // load a UI VLW size onto tft (menus/chrome/keyboard)
    void setUiFontEx(uint8_t idx, bool frak_title);  // normal or Fraktur-title family
    void setUiFontFam(uint8_t idx, uint8_t fam);     // fam: 0=normal 1=Fraktur-title 2=Fraktur-body
    bool headerFraktur() const;   // reading-view song-title header → Fraktur
    bool rowsFraktur() const;     // song-list (BV_BOOK_SELECT) rows → Fraktur
    bool transIsFraktur(uint8_t t) const;  // songbook t is a Fraktur book (stem "*_fraktur")

    // ── Colors ────────────────────────────────────────────────────────────
    uint16_t fg()      const;
    uint16_t bg()      const;
    uint16_t hdr_bg()  const;
    uint16_t sel_bg()  const;
    uint16_t dim_fg()  const;
    uint16_t verse_num_fg() const;
    uint16_t font_fg()      const;  // reading-text colour (Font Color setting)
    uint16_t chromeFg()     const;  // global UI chrome text (header/nav/menu) — Main Menu Font Color
    void     loadMenuChrome();      // refresh menu_font_color_idx from the "menu" NVS namespace
    bool     isNeon()       const;  // true when the Neon (rainbow-outline) theme is active
    uint16_t edgeColor(int16_t seed, uint16_t def) const; // outline colour: rainbow if neon, else def

    // ── Structure accessors (Bible static table  OR  runtime Songs/Dict) ───
    uint16_t numBooks() const { return (mode == MODE_BIBLE) ? BIBLE_BOOK_COUNT : rt_book_count; }
    uint8_t  numSecs()  const { return (mode == MODE_BIBLE) ? BIBLE_SEC_COUNT  : (uint8_t)rt_sec_count; }
    const char* bookDisplay(uint16_t i) const { return (mode == MODE_BIBLE) ? BOOKS[i].display   : rt_books[i].display; }
    const char* bookCode(uint16_t i)    const { return (mode == MODE_BIBLE) ? BOOKS[i].osis_code : rt_books[i].code; }
    uint16_t bookChapters(uint16_t i)   const { return (mode == MODE_BIBLE) ? BOOKS[i].chapters  : rt_books[i].chapters; }
    uint16_t bookSection(uint16_t i)    const { return (mode == MODE_BIBLE) ? BOOKS[i].section   : rt_books[i].section; }
    const char* secName(uint8_t s)      const { return (mode == MODE_BIBLE) ? SEC_NAME_EN[s]     : rt_secs[s].name; }
    uint16_t secStart(uint8_t s)        const { return (mode == MODE_BIBLE) ? SEC_BOOK_START[s]  : rt_secs[s].start; }
    uint16_t secLen(uint8_t s)          const { return (mode == MODE_BIBLE) ? SEC_BOOK_COUNT_[s] : rt_secs[s].len; }

    // ── Mode-parameterized SD paths / NVS namespace ───────────────────────
    const char* basePath() const;                       // /esp32_library/{bible,songs,dictionary}
    const char* nvsNamespace() const;                   // "bible" | "songs" | "dict"
    void        bmPath(char* out, size_t n) const;      // <base>/bookmarks.txt
    void        srchHistPath(char* out, size_t n) const;// <base>/srch_hist.txt

    // ── Layout ────────────────────────────────────────────────────────────
    uint16_t scrW()     const;
    uint16_t scrH()     const;
    uint16_t hdrH()     const { return 28; }
    uint16_t navH()     const { return 28; }
    uint16_t itemH()    const { return 34; }
    uint16_t contentY() const { return hdrH(); }
    uint16_t contentH() const { return scrH() - hdrH() - navH(); }
    uint16_t lineH()         const;
    uint16_t srchH()         const { return 50; }   // search result row height (ref + snippet)
    uint8_t  visItems()      const;
    uint8_t  visSearchItems() const { return (uint8_t)(contentH() / srchH()); }
    uint8_t  visLines()      const;

    // ── Touch input ───────────────────────────────────────────────────────
    bool     getTouch(uint16_t* tx, uint16_t* ty);
    bool     pollTouch(uint16_t* tx, uint16_t* ty);  // raw, no debounce
#ifndef HAS_CAP_TOUCH
    uint16_t cal_data[5];                  // active resistive calibration (portrait, rotation 0)
    bool     resistiveTouch(uint16_t* x, uint16_t* y); // raw→portrait→orientation map
#endif
    bool     touchInHeader(uint16_t x, uint16_t y);
    bool     touchInNav(uint16_t x, uint16_t y);
    int16_t  touchItem(uint16_t x, uint16_t y);
    // UTF-8 German character handling
    // Codes 0x80-0x85 are private single-byte placeholders for Ä,ä,Ö,ö,Ü,ü
    // Code 0x86 is the private placeholder for ß (pixel-drawn glyph).
    // All stored in verse_buf and lines[].
    void     utf8Encode(char* buf);             // compress 2-byte UTF-8 umlauts → private codes
    int16_t  textWidthUTF8(const char* s, uint8_t font); // width-measure (loaded UI VLW font)

    void  recordVel(int16_t y, uint32_t t);    // push sample into velocity ring buffer
    float computeFlingVel() const;              // px/s from last samples at lift
    void  updateFling(uint32_t now);            // called each frame when fling_active
    void  stopFling();                          // cancel momentum, clear buffer

    void handleMainMenuInput();
    void handleListInput(uint16_t item_count);
    void handleChapterInput();
    void handleReadingInput();
    void handleSettingsInput();
    void handleBookmarksInput();
    void handleAboutInput();
    void handleSearchInputInput();
    void handleSearchResultsInput();

    // ── Navigation helpers ────────────────────────────────────────────────
    void applyOrientation();                // tft.setRotation per `orientation` (0-3)
    void orientTouch(uint16_t& x, uint16_t& y) const; // map raw cap-touch to orientation
    void goToMainMenu();
    void enterMode(ContentMode m);          // switch namespace/paths/state, scan, route
    void selectTranslation(uint16_t idx);   // set cur_trans, load .toc (Songs/Dict), route
    bool loadToc(const char* stem);         // fill rt_books/rt_secs/book_offsets from <base>/<stem>.toc
    bool loadDictPages(uint16_t book);      // fill rt_pages from <base>/<stem>.pgx for a letter
    const char* pageLabel(uint16_t i) const;// dict page label, or "Page N" fallback
    void freeRuntime();                      // free rt_books/rt_secs/book_offsets
    void goToTransSelect();
    void goToSection();
    void goToBook(uint8_t sec);
    void goToChapter(uint16_t book);
    void goToReading(uint16_t chapter, int16_t start_line = 0);
    void goToSettings(bool from_menu = false);
    void goToBookmarks();
    void goToAbout();
    void addBookmarkCurrent();
    void jumpToBookmark(uint8_t bm_idx);
    void goToSearchInput();
    void goToSearchResults();
    // Opens the on-screen keyboard for a search. search_query is in/out: pass it
    // empty for a fresh search, or pre-filled (e.g. from history) to edit first.
    // Handles the mode-specific option row (Bible scope / Songs Find / Dict picker).
    bool openSearchKeyboard();
    bool searchBible(const char* query);
    bool searchSongsAll(const char* query);  // Songs "All" body scan; false if cancelled
    void jumpToSearchResult(uint16_t idx);
    bool searchContains(const char* text, const char* query);
    bool touchInSearchIcon(uint16_t x, uint16_t y);
    void addToSearchHistory(const char* query, bool frak = false);
    void saveSearchHistory();
    void loadSearchHistory();
    bool parseOsisID(const char* osisID, uint16_t& book_out, uint16_t& chap_out, uint8_t& verse_out);

    // ── XML streaming parser ──────────────────────────────────────────────
    // Loads all verses for (book, chapter) from /bible/<stem>.xml into verse_buf.
    bool   cacheChapter(uint16_t book, uint16_t chapter);

    // Low-level XML parser state machine used by cacheChapter.
    struct XmlState {
        File    f;
        uint8_t chunk[XML_CHUNK_SIZE + 1];
        int32_t chunk_len;
        int32_t chunk_pos;
        char    tag_buf[XML_TAG_BUF];
        int16_t tag_len;
        char    attr_buf[XML_ATTR_BUF];
        int16_t attr_len;
        bool    in_verse;
        bool    in_note;
        uint8_t nest_level;   // nesting inside <verse>
        int16_t verse_text_len;
        char    verse_text[BIBLE_VERSE_BUF];
        bool    collecting;   // true = we are storing text into verse_text
    };
    bool   xmlNextByte(XmlState& s, char& c);
    bool   xmlReadTag(XmlState& s);         // reads tag name+attrs into tag_buf
    void   xmlDecodeEntities(char* buf, size_t len); // &amp; &lt; &gt; &nbsp; etc.
    bool   xmlGetAttr(const char* tag, const char* attr, char* out, size_t out_len);

    // ── Book index (first-run byte-offset scan) ───────────────────────────
    bool   loadBookIndex(const char* stem);   // load /bible/<stem>.idx → book_offsets
    bool   buildBookIndex(const char* stem);  // scan XML → build book_offsets
    void   saveBookIndex(const char* stem);   // write book_offsets → /bible/<stem>.idx

    // ── Text wrapping ─────────────────────────────────────────────────────
    void    buildWrappedLines();
    void    addWrappedLine(uint8_t verse_num, const char* text,
                           uint16_t max_px, uint8_t fnt, uint16_t& idx);

    // ── Persistence ───────────────────────────────────────────────────────
    void    saveState();
    void    loadState();
    void    saveBookmarks();
    void    loadBookmarks();
    void    scanTranslations();
    // Display name for a translation/songbook stem: reads the "T|<name>" line from
    // <base>/<stem>.toc (UTF-8 → private umlaut codes); falls back to a prettified
    // stem when there is no .toc or no T| line (e.g. Bible).
    void    transDisplayName(const char* base, const char* stem, char* out, size_t n);

    // ── Brightness ────────────────────────────────────────────────────────
    void    blInit();
    void    blSet(uint8_t idx);
    void    blCycle();

    // ── OTA boot switching ────────────────────────────────────────────────
    void    bootMarauder();  // sets ota_1 as next boot partition and restarts

public:
    // ── Static tables ─────────────────────────────────────────────────────
    static const BibleBook   BOOKS[BIBLE_BOOK_COUNT];
    static const uint8_t     SEC_BOOK_START[BIBLE_SEC_COUNT];  // {0,22,39,49}
    static const uint8_t     SEC_BOOK_COUNT_[BIBLE_SEC_COUNT]; // {22,17,10,27}
    static const char* const SEC_NAME_EN[BIBLE_SEC_COUNT];

    BibleInterface();

    // Called once at startup to init display, SD, touch, and load state.
    void RunSetup();

    // Called every loop() iteration.
    void main(uint32_t currentTime);

    // Navigate back in the view hierarchy.
    void goBack();
};

// Global instance (defined in BibleInterface.cpp)
extern BibleInterface bible_obj;

#endif  // HAS_SCREEN
#endif  // BibleInterface_h
