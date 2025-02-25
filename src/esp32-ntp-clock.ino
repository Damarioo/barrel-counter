#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include <ESPAsyncWebServer.h>
#include <SD.h>
#include <SPI.h>

// Definisi konstanta sistem
#define SENSOR_PIN 26
#define RESET_PIN 27
#define SD_CS_PIN 5            // Pin CS untuk SD Card module
#define DATA_LOG_FILE "/drum_log.csv"
#define MONTHLY_SUMMARY_FILE "/monthly_summary.csv"
#define DEBOUNCE_DELAY 150
#define LCD_ADDRESS 0x27
#define LCD_COLS 16
#define LCD_ROWS 2
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 7 * 3600
#define DAYLIGHT_OFFSET_SEC 0

// Objek sistem
Preferences preferences;
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
AsyncWebServer server(80);

// Struktur data untuk pencatatan tanggal
struct DateData {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
};

// Struktur data untuk status sistem
struct SystemStatus {
  volatile uint32_t drumCount;
  volatile uint32_t dailyCount;
  volatile uint32_t monthlyCount;
  volatile unsigned long lastInterruptTime;
  volatile bool countUpdated;
  bool wifiConnected;
  bool timeInitialized;
  bool sdCardMounted;
  char timeStr[20];
  char dateStr[11];
  DateData currentDate;
  DateData lastResetDate;
};

SystemStatus status = {0};

// Fungsi interupsi untuk penghitungan drum
void IRAM_ATTR countDrum() {
  unsigned long interruptTime = millis();
  if (interruptTime - status.lastInterruptTime > DEBOUNCE_DELAY) {
    status.drumCount++;
    status.dailyCount++;
    status.monthlyCount++;
    status.countUpdated = true;
    status.lastInterruptTime = interruptTime;
  }
}

// Fungsi untuk mendapatkan informasi waktu sistem
bool getSystemTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Gagal mendapatkan waktu");
    return false;
  }
  
  // Simpan waktu dalam format yang diperlukan
  status.currentDate.year = timeinfo.tm_year + 1900;
  status.currentDate.month = timeinfo.tm_mon + 1;
  status.currentDate.day = timeinfo.tm_mday;
  status.currentDate.hour = timeinfo.tm_hour;
  status.currentDate.minute = timeinfo.tm_min;
  status.currentDate.second = timeinfo.tm_sec;
  
  // Format string tanggal dan waktu untuk tampilan
  sprintf(status.dateStr, "%04d-%02d-%02d", 
          status.currentDate.year, 
          status.currentDate.month, 
          status.currentDate.day);
          
  sprintf(status.timeStr, "%02d:%02d:%02d", 
          status.currentDate.hour,
          status.currentDate.minute,
          status.currentDate.second);
          
  return true;
}

// Fungsi untuk menyimpan data harian ke SD Card
void saveDailyDataToSD() {
  if (!status.sdCardMounted) {
    Serial.println("SD Card tidak terpasang, data tidak dapat disimpan");
    return;
  }
  
  // Format: tanggal,jumlah_harian
  String dataLine = String(status.dateStr) + "," + String(status.dailyCount);
  
  File dataFile = SD.open(DATA_LOG_FILE, FILE_APPEND);
  if (dataFile) {
    dataFile.println(dataLine);
    dataFile.close();
    Serial.println("Data harian berhasil disimpan: " + dataLine);
  } else {
    Serial.println("Gagal membuka file log untuk penyimpanan data");
  }
}

// Fungsi untuk menyimpan ringkasan bulanan
void saveMonthlyDataToSD() {
  if (!status.sdCardMounted) {
    return;
  }
  
  // Format: tahun-bulan,jumlah_bulanan
  String monthStr = String(status.currentDate.year) + "-" + 
                   (status.currentDate.month < 10 ? "0" : "") + 
                   String(status.currentDate.month);
  
  String dataLine = monthStr + "," + String(status.monthlyCount);
  
  File dataFile = SD.open(MONTHLY_SUMMARY_FILE, FILE_APPEND);
  if (dataFile) {
    dataFile.println(dataLine);
    dataFile.close();
    Serial.println("Data bulanan berhasil disimpan: " + dataLine);
  }
}

