#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

// WiFi
const char* ssid = "Infinix NOTE 30 Pro";
const char* password = "11111111";

// Telegram
#define BOT_TOKEN "8109104143:AAFqKC4ufEQCvweZZfC8IzbjKToUDFp1_d8"
#define CHAT_ID "5845175001"

// Google Sheets (optional)
const char* GOOGLE_SCRIPT_ID = "YOUR_SCRIPT_ID";

// Pin
#define TRIG_PIN 14
#define ECHO_PIN 13
#define RELAY_PIN 10
#define TDS_PIN A0

// Konstanta
#define TANK_HEIGHT 300.0 // cm
#define EEPROM_MODE_ADDR 0
#define MAX_LOG_ENTRIES 10

// Ambang
const float ALERT_LEVEL_LOW = 50.0;    // persentase -> notifikasi air rendah ketika < ini
const float ALERT_LEVEL_HIGH = 95.0;   // persentase -> notifikasi penuh ketika > ini
const float ALERT_TDS = 500.0;         // ppm -> notifikasi tds buruk ketika > ini

// LCD
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Server
ESP8266WebServer server(80);

// Sistem
bool autoMode = true;
bool autoPumpState = false;
bool lastPumpState = false;

// Log struct
struct LogEntry {
  float waterLevel;
  float tdsValue;
  bool pumpState;
  unsigned long timestamp;
};
LogEntry logs[MAX_LOG_ENTRIES];
int logIndex = 0;

// Notifikasi flag (hindari spam)
bool lowAlertSent = false;
bool highAlertSent = false;
bool tdsAlertSent = false;

