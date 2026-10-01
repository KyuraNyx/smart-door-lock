/* ====================================================================
 * Project     : Smart Door Lock with Edge AI Face Recognition & Telegram Telemetry
 * Platform    : ESP32-S3 WROOM N16R8 CAM + OV3660 + Keypad 4x4 + OLED 1.3"
 * 
 * Contributors:
 * - Hayqal Husein Alhabsyi
 * - Rizky Mardhani
 * - Mochammad Salman Al Farizy
 * - Muhammad Minanur Rohman
 * ==================================================================== */

#include "esp_camera.h"
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"
#include "face_recognition_112_v1_s8.hpp"
#include <Keypad.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_http_server.h"

// PUSTAKA TAMBAHAN UNTUK UI (OLED & I2C)
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// ======================================================================
// KONFIGURASI JARINGAN & KREDENSIAL
// ======================================================================
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define BOT_TOKEN "YOUR_TELEGRAM_BOT_TOKEN"
#define CHAT_ID "YOUR_TELEGRAM_CHAT_ID"

#define PIN_ALTERNATIF "1234"
#define PIN_ADMIN "9999"

// ======================================================================
// DEFINISI PIN PERANGKAT KERAS (REVISI FINAL)
// ======================================================================
#define SDA_PIN      39  // I2C Layar OLED
#define SCL_PIN      40  // I2C Layar OLED
#define RELAY_PIN    48  // Ke Transistor 2N2222 (Solenoid)
#define BUZZER_PIN   14  // Pengganti LED Merah (Audio Feedback)
#define MC38_PIN     38  // Sensor Magnetik Pintu
#define EXIT_BTN_PIN 46  // Push Button Pintu Dalam

// CAMERA PINS — ESP32-S3 WROOM N16R8 CAM
#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 15
#define SIOD_GPIO_NUM 4
#define SIOC_GPIO_NUM 5
#define Y9_GPIO_NUM 16
#define Y8_GPIO_NUM 17
#define Y7_GPIO_NUM 18
#define Y6_GPIO_NUM 12
#define Y5_GPIO_NUM 10
#define Y4_GPIO_NUM 8
#define Y3_GPIO_NUM 9
#define Y2_GPIO_NUM 11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 7
#define PCLK_GPIO_NUM 13

// ======================================================================
// INISIALISASI OBJEK (AI, OLED, KEYPAD)
// ======================================================================
HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);
FaceRecognition112V1S8 recognizer;

// OLED 1.3"
#define i2c_Address 0x3c
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// KEYPAD 4x4
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};
byte rowPins[ROWS] = { 1, 2, 42, 41 };
byte colPins[COLS] = { 47, 21, 20, 19 };
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ======================================================================
// VARIABEL STATE MACHINE & LOGIKA PINTU
// ======================================================================
typedef enum {
  STATE_MONITORING,
  STATE_INPUT_PIN,
  STATE_INPUT_PIN_ADMIN,
  STATE_FACE_ENROLLMENT,
  STATE_DOOR_OPEN,
  STATE_LIGHT_SLEEP
} SystemState;
SystemState currentState = STATE_MONITORING;

String inputBuffer = "";
unsigned long doorOpenTime = 0;
unsigned long lastActivityTime = 0;
const unsigned long LOCK_DELAY = 5000;   // Waktu jeda mengunci (5 Detik)
const unsigned long IDLE_TIMEOUT = 30000; 
const unsigned long PIN_TIMEOUT = 15000;

httpd_handle_t stream_httpd = NULL;

// ======================================================================
// FUNGSI UI BANTUAN (OLED)
// ======================================================================
void updateOLED(String baris1, String baris2, int textSize) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.print(baris1);
  
  display.setTextSize(textSize);
  display.setCursor(0, 20);
  display.print(baris2);
  display.display();
}

// ======================================================================
// FUNGSI INISIALISASI (Kamera, WiFi, Face Recog)
// ======================================================================
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_240X240;
    config.jpeg_quality = 15;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_240X240;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) return false;

  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
  return true;
}

