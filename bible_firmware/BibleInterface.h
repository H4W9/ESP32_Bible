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
#define BIBLE_SD_BASE      "/bible"          // directory scanned for .xml files
#define BIBLE_BM_FILE      "/bible/bookmarks.txt"
#define BIBLE_SRCH_HIST_FILE    "/bible/srch_hist.txt"
#define BIBLE_SEARCH_QUERY_LEN  48    // max query length incl null
#define BIBLE_SEARCH_HIST_MAX   10    // max history entries
#define BIBLE_SRCH_SNIPPET_LEN   80   // max snippet bytes per result (incl null)
#ifdef BOARD_HAS_PSRAM
#  define BIBLE_MAX_SEARCH_RESULTS 200
#else
#  define BIBLE_MAX_SEARCH_RESULTS  50
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Limits and capacities
// ─────────────────────────────────────────────────────────────────────────────
#define BIBLE_MAX_TRANS          10     // max detected translations
#define BIBLE_TRANS_LEN          32     // max chars in translation filename stem
#define BIBLE_MAX_BM             50     // max stored bookmarks
#define BIBLE_BM_LABEL_LEN       32     // max chars in bookmark label
#define BIBLE_VERSE_BUF         512     // max chars per verse (with null)
#define BIBLE_LINE_BUF          160     // max chars per wrapped display line
#ifdef BOARD_HAS_PSRAM
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
enum BibleView {
    BV_TRANS_SELECT,    // pick translation (skipped if only one exists)
    BV_SECTION_SELECT,  // pick OT / Prophets / Apocrypha / NT
    BV_BOOK_SELECT,     // pick book within selected section
    BV_CHAPTER_SELECT,  // pick chapter number
    BV_READING,         // full-screen verse reader with page scroll
    BV_SETTINGS,        // font size, dark mode, brightness, "Boot Marauder"
    BV_BOOKMARKS,       // saved bookmarks list
    BV_SEARCH_INPUT,    // search history list + "New" keyboard entry
    BV_SEARCH_RESULTS,  // scrollable list of matching verse references
};

// ─────────────────────────────────────────────────────────────────────────────
// Structs
// ─────────────────────────────────────────────────────────────────────────────
struct BibleBookmark {
    uint8_t book;                       // index into BOOKS[]
    uint8_t chapter;                    // 1-based
    uint8_t verse_first;                // 0 = whole chapter; else 1-based verse start
    uint8_t verse_last;                 // 0 = whole chapter; else >= verse_first
    char    label[BIBLE_BM_LABEL_LEN];  // e.g. "Gen 1" or "Gen 1:5" or "Gen 1:5-8"
};

struct BibleSearchResult {
    uint8_t book;       // index into BOOKS[]
    uint8_t chapter;    // 1-based
    uint8_t verse;      // 1-based
    char    snippet[BIBLE_SRCH_SNIPPET_LEN]; // verse text excerpt (private codes)
};

struct BibleBook {
    const char* display;    // "Genesis"     shown in UI
    const char* osis_code;  // "Gen"         OSIS book identifier used in XML osisID
    uint8_t     chapters;   // canonical chapter count
    uint8_t     section;    // BIBLE_SEC_OT / PR / AP / NT
};

// ─────────────────────────────────────────────────────────────────────────────
// BibleInterface
// ─────────────────────────────────────────────────────────────────────────────
class BibleInterface {
private:
    // ── Display ───────────────────────────────────────────────────────────
    TFT_eSPI    tft;
    TFT_eSprite line_spr;   // per-line off-screen buffer for flicker-free reading scroll

    // ── Navigation position ───────────────────────────────────────────────
    uint8_t  cur_sec;
    uint8_t  cur_book;      // index into BOOKS[]
    uint8_t  cur_chapter;   // 1-based
    uint8_t  cur_trans;     // index into trans_stems[]

    // ── View state ────────────────────────────────────────────────────────
    BibleView view;
    bool      dark_mode;
    uint8_t   font_num;     // 1=small(8px), 2=medium(16px), 4=large(26px)
    bool      needs_redraw;

    // ── List scroll/select ────────────────────────────────────────────────
    int16_t   menu_sel;     // highlighted item index (absolute)
    int16_t   menu_scroll;  // first visible item index

    // ── Reading view ──────────────────────────────────────────────────────
    int16_t   read_line;    // index of first visible wrapped line

    // ── Verse cache (one chapter at a time) ───────────────────────────────
    char      verse_buf[BIBLE_MAX_VERSES_CACHED][BIBLE_VERSE_BUF];
    uint8_t   cached_book;
    uint8_t   cached_chap;
    uint8_t   cached_count;  // number of verses actually loaded

    // ── Wrapped text lines ────────────────────────────────────────────────
    // Format: "^N|text" where N is 1-based verse number (0 = continuation)
    char      lines[BIBLE_MAX_LINES][BIBLE_LINE_BUF];
    uint16_t  line_count;

    // ── Translations ──────────────────────────────────────────────────────
    char      trans_stems[BIBLE_MAX_TRANS][BIBLE_TRANS_LEN]; // e.g. "asv", "web"
    uint8_t   trans_count;

