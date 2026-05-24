# Marauder Bible Firmware — Setup Guide

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
- In the Bible app's **Settings → Boot Marauder** restarts into Marauder.
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

Available OSIS XML source files (in `FZ_Bible_App/sd_directory_builder/xml/`):

| File | Translation | Language |
|------|-------------|----------|
| `asv.xml` | American Standard Version | English |
| `bible.akjv.xml` | American King James Version | English |
| `web.xml` | World English Bible | English |
| `mkjv1962.xml` | Modern King James Version | English |
| `luth1912ap.xml` | Luther Bibel 1912 (with Apocrypha) | German |
| `sch1951.xml` | Schlachter 1951 | German |

Copy as many as you want — the app detects them all and lets you switch between them.

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

### Navigation (all screens)
- **Tap `<` (top-left header)** → go back
- **Tap right nav button** → cycle brightness
- **Tap left nav button** → bookmarks (from reading view or section screen)
- **Tap center nav button** → settings

### Book / Chapter Selection
- **Tap item** → open it
- List scrolls automatically based on touch position

### Reading View
- **Tap upper half of text area** → previous page (or previous chapter)
- **Tap lower half of text area** → next page (or next chapter)
- **Right nav** → add bookmark for current chapter

### Settings
| Row | Action |
|-----|--------|
| Font | Tap to cycle: Small → Medium → Large |
| Theme | Tap to toggle Dark / Light |
| Brightness | Tap to cycle through 10 levels (saved to NVS) |
| Translation | Tap to cycle if multiple .xml files are on SD |
| Boot Marauder | Sets ota_1 as boot target and restarts |
| Back | Return to reading |

### Bookmarks
- **Tap bookmark** → jump to that chapter
- **Del (right nav)** → delete selected bookmark

---

## Troubleshooting

**"No Bible found on SD!"**
- Verify `/bible/` exists at the SD card root.
- Verify at least one `.xml` file is inside `/bible/`.
- Check SD card is FAT32 formatted.

**Verses missing / blank chapters**
- The XML parser reads up to 30 verses per chapter (`BIBLE_MAX_VERSES_CACHED`).
  Very long chapters (e.g. Psalm 119 = 176 verses) show only the first 30.
  Increase `BIBLE_MAX_VERSES_CACHED` in `BibleInterface.h` if your board has PSRAM.
- Some translations use non-standard OSIS book codes. Check that `osis_code` entries
  in the `BOOKS[]` table in `BibleInterface.cpp` match your XML file.

**Touch not responding**
- Pancake uses FT6336 (capacitive, I2C). Verify `HAS_CAP_TOUCH` is defined for Pancake.
- V8/V6.1 use XPT2046 (resistive, SPI). `tft.getTouch()` should work after `tft.init()`.
- Calibrate resistive touch if taps are offset (call `tft.calibrateTouch()` once).

**"Boot Marauder" shows error**
- Flash Marauder to `ota_1` first using esptool or Marauder's OTA tool.
  If `ota_1` is empty, `esp_ota_set_boot_partition` will fail gracefully.

**Display colours wrong (V8)**
- `User_Setup_marauder_v8.h` sets `TFT_RGB_ORDER TFT_BGR` to correct the ILI9341
  colour swap. If you see red and blue swapped, verify this line is present.