void initFaceRecognition() {
  recognizer.set_partition(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fr");
  recognizer.set_ids_from_flash();
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  updateOLED("Sistem", "Koneksi WiFi", 1);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    // Ambil IP Address
    String ipAddress = WiFi.localIP().toString();
    
    // 1. Cetak ke Serial Monitor laptop
    Serial.println("\n[WiFi] Terhubung!");
    Serial.println("[STREAM] Buka browser dan ketik: http://" + ipAddress);
  } else {
    Serial.println("\n[WiFi] Gagal terhubung.");
    updateOLED("WiFi", "Gagal", 2);
    delay(2000);
  }
}

// ======================================================================
// FUNGSI TELEGRAM & AI PROCESSING
// ======================================================================
void sendTelegramAlert(camera_fb_t *fb) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  updateOLED("PERINGATAN", "Kirim Foto!", 1);
  tone(BUZZER_PIN, 800, 500); // Nada alarm Telegram
  
  WiFiClientSecure client;
  client.setInsecure(); // Abaikan sertifikat SSL

  Serial.println("[Telegram] Menghubungkan ke server...");
  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("[Telegram] Gagal terhubung ke server!");
    return;
  }

  // Siapkan bungkus data (Headers & Boundaries)
  String boundary = "ESP32BoundaryXYZ";
  String bodyHead = "--" + boundary + "\r\n";
  bodyHead += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
  bodyHead += String(CHAT_ID) + "\r\n";
  bodyHead += "--" + boundary + "\r\n";
  bodyHead += "Content-Disposition: form-data; name=\"caption\"\r\n\r\n\xE2\x9A\xA0 PERINGATAN: Penyusup!\r\n";
  bodyHead += "--" + boundary + "\r\n";
  bodyHead += "Content-Disposition: form-data; name=\"photo\"; filename=\"penyusup.jpg\"\r\n";
  bodyHead += "Content-Type: image/jpeg\r\n\r\n";

  String bodyTail = "\r\n--" + boundary + "--\r\n";

  // Hitung total panjang data yang akan dikirim
  size_t totalLen = bodyHead.length() + fb->len + bodyTail.length();

  // 1. Kirim HTTP POST Request secara manual
  client.println("POST /bot" + String(BOT_TOKEN) + "/sendPhoto HTTP/1.1");
  client.println("Host: api.telegram.org");
  client.println("Content-Length: " + String(totalLen));
  client.println("Content-Type: multipart/form-data; boundary=" + boundary);
  client.println("Connection: close");
  client.println(); // Baris kosong pembatas header dan body

  // 2. Kirim Header Body
  client.print(bodyHead);

  // 3. KIRIM FOTO SECARA STREAMING (1024 bytes per potongan)
  // Ini menghindari penggunaan Malloc raksasa yang bikin eror!
  uint8_t *fbBuf = fb->buf;
  size_t fbLen = fb->len;
  for (size_t n = 0; n < fbLen; n += 1024) {
    if (n + 1024 < fbLen) {
      client.write(fbBuf, 1024);
      fbBuf += 1024;
    } else if (fbLen % 1024 > 0) {
      size_t remainder = fbLen % 1024;
      client.write(fbBuf, remainder);
    }
  }

  // 4. Kirim Penutup Body
  client.print(bodyTail);

  // 5. Baca konfirmasi dari Telegram
  Serial.println("[Telegram] Mengirim...");
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }
  
  Serial.println("[Telegram] Foto berhasil dikirim!");
  client.stop(); // Putus koneksi agar memori kembali bersih
}

camera_fb_t *ambilFotoHQ() {
  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_UXGA);
  delay(300);
  camera_fb_t *fb = esp_camera_fb_get();
  s->set_framesize(s, FRAMESIZE_240X240);
  return fb;
}

