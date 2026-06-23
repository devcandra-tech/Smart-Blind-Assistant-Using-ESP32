/*sc
 * ============================================================
 *  ESP32 MASTER — Alat Bantu Tunanetra
 *  GPS + Kamera (via Telegram) | Ultrasonik + Buzzer (otomatis)
 * ============================================================
 */

#include "driver/gpio.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <TinyGPS++.h>
#include <HardwareSerial.h>

// ─── KONFIGURASI ─────────────────────────────────────────────
const char* WIFI_SSID     = "Kurnia Juliza";
const char* WIFI_PASSWORD = "12345678910";
const char* BOT_TOKEN     = "8993347518:AAHnkvTKI7vb5fjqzpqUS19neu2MWb_weIE";
const char* CAM_IP        = "esp32cam.local";
const int   CAM_PORT      = 80;

// Pin
#define GPS_RX_PIN  13
#define GPS_TX_PIN  17
#define GPS_BAUD    9600
#define BUZZER_PIN  4
#define LED_PIN     2

// Pin Ultrasonik
#define TRIG1 25
#define ECHO1 26
#define TRIG2 27
#define ECHO2 14
#define TRIG3 32
#define ECHO3 33
#define TRIG4 22
#define ECHO4 34

#define ZONA_BAHAYA       40
#define TELEGRAM_INTERVAL 1000

// ─── Objek Global ────────────────────────────────────────────
TinyGPSPlus          gps;
HardwareSerial       gpsSerial(2);
WiFiClientSecure     tlsClient;
UniversalTelegramBot bot(BOT_TOKEN, tlsClient);

unsigned long lastTelegramCheck = 0;

// GPS
bool    gpsHasFix = false;
double  lastLat = 0.0, lastLng = 0.0;
uint8_t lastSats = 0;
double  lastAlt = 0.0, lastSpeed = 0.0, lastHdop = 99.0;

// Sensor
float  dist[4]     = {999, 999, 999, 999};
int    trigPins[4] = {TRIG1, TRIG2, TRIG3, TRIG4};
int    echoPins[4] = {ECHO1, ECHO2, ECHO3, ECHO4};
String namaDir[4]  = {"Depan", "Belakang", "Kiri", "Kanan"};

// ─── 3 Titik Dummy Bergantian ─────────────────────────────────
struct TitikGPS {
  double  lat, lng, alt, speed, hdop;
  uint8_t sats;
  String  label;
};
TitikGPS dummyTitik[3] = {
  { -6.873570, 107.561900, 761.0, 0.5, 1.2, 8,  },
  { -6.875441, 107.560666, 762.0, 0.8, 1.1, 9,                            },
  { -6.873802, 107.561297, 760.0, 0.6, 1.3, 7,            },
};

int indexTitikDummy = 0;  // indeks bergantian global

// ─── WiFi ────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("[WiFi] Menghubungkan");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
    if (++attempt > 30) { Serial.println("\nGagal! Restart..."); ESP.restart(); }
  }
  Serial.println("\n[WiFi] IP: " + WiFi.localIP().toString());
}

// ─── GPS ─────────────────────────────────────────────────────
void readGPS() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());
  if (gps.location.isUpdated()) {
    gpsHasFix = gps.location.isValid();
    lastLat   = gps.location.lat();
    lastLng   = gps.location.lng();
    lastSats  = gps.satellites.isValid() ? gps.satellites.value() : 0;
    lastAlt   = gps.altitude.isValid()   ? gps.altitude.meters()  : 0.0;
    lastSpeed = gps.speed.isValid()      ? gps.speed.kmph()       : 0.0;
    lastHdop  = gps.hdop.isValid()       ? gps.hdop.hdop()        : 99.0;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
}

bool isGPSReady() {
  return gpsHasFix && lastLat != 0.0 && lastLng != 0.0
      && lastSats >= 4 && lastHdop < 5.0;
}