    // ── Bookmarks ─────────────────────────────────────────────────────────
    BibleBookmark bookmarks[BIBLE_MAX_BM];
    uint8_t   bm_count;
    int16_t   bm_sel;
    int16_t   bm_scroll;
    bool      bm_confirm_pending;   // true while delete-confirmation popup is shown

    // ── Search ────────────────────────────────────────────────────────────
    char             search_query[BIBLE_SEARCH_QUERY_LEN];
    char             search_hist[BIBLE_SEARCH_HIST_MAX][BIBLE_SEARCH_QUERY_LEN];
    uint8_t          search_hist_count;
    int16_t          search_hist_sel;
    BibleSearchResult search_results[BIBLE_MAX_SEARCH_RESULTS];
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

    // ── Book byte-offset index (fast chapter loads) ───────────────────────
    uint32_t  book_offsets[BIBLE_BOOK_COUNT]; // byte offset of first verse per book in XML
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
    void clearContent();

    void drawLoading();
    void drawTransSelect();
    void drawSectionSelect();
    void drawBookSelect();
    void drawChapterSelect();
    void drawReading();
    void drawSettings();
    void redrawSettingsContent(); // partial redraw — rows only, no fillScreen/header/nav
#ifndef HAS_CAP_TOUCH
    void runTouchCalibration();   // show TFT_eSPI calibration wizard, save result to Prefs
#endif
    void drawBookmarks();
    void drawConfirmDelete();   // overlay popup drawn on top of bookmark list
    void drawSearchInput();
    void drawSearchDelConfirm(); // overlay popup drawn on top of search history
    void drawSearchResults();
    void drawSearchResultRow(int16_t y_px, uint16_t idx, bool sel); // one search result row
    void drawSearchProgress(uint32_t done, uint32_t total);

    void drawListRow(int16_t y_px, const char* text, bool selected, bool has_arrow = true);
    void redrawListContent(uint8_t item_count); // partial redraw during scroll (no header/nav)
    void redrawChapterContent(); // partial redraw during scroll (no header/nav)
    void redrawSearchResultsContent(); // partial redraw of search result list
    void drawScrollBar(int16_t total, int16_t vis, int16_t top);
    void drawReadingLines();

    // ── Colors ────────────────────────────────────────────────────────────
    uint16_t fg()      const;
    uint16_t bg()      const;
    uint16_t hdr_bg()  const;
    uint16_t sel_bg()  const;
    uint16_t dim_fg()  const;
    uint16_t verse_num_fg() const;

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
    bool     touchInHeader(uint16_t x, uint16_t y);
    bool     touchInNav(uint16_t x, uint16_t y);
    int16_t  touchItem(uint16_t x, uint16_t y);
    // UTF-8 German character handling
    // Codes 0x80-0x85 are private single-byte placeholders for Ä,ä,Ö,ö,Ü,ü
    // Code 0x86 is the private placeholder for ß (pixel-drawn glyph).
    // All stored in verse_buf and lines[].
    void     utf8Encode(char* buf);             // compress 2-byte UTF-8 umlauts → private codes
    int16_t  textWidthUTF8(const char* s, uint8_t font); // width-measure aware of private codes
    // Draw into a sprite (or tft treated as sprite-compatible target).
    // TFT_eSprite& required (not TFT_eSPI&) so non-virtual overrides dispatch correctly.
    void     drawStringUTF8(TFT_eSprite& spr, const char* s, int16_t x, int16_t y,
                             uint8_t font, uint16_t color); // draw with dot-diaeresis overlay
    int16_t  drawSzlig(TFT_eSprite& spr, int16_t x, int16_t y,
                       uint8_t font, uint16_t color); // pixel ß glyph

    void  recordVel(int16_t y, uint32_t t);    // push sample into velocity ring buffer
    float computeFlingVel() const;              // px/s from last samples at lift
    void  updateFling(uint32_t now);            // called each frame when fling_active
    void  stopFling();                          // cancel momentum, clear buffer

    void handleListInput(uint8_t item_count);
    void handleChapterInput();
    void handleReadingInput();
    void handleSettingsInput();
    void handleBookmarksInput();
    void handleSearchInputInput();
    void handleSearchResultsInput();

    // ── Navigation helpers ────────────────────────────────────────────────
    void goToTransSelect();
    void goToSection();
    void goToBook(uint8_t sec);
    void goToChapter(uint8_t book);
    void goToReading(uint8_t chapter, int16_t start_line = 0);
    void goToSettings();
    void goToBookmarks();
    void addBookmarkCurrent();
    void jumpToBookmark(uint8_t bm_idx);
    void goToSearchInput();
    void goToSearchResults();
    bool searchBible(const char* query);
    void jumpToSearchResult(uint16_t idx);
    bool searchContains(const char* text, const char* query);
    bool touchInSearchIcon(uint16_t x, uint16_t y);
    void addToSearchHistory(const char* query);
    void saveSearchHistory();
    void loadSearchHistory();
    bool parseOsisID(const char* osisID, uint8_t& book_out, uint8_t& chap_out, uint8_t& verse_out);

    // ── XML streaming parser ──────────────────────────────────────────────
    // Loads all verses for (book, chapter) from /bible/<stem>.xml into verse_buf.
    bool   cacheChapter(uint8_t book, uint8_t chapter);

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