int runFaceRecognition() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return -2;

  uint8_t *rgb_buf = (uint8_t *)malloc(fb->width * fb->height * 3);
  if (!rgb_buf) {
    esp_camera_fb_return(fb);
    return -2;
  }

  int matched_id = -2;
  if (fmt2rgb888(fb->buf, fb->len, fb->format, rgb_buf)) {
    std::list<dl::detect::result_t> &candidates = s1.infer(rgb_buf, { (int)fb->height, (int)fb->width, 3 });
    std::list<dl::detect::result_t> &results = s2.infer(rgb_buf, { (int)fb->height, (int)fb->width, 3 }, candidates);
    if (results.size() > 0) {
      Tensor<uint8_t> tensor;
      tensor.set_element(rgb_buf).set_shape({ (int)fb->height, (int)fb->width, 3 }).set_auto_free(false);
      auto rec_res = recognizer.recognize(tensor, results.front().keypoint);
      if (rec_res.id >= 0) matched_id = rec_res.id;
      else matched_id = -1;
    }
  }
  free(rgb_buf);
  esp_camera_fb_return(fb);
  return matched_id;
}

// ======================================================================
// FUNGSI WEB SERVER (Live Stream)
// ======================================================================
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];
  res = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=123456789000000000000987654321");
  if (res != ESP_OK) return res;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) { delay(100); continue; }
    size_t hlen = snprintf(part_buf, 64, "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
    res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, "\r\n--123456789000000000000987654321\r\n", 37);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;
  }
  return res;
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &index_uri);
  }
}

// ======================================================================
// FUNGSI PINTU & UX
// ======================================================================
void bukaPintu(String sumber) {
  digitalWrite(RELAY_PIN, HIGH); 
  tone(BUZZER_PIN, 2000, 150); delay(200); tone(BUZZER_PIN, 2500, 250);
  updateOLED(" TERBUKA ", sumber, 2);
  doorOpenTime = millis();
  currentState = STATE_DOOR_OPEN;
  lastActivityTime = millis();
}

void kunciPintu() {
  digitalWrite(RELAY_PIN, LOW);
  tone(BUZZER_PIN, 1500, 300);
  kembaliKeMonitoring();
}

void tolakAkses(String alasan) {
  tone(BUZZER_PIN, 500, 800);
  updateOLED(" DITOLAK ", alasan, 2);
  delay(2000); 
  kembaliKeMonitoring();
  lastActivityTime = millis();
}

void kembaliKeMonitoring() {
  inputBuffer = "";
  currentState = STATE_MONITORING;
  updateOLED(" SIAP ", "Scan Wajah", 2);
}

// ======================================================================
// FUNGSI LIGHT SLEEP (Hemat Daya)
// ======================================================================
void masukLightSleep() {
  display.clearDisplay();
  display.display(); // Matikan layar OLED
  
  digitalWrite(RELAY_PIN, LOW);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_camera_deinit();

  gpio_set_direction((gpio_num_t)19, GPIO_MODE_OUTPUT);
  gpio_set_direction((gpio_num_t)20, GPIO_MODE_OUTPUT);
  gpio_set_direction((gpio_num_t)21, GPIO_MODE_OUTPUT);
  gpio_set_direction((gpio_num_t)47, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)19, 0);
  gpio_set_level((gpio_num_t)20, 0);
  gpio_set_level((gpio_num_t)21, 0);
  gpio_set_level((gpio_num_t)47, 0);

  gpio_set_direction((gpio_num_t)41, GPIO_MODE_INPUT);
  gpio_set_direction((gpio_num_t)42, GPIO_MODE_INPUT);
  gpio_set_direction((gpio_num_t)2, GPIO_MODE_INPUT);
  gpio_set_direction((gpio_num_t)1, GPIO_MODE_INPUT);

  gpio_pullup_en((gpio_num_t)41);
  gpio_pullup_en((gpio_num_t)42);
  gpio_pullup_en((gpio_num_t)2);
  gpio_pullup_en((gpio_num_t)1);

  // Daftarkan pin Keypad untuk wakeup
  gpio_wakeup_enable((gpio_num_t)41, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)42, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)2, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)1, GPIO_INTR_LOW_LEVEL);

  // Daftarkan Tombol Exit untuk wakeup
  gpio_wakeup_enable((gpio_num_t)EXIT_BTN_PIN, GPIO_INTR_LOW_LEVEL);

  esp_sleep_enable_gpio_wakeup();
  esp_light_sleep_start();

  // ==========================================================
  // ----- SISTEM BARU SAJA BANGUN TIDUR -----
  // ==========================================================
  
  // 1. Dapatkan alasan kenapa sistem bangun
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  // 2. Matikan fitur wakeup agar tidak mengganggu operasional normal
  gpio_wakeup_disable((gpio_num_t)41);
  gpio_wakeup_disable((gpio_num_t)42);
  gpio_wakeup_disable((gpio_num_t)2);
  gpio_wakeup_disable((gpio_num_t)1);
  gpio_wakeup_disable((gpio_num_t)EXIT_BTN_PIN); 

  // 3. JIKA DIBANGUNKAN OLEH PIN DAN TOMBOL EXIT MASIH DITEKAN, BUKA PINTU!
  if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO && digitalRead(EXIT_BTN_PIN) == LOW) {
    bukaPintu("TOMBOL EXIT");
  } else {
    // Jika dibangunkan keypad, kembali ke layar utama dengan normal
    kembaliKeMonitoring();
  }

  // 4. Setelah pintu diklik terbuka, baru proses Kamera & WiFi menyusul (Background)
  initCamera();
  connectWiFi();
  
  lastActivityTime = millis();
}