// ─── Ultrasonik ──────────────────────────────────────────────
float bacaJarak(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(4);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long dur = pulseIn(echo, HIGH, 8000);
  if (dur == 0) return 999.0f;
  float cm = (dur * 0.034f) / 2.0f;
  if (cm < 2.0f || cm > 200.0f) return 999.0f;
  return cm;
}

float bacaJarakStabil(int trig, int echo) {
  float samples[3];
  for (int j = 0; j < 3; j++) {
    samples[j] = bacaJarak(trig, echo);
    delayMicroseconds(500);
  }
  if (samples[0] > samples[1]) { float t = samples[0]; samples[0] = samples[1]; samples[1] = t; }
  if (samples[1] > samples[2]) { float t = samples[1]; samples[1] = samples[2]; samples[2] = t; }
  if (samples[0] > samples[1]) { float t = samples[0]; samples[0] = samples[1]; samples[1] = t; }
  return samples[1];
}

// ─── Buzzer ──────────────────────────────────────────────────
#define BIP_FREQ     1000
#define JARAK_MAX_CM 40

void bipBuzzer(int jumlah, int durasi_ms, int jeda_ms) {
  for (int i = 0; i < jumlah; i++) {
    tone(BUZZER_PIN, BIP_FREQ);
    vTaskDelay(pdMS_TO_TICKS(durasi_ms));
    noTone(BUZZER_PIN);
    if (i < jumlah - 1)
      vTaskDelay(pdMS_TO_TICKS(jeda_ms));
  }
}

// ─── Update Sonar + Buzzer ───────────────────────────────────
void updateSonarDanBuzzer() {
  static int konfirmasi[4] = {0, 0, 0, 0};

  for (int i = 0; i < 4; i++) {
    dist[i] = bacaJarakStabil(trigPins[i], echoPins[i]);
    if (dist[i] > 0 && dist[i] < JARAK_MAX_CM) {
      konfirmasi[i]++;
      if (konfirmasi[i] > 2) konfirmasi[i] = 2;
    } else {
      konfirmasi[i] = 0;
    }
  }

  int jumlahTerdeteksi = 0;
  for (int i = 0; i < 4; i++) {
    if (konfirmasi[i] >= 2) jumlahTerdeteksi++;
  }

  if (jumlahTerdeteksi == 0) {
    noTone(BUZZER_PIN);
    return;
  }

  if (jumlahTerdeteksi == 4) {
    tone(BUZZER_PIN, BIP_FREQ); vTaskDelay(pdMS_TO_TICKS(600));
    noTone(BUZZER_PIN);         vTaskDelay(pdMS_TO_TICKS(100));
    tone(BUZZER_PIN, BIP_FREQ); vTaskDelay(pdMS_TO_TICKS(600));
    noTone(BUZZER_PIN);
    return;
  }

  for (int i = 0; i < 4; i++) {
    if (konfirmasi[i] >= 2) {
      bipBuzzer(i + 1, 150, 100);
      vTaskDelay(pdMS_TO_TICKS(400));
      return;
    }
  }
}

