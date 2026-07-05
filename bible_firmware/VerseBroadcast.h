// VerseBroadcast.h — optional "broadcast a Bible verse over the air" novelty.
// ─────────────────────────────────────────────────────────────────────────────
// Splits the selected verse into small chunks and either beacon-spams them as WiFi
// AP SSIDs (raw 802.11 frames) or advertises them as BLE device names (NimBLE).
// The radio approach mirrors ESP32Marauder, which runs on this same hardware.
//
// SELF-CONTAINED so it's easy to remove later: delete VerseBroadcast.h +
// VerseBroadcast.cpp and the small `#ifdef ENABLE_VERSE_BROADCAST` blocks in
// BibleInterface (search for that macro). Comment out the #define below to compile
// the feature out entirely — the firmware then links without WiFi/NimBLE.
#pragma once

#define ENABLE_VERSE_BROADCAST      // ← comment out to remove the feature

#ifdef ENABLE_VERSE_BROADCAST
#include <stdint.h>

namespace VerseBroadcast {

// A single SSID / BLE-name is limited so it fits both a WiFi SSID (<=32 bytes) and
// a BLE advertising payload (complete-local-name AD, ~29 usable bytes). 29 satisfies
// both; chunks never split a UTF-8 multibyte sequence.
static const int CHUNK_BYTES = 29;
static const int CHUNK_CAP   = 30;     // CHUNK_BYTES + NUL
static const int MAX_CHUNKS  = 48;     // enough for a long verse

// Split UTF-8 `text` into <=max_len-byte word-boundary chunks (NUL-terminated in
// out[]; max_len clamped to CHUNK_BYTES). Returns the chunk count (<= max_chunks).
// Callers that add a numbering prefix pass a smaller max_len to leave room for it.
int splitChunks(const char* text, char out[][CHUNK_CAP], int max_chunks, int max_len);

// ── WiFi beacon spam (raw AP beacons) ───────────────────────────────────────
void wifiBegin();                       // bring WiFi up in AP mode for raw TX
int  wifiSendSSID(const char* ssid);    // broadcast one SSID; returns beacon frames sent
void wifiEnd();                         // tear WiFi back down

// ── BLE name advertising (NimBLE) ───────────────────────────────────────────
void bleBegin();
int  bleSetName(const char* name);      // (re)start advertising; returns adverts started (1)
void bleEnd();

}  // namespace VerseBroadcast
#endif // ENABLE_VERSE_BROADCAST
