#pragma once

#ifndef configs_h
#define configs_h

// ─────────────────────────────────────────────────────────────────────────────
// Board selection — uncomment exactly ONE
// ─────────────────────────────────────────────────────────────────────────────
// #define MARAUDER_V6_1      // Original ESP32   — ILI9341 240×320, XPT2046 resistive touch
// #define MARAUDER_V8        // ESP32-C5          — ILI9341 240×320, XPT2046 resistive touch
 #define MARAUDER_PANCAKE   // ESP32-C5          — ST7796  320×480, FT6336  capacitive touch

// Uncomment to enable verbose serial logging (suppressed in release builds)
// #define DEVELOPER

// ─────────────────────────────────────────────────────────────────────────────
// MARAUDER_V6_1
//   Original ESP32, no PSRAM, default VSPI bus for SD (SD_CS only needed).
// ─────────────────────────────────────────────────────────────────────────────
#ifdef MARAUDER_V6_1
  #define HAS_SCREEN
  #define HAS_FULL_SCREEN
  #define HAS_TOUCH           // XPT2046 resistive — calibration in RunSetup
  #define HAS_SD
  #define USE_SD
  #define HAS_IDF_3           // skip explicit esp_spiram_init(); no PSRAM on V6.1
  #define HAS_BATTERY         // MAX17048 fuel gauge on I2C (SDA=33, SCL=22)

  #define TFT_WIDTH  240
  #define TFT_HEIGHT 320
  #define SCREEN_WIDTH  TFT_WIDTH
  #define SCREEN_HEIGHT TFT_HEIGHT

  #define SD_CS  14           // SD chip-select; SPI pins are default VSPI

  #define I2C_SDA 33
  #define I2C_SCL 22
#endif

// ─────────────────────────────────────────────────────────────────────────────
// MARAUDER_V8
//   ESP32-C5, shared FSPI bus (TFT + SD + XPT2046), PSRAM.
// ─────────────────────────────────────────────────────────────────────────────
#ifdef MARAUDER_V8
  #define HAS_SCREEN
  #define HAS_FULL_SCREEN
  #define HAS_TOUCH           // XPT2046 resistive — calibration in RunSetup
  #define HAS_SD
  #define USE_SD
  #define HAS_C5_SD           // explicit SPIClass init required before SD.begin()
  #define HAS_PSRAM
  #define HAS_IDF_3           // ESP32-C5 uses IDF 5.x; psramInit() handles PSRAM
  #define HAS_BATTERY         // MAX17048 fuel gauge on I2C bus

  #define TFT_WIDTH  240
  #define TFT_HEIGHT 320
  #define SCREEN_WIDTH  TFT_WIDTH
  #define SCREEN_HEIGHT TFT_HEIGHT

  #define SD_CS   10          // SD chip-select
  #define SD_MISO TFT_MISO   // shared FSPI bus (defined in TFT_eSPI User_Setup)
  #define SD_MOSI TFT_MOSI
  #define SD_SCK  TFT_SCLK

  #define I2C_SCL 4
  #define I2C_SDA 5
#endif

// ─────────────────────────────────────────────────────────────────────────────
// MARAUDER_PANCAKE
//   ESP32-C5, shared FSPI bus (TFT + SD), FT6336 capacitive touch via I2C, PSRAM.
// ─────────────────────────────────────────────────────────────────────────────
#ifdef MARAUDER_PANCAKE
  #define HAS_SCREEN
  #define HAS_FULL_SCREEN
  #define HAS_TOUCH
  #define HAS_CAP_TOUCH       // FT6336 capacitive — no calibration needed
  #define HAS_SD
  #define USE_SD
  #define HAS_C5_SD           // explicit SPIClass init required before SD.begin()
  #define HAS_PSRAM
  #define HAS_IDF_3           // ESP32-C5 uses IDF 5.x; psramInit() handles PSRAM

  #define TFT_WIDTH  320
  #define TFT_HEIGHT 480
  #define SCREEN_WIDTH  TFT_WIDTH
  #define SCREEN_HEIGHT TFT_HEIGHT

  #define SD_CS   7           // SD chip-select
  #define SD_MISO TFT_MISO   // shared FSPI bus (defined in TFT_eSPI User_Setup)
  #define SD_MOSI TFT_MOSI
  #define SD_SCK  TFT_SCLK

  #define I2C_SDA  9
  #define I2C_SCL 10
  // FT6336 capacitive touch controller (shares I2C bus)
  #define CTP_RST  8
  #define CTP_SDA  I2C_SDA
  #define CTP_SCL  I2C_SCL
  #define HAS_BATTERY         // MAX17048 fuel gauge on shared I2C bus
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Sanity check
// ─────────────────────────────────────────────────────────────────────────────
#ifndef HAS_SCREEN
  #error "No board selected — uncomment one MARAUDER_* define above in configs.h"
#endif

#endif // configs_h
