# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash

**IDE:** Arduino IDE only (no PlatformIO, no Makefile).
**Entry point:** `bible_firmware/bible_firmware.ino`

### Board configuration (must match in two places)

1. `bible_firmware/configs.h` — uncomment exactly one:
   ```cpp
   #define MARAUDER_PANCAKE   // ST7796 320×480, FT6336 cap touch  ← current
   #define MARAUDER_V8        // ILI9341 240×320, XPT2046 resistive
   #define MARAUDER_V6_1      // ILI9341 240×320, XPT2046 resistive
   ```

2. `bible_firmware/libraries/TFT_eSPI-ESP32-C5/User_Setup_Select.h` — uncomment matching line:
   ```cpp
   #include <User_Setup_marauder_pancake.h>
   //#include <User_Setup_marauder_v8.h>
   //#include <User_Setup_og_marauder.h>
   ```

### Arduino IDE settings (Pancake / V8 — ESP32-C5)
| Setting | Value |
|---------|-------|
| Board | ESP32C5 Dev Module |
| Flash Size | 8 MB |
| Partition Scheme | Custom → `bible_firmware/partitions.csv` |
| Flash Frequency | 80 MHz |

The custom partition scheme is **required** — it creates the `ota_0`/`ota_1` dual-boot layout. The Bible firmware flashes to `ota_0` (offset `0x20000`); Marauder goes to `ota_1` (`0x390000`).

### SD card setup
Copy OSIS XML Bible files to `/esp32_library/bible/` on a FAT32 SD card. All firmware data lives under `/esp32_library/` (`SD_LIB_ROOT` in `BibleInterface.h`): `bible/`, `songs/`, `dictionary/`, `commentary/`, and `splash.raw`/`splash_land.raw`. No pre-processing needed — the firmware streams XML directly.

**Commentary** (`/esp32_library/commentary/`): `.cmt` binaries produced by `sd_prep/convert_commentary.py` from MySword/e-Sword `.cmti` SQLite databases. Each `.cmt` is a seek-friendly flat file (little-endian header + sorted Book/Chapter/Verse index tables keyed by the firmware `BOOKS[]` index + a private-byte text blob). The reader (`BV_COMMENTARY`) binary-searches the index for the current book/chapter/verse — no SQLite on-device. In the Bible reader, the `CMT` footer button opens it (replaces `Settings` on V8/V6.1; a 4th button on the wider Pancake). Scope toggles Book/Chapter/Verse; the `CMT` button in-view picks the active commentary; header-back returns to the reader.

---

## Architecture

### Single-class UI state machine
`BibleInterface` (singleton `bible_obj`) owns all state: display, touch, navigation, parser, bookmarks, search. `main(uint32_t)` is called every Arduino `loop()` tick. It calls `updateFling()` first (momentum physics), then redraws if `needs_redraw`, then dispatches to the active view's input handler.

**Views (BibleView enum):**
`BV_TRANS_SELECT → BV_SECTION_SELECT → BV_BOOK_SELECT → BV_CHAPTER_SELECT → BV_READING`
Side views: `BV_SETTINGS`, `BV_BOOKMARKS`, `BV_SEARCH_INPUT`, `BV_SEARCH_RESULTS`

Each view has a `draw*()` function (full redraw with `fillScreen`) and a `handle*Input()` function. Partial redraws (e.g. `redrawListContent()`, `redrawSettingsContent()`) avoid `fillScreen` to prevent flash.

### Touch input: two patterns
- **`getTouch()`** — debounced (200 ms), fires once per press. Used nowhere now (was settings).
- **`pollTouch()`** — raw, no debounce. Used by all list/settings/reading handlers with the three-phase pattern:
  ```
  down && !touch_was_down  → press:  save touch_down_x/y, highlight, fire header/nav immediately
  down &&  touch_was_down  → held:   drag / fling velocity tracking
  !down && touch_was_down  → lift:   activate action, kick fling if velocity > 50 px/s
  ```
  `touch_down_x`/`touch_down_y` are saved on press and used on lift (coords unavailable from `pollTouch` on lift).

### Momentum scroll system
`scroll_px` (float) is the single source of truth for scroll position across all scrollable views. Derived integer vars (`menu_scroll`, `bm_scroll`, `read_line`) are synced from it.

