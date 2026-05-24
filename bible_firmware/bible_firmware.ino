/* ============================================================================
   Marauder Bible Firmware — Standalone Bible Reader
   ============================================================================
   Separate firmware binary for OTA dual-boot alongside ESP32 Marauder.

   Flash layout (partitions.csv):
     ota_0  (0x20000,  3.5 MB) — this firmware (Bible reader)
     ota_1  (0x390000, 3.5 MB) — Marauder (flash separately)

   To flash:
     Flash THIS sketch to ota_0 (offset 0x20000).
     Flash Marauder binary to ota_1 (offset 0x390000).
     The "Boot Marauder" option in Settings calls esp_ota_set_boot_partition(ota_1)
     and restarts into Marauder. Marauder's OTA update feature switches back to ota_0.

   Board selection — edit ONE line in configs.h:
     #define MARAUDER_PANCAKE   — ST7796 320x480, FT6336 cap touch
     #define MARAUDER_V8        — ILI9341 240x320, XPT2046 resistive touch
     #define MARAUDER_V6_1      — ILI9341 240x320, XPT2046 resistive touch

   TFT_eSPI board selection — edit ONE line in
     libraries/TFT_eSPI-ESP32-C5/User_Setup_Select.h:
     #include <User_Setup_marauder_pancake.h>
     #include <User_Setup_marauder_v8.h>
     #include <User_Setup_og_marauder.h>

   SD card — copy OSIS XML Bible files to the /bible/ directory:
     /bible/asv.xml
     /bible/web.xml
     /bible/luth1912ap.xml   (etc.)

   Arduino IDE settings (ESP32-C5 boards — Pancake / V8):
     Board            : ESP32C5 Dev Module
     Flash Size       : 8MB
     Partition Scheme : Custom  →  select partitions.csv from this folder
     Flash Frequency  : 80 MHz

   Arduino IDE settings (original ESP32 — V6.1):
     Board            : LOLIN D32  (or equivalent ESP32 board)
     Partition Scheme : Custom  →  select partitions.csv from this folder
   ============================================================================ */

#include "configs.h"

#ifndef HAS_SCREEN
  #error "Bible firmware requires a display. Check configs.h board selection."
#endif

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#ifdef HAS_CAP_TOUCH
  #include "ft6336.h"
#endif

#include "BibleInterface.h"

// ── Brightness PWM (shared between blInit/blSet inside BibleInterface) ───────
// BL_CHANNEL not needed for Arduino 3.x (ledcAttach API) — handled in BibleInterface.cpp

// ── SD (shared SPI for ESP32-C5 boards) ─────────────────────────────────────
#ifdef HAS_C5_SD
  SPIClass sharedSPI(SPI);
#endif

// ─────────────────────────────────────────────────────────────────────────────
// setup()
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  randomSeed(esp_random());

  #ifndef DEVELOPER
    esp_log_level_set("*", ESP_LOG_NONE);
  #endif

  #ifndef HAS_IDF_3
    esp_spiram_init();
  #endif

  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println(F("[Bible] Firmware starting..."));

  // ── Backlight off during init ──────────────────────────────────────────
  #ifdef HAS_SCREEN
    pinMode(TFT_BL, OUTPUT);
    #ifndef HAS_MINI_SCREEN
      #if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcAttach(TFT_BL, 5000, 8);
        ledcWrite(TFT_BL, 0);
      #else
        ledcSetup(0, 5000, 8);
        ledcAttachPin(TFT_BL, 0);
        ledcWrite(0, 0);
      #endif
    #else
      digitalWrite(TFT_BL, LOW);
    #endif
  #endif

  // ── SD init ───────────────────────────────────────────────────────────
  #ifdef HAS_C5_SD
    sharedSPI.begin(SD_SCK, SD_MISO, SD_MOSI);
    delay(100);
    if (!SD.begin(SD_CS, sharedSPI)) {
      Serial.println(F("[Bible] SD init failed"));
    } else {
      Serial.println(F("[Bible] SD OK"));
    }
  #else
    if (!SD.begin(SD_CS)) {
      Serial.println(F("[Bible] SD init failed"));
    } else {
      Serial.println(F("[Bible] SD OK"));
    }
  #endif

  // ── PSRAM ─────────────────────────────────────────────────────────────
  #ifdef HAS_PSRAM
    if (!psramInit()) Serial.println(F("[Bible] PSRAM unavailable"));
  #endif

  // ── Bible reader init (display, touch, NVS, XML scan) ─────────────────
  bible_obj.RunSetup();

  Serial.println(F("[Bible] Ready."));
}

// ─────────────────────────────────────────────────────────────────────────────
// loop()
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  bible_obj.main(millis());
  delay(10);
}
