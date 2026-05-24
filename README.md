# ESP32 Bible Firmware — Setup Guide

Standalone offline Bible reader for Marauder Pancake, V8, and V6.1.
Runs as a **separate firmware** in the `ota_0` partition alongside Marauder in `ota_1`.

---

## OTA Dual-Boot Overview

| Partition | Firmware | Flash Offset |
|-----------|----------|-------------|
| `ota_0`   | Bible firmware (this) | `0x20000` |
| `ota_1`   | ESP32 Marauder        | `0x390000` |

- Flash the Bible firmware to `ota_0`.
- Flash Marauder separately to `ota_1` (via Marauder's own OTA tool or esptool).
- In the Bible app's **Settings → Boot OTA_1** restarts into Marauder.
- Marauder's OTA update feature can switch back to `ota_0`.

---

## Step 1 — SD Card Setup (No Python Script Needed)

The firmware reads OSIS XML Bible files **directly** — no pre-processing required.

Create a `/bible/` folder on the root of your SD card and copy XML files into it:

```
SD card root
└── bible/
    ├── asv.xml          ← American Standard Version
    ├── web.xml          ← World English Bible
    ├── luth1912ap.xml   ← Luther 1912 (German, includes Apocrypha)
    └── bookmarks.txt    ← auto-created by the firmware
```

Available OSIS XML source files:

| File | Translation | Language |
|------|-------------|----------|
| `asv.xml` | American Standard Version | English |
| `bible.akjv.xml` | American King James Version | English |
| `web.xml` | World English Bible | English |
| `mkjv1962.xml` | Modern King James Version | English |
| `luth1912ap.xml` | Luther Bibel 1912 (with Apocrypha) | German |
| `sch1951.xml` | Schlachter 1951 | German |

Copy as many as you want — the app detects them all and lets you switch between them in Settings.

**Approximate size per file:**

| File | Size |
|------|------|
| English translations | ~5–6 MB each |
| Luther 1912 (DE) | ~7 MB (includes Apocrypha) |

**SD card requirements:** FAT32, 2 GB minimum, Class 4 or faster.

---

## Step 2 — Configure the Board

### 2a. Select board in `configs.h`

Open `bible_firmware/configs.h` and uncomment **one** board:

```cpp
// #define MARAUDER_PANCAKE   ← ST7796 320x480, FT6336 cap touch
// #define MARAUDER_V8        ← ILI9341 240x320, XPT2046 resistive
// #define MARAUDER_V6_1      ← ILI9341 240x320, XPT2046 resistive
```

### 2b. Select board in TFT_eSPI

Open `bible_firmware/libraries/TFT_eSPI-ESP32-C5/User_Setup_Select.h`
and uncomment **one** matching line:

```cpp
#include <User_Setup_marauder_pancake.h>   // ← Pancake
//#include <User_Setup_marauder_v8.h>      // ← V8
//#include <User_Setup_og_marauder.h>      // ← V6.1
```

Both selections must match.

---

## Step 3 — Build and Flash

### Arduino IDE settings

**Pancake / V8 (ESP32-C5):**
| Setting | Value |
|---------|-------|
| Board | ESP32C5 Dev Module |
| Flash Size | 8 MB |
| Partition Scheme | Custom → select `bible_firmware/partitions.csv` |
| Flash Frequency | 80 MHz |

**V6.1 (original ESP32):**
| Setting | Value |
|---------|-------|
| Board | LOLIN D32 (or your ESP32 board) |
| Partition Scheme | Custom → select `bible_firmware/partitions.csv` |

Open `bible_firmware/bible_firmware.ino` in Arduino IDE, compile, and upload.

> The custom `partitions.csv` creates the `ota_0`/`ota_1` layout. You must select it
> or the Bible firmware will flash to the wrong location.

---

## Bible Reader UI — Quick Reference

### Global controls (all screens)

| Control | Action |
|---------|--------|
| **`<` button (top-left header)** | Go back |
| **Search icon (top-right header)** | Open Search (available on most screens) |

Navigation flows: Translation → Section → Book → Chapter → Reading.  
The Translation screen is skipped if only one `.xml` file is on the SD card.

---

### Navigation screens (Translation / Section / Book / Chapter)

| Control | Action |
|---------|--------|
| **Tap item** | Open it |
| **Drag / fling** | Scroll the list |
| **Left nav — "Marks"** | Open Bookmarks |
| **Middle nav — "Settings"** | Open Settings |
| **Right nav — "Bright"** | Cycle brightness (wraps 1→20→1) |

---

### Reading View

| Control | Action |
|---------|--------|
| **Drag / fling** | Smooth-scroll verses with momentum |
| **Tap upper half** | Previous page; at top of chapter → previous chapter |
| **Tap lower half** | Next page; at last line → next chapter |
| **Tap verse number** | Select that verse (highlighted in accent color) |
| **Tap adjacent verse number** | Extend selection to include it |
| **Tap selected verse number** | Shrink or deselect |
| **Left nav — "Marks"** | Open Bookmarks |
| **Middle nav — "Settings"** | Open Settings |
| **Right nav — "+Mark"** | Save bookmark — verse/range if selected, whole chapter if not |

---

### Settings

| Row | Action |
|-----|--------|
| **Font** | Tap to cycle: Small → Medium → Large |
| **Theme** | Tap to toggle: Dark / Light |
| **Brightness** | Tap **[−]** / **[+]** to adjust (20 levels, saved immediately) |
| **Translation** | Tap to cycle through detected `.xml` files on SD |
| **Highlight** | Tap **[<]** / **[>]** to cycle accent color (24 options) |
| **Boot OTA_1** | Sets `ota_1` as boot target and restarts into Marauder |
| **Calibrate Touch** | *(Resistive boards only)* Run TFT_eSPI touch calibration |
| **Back nav — "Back"** | Return to previous screen |

---

### Bookmarks

| Control | Action |
|---------|--------|
| **Tap bookmark** | Select it (highlight) |
| **Drag / fling** | Scroll the list |
| **Middle nav — "View"** | Jump to the selected bookmark |
| **Right nav — "Del"** | Delete the selected bookmark (confirmation popup) |
| **Left nav — "Back"** | Return to previous screen |

---

### Search

**Search History screen:**

| Control | Action |
|---------|--------|
| **Tap history item** | Select it |
| **Left nav — "New"** | Open keyboard to type a new search query |
| **Middle nav — "View"** | Run the selected history item as a search |
| **Right nav — "Del"** | Delete the selected history item (confirmation popup) |

**Keyboard / search options** (shown above the keyboard when typing):

| Option | Default | Description |
|--------|---------|-------------|
| **Partial Match** | On | All query words must appear somewhere in the verse (any order); off = exact phrase |
| **Ignore Punctuation** | On | Strip punctuation from verse and query before matching |
| **Scope** | Bible | Tap to cycle: Bible → Section → Book |

**Search Results screen:**

| Control | Action |
|---------|--------|
| **Tap result** | Select it (shows verse reference + snippet) |
| **Drag / fling** | Scroll the list |
| **Right nav — "View"** | Jump to the selected verse in the reading view |
| **Left nav — "Back"** | Return to Search History (scroll position preserved) |

---

## Troubleshooting

**"No Bible found on SD!"**
- Verify `/bible/` exists at the SD card root.
- Verify at least one `.xml` file is inside `/bible/`.
- Check SD card is FAT32 formatted.

**Verses missing / blank chapters**
- The XML parser reads up to 30 verses per chapter (`BIBLE_MAX_VERSES_CACHED`) on boards without PSRAM, or 200 with PSRAM (enough for Psalm 119).
- Some translations use non-standard OSIS book codes. Check that `osis_code` entries in the `BOOKS[]` table in `BibleInterface.cpp` match your XML file.

**Touch not responding**
- Pancake uses FT6336 (capacitive, I2C). Verify `HAS_CAP_TOUCH` is defined for Pancake.
- V8/V6.1 use XPT2046 (resistive, SPI). `tft.getTouch()` should work after `tft.init()`.
- Resistive boards: run **Settings → Calibrate Touch** if taps are offset.

**"Boot OTA_1" shows error**
- Flash Marauder to `ota_1` first using esptool or Marauder's OTA tool.
  If `ota_1` is empty, `esp_ota_set_boot_partition` will fail gracefully.

**Display colours wrong (V8)**
- `User_Setup_marauder_v8.h` sets `TFT_RGB_ORDER TFT_BGR` to correct the ILI9341
  colour swap. If you see red and blue swapped, verify this line is present.