- Drag: `scroll_px = drag_origin_px + (touch_down_y - ty)`
- Lift: `computeFlingVel()` from 4-sample ring buffer (`vbuf_y[]`/`vbuf_t[]`) → kick `fling_vel` if |vel| > 50 px/s
- Decay: `fling_vel *= expf(-2.0f * dt)` each frame; stop when |vel| < 30 px/s or hits boundary
- All `go*()` navigation functions call `stopFling()` and reset `scroll_px`

### XML streaming parser
Reads OSIS `.xml` files from SD in 512-byte chunks. Parses `<verse osisID="Gen.1.1">` tags to extract book/chapter/verse. Stores up to `BIBLE_MAX_VERSES_CACHED` verses (30 without PSRAM, 200 with) in `verse_buf[]`. `buildWrappedLines()` wraps verse text into `lines[]` (format: `^N|text` for verse-start lines, plain text for continuations).

### German UTF-8 encoding
Multi-byte UTF-8 umlauts/ß and German typographic marks are compressed to single private bytes in `verse_buf[]` and `lines[]` by `utf8Encode()`:
```
0x80=Ä 0x81=ä 0x82=Ö 0x83=ö 0x84=Ü 0x85=ü 0x86=ß
0x87=„ 0x88=" 0x89=‚ 0x8A=' 0x8B=' 0x8C=– 0x8D=— 0x8E=…
```
Rendering uses anti-aliased **VLW smooth fonts** (`fonts_vlw.h` + Fraktur variants). Private codes are mapped back to Unicode via `vlwPrivToUnicode()` and drawn either as UTF-8 via the reader's `printToSprite` (`vlwPrivToUtf8()`) or per-glyph via `tftCharUTF8()` (`drawGlyph`) in the UI. The font arrays include the umlaut and typographic glyphs (`make_vlw.py --extended`).

### Layout constants
```
hdrH = 28 px    navH = 28 px    itemH = 34 px    contentY = 28 px
ILI9341: scrW=240, scrH=320     ST7796: scrW=320, scrH=480
lineH: font1=12, font2=18, font4=30     chapter tile: w=scrW/5, h=36
```

### Drawing conventions
- `drawListRow(y_px, text, selected, has_arrow)` — `y_px` is **absolute screen pixel**, not row index.
- Buttons (header back, search icon, nav bar, brightness ±) use `fillRoundRect` + `drawRoundRect` (28×22 px, radius 4, `hdr_bg()` fill, `dim_fg()` border, `TFT_WHITE` text).
- Settings rows avoid `fillScreen` — use `redrawSettingsContent()` directly from the handler instead of `needs_redraw = true`, except theme change which also repaints header/nav with targeted fills.

### Persistent storage
- **NVS** (`Preferences`): `font`, `dark`, `bright`, `trans`, `accent` — all saved immediately on change.
- **SD files**: `/esp32_library/bible/bookmarks.txt` (format: `book chapter verse_first verse_last label\n`), `/esp32_library/bible/srch_hist.txt` (one query per line, max 10).
- Bookmark file is backward-compatible: old format (`book chapter label`) is detected by checking whether tokens 3 and 4 are pure integers.

### Verse selection & bookmarks
`sel_verse_first`/`sel_verse_last` (uint8_t, 1-based; 0 = no selection) track the current reading view selection. Tap verse-number column (x < 55 px) to select; adjacent taps extend; non-adjacent resets. `+Mark` with selection saves a verse/range bookmark (`verse_first > 0`); without selection saves a chapter bookmark (`verse_first = 0`). `BibleBookmark` struct: `{book, chapter, verse_first, verse_last, label[]}`.

### Accent palette
20 colors (`ACCENT_COUNT`). `ACCENT_DARK[]`/`ACCENT_LIGHT[]` in BibleInterface.cpp. `sel_bg()` returns the active color. Saved to NVS as `accent` index.

### Brightness
20 PWM levels (`BL_LEVELS[20]`), minimum ≈ 1% duty cycle. `blSet(idx)` writes to LEDC and saves to NVS immediately. `blCycle()` wraps 0→19→0 (used by nav bar shortcut). Settings screen uses `[−]`/`[+]` buttons; touch boundary at `scrW() - 32` separates − (left) from + (right).

### Search
`searchBible()` streams the full XML, calls `drawSearchProgress()` every 8 KB (border drawn once on `done==0` to avoid strobe). Results in `BibleSearchResult[] {book, chapter, verse}`, max `BIBLE_MAX_SEARCH_RESULTS`. Jumping to a result sets `highlight_verse` and `reading_from_search`; `goBack()` returns to results instead of chapter grid.