// ─── Telegram: Kirim Lokasi ──────────────────────────────────
void sendLocation(String chat_id) {
  double  useLat, useLng, useAlt, useSpeed, useHdop;
  uint8_t useSats;
  String  sumberLabel;

  if (isGPSReady()) {
    // ── GPS sudah fix → pakai data real ──────────────────────
    useLat      = lastLat;
    useLng      = lastLng;
    useAlt      = lastAlt;
    useSpeed    = lastSpeed;
    useHdop     = lastHdop;
    useSats     = lastSats;
    sumberLabel = "📡 _Sumber: GPS Real_";
  } else {
    // ── GPS belum fix → pakai dummy bergantian ────────────────
    TitikGPS& t = dummyTitik[indexTitikDummy];
    useLat      = t.lat;
    useLng      = t.lng;
    useAlt      = t.alt;
    useSpeed    = t.speed;
    useHdop     = t.hdop;
    useSats     = t.sats;
    sumberLabel = "📌 _" + t.label + "_";

    indexTitikDummy = (indexTitikDummy + 1) % 3; // maju ke titik berikutnya
  }

  String msg = "📍 *Lokasi Pengguna*\n\n";
  msg += "🌐 Latitude  : `" + String(useLat,   6) + "`\n";
  msg += "🌐 Longitude : `" + String(useLng,   6) + "`\n";
  msg += "🛰 Satelit   : `" + String(useSats)     + "`\n";
  msg += "🎯 Akurasi   : `HDOP " + String(useHdop, 1) + "`\n";
  msg += "⛰ Altitude  : `" + String(useAlt,   1) + " m`\n";
  msg += "🚀 Kecepatan : `" + String(useSpeed, 1) + " km/h`\n\n";
  msg += "🗺 [Buka di Google Maps](https://maps.google.com/?q=";
  msg += String(useLat, 6) + "," + String(useLng, 6) + ")\n\n";
  msg += sumberLabel;
  bot.sendMessage(chat_id, msg, "Markdown");

  WiFiClientSecure lc;
  lc.setInsecure();
  if (lc.connect("api.telegram.org", 443)) {
    String url = "/bot" + String(BOT_TOKEN) + "/sendLocation?chat_id=" + chat_id
               + "&latitude="  + String(useLat, 6)
               + "&longitude=" + String(useLng, 6);
    lc.print("GET " + url + " HTTP/1.1\r\nHost: api.telegram.org\r\nConnection: close\r\n\r\n");
    delay(500); lc.stop();
  }
}

// ─── Telegram: Teruskan /foto ke ESP32-CAM ───────────────────
void teruskanFoto(String chat_id) {
  bot.sendMessage(chat_id, "📸 Mengambil foto...", "");
  WiFiClient cc;
  if (cc.connect(CAM_IP, CAM_PORT)) {
    cc.print("GET /foto?chat_id=" + chat_id + " HTTP/1.1\r\nHost: "
           + String(CAM_IP) + "\r\nConnection: close\r\n\r\n");
    delay(800); cc.stop();
    Serial.println("[CAM] Request foto dikirim.");
  } else {
    bot.sendMessage(chat_id, "❌ ESP32-CAM tidak merespons.", "");
  }
}

// ─── Telegram: Status Sonar ──────────────────────────────────
void sendStatusSonar(String chat_id) {
  String msg = "📡 *Status Sensor Jarak*\n\n";
  for (int i = 0; i < 4; i++) {
    String jarak = (dist[i] >= 999) ? "--" : String((int)dist[i]) + " cm";
    String zona  = (dist[i] < ZONA_BAHAYA) ? "🔴 Bahaya" : "🟢 Aman";
    msg += namaDir[i] + ": `" + jarak + "` " + zona + "\n";
  }
  msg += "\n_Bahaya jika ada sensor < 40cm_";
  bot.sendMessage(chat_id, msg, "Markdown");
}

// ─── Telegram: Handle Perintah ───────────────────────────────
void handleTelegramMessages(int n) {
  for (int i = 0; i < n; i++) {
    String chat_id = bot.messages[i].chat_id;
    String text    = bot.messages[i].text;
    String from    = bot.messages[i].from_name;
    Serial.println("[Telegram] " + from + ": " + text);

    if      (text == "/lokasi") { bot.sendChatAction(chat_id, "find_location"); sendLocation(chat_id); }
    else if (text == "/foto")   { bot.sendChatAction(chat_id, "upload_photo");  teruskanFoto(chat_id); }
    else if (text == "/sonar")  { sendStatusSonar(chat_id); }
    else if (text == "/gpsraw") {
      String msg = "🛰 *GPS Raw Data*\n\n";
      msg += "Chars processed: `" + String(gps.charsProcessed()) + "`\n";
      msg += "Passed checksum: `" + String(gps.passedChecksum()) + "`\n";
      msg += "Failed checksum: `" + String(gps.failedChecksum()) + "`\n";
      msg += "Satelit: `" + String(gps.satellites.value()) + "`";
      bot.sendMessage(chat_id, msg, "Markdown");
    }
    else if (text == "/status") {
      String s = "📊 *Status Sistem*\n\n";
      s += "📶 WiFi   : `" + WiFi.localIP().toString() + "`\n";
      s += "🛰 GPS    : `" + String(gpsHasFix ? "✅ Fix" : "❌ Belum") + "`\n";
      s += "🌐 Satelit: `" + String(lastSats) + "`\n";
      s += "🎯 HDOP   : `" + String(lastHdop, 1) + "`\n";
      s += "⏱ Uptime : `" + String(millis() / 1000) + " detik`";
      bot.sendMessage(chat_id, s, "Markdown");
    }
    else if (text == "/start" || text == "/help") {
      String h = "👋 Halo *" + from + "*!\n\n";
      h += "📍 /lokasi  — Koordinat GPS pengguna\n";
      h += "📸 /foto    — Ambil foto via kamera\n";
      h += "📡 /sonar   — Cek jarak semua sensor\n";
      h += "📊 /status  — Status sistem\n";
      h += "🛰 /gpsraw  — Data mentah GPS\n";
      h += "❓ /help    — Menu ini\n\n";
      h += "_Sensor & buzzer bekerja otomatis._";
      bot.sendMessage(chat_id, h, "Markdown");
    }
    else { bot.sendMessage(chat_id, "❓ Ketik /help untuk daftar perintah.", ""); }
  }
}

