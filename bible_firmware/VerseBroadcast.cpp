// VerseBroadcast.cpp — see VerseBroadcast.h. Radio code mirrors ESP32Marauder.
#include "VerseBroadcast.h"
#ifdef ENABLE_VERSE_BROADCAST

#include <Arduino.h>
#include <string.h>

// WiFi (raw 802.11 beacon TX)
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>

// BLE (NimBLE-Arduino, same library the Marauder firmware uses)
#include <NimBLEDevice.h>

namespace VerseBroadcast {

// ─────────────────────────────────────────────────────────────────────────────
// Chunk splitter
// ─────────────────────────────────────────────────────────────────────────────
int splitChunks(const char* text, char out[][CHUNK_CAP], int max_chunks, int max_len) {
    if (!text) { if (max_chunks > 0) out[0][0] = 0; return 0; }
    if (max_len <= 0 || max_len > CHUNK_BYTES) max_len = CHUNK_BYTES;
    int n = 0;
    const char* p = text;
    while (*p && n < max_chunks) {
        while (*p == ' ') p++;                 // skip leading spaces
        if (!*p) break;

        int len = 0, last_space = -1;
        while (p[len] && len < max_len) {
            if (p[len] == ' ') last_space = len;
            len++;
        }
        // Prefer to break at the last space if the word is cut mid-way.
        if (p[len] && last_space > 0) len = last_space;
        // Never cut a UTF-8 multibyte sequence: back off any trailing continuation.
        while (len > 0 && (uint8_t)p[len] >= 0x80 && (uint8_t)p[len] < 0xC0) len--;
        if (len <= 0) len = 1;
        if (len > max_len) len = max_len;

        memcpy(out[n], p, len);
        out[n][len] = 0;
        n++;
        p += len;
    }
    if (n == 0 && max_chunks > 0) { out[0][0] = 0; }
    return n;
}

// ─────────────────────────────────────────────────────────────────────────────
// WiFi beacon spam — mirrors ESP32Marauder::broadcastSetSSID
// ─────────────────────────────────────────────────────────────────────────────
// 802.11 beacon template. SSID element is appended at offset 36 (0x00 id, [37]=len,
// [38..]=ssid), followed by supported-rates + DSSS(current channel).
static uint8_t beacon[128] = {
    0x80, 0x00, 0x00, 0x00,                            // frame control, duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,                // dest = broadcast
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,                // src   (randomised each frame)
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,                // bssid (randomised each frame)
    0xc0, 0x6c,                                        // seq-ctl
    0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00, 0x00, 0x00,    // timestamp
    0x64, 0x00,                                        // beacon interval
    0x31, 0x00,                                        // capability info
    0x00                                               // SSID element id
};

void wifiBegin() {
    // These are idempotent — return an error (ignored) if the Arduino core already
    // set them up. Needed because the Bible firmware never otherwise starts WiFi.
    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.ap.ssid_hidden     = 1;
    ap.ap.beacon_interval = 10000;
    ap.ap.ssid_len        = 0;
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    esp_wifi_start();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_max_tx_power(82);
}

void wifiSendSSID(const char* ssid) {
    uint8_t ch = (uint8_t)random(1, 12);
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    delay(1);

    // Randomise source + BSSID MAC so each SSID looks like its own AP.
    for (int k = 0; k < 6; k++)
        beacon[10 + k] = beacon[16 + k] = (uint8_t)random(256);

    int len = (int)strlen(ssid);
    if (len > 32) len = 32;
    beacon[37] = (uint8_t)len;
    for (int i = 0; i < len; i++) beacon[38 + i] = (uint8_t)ssid[i];

    // Post-SSID tagged params: supported rates + DSSS (current channel).
    static const uint8_t post[] = {
        0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c,  // supported rates
        0x03, 0x01, 0x04                                             // DSSS (chan, set below)
    };
    for (unsigned i = 0; i < sizeof(post); i++) beacon[38 + len + i] = post[i];
    beacon[38 + len + (int)sizeof(post) - 1] = ch;                   // real current channel

    int frame_len = 38 + len + (int)sizeof(post);
    esp_wifi_80211_tx(WIFI_IF_AP, beacon, frame_len, false);
    esp_wifi_80211_tx(WIFI_IF_AP, beacon, frame_len, false);
    esp_wifi_80211_tx(WIFI_IF_AP, beacon, frame_len, false);
}

void wifiEnd() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();
}

// ─────────────────────────────────────────────────────────────────────────────
// BLE name advertising — mirrors ESP32Marauder's NimBLE advertising setup
// ─────────────────────────────────────────────────────────────────────────────
static NimBLEAdvertising* pAdv = nullptr;

void bleBegin() {
    NimBLEDevice::init("");
    NimBLEServer* server = NimBLEDevice::createServer();
    pAdv = server->getAdvertising();
}

void bleSetName(const char* name) {
    if (!pAdv) return;
    pAdv->stop();
    NimBLEAdvertisementData data;
    data.setName(std::string(name));
    pAdv->setAdvertisementData(data);
    pAdv->start();
}

void bleEnd() {
    if (pAdv) { pAdv->stop(); pAdv = nullptr; }
    NimBLEDevice::deinit(true);
}

}  // namespace VerseBroadcast
#endif // ENABLE_VERSE_BROADCAST