// ======================================================================
// SETUP
// ======================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Kunci Pin Relay (Cegah Floating)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); 
  
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MC38_PIN, INPUT_PULLUP);
  pinMode(EXIT_BTN_PIN, INPUT_PULLUP);

  // Jalur I2C Layar
  Wire.begin(SDA_PIN, SCL_PIN);
  display.begin(i2c_Address, true);
  updateOLED("Sistem", "Booting...", 2);
  
  tone(BUZZER_PIN, 1500, 300); // Startup sound

  if (!initCamera()) {
    updateOLED("FATAL", "Kamera Mati", 2);
    while (true) { delay(100); }
  }

  initFaceRecognition();
  connectWiFi();
  startCameraServer();

  kembaliKeMonitoring();
  lastActivityTime = millis();
}

// ======================================================================
// LOOP UTAMA — STATE MACHINE
// ======================================================================
void loop() {
  char key = keypad.getKey();
  
  if (key) {
    lastActivityTime = millis();
    tone(BUZZER_PIN, 2500, 80); // Audio feedback tuts keypad
  }

  switch (currentState) {
    case STATE_MONITORING:
      {
        // 1. Cek Tombol Exit Dari Dalam Ruangan
        if (digitalRead(EXIT_BTN_PIN) == LOW) {
          bukaPintu("TOMBOL EXIT");
          delay(500); 
          break;
        }

        // 2. Cek Keypad
        if (key == 'A') {
          inputBuffer = "";
          updateOLED("Admin", "PIN:", 2);
          currentState = STATE_INPUT_PIN_ADMIN;
          break;
        }
        if (key >= '0' && key <= '9') {
          inputBuffer = String(key);
          updateOLED("PIN Akses", "*", 3);
          currentState = STATE_INPUT_PIN;
          break;
        }

        // 3. Eksekusi Face Recognition
        int faceResult = runFaceRecognition();
        if (faceResult >= 0) {
          bukaPintu("WAJAH DIKENAL");
        } else if (faceResult == -1) {
          
          // 1. Ambil foto SEGERA saat wajah ditangkap
          camera_fb_t *fb_hq = ambilFotoHQ();
          
          // 2. Tampilkan notifikasi ditolak & Buzzer bunyi
          tolakAkses("TIDAK DIKENAL");
          
          // 3. Kirim ke Telegram tanpa mengganggu responsivitas pintu
          if (fb_hq) {
            sendTelegramAlert(fb_hq);
            esp_camera_fb_return(fb_hq);
          }
        }

        // 4. Idle Timeout (Menuju Mode Hemat Daya)
        if (millis() - lastActivityTime > IDLE_TIMEOUT) {
          currentState = STATE_LIGHT_SLEEP;
        }
        break;
      }

    case STATE_INPUT_PIN:
      {
        if (key >= '0' && key <= '9') {
          inputBuffer += key;
          String hiddenPin = "";
          for(int i=0; i<inputBuffer.length(); i++) hiddenPin += "*";
          updateOLED("PIN Akses", hiddenPin, 3);
        } else if (key == '*') {
          if (inputBuffer == PIN_ALTERNATIF) bukaPintu("PIN BENAR");
          else tolakAkses("PIN SALAH");
        } else if (key == '#') {
          kembaliKeMonitoring();
        }

        if (millis() - lastActivityTime > PIN_TIMEOUT) kembaliKeMonitoring();
        break;
      }

    case STATE_INPUT_PIN_ADMIN:
      {
        if (key >= '0' && key <= '9') {
          inputBuffer += key;
          String hiddenPin = "";
          for(int i=0; i<inputBuffer.length(); i++) hiddenPin += "*";
          updateOLED("Admin", hiddenPin, 3);
        } else if (key == '*') {
          if (inputBuffer == PIN_ADMIN) {
            inputBuffer = "";
            updateOLED("Admin Mode", "Scan Wajah..", 1);
            tone(BUZZER_PIN, 2000, 500); // Nada admin siap
            currentState = STATE_FACE_ENROLLMENT;
          } else {
            tolakAkses("Admin Salah");
          }
        } else if (key == '#') {
          kembaliKeMonitoring();
        }

        if (millis() - lastActivityTime > PIN_TIMEOUT) kembaliKeMonitoring();
        break;
      }

    case STATE_FACE_ENROLLMENT:
      {
        bool enrolled = false;
        int maxAttempts = 30;

        for (int attempt = 0; attempt < maxAttempts && !enrolled; attempt++) {
          camera_fb_t *fb = esp_camera_fb_get();
          if (!fb) { delay(300); continue; }

          uint8_t *rgb_buf = (uint8_t *)malloc(fb->width * fb->height * 3);
          if (rgb_buf && fmt2rgb888(fb->buf, fb->len, fb->format, rgb_buf)) {
            std::list<dl::detect::result_t> &candidates = s1.infer(rgb_buf, { (int)fb->height, (int)fb->width, 3 });
            std::list<dl::detect::result_t> &results = s2.infer(rgb_buf, { (int)fb->height, (int)fb->width, 3 }, candidates);
            if (results.size() > 0) {
              Tensor<uint8_t> tensor;
              tensor.set_element(rgb_buf).set_shape({ (int)fb->height, (int)fb->width, 3 }).set_auto_free(false);
              int enroll_result = recognizer.enroll_id(tensor, results.front().keypoint, "user", true);
              if (enroll_result >= 0) {
                enrolled = true;
                tone(BUZZER_PIN, 3000, 1000); // Nada sukses panjang
                updateOLED("BERHASIL", "Tersimpan!", 2);
                delay(2000);
              }
            }
          }
          if (rgb_buf) free(rgb_buf);
          esp_camera_fb_return(fb);
          delay(400);
        }

        if (!enrolled) tolakAkses("Daftar Gagal");
        else kembaliKeMonitoring();
        
        lastActivityTime = millis();
        break;
      }

    case STATE_DOOR_OPEN:
      {
        // Logika anti-tabrakan MC-38
        bool isDoorPhysicallyOpen = (digitalRead(MC38_PIN) == HIGH);

        if (isDoorPhysicallyOpen) {
          doorOpenTime = millis(); // Bekukan timer 
        } else {
          if (millis() - doorOpenTime >= LOCK_DELAY) {
            kunciPintu();
          }
        }
        
        // Reset manual dari tombol dalam
        if (digitalRead(EXIT_BTN_PIN) == LOW) {
          doorOpenTime = millis(); 
          tone(BUZZER_PIN, 2000, 50);
          delay(200);
        }
        break;
      }

    case STATE_LIGHT_SLEEP:
      {
        masukLightSleep();
        break;
      }
  }
}