// ─── SETUP ───────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  for (int i = 0; i < 4; i++) {
    pinMode(trigPins[i], OUTPUT);
    pinMode(echoPins[i], INPUT);
  }

  Serial.println("==============================================");
  Serial.println("  Alat Bantu Tunanetra — ESP32 Master");
  Serial.println("==============================================");

  gpio_reset_pin(GPIO_NUM_13);
  pinMode(GPS_RX_PIN, INPUT);

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  connectWiFi();
  tlsClient.setInsecure();

  tone(BUZZER_PIN, 1500); delay(200); noTone(BUZZER_PIN); delay(100);
  tone(BUZZER_PIN, 2000); delay(300); noTone(BUZZER_PIN);
  digitalWrite(LED_PIN, HIGH);
  Serial.println("[OK] Siap!");

  xTaskCreatePinnedToCore(taskGPS,         "GPS",         4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(taskSonarBuzzer, "SonarBuzzer", 8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(taskTelegram,    "Telegram",    8192, NULL, 1, NULL, 1);
}

// ─── TASK GPS — Core 0, Prioritas Tertinggi ──────────────────
void taskGPS(void* parameter) {
  for (;;) {
    readGPS();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ─── TASK CORE 0: Sonar + Buzzer ─────────────────────────────
void taskSonarBuzzer(void* parameter) {
  for (int i = 0; i < 4; i++) {
    pinMode(trigPins[i], OUTPUT);
    pinMode(echoPins[i], INPUT);
  }
  pinMode(BUZZER_PIN, OUTPUT);

  for (;;) {
    updateSonarDanBuzzer();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ─── TASK CORE 1: Telegram + WiFi ────────────────────────────
void taskTelegram(void* parameter) {
  for (;;) {
    unsigned long now = millis();
    if (now - lastTelegramCheck >= TELEGRAM_INTERVAL) {
      lastTelegramCheck = now;
      if (WiFi.status() != WL_CONNECTED) connectWiFi();
      int n = bot.getUpdates(bot.last_message_received + 1);
      if (n > 0) handleTelegramMessages(n);
    }

    if (millis() > 30000 && gps.charsProcessed() < 10) {
      Serial.println("[GPS] ⚠️ Tidak ada data! Cek TX NEO-6M --> GPIO 13");
      static bool gpsDiagSent = false;
      if (!gpsDiagSent) {
        int n = bot.getUpdates(bot.last_message_received + 1);
        if (n > 0) {
          String chat_id = bot.messages[0].chat_id;
          bot.sendMessage(chat_id, "⚠️ *GPS Warning*\n\nTidak ada data dari NEO-6M!\nCek wiring: TX NEO-6M → GPIO 13", "Markdown");
          gpsDiagSent = true;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// loop() tidak dipakai
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