// Fungsi untuk mengecek dan melakukan reset harian
void checkAndPerformDailyReset() {
  if (!status.timeInitialized) {
    return;
  }
  
  bool isMidnight = (status.currentDate.hour == 0 && 
                     status.currentDate.minute == 0 && 
                     status.currentDate.second < 10);
  
  // Cek apakah tanggal sudah berubah dari reset terakhir
  bool isNewDay = (status.currentDate.day != status.lastResetDate.day ||
                   status.currentDate.month != status.lastResetDate.month ||
                   status.currentDate.year != status.lastResetDate.year);
                   
  // Reset dilakukan pada tengah malam atau ketika terdeteksi perubahan tanggal
  if (isMidnight || (isNewDay && status.lastResetDate.year != 0)) {
    // Simpan data harian sebelum reset
    saveDailyDataToSD();
    
    // Reset penghitung harian
    Serial.println("Melakukan reset harian pada " + String(status.dateStr));
    status.dailyCount = 0;
    
    // Update tanggal reset terakhir
    status.lastResetDate = status.currentDate;
    
    // Simpan status ke preferences
    preferences.putUInt("dailyCount", status.dailyCount);
    preferences.putInt("lastResetYear", status.lastResetDate.year);
    preferences.putInt("lastResetMonth", status.lastResetDate.month);
    preferences.putInt("lastResetDay", status.lastResetDate.day);
    
    // Cek reset bulanan
    if (status.currentDate.day == 1 && isMidnight) {
      saveMonthlyDataToSD();
      status.monthlyCount = 0;
      preferences.putUInt("monthlyCount", status.monthlyCount);
    }
  }
}

// Fungsi inisialisasi SD Card
bool initSDCard() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("Inisialisasi SD Card gagal");
    return false;
  }
  
  Serial.println("SD Card berhasil diinisialisasi");
  
  // Cek dan buat file log jika belum ada
  if (!SD.exists(DATA_LOG_FILE)) {
    File dataFile = SD.open(DATA_LOG_FILE, FILE_WRITE);
    if (dataFile) {
      // Tulis header file CSV
      dataFile.println("tanggal,jumlah_drum");
      dataFile.close();
      Serial.println("File log harian berhasil dibuat");
    } else {
      Serial.println("Gagal membuat file log harian");
      return false;
    }
  }
  
  // Cek dan buat file ringkasan bulanan jika belum ada
  if (!SD.exists(MONTHLY_SUMMARY_FILE)) {
    File dataFile = SD.open(MONTHLY_SUMMARY_FILE, FILE_WRITE);
    if (dataFile) {
      dataFile.println("bulan,jumlah_drum");
      dataFile.close();
      Serial.println("File ringkasan bulanan berhasil dibuat");
    } else {
      Serial.println("Gagal membuat file ringkasan bulanan");
      return false;
    }
  }
  
  return true;
}

// Inisialisasi server web dengan fitur ekspor data
void initWebServer() {
  // Halaman utama
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<meta http-equiv='refresh' content='5'>";
    html += "<title>Sistem Penghitung Drum</title>";
    html += "<style>body{font-family:Arial,sans-serif;text-align:center;margin:20px;} ";
    html += "table{margin:0 auto;border-collapse:collapse;width:80%;max-width:600px;} ";
    html += "td,th{border:1px solid #ddd;padding:8px;} ";
    html += "tr:nth-child(even){background-color:#f2f2f2;} ";
    html += "th{padding-top:12px;padding-bottom:12px;background-color:#4CAF50;color:white;} ";
    html += ".btn{background-color:#4CAF50;border:none;color:white;padding:10px 20px;text-align:center;";
    html += "text-decoration:none;display:inline-block;font-size:16px;margin:4px 2px;cursor:pointer;border-radius:5px;}</style>";
    html += "</head><body>";
    html += "<h1>Sistem Penghitung Drum</h1>";
    html += "<p>Tanggal: " + String(status.dateStr) + " | Waktu: " + String(status.timeStr) + "</p>";
    html += "<div style='display:flex;justify-content:center;margin:20px 0;'>";
    html += "<div style='background-color:#f0f0f0;border-radius:10px;padding:20px;margin:10px;min-width:150px;'>";
    html += "<h2 style='margin-top:0;'>Total Hari Ini</h2>";
    html += "<p style='font-size:24px;font-weight:bold;'>" + String(status.dailyCount) + "</p>";
    html += "</div>";
    html += "<div style='background-color:#f0f0f0;border-radius:10px;padding:20px;margin:10px;min-width:150px;'>";
    html += "<h2 style='margin-top:0;'>Total Bulan Ini</h2>";
    html += "<p style='font-size:24px;font-weight:bold;'>" + String(status.monthlyCount) + "</p>";
    html += "</div>";
    html += "<div style='background-color:#f0f0f0;border-radius:10px;padding:20px;margin:10px;min-width:150px;'>";
    html += "<h2 style='margin-top:0;'>Total Keseluruhan</h2>";
    html += "<p style='font-size:24px;font-weight:bold;'>" + String(status.drumCount) + "</p>";
    html += "</div>";
    html += "</div>";
    html += "<div style='margin:20px 0;'>";
    html += "<a href='/export' class='btn'>Unduh Data CSV</a>";
    html += "</div>";
    html += "</body></html>";
    request->send(200, "text/html", html);
  });
  
  // Endpoint untuk ekspor data CSV
  server.on("/export", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!status.sdCardMounted) {
      request->send(500, "text/plain", "SD Card tidak terpasang, tidak dapat mengekspor data");
      return;
    }
    
    AsyncResponseStream *response = request->beginResponseStream("text/csv");
    response->addHeader("Content-Disposition", "attachment; filename=drum_data.csv");
    
    File dataFile = SD.open(DATA_LOG_FILE);
    if (dataFile) {
      while (dataFile.available()) {
        String line = dataFile.readStringUntil('\n');
        response->print(line + "\r\n");
      }
      dataFile.close();
    }
    
    request->send(response);
  });
  
  server.begin();
  Serial.println("Server web berhasil diinisialisasi");
}