// Deklarasi fungsi
void sendTelegram(String message);
void sendToGoogleSheets(float percentage, float tds);
void addLog(float level, float tds, bool pump);
void handleLogs();
String getTDSCategory(float tds);
void updateLCD(float percentage, float tds);
float readWaterLevel();
float readTDS();
void handleRoot();
String urlEncode(const String &str);
void ensureWiFi();

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Pompa OFF (aktif LOW assumed)

  lcd.begin();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("Booting...");
  lcd.setCursor(0, 1);
  lcd.print("Menghubung WiFi");

  EEPROM.begin(512);
  autoMode = EEPROM.read(EEPROM_MODE_ADDR);

  // mulai WiFi (non-blocking wait dengan timeout)
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }

  // Notifikasi saat perangkat menyala (sertakan IP bila ada)
  if (WiFi.status() == WL_CONNECTED) {
    String bootMsg = "⚡ Perangkat *ON*\nIP: " + WiFi.localIP().toString();
    sendTelegram(bootMsg);
  } else {
    sendTelegram("⚡ Perangkat *ON*\n(Tidak terhubung WiFi)");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi connected");
    Serial.println("IP Address: " + WiFi.localIP().toString());

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Terhubung!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    delay(2000);
    lcd.clear();
  } else {
    Serial.println();
    Serial.println("WiFi NOT connected (timeout)");
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("WiFi Gagal");
    lcd.setCursor(0,1);
    lcd.print("Check creds");
    delay(2000);
    lcd.clear();
  }

  // Web routes
  server.on("/", handleRoot);
  server.on("/logs", handleLogs);
  server.on("/pump/on", []() {
    autoMode = false;
    EEPROM.write(EEPROM_MODE_ADDR, 0);
    EEPROM.commit();
    digitalWrite(RELAY_PIN, LOW);
    sendTelegram("💡 Pompa dinyalakan secara *MANUAL*");
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/pump/off", []() {
    autoMode = false;
    EEPROM.write(EEPROM_MODE_ADDR, 0);
    EEPROM.commit();
    digitalWrite(RELAY_PIN, HIGH);
    sendTelegram("🛑 Pompa dimatikan secara *MANUAL*.\nPerangkat dianggap *OFF*.");
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/pump/auto", []() {
    autoMode = true;
    EEPROM.write(EEPROM_MODE_ADDR, 1);
    EEPROM.commit();
    sendTelegram("🔄 Mode kembali ke *AUTO*");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.begin();
}

void loop() {
  server.handleClient();

  // Pastikan WiFi masih ada; jika hilang coba reconnect di background
  ensureWiFi();

  float levelCM = readWaterLevel(); // jarak dari sensor ke permukaan air (cm)
  float percentage = constrain(((TANK_HEIGHT - levelCM) / TANK_HEIGHT) * 100.0, 0.0, 100.0);
  float tds = readTDS();

  // otomatis pump control (sama seperti kode awal)
  if (autoMode) {
    if (percentage < ALERT_LEVEL_LOW) {
      digitalWrite(RELAY_PIN, LOW);
      autoPumpState = true;
      if (lastPumpState != true) {
        String msg = "🔔 *Status Pompa*: AKTIF (Auto)\n";
        msg += "📊 *Ketinggian Air*: " + String(percentage, 1) + "%\n";
        msg += "💧 *Kualitas Air (TDS)*: " + String(tds, 1) + " ppm (" + getTDSCategory(tds) + ")";
        sendTelegram(msg);
      }
    } else if (percentage > ALERT_LEVEL_HIGH) {
      digitalWrite(RELAY_PIN, HIGH);
      autoPumpState = false;
      if (lastPumpState != false) {
        String msg = "🔕 *Status Pompa*: NONAKTIF (Auto)\n";
        msg += "📊 *Ketinggian Air*: " + String(percentage, 1) + "%\n";
        msg += "💧 *Kualitas Air (TDS)*: " + String(tds, 1) + " ppm (" + getTDSCategory(tds) + ")";
        sendTelegram(msg);
      }
    }
    lastPumpState = autoPumpState;
  }

  // --- NOTIFIKASI ALERT KHUSUS ---
  // Air rendah
  if (percentage < ALERT_LEVEL_LOW) {
    if (!lowAlertSent) {
      String msg = "⚠️ *ALERT*: Air Rendah\n";
      msg += "📊 Ketinggian: " + String(percentage, 1) + "%\n";
      msg += "💧 TDS: " + String(tds, 1) + " ppm (" + getTDSCategory(tds) + ")";
      sendTelegram(msg);
      lowAlertSent = true;
      // reset high alert because sekarang rendah
      highAlertSent = false;
    }
  } else {
    // reset flag bila kembali normal (bisa kirim notifikasi normal jika diinginkan)
    if (lowAlertSent) {
      String msg = "✅ Kondisi Air kembali normal\nKetinggian: " + String(percentage,1) + "%";
      sendTelegram(msg);
    }
    lowAlertSent = false;
  }

  // Air penuh (kembali ke atas threshold tinggi)
  if (percentage > ALERT_LEVEL_HIGH) {
    if (!highAlertSent) {
      String msg = "✅ *Tangki PENUH*\nKetinggian: " + String(percentage,1) + "%";
      sendTelegram(msg);
      highAlertSent = true;
      lowAlertSent = false;
    }
  } else {
    if (highAlertSent) {
      // optional: kirim notifikasi ketika turun dari penuh (komentar bila tidak mau)
      // sendTelegram("Tangki turun dari kondisi penuh.");
    }
    // highAlertSent = false; // tetap biarkan sampai naik lagi (atau aktifkan jika ingin reset selalu)
  }

  // TDS buruk
  if (tds > ALERT_TDS) {
    if (!tdsAlertSent) {
      String msg = "⚠️ *ALERT*: TDS di atas batas\n";
      msg += "💧 TDS: " + String(tds,1) + " ppm (" + getTDSCategory(tds) + ")\n";
      msg += "📊 Ketinggian: " + String(percentage,1) + "%";
      sendTelegram(msg);
      tdsAlertSent = true;
    }
  } else {
    if (tdsAlertSent) {
      String msg = "✅ TDS kembali normal\nTDS: " + String(tds,1) + " ppm";
      sendTelegram(msg);
    }
    tdsAlertSent = false;
  }

  // Update LCD dan kirim data ke google sheets
  updateLCD(percentage, tds);
  sendToGoogleSheets(percentage, tds);

  // Log kondisi jika memenuhi ambang log (seperti awal)
  if (percentage < ALERT_LEVEL_LOW || tds > ALERT_TDS) {
    addLog(percentage, tds, digitalRead(RELAY_PIN) == LOW);
  }

  delay(10000); // jeda 10 detik
}

float readWaterLevel() {
  // Ultrasonic HC-SR04: trigger->echo (hasil dalam cm)
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // timeout 30ms
  if (duration == 0) {
    // timeout -> kemungkinan tidak ada echo (atau jarak > range)
    Serial.println("Ultrasonic: timeout");
    return TANK_HEIGHT; // anggap kosong / penuh sesuai kebutuhan (kembalikan tinggi max)
  }
  float distance = duration * 0.034 / 2.0;
  Serial.println("Ultrasonic cm: " + String(distance));
  return distance;
}

float readTDS() {
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(TDS_PIN);
    delay(10);
  }
  int sensorValue = sum / 10;
  float voltage = sensorValue * (3.3 / 1023.0);
  // Kurva kalibrasi TDS (sama seperti kode Anda)
  float tds = (133.42 * voltage * voltage * voltage - 255.86 * voltage * voltage + 857.39 * voltage) * 0.5;
  Serial.println("TDS sensor: " + String(sensorValue) + " -> " + String(tds));
  return tds;
}

