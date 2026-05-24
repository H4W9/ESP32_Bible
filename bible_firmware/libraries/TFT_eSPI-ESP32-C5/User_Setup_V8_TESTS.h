// ============================================================
// TFT_eSPI User Setup for Marauder V8 (ESP32-C5 + ILI9341)
//
// HOW TO USE:
//   Replace the existing User_Setup.h in your TFT_eSPI library
//   folder with this file. Library folder is usually:
//     ~/Documents/Arduino/libraries/TFT_eSPI/User_Setup.h
//
// Confirmed V8 pins (voltmeter + brute-force scan):
//   TFT_MOSI  = GPIO7   (FSPI MOSI, shared with SD + touch)
//   TFT_MISO  = GPIO2   (FSPI MISO, shared with SD + touch)
//   TFT_SCLK  = GPIO6   (FSPI SCLK, shared with SD + touch)
//   TFT_CS    = GPIO23
//   TFT_DC    = GPIO24
//   TFT_RST   = -1      (hardware tied to EN circuit, no GPIO)
//   TFT_BL    = GPIO8   (PWM backlight, controlled in sketch)
//   TOUCH_CS  = GPIO3   (XPT2046, confirmed by touch test)
//   SD_CS     = GPIO10
//
// Color note:
//   Display panel uses RGB order (not BGR). The ILI9341 default
//   MADCTL BGR=1 causes R/B swap (RED displays as BLUE).
//   TFT_RGB_ORDER TFT_RGB below clears the BGR bit to fix this.
// ============================================================

#define USER_SETUP_INFO "Marauder V8 ESP32-C5 ILI9341"

// Driver
#define ILI9341_DRIVER

// Physical panel dimensions
#define TFT_WIDTH   240
#define TFT_HEIGHT  320

// SPI pins
#define TFT_MOSI   7
#define TFT_MISO   2
#define TFT_SCLK   6
#define TFT_CS    23
#define TFT_DC    24
#define TFT_RST   -1   // Hardware tied to EN — no GPIO control needed

// Backlight controlled in sketch via analogWrite/ledcWrite
// #define TFT_BL  8
// #define TFT_BACKLIGHT_ON HIGH

// Touch controller (XPT2046, shares SPI bus)
#define TOUCH_CS   3

// Fix R/B color swap:
// ILI9341 default MADCTL has BGR=1, causing R/B swap on display.
// Telling TFT_eSPI to pre-swap (TFT_BGR) cancels it out.
#define TFT_RGB_ORDER TFT_BGR

// SPI clock frequencies
#define SPI_FREQUENCY       10000000   // 10 MHz write
#define SPI_READ_FREQUENCY  10000000
#define SPI_TOUCH_FREQUENCY  2500000   // 2.5 MHz touch reads

// Fonts
#define LOAD_GLCD    // Font 1
#define LOAD_FONT2   // Font 2
#define LOAD_FONT4   // Font 4
#define LOAD_FONT6   // Font 6 (digits only)
#define LOAD_FONT7   // Font 7 (digits only)
#define LOAD_FONT8   // Font 8 (digits only)
#define LOAD_GFXFF   // FreeFonts

#define SMOOTH_FONT