// Fungsi setup
void setup() {
  Serial.begin(115200);
  Serial.println("Inisialisasi Sistem Penghitung Drum Dengan Pencatatan Data");
  
  // Inisialisasi penyimpanan persisten
  preferences.begin("drum-count", false);
  
  // Memuat data dari penyimpanan persisten
  status.drumCount = preferences.getUInt("drumCount", 0);
  status.dailyCount = preferences.getUInt("dailyCount", 0);
  status.monthlyCount = preferences.getUInt("monthlyCount", 0);
  status.lastResetDate.year = preferences.getInt("lastResetYear", 0);
  status.lastResetDate.month = preferences.getInt("lastResetMonth", 0);
  status.lastResetDate.day = preferences.getInt("lastResetDay", 0);
  
  // Konfigurasi pin
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(RESET_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), countDrum, FALLING);
  
  // Inisialisasi LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sistem Penghitung");
  lcd.setCursor(0, 1);
  lcd.print("Drum v3.0");
  delay(2000);
  
  // Inisialisasi SD Card
  status.sdCardMounted = initSDCard();
  
  // Koneksi WiFi
  WiFi.begin("Wokwi-GUEST", "");
  Serial.print("Menghubungkan ke WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    status.wifiConnected = true;
    Serial.println("\nWiFi terhubung.");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    
    // Konfigurasi waktu NTP
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    
    // Inisialisasi server web
    initWebServer();
    
    // Dapatkan waktu sistem
    if (getSystemTime()) {
      status.timeInitialized = true;
    }
  } else {
    Serial.println("\nGagal terhubung ke WiFi. Sistem berjalan dalam mode offline.");
  }
}

// Fungsi loop
void loop() {
  // Update waktu sistem jika terhubung ke WiFi
  if (status.wifiConnected) {
    if (getSystemTime()) {
      status.timeInitialized = true;
      
      // Cek dan lakukan reset harian
      checkAndPerformDailyReset();
    }
  }
  
  // Penyimpanan periodik untuk mengamankan data
  static unsigned long lastSaveTime = 0;
  if (millis() - lastSaveTime > 60000 || status.countUpdated) {  // Simpan setiap menit atau saat ada perubahan
    preferences.putUInt("drumCount", status.drumCount);
    preferences.putUInt("dailyCount", status.dailyCount);
    preferences.putUInt("monthlyCount", status.monthlyCount);
    lastSaveTime = millis();
    status.countUpdated = false;
  }
  
  // Pembaruan tampilan LCD
  static unsigned long lastDisplayUpdate = 0;
  if (millis() - lastDisplayUpdate > 1000) {
    lcd.clear();
    
    // Baris pertama: Tanggal/Waktu atau Status
    lcd.setCursor(0, 0);
    if (status.timeInitialized) {
      lcd.print(status.dateStr);
    } else {
      lcd.print("Mode Offline");
    }
    
    // Baris kedua: Jumlah Drum
    lcd.setCursor(0, 1);
    lcd.print("Hari ini: ");
    lcd.print(status.dailyCount);
    
    lastDisplayUpdate = millis();
  }
  
  // Mekanisme watchdog
  yield();
}