void updateLCD(float percentage, float tds) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Air:");
  lcd.print(percentage, 0);
  lcd.print("%");
  lcd.setCursor(11, 0);
  if (digitalRead(RELAY_PIN) == LOW) {
    lcd.print("ON ");
  } else {
    lcd.print("OFF");
  }
  lcd.setCursor(0, 1);
  lcd.print("TDS:");
  lcd.print(tds, 0);
  lcd.print(" ");
  String cat = getTDSCategory(tds);
  lcd.print(cat.substring(0, min(5, (int)cat.length())));
}

String getTDSCategory(float tds) {
  if (tds < 50) return "S M";
  else if (tds < 150) return "Baik";
  else if (tds < 300) return "Normal";
  else if (tds < 500) return "Layak";
  else return "T L";
}

void handleRoot() {
  float level = readWaterLevel();
  float percentage = constrain(((TANK_HEIGHT - level) / TANK_HEIGHT) * 100.0, 0.0, 100.0);
  float tds = readTDS();

  String html = R"rawliteral(
<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>
<style>body{font-family:sans-serif;text-align:center;}button{padding:10px 20px;margin:5px;}canvas{max-width:100%;height:auto;}</style>
<script src='https://cdn.jsdelivr.net/npm/chart.js'></script></head><body>
<h2>Status Air</h2>
)rawliteral";

  html += "<p>Ketinggian: " + String(percentage, 0) + "%</p>";
  html += "<p>TDS: " + String(tds, 1) + " ppm (" + getTDSCategory(tds) + ")</p>";
  html += "<p>Pompa: " + String((digitalRead(RELAY_PIN) == LOW) ? "ON" : "OFF") + "</p>";
  html += "<p>Mode: " + String(autoMode ? "AUTO" : "MANUAL") + "</p>";
  html += "<form action='/pump/on' method='POST'><button>POMPA ON</button></form>";
  html += "<form action='/pump/off' method='POST'><button>POMPA OFF</button></form>";
  html += "<form action='/pump/auto' method='POST'><button>Kembali ke AUTO</button></form>";
  html += "<p><a href='/logs'>Lihat Riwayat</a></p>";

  html += "<h3>Grafik Riwayat</h3><canvas id='logChart'></canvas><script>";
  html += "const labels = [";
  for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
    html += "'Log " + String(i + 1) + "',";
  }
  html += "];";
  html += "const data = {labels: labels, datasets: [";
  html += "{label: 'Air (%)', data: [";
  for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
    int index = (logIndex + i) % MAX_LOG_ENTRIES;
    html += String(logs[index].waterLevel, 0) + ",";
  }
  html += "], backgroundColor: 'rgba(54,162,235,0.6)'},";

  html += "{label: 'TDS (ppm)', data: [";
  for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
    int index = (logIndex + i) % MAX_LOG_ENTRIES;
    html += String(logs[index].tdsValue, 1) + ",";
  }
  html += "], backgroundColor: 'rgba(255,99,132,0.6)'}]};";
  html += "new Chart(document.getElementById('logChart'), {type: 'bar', data: data, options: {responsive: true, scales:{y:{beginAtZero:true}}}});";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

