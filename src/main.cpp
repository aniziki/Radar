/*
 * BadgeTest — ROM-fit / hardware bring-up check
 *
 * Target: ESP32-S3-MINI-1-N8  (8 MB flash, no PSRAM)
 *
 * BOM:
 *   Display  CFAF240240A4-013TN  ST7789H2  SPI
 *   IMU      ICM-20948           I2C
 *   Fuel     MAX17048G+T10       I2C (shared bus)
 *   BLE      NimBLE              all 4 roles
 *   Comms    ESP-NOW + WiFi
 *   Crypto   mbedtls HMAC-SHA256
 *   NVS      Preferences
 *   QR       libqrencode         [copy source → lib/libqrencode/]
 *
 * Pins (set in platformio.ini, adjust to PCB):
 *   I2C  SDA=8   SCL=9
 *   SPI  MOSI=11 SCLK=12 CS=10 DC=13 RST=14 BL=15
 *
 * Build output targets:
 *   RAM   < 327680 bytes (320 KB)
 *   Flash < 8388608 bytes (8 MB)
 */

#include <Arduino.h>

// BLE
#include <NimBLEDevice.h>

// ESP-NOW — WiFi must init before esp_now_init()
#include <WiFi.h>
#include <esp_now.h>

// NVS
#include <Preferences.h>

// HMAC-SHA256
#include <mbedtls/md.h>

// IMU
#include <ICM_20948.h>

// Battery gauge
#include <Adafruit_MAX1704X.h>

// Display — copy st7789h2.cpp/.h into lib/st7789h2/ then uncomment
// #include "st7789h2.h"

// QR — copy libqrencode source into lib/libqrencode/ then uncomment
// #include "qrencode.h"

// AP anchor map for WiFi positioning
// #include "ap_anchor.h"

Adafruit_MAX17048 battery;
ICM_20948_I2C     imu;
Preferences       prefs;

// ESP-NOW recv — Arduino ESP32 3.x uses the old IDF 4.x callback signature
static void on_espnow_recv(const uint8_t *mac, const uint8_t *data, int len)
{
    Serial.printf("[ESP-NOW] rx %d bytes from %02x:%02x:%02x:%02x:%02x:%02x\n",
                  len, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== MechBadge ROM-fit check ===");
    Serial.printf("Chip: %s  Rev: %d  Cores: %d  Freq: %d MHz\n",
                  ESP.getChipModel(), ESP.getChipRevision(),
                  ESP.getChipCores(), ESP.getCpuFreqMHz());
    Serial.printf("Flash: %lu KB   Heap: %lu KB\n",
                  (unsigned long)ESP.getFlashChipSize() / 1024,
                  (unsigned long)ESP.getFreeHeap()      / 1024);

    // I2C — shared by ICM-20948 and MAX17048
    Wire.begin(PIN_SDA, PIN_SCL);
    Serial.printf("[OK] I2C  SDA=%d SCL=%d\n", PIN_SDA, PIN_SCL);

    // MAX17048 battery gauge (0x36)
    if (!battery.begin()) {
        Serial.println("[WARN] MAX17048 not found");
    } else {
        Serial.printf("[OK] MAX17048  %.1f%%  %.3fV\n",
                      battery.cellPercent(), battery.cellVoltage());
    }

    // ICM-20948 (AD0 high = 0x69)
    imu.begin(Wire, 1);
    if (imu.status != ICM_20948_Stat_Ok) {
        Serial.println("[WARN] ICM-20948 not found");
    } else {
        Serial.println("[OK] ICM-20948  accel+gyro+mag ready");
    }

    // SPI display bus — driver not yet in lib/st7789h2/, bus init only
    SPI.begin(PIN_DISP_SCLK, /*MISO=*/-1, PIN_DISP_MOSI, PIN_DISP_CS);
    pinMode(PIN_DISP_DC,  OUTPUT);
    pinMode(PIN_DISP_RST, OUTPUT);
    pinMode(PIN_DISP_BL,  OUTPUT);
    digitalWrite(PIN_DISP_RST, HIGH);
    digitalWrite(PIN_DISP_BL,  HIGH);
    Serial.printf("[OK] SPI  MOSI=%d SCLK=%d CS=%d DC=%d RST=%d BL=%d\n",
                  PIN_DISP_MOSI, PIN_DISP_SCLK, PIN_DISP_CS,
                  PIN_DISP_DC, PIN_DISP_RST, PIN_DISP_BL);

    // NVS
    prefs.begin("badge", /*readOnly=*/true);
    uint32_t player_id = prefs.getUInt("player_id", BADGE_PLAYER_ID);
    uint8_t  faction   = prefs.getUChar("faction",   BADGE_FACTION_ID);
    prefs.end();
    Serial.printf("[OK] NVS  player_id=0x%08lX  faction=0x%02X\n",
                  (unsigned long)player_id, faction);

    // WiFi STA — required before esp_now_init()
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    Serial.printf("[OK] WiFi STA  MAC: %s\n", WiFi.macAddress().c_str());

    // ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("[WARN] esp_now_init failed");
    } else {
        esp_now_register_recv_cb(on_espnow_recv);
        Serial.println("[OK] ESP-NOW ready");
    }

    // HMAC-SHA256 smoke test
    {
        const uint8_t key[] = "BADGE_HMAC_KEY__";
        const uint8_t msg[] = "ping";
        uint8_t       out[32];
        mbedtls_md_context_t    ctx;
        const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, info, 1);
        mbedtls_md_hmac_starts(&ctx, key, sizeof(key) - 1);
        mbedtls_md_hmac_update(&ctx, msg, sizeof(msg) - 1);
        mbedtls_md_hmac_finish(&ctx, out);
        mbedtls_md_free(&ctx);
        Serial.printf("[OK] HMAC-SHA256: %02x%02x..%02x%02x\n",
                      out[0], out[1], out[30], out[31]);
    }

    // NimBLE
    NimBLEDevice::init("MechBadge");
    Serial.println("[OK] NimBLE ready");

    Serial.println("=== done ===");
    Serial.printf("heap: %lu bytes free\n", (unsigned long)ESP.getFreeHeap());
}

void loop()
{
    delay(5000);
    Serial.printf("heap: %lu bytes free\n", (unsigned long)ESP.getFreeHeap());
}