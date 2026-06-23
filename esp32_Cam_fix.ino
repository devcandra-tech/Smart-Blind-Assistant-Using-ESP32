/*
 * ============================================================
 *  ESP32-CAM — Kamera + Kirim Foto ke Telegram
 *  Menerima request dari ESP32 Master via HTTP lokal
 * ============================================================
 *
 *  LIBRARY (install via Library Manager):
 *    - UniversalTelegramBot by Brian Lough
 *    - ArduinoJson          by Benoit Blanchon (v6.x)
 *
 *  BOARD: "AI Thinker ESP32-CAM"
 *  Pilih board di Arduino IDE:
 *    Tools > Board > ESP32 Arduino > AI Thinker ESP32-CAM
 *
 *  UPLOAD via MB:
 *    Colok USB ke MB ESP32-CAM, pilih port yang muncul,
 *    upload langsung — tidak perlu jumper GPIO0
 *
 *  CATATAN PENTING:
 *    ESP32-CAM sekarang bisa diakses via: esp32cam.local
 *    Masukkan "esp32cam.local" ke CAM_IP di kode ESP32 Master
 *    (tidak perlu lihat IP lagi!)
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ─── KONFIGURASI — GANTI SESUAI DATA ANDA ───────────────────
const char* WIFI_SSID     = "Kurnia Juliza";
const char* WIFI_PASSWORD = "12345678910";
const char* BOT_TOKEN     = "8993347518:AAHnkvTKI7vb5fjqzpqUS19neu2MWb_weIE";
// ─────────────────────────────────────────────────────────────

// ─── Pin Kamera AI-Thinker ESP32-CAM ────────────────────────
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// LED Flash
#define FLASH_PIN          4

// Port HTTP server lokal
#define SERVER_PORT       80

WiFiServer    server(SERVER_PORT);
WiFiClientSecure tlsClient;
UniversalTelegramBot bot(BOT_TOKEN, tlsClient);

// ─── FUNGSI: Inisialisasi Kamera ─────────────────────────────
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Inisialisasi gagal: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_brightness(s, 1);
  s->set_contrast(s, 1);
  s->set_saturation(s, 0);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);

  // Rotasi 180 derajat (kamera terpasang terbalik)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);

  Serial.println("[CAM] Kamera siap!");
  return true;
}

// ─── FUNGSI: Sambung WiFi + mDNS ────────────────────────────
void connectWiFi() {
  Serial.print("[WiFi] Menghubungkan ke ");
  Serial.print(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempt++;
    if (attempt > 30) {
      Serial.println("\n[WiFi] Gagal! Restart...");
      ESP.restart();
    }
  }
  Serial.println("\n[WiFi] Terhubung!");
  Serial.print("[WiFi] IP ESP32-CAM: ");
  Serial.println(WiFi.localIP());

  // Daftarkan mDNS agar bisa diakses via esp32cam.local
  if (MDNS.begin("esp32cam")) {
    Serial.println("[mDNS] Berhasil! Akses via: esp32cam.local");
    Serial.println("[INFO] Masukkan 'esp32cam.local' ke CAM_IP di ESP32 Master");
  } else {
    Serial.println("[mDNS] Gagal start mDNS, gunakan IP di atas");
  }
}

// ─── FUNGSI: Ambil & Kirim Foto ke Telegram ──────────────────
bool ambilDanKirimFoto(String chat_id) {
  camera_fb_t* fb_flush = esp_camera_fb_get();
  if (fb_flush) esp_camera_fb_return(fb_flush);
  fb_flush = esp_camera_fb_get();
  if (fb_flush) esp_camera_fb_return(fb_flush);
  delay(150);

  digitalWrite(FLASH_PIN, HIGH);
  delay(150);

  camera_fb_t* fb = esp_camera_fb_get();
  digitalWrite(FLASH_PIN, LOW);

  if (!fb) {
    Serial.println("[CAM] Gagal ambil gambar!");
    bot.sendMessage(chat_id, "❌ Gagal mengambil foto. Coba lagi.", "");
    return false;
  }

  Serial.printf("[CAM] Foto diambil: %d bytes\n", fb->len);

  bool sukses = false;
  WiFiClientSecure sendClient;
  sendClient.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  if (sendClient.connect("api.telegram.org", 443)) {
    String boundary = "----ESP32CAMBoundary";
    String header = "--" + boundary + "\r\n";
    header += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
    header += chat_id + "\r\n";
    header += "--" + boundary + "\r\n";
    header += "Content-Disposition: form-data; name=\"photo\"; filename=\"foto.jpg\"\r\n";
    header += "Content-Type: image/jpeg\r\n\r\n";

    String footer = "\r\n--" + boundary + "--\r\n";

    int totalLen = header.length() + fb->len + footer.length();

    sendClient.print("POST /bot" + String(BOT_TOKEN) + "/sendPhoto HTTP/1.1\r\n");
    sendClient.print("Host: api.telegram.org\r\n");
    sendClient.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
    sendClient.print("Content-Length: " + String(totalLen) + "\r\n");
    sendClient.print("Connection: close\r\n\r\n");
    sendClient.print(header);

    uint8_t* imgData = fb->buf;
    size_t   imgLen  = fb->len;
    size_t   chunk   = 1024;
    while (imgLen > 0) {
      size_t toSend = min(chunk, imgLen);
      sendClient.write(imgData, toSend);
      imgData += toSend;
      imgLen  -= toSend;
    }

    sendClient.print(footer);

    unsigned long timeout = millis() + 5000;
    while (sendClient.connected() && millis() < timeout) {
      if (sendClient.available()) {
        String line = sendClient.readStringUntil('\n');
        if (line.indexOf("\"ok\":true") >= 0) {
          sukses = true;
          Serial.println("[CAM] Foto berhasil dikirim ke Telegram!");
          break;
        }
      }
    }
    sendClient.stop();
  } else {
    Serial.println("[CAM] Gagal connect ke Telegram API!");
    bot.sendMessage(chat_id, "❌ Gagal upload foto ke Telegram.", "");
  }

  esp_camera_fb_return(fb);
  return sukses;
}

// ─── FUNGSI: Parse chat_id dari URL ──────────────────────────
String parseChatId(String request) {
  int idx = request.indexOf("chat_id=");
  if (idx < 0) return "";
  int end = request.indexOf(' ', idx);
  if (end < 0) end = request.indexOf('\n', idx);
  return request.substring(idx + 8, end);
}

// ─── SETUP ───────────────────────────────────────────────────
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);

  Serial.println("==============================================");
  Serial.println("  ESP32-CAM — Kamera Telegram");
  Serial.println("==============================================");

  if (!initCamera()) {
    Serial.println("[CAM] ERROR: Kamera gagal init! Cek modul.");
    while (true) delay(1000);
  }

  connectWiFi();
  tlsClient.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  server.begin();
  Serial.printf("[Server] HTTP server berjalan di port %d\n", SERVER_PORT);
  Serial.println("[INFO] ESP32-CAM siap menerima perintah dari Master.");
}

// ─── LOOP ────────────────────────────────────────────────────
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Terputus! Reconnecting...");
    connectWiFi();
  }

  WiFiClient client = server.available();
  if (client) {
    Serial.println("[Server] Ada request masuk dari ESP32 Master");
    String request = "";
    unsigned long timeout = millis() + 2000;

    while (client.connected() && millis() < timeout) {
      if (client.available()) {
        char c = client.read();
        request += c;
        if (request.endsWith("\r\n\r\n")) break;
      }
    }

    Serial.println("[Server] Request: " + request.substring(0, 50));

    client.print("HTTP/1.1 200 OK\r\n");
    client.print("Content-Type: text/plain\r\n");
    client.print("Connection: close\r\n\r\n");
    client.print("OK - Mengambil foto...\r\n");
    client.stop();

    if (request.indexOf("GET /foto") >= 0) {
      String chat_id = parseChatId(request);
      if (chat_id.length() > 0) {
        Serial.println("[CAM] Ambil foto untuk chat_id: " + chat_id);
        ambilDanKirimFoto(chat_id);
      } else {
        Serial.println("[CAM] chat_id tidak ditemukan di request!");
      }
    }
  }
}