void handleLogs() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>body{font-family:sans-serif;}table{width:100%;border-collapse:collapse;}th,td{padding:8px;border:1px solid #ccc;text-align:center;}@media(max-width:600px){td,th{font-size:12px;}}</style></head><body>";
  html += "<h2>Riwayat Sensor</h2><table><tr><th>Air (%)</th><th>TDS (ppm)</th><th>Kategori</th><th>Pompa</th><th>Waktu (s)</th></tr>";
  for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
    int index = (logIndex + i) % MAX_LOG_ENTRIES;
    html += "<tr><td>" + String(logs[index].waterLevel, 0) + "</td><td>" + String(logs[index].tdsValue, 1) + "</td><td>" + getTDSCategory(logs[index].tdsValue) + "</td><td>" + (logs[index].pumpState ? "ON" : "OFF") + "</td><td>" + String((millis() - logs[index].timestamp) / 1000) + "s lalu</td></tr>";
  }
  html += "</table><p><a href='/'>Kembali</a></p></body></html>";
  server.send(200, "text/html", html);
}

void addLog(float level, float tds, bool pump) {
  logs[logIndex].waterLevel = level;
  logs[logIndex].tdsValue = tds;
  logs[logIndex].pumpState = pump;
  logs[logIndex].timestamp = millis();
  logIndex = (logIndex + 1) % MAX_LOG_ENTRIES;
}

// URL encode function (basic)
String urlEncode(const String &str) {
  String encoded = "";
  char c;
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if ( (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else if (c == ' ') encoded += "%20";
    else if (c == '\n') encoded += "%0A";
    else {
      char buf[5];
      sprintf(buf, "%%%02X", (uint8_t)c);
      encoded += buf;
    }
  }
  return encoded;
}

void sendTelegram(String message) {
  // Pastikan WiFi tersambung
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("sendTelegram: WiFi not connected, skip");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // untuk prototyping. Ganti bila perlu.

  HTTPClient https;
  String text = urlEncode(message);
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage?chat_id=" + String(CHAT_ID) + "&text=" + text + "&parse_mode=Markdown";

  Serial.println("sendTelegram URL: " + url); // debug ( panjang bisa ) 
  if (https.begin(client, url)) {
    int httpCode = https.GET();
    Serial.println("Telegram httpCode: " + String(httpCode));
    if (httpCode != 200) {
      String payload = https.getString();
      Serial.println("Telegram response: " + payload);
    }
    https.end();
  } else {
    Serial.println("HTTPS begin failed");
  }
}

void sendToGoogleSheets(float level, float tds) {
  if (String(GOOGLE_SCRIPT_ID) == String("YOUR_SCRIPT_ID")) return; // skip jika belum diisi
  WiFiClient wifiClient;
  HTTPClient http;
  String url = "https://script.google.com/macros/s/" + String(GOOGLE_SCRIPT_ID) + "/exec?";
  url += "water=" + String(level, 2);
  url += "&tds=" + String(tds, 2);
  url += "&pump=" + String((digitalRead(RELAY_PIN) == LOW) ? "ON" : "OFF");
  url += "&mode=" + String(autoMode ? "AUTO" : "MANUAL");
  http.begin(wifiClient, url);
  int code = http.GET();
  Serial.println("GoogleSheets code: " + String(code));
  http.end();
}

void ensureWiFi() {
  // jika WiFi terputus, coba reconnect cepat tanpa blocking lama.
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastTry = 0;
    if (millis() - lastTry > 10000) { // coba tiap 10 detik
      Serial.println("WiFi disconnected, trying reconnect...");
      lastTry = millis();
      WiFi.disconnect();
      WiFi.begin(ssid, password);
    }
  }
}
