/*
  Tampinha Magica - ESP32 firmware

  Code/comments are in English. LCD messages are in Brazilian Portuguese.

  Reliability goals:
  - Teacher confirms deposits.
  - Student/card data is cached locally after a successful online lookup.
  - Known cards can be used offline.
  - Transactions are saved locally before sync.
  - Pending transactions are retried automatically.
  - Weight flow waits for caps to be placed before checking stability.
  - The scale must be empty before tare and after each operation.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <HX711.h>
#include <SPI.h>
#include <MFRC522.h>
#include <LittleFS.h>

// ---------------- Configuration ----------------
// IMPORTANT: copy your working values here before uploading.
const char* WIFI_SSID = "";
const char* WIFI_PASSWORD = ";
const char* API_BASE_URL = "";
const char* DEVICE_ID = "terminal-01";
const char* DEVICE_KEY = "change-this-device-key";
const char* FIRMWARE_VERSION = "2.1.1-buzzer-safe-save";

// RFID pins.
#define RFID_SS_PIN 5
#define RFID_RST_PIN 27

// HX711 pins.
#define HX711_DOUT_PIN 32
#define HX711_SCK_PIN 33
float SCALE_CALIBRATION_FACTOR = -453.5;

// Buzzer pin.
// Simple 2-leg buzzer: + to GPIO2, - to GND.
// 3-pin buzzer module: SIG to GPIO2, VCC to 3V3, GND to GND.
#define BUZZER_PIN 2

// LCD address can be 0x27 or 0x3F depending on the module.
LiquidCrystal_I2C lcd(0x27, 16, 2);
HX711 scale;
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);

// Keypad pins.
// Keep these equal to your real wiring.
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {25, 26, 14, 13};
byte colPins[COLS] = {15, 4, 17, 16};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ---------------- Runtime flags ----------------
bool studentLoadedFromCache = false;

// ---------------- LCD helpers ----------------
void showMessage(const String& line1, const String& line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

// ---------------- Buzzer helpers ----------------
void beep(int frequency, int durationMs) {
  tone(BUZZER_PIN, frequency, durationMs);
  delay(durationMs + 20);
  noTone(BUZZER_PIN);
}

void beepReady() {
  beep(1800, 80);
}

void beepCardRead() {
  beep(2200, 90);
}

void beepConfirmRequest() {
  beep(1600, 80);
  delay(70);
  beep(1600, 80);
}

void beepSuccess() {
  beep(1800, 100);
  delay(70);
  beep(2400, 130);
}

void beepOfflineSaved() {
  beep(900, 120);
  delay(80);
  beep(1300, 120);
}

void beepCancel() {
  beep(700, 120);
}

void beepError() {
  beep(500, 180);
  delay(80);
  beep(400, 220);
}

// ---------------- Wi-Fi helpers ----------------
bool ensureWiFi(unsigned long timeoutMs = 5000) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.println("Wi-Fi disconnected. Trying reconnect...");
  WiFi.disconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi reconnected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("Wi-Fi still unavailable.");
  return false;
}

void connectWiFi() {
  showMessage("Conectando", "Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    showMessage("Wi-Fi OK", WiFi.localIP().toString());
    Serial.print("Wi-Fi OK. ESP32 IP: ");
    Serial.println(WiFi.localIP());
    delay(1000);
  } else {
    showMessage("Sem internet", "Modo seguro");
    Serial.println("Wi-Fi unavailable. Running offline-safe mode.");
    delay(1200);
  }
}

// ---------------- RFID helpers ----------------
String readCardUid() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return "";
  }

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  Serial.print("RFID UID: ");
  Serial.println(uid);
  beepCardRead();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return uid;
}

// ---------------- Keypad helpers ----------------
bool waitForKey(char expected, unsigned long timeoutMs) {
  unsigned long started = millis();
  while (millis() - started < timeoutMs) {
    char key = keypad.getKey();
    if (key) {
      Serial.print("Key pressed: ");
      Serial.println(key);
    }
    if (key == expected) return true;
    if (key == 'B') return false;
    delay(20);
  }
  return false;
}

// ---------------- Scale helpers ----------------
long readAverageWeightGrams(int samples = 5) {
  float sum = 0;

  for (int i = 0; i < samples; i++) {
    sum += scale.get_units(1);
    delay(60);
  }

  return lround(sum / samples);
}

bool waitForScaleEmpty(unsigned long timeoutMs, const String& line1 = "Retire", const String& line2 = "tampinhas") {
  const long EMPTY_THRESHOLD_GRAMS = 3;
  unsigned long started = millis();

  while (millis() - started < timeoutMs) {
    long grams = readAverageWeightGrams(3);
    Serial.print("Checking empty scale, grams: ");
    Serial.println(grams);

    if (grams <= EMPTY_THRESHOLD_GRAMS) {
      return true;
    }

    showMessage(line1, line2);
    delay(500);
  }

  return false;
}

bool waitForWeightPlaced(unsigned long timeoutMs) {
  const long LOAD_DETECTION_GRAMS = 3;
  unsigned long started = millis();

  showMessage("Coloque", "tampinhas");

  while (millis() - started < timeoutMs) {
    char key = keypad.getKey();
    if (key == 'B') {
      beepCancel();
      showMessage("Cancelado", "Professor");
      delay(1000);
      return false;
    }

    long grams = readAverageWeightGrams(3);
    Serial.print("Waiting weight, grams: ");
    Serial.println(grams);

    if (grams > LOAD_DETECTION_GRAMS) {
      return true;
    }

    delay(150);
  }

  beepError();
  showMessage("Tempo esgotado", "Sem lancamento");
  delay(1500);
  return false;
}

long stableWeightGrams() {
  const int samples = 10;
  const long MAX_VARIATION_GRAMS = 3;
  const unsigned long STABILITY_TIMEOUT_MS = 25000;

  unsigned long started = millis();
  showMessage("Pesando...", "Aguarde");

  while (millis() - started < STABILITY_TIMEOUT_MS) {
    long readings[samples];

    for (int i = 0; i < samples; i++) {
      readings[i] = readAverageWeightGrams(2);
      delay(100);
    }

    long minV = readings[0];
    long maxV = readings[0];
    long sum = 0;

    for (int i = 0; i < samples; i++) {
      if (readings[i] < minV) minV = readings[i];
      if (readings[i] > maxV) maxV = readings[i];
      sum += readings[i];
    }

    long grams = sum / samples;

    Serial.print("Stable check | min: ");
    Serial.print(minV);
    Serial.print(" max: ");
    Serial.print(maxV);
    Serial.print(" avg: ");
    Serial.println(grams);

    if (grams <= 0) {
      return 0;
    }

    if (maxV - minV <= MAX_VARIATION_GRAMS) {
      return grams;
    }

    showMessage("Peso variando", "Aguarde...");
    delay(500);
  }

  return -1;
}

// ---------------- Local student cache ----------------
String cachePathForStudent(const String& rfidUid) {
  return "/student_" + rfidUid + ".json";
}

void saveStudentCache(
  const String& rfidUid,
  const String& name,
  const String& classroom,
  const String& callNumber,
  int totalCredits
) {
  String path = cachePathForStudent(rfidUid);
  File file = LittleFS.open(path, "w");

  if (!file) {
    Serial.println("Failed to save student cache.");
    return;
  }

  StaticJsonDocument<512> doc;
  doc["rfid_uid"] = rfidUid;
  doc["name"] = name;
  doc["classroom"] = classroom;
  doc["call_number"] = callNumber;
  doc["total_credits"] = totalCredits;

  serializeJson(doc, file);
  file.close();

  Serial.print("Student cached locally: ");
  Serial.println(rfidUid);
}

bool loadStudentCache(
  const String& rfidUid,
  String& name,
  String& classroom,
  String& callNumber,
  int& totalCredits
) {
  String path = cachePathForStudent(rfidUid);

  if (!LittleFS.exists(path)) {
    Serial.print("No local cache for RFID: ");
    Serial.println(rfidUid);
    return false;
  }

  File file = LittleFS.open(path, "r");
  if (!file) {
    return false;
  }

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.println("Invalid student cache file.");
    return false;
  }

  name = doc["name"].as<String>();
  classroom = doc["classroom"].as<String>();
  callNumber = doc["call_number"].as<String>();
  totalCredits = doc["total_credits"].as<int>();

  Serial.print("Student loaded from local cache: ");
  Serial.println(name);
  return true;
}

// ---------------- API helpers ----------------
bool postJson(const String& path, const String& body, String& response) {
  if (!ensureWiFi(3000)) return false;

  HTTPClient http;
  http.begin(String(API_BASE_URL) + path);
  http.addHeader("Content-Type", "application/json");
  int status = http.POST(body);
  response = http.getString();
  http.end();

  Serial.print("POST ");
  Serial.print(path);
  Serial.print(" -> HTTP ");
  Serial.println(status);
  Serial.println(response);

  return status >= 200 && status < 300;
}

bool lookupStudent(
  const String& rfidUid,
  String& name,
  String& classroom,
  String& callNumber,
  int& totalCredits
) {
  studentLoadedFromCache = false;

  if (!ensureWiFi(2500)) {
    Serial.println("No Wi-Fi. Trying local student cache...");
    if (loadStudentCache(rfidUid, name, classroom, callNumber, totalCredits)) {
      studentLoadedFromCache = true;
      return true;
    }
    return false;
  }

  HTTPClient http;
  http.begin(String(API_BASE_URL) + "/api/students/by-rfid/" + rfidUid);
  int status = http.GET();
  String payload = http.getString();
  http.end();

  Serial.print("Student lookup HTTP status: ");
  Serial.println(status);
  Serial.print("Student lookup response: ");
  Serial.println(payload);

  if (status == 200) {
    StaticJsonDocument<512> doc;

    if (deserializeJson(doc, payload)) {
      Serial.println("Failed to parse student lookup response.");
      return false;
    }

    if (!doc["found"]) {
      Serial.println("RFID not linked to an active student on server.");
      return false;
    }

    name = doc["name"].as<String>();
    classroom = doc["classroom"].as<String>();
    callNumber = doc["call_number"].as<String>();
    totalCredits = doc["total_credits"].as<int>();

    saveStudentCache(rfidUid, name, classroom, callNumber, totalCredits);
    return true;
  }

  Serial.println("Server unavailable. Trying local student cache...");
  if (loadStudentCache(rfidUid, name, classroom, callNumber, totalCredits)) {
    studentLoadedFromCache = true;
    return true;
  }

  return false;
}

// ---------------- Transactions ----------------
String makeTransactionUid() {
  return String(DEVICE_ID) + "-" + String(millis()) + "-" + String((uint32_t)esp_random(), HEX);
}

int countPendingTransactions() {
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int count = 0;

  while (file) {
    String path = file.name();
    file.close();

    if (path.indexOf("pending_") >= 0) {
      count++;
    }

    file = root.openNextFile();
  }

  return count;
}

bool savePendingTransaction(const String& transactionUid, const String& rfidUid, long grams) {
  String path = "/pending_" + transactionUid + ".json";
  File file = LittleFS.open(path, "w");

  if (!file) {
    beepError();
    showMessage("Erro memoria", "Nao salvou");
    Serial.println("Failed to save pending transaction.");
    delay(2000);
    return false;
  }

  StaticJsonDocument<384> doc;
  doc["transaction_uid"] = transactionUid;
  doc["device_id"] = DEVICE_ID;
  doc["device_key"] = DEVICE_KEY;
  doc["rfid_uid"] = rfidUid;
  doc["weight_grams"] = grams;
  doc["credits"] = grams;
  doc["created_at_device"] = String(millis());

  size_t written = serializeJson(doc, file);
  file.flush();
  file.close();

  if (written == 0 || !LittleFS.exists(path)) {
    beepError();
    showMessage("Erro memoria", "Nao confirmou");
    Serial.print("Pending transaction was not confirmed on disk: ");
    Serial.println(path);
    delay(2000);
    return false;
  }

  Serial.print("Pending transaction saved: ");
  Serial.println(path);
  Serial.print("Pending count after save: ");
  Serial.println(countPendingTransactions());
  return true;
}

bool syncTransactionFile(const String& path) {
  File file = LittleFS.open(path, "r");
  if (!file) return false;

  String body = file.readString();
  file.close();

  String response;
  bool ok = postJson("/api/transactions/sync", body, response);

  if (ok) {
    LittleFS.remove(path);
    Serial.print("Synced and removed: ");
    Serial.println(path);
  } else {
    Serial.print("Sync failed, keeping pending file: ");
    Serial.println(path);
  }

  return ok;
}

void syncPendingTransactions(bool showResult = false) {
  int before = countPendingTransactions();

  if (!ensureWiFi(1500)) {
    if (showResult && before > 0) {
      showMessage("Sem internet", String(before) + " pendentes");
      delay(1200);
    }
    return;
  }

  if (before > 0) {
    Serial.print("Syncing pending transactions. Found: ");
    Serial.println(before);
  }

  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int synced = 0;

  while (file) {
    String path = file.name();
    file.close();

    if (path.indexOf("pending_") >= 0) {
      if (syncTransactionFile(path)) {
        synced++;
      }
    }

    file = root.openNextFile();
  }

  int after = countPendingTransactions();

  if (before > 0 || synced > 0 || after > 0) {
    Serial.print("Sync finished. Synced: ");
    Serial.print(synced);
    Serial.print(" | Remaining: ");
    Serial.println(after);
  }

  if (showResult || synced > 0) {
    if (after == 0) {
      if (synced > 0) {
        beepSuccess();
        showMessage("Sincronizado", String(synced) + " enviados");
      } else if (showResult) {
        showMessage("Sem pendencias", "Tudo certo");
      }
    } else {
      showMessage("Pendentes", String(after) + " salvos");
    }
    delay(1200);
  }
}

void pingServer() {
  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  doc["device_key"] = DEVICE_KEY;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["pending_count"] = countPendingTransactions();

  String body;
  serializeJson(doc, body);

  String response;
  postJson("/api/devices/ping", body, response);
}

// ---------------- Setup and main loop ----------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Tampinha Magica firmware starting...");

  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);
  beepReady();

  Wire.begin();
  lcd.init();
  lcd.backlight();
  showMessage("Tampinha", "Magica");

  if (!LittleFS.begin(true)) {
    showMessage("Erro memoria", "Local");
    Serial.println("LittleFS failed.");
    delay(2000);
  }

  SPI.begin();
  rfid.PCD_Init();

  scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
  scale.set_scale(SCALE_CALIBRATION_FACTOR);
  showMessage("Zerando", "balanca...");
  scale.tare();
  delay(800);

  connectWiFi();
  pingServer();
  syncPendingTransactions(true);
}

void loop() {
  showMessage("Sistema pronto", "Aprox. cartao");

  String uid = "";
  unsigned long lastBackgroundSync = 0;

  while (uid == "") {
    uid = readCardUid();

    if (millis() - lastBackgroundSync > 10000) {
      syncPendingTransactions(false);
      lastBackgroundSync = millis();
    }

    delay(100);
  }

  String name, classroom, callNumber;
  int totalCredits = 0;

  if (!lookupStudent(uid, name, classroom, callNumber, totalCredits)) {
    beepError();

    if (WiFi.status() != WL_CONNECTED) {
      showMessage("Sem internet", "Cartao sem cache");
    } else {
      showMessage("Cartao nao", "cadastrado");
    }

    delay(2200);
    return;
  }

  showMessage(name, classroom + " N:" + callNumber);
  delay(1800);

  if (studentLoadedFromCache) {
    beepOfflineSaved();
    showMessage("Modo offline", "Aluno salvo");
    delay(1200);
  }

  // Prevent accidental tare with caps already on the scale.
  if (!waitForScaleEmpty(30000, "Retire peso", "da balanca")) {
    beepError();
    showMessage("Balanca ocupada", "Tente depois");
    delay(1800);
    return;
  }

  showMessage("Zerando", "balanca...");
  scale.tare();
  delay(800);

  if (!waitForWeightPlaced(90000)) {
    return;
  }

  long grams = stableWeightGrams();

  if (grams < 0) {
    beepError();
    showMessage("Peso instavel", "Tente de novo");
    delay(1800);
    return;
  }

  if (grams == 0) {
    beepError();
    showMessage("Peso zerado", "Sem lancamento");
    delay(1800);
    return;
  }

  showMessage("Peso: " + String(grams) + "g", "A=OK B=Canc");
  beepConfirmRequest();

  if (!waitForKey('A', 30000)) {
    beepCancel();
    showMessage("Cancelado", "Professor");
    delay(1200);
    return;
  }

  String txUid = makeTransactionUid();

  if (!savePendingTransaction(txUid, uid, grams)) {
    showMessage("Falha ao salvar", "Nao recolher");
    delay(2500);
    return;
  }

  showMessage("Credito salvo", "Sincronizando");

  String path = "/pending_" + txUid + ".json";
  if (syncTransactionFile(path)) {
    beepSuccess();
    showMessage("Registrado!", "+" + String(grams) + " creditos");
  } else {
    beepOfflineSaved();
    showMessage("Sem internet", "Credito salvo");
  }

  delay(2200);

  // Avoid a second operation while the same caps are still on the scale.
  waitForScaleEmpty(90000, "Retire", "tampinhas");
}
