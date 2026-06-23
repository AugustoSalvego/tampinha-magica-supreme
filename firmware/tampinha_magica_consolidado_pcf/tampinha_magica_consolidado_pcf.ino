/*
  Tampinha Magica Supreme - Consolidated ESP32 Firmware

  Main characteristics:
  - LCD 16x2 on I2C address 0x27
  - PCF8574 + 4x4 keypad on I2C address 0x20
  - RC522 RFID
  - HX711 + load cell
  - Active buzzer
  - LittleFS local student cache and robust offline queue
  - Online-first, offline-safe flow

  IMPORTANT:
  1) Put your real Wi-Fi and API values in the Network Configuration section.
  2) API_BASE_URL must be the LAN IPv4 address of the computer running FastAPI,
     not 127.0.0.1.
  3) Current calibration factor was measured with a 64 g reference weight.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HX711.h>
#include <SPI.h>
#include <MFRC522.h>
#include <LittleFS.h>
#include <math.h>

// ---------------- Network Configuration ----------------
// Keep real credentials out of GitHub. Later, move them to secrets.h.
const char* WIFI_SSID = "Augusto";
const char* WIFI_PASSWORD = "777pato777";
const char* API_BASE_URL = "http://192.168.100.5:8000";
const char* DEVICE_ID = "terminal-01";
const char* DEVICE_KEY = "change-this-device-key";
const char* FIRMWARE_VERSION = "3.0.0-pcf-robust";

// ---------------- I2C Configuration ----------------
#define I2C_SDA 21
#define I2C_SCL 22
#define LCD_ADDRESS 0x27
#define PCF8574_ADDRESS 0x20

// ---------------- RFID Configuration ----------------
#define RFID_SS_PIN 5
#define RFID_RST_PIN 27

// ---------------- HX711 Configuration ----------------
#define HX711_DOUT_PIN 32
#define HX711_SCK_PIN 33
float SCALE_CALIBRATION_FACTOR = -431.8;

// ---------------- Buzzer Configuration ----------------
// Current consolidated code uses GPIO25 because the direct keypad no longer uses it.
// If your buzzer is physically wired to another pin, change only this line.
#define BUZZER_PIN 25

// ---------------- Keypad Configuration via PCF8574 ----------------
const byte ROWS = 4;
const byte COLS = 4;

// PCF8574 pin mapping:
// Columns P0 P1 P2 P3
// Rows    P4 P5 P6 P7
const byte rowPins[ROWS] = {4, 5, 6, 7};
const byte colPins[COLS] = {0, 1, 2, 3};

const char keyMap[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// ---------------- Storage Configuration ----------------
const char* PENDING_DIR = "/queue";
const char* CACHE_DIR = "/cache";

// ---------------- Objects ----------------
LiquidCrystal_I2C lcd(LCD_ADDRESS, 16, 2);
HX711 scale;
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);

// ---------------- Runtime State ----------------
String lastDisplayLine1 = "";
String lastDisplayLine2 = "";
bool studentLoadedFromCache = false;
unsigned long lastBackgroundSync = 0;
unsigned long lastWiFiReconnectAttempt = 0;

const unsigned long BACKGROUND_SYNC_INTERVAL_MS = 30000;
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 15000;

// ---------------- Buzzer Helpers ----------------
void buzzerOn() {
  digitalWrite(BUZZER_PIN, HIGH);
}

void buzzerOff() {
  digitalWrite(BUZZER_PIN, LOW);
}

void beep(unsigned long durationMs) {
  buzzerOn();
  delay(durationMs);
  buzzerOff();
}

void beepCardRead() {
  beep(70);
}

void beepSuccess() {
  beep(80);
  delay(80);
  beep(150);
}

void beepError() {
  beep(250);
}

void beepCancel() {
  beep(100);
  delay(100);
  beep(100);
}

void beepOfflineSaved() {
  beep(70);
  delay(70);
  beep(70);
  delay(70);
  beep(70);
}

// ---------------- LCD Helpers ----------------
void showMessage(const String& line1, const String& line2 = "") {
  String safeLine1 = line1.substring(0, 16);
  String safeLine2 = line2.substring(0, 16);

  if (safeLine1 == lastDisplayLine1 && safeLine2 == lastDisplayLine2) {
    return;
  }

  lastDisplayLine1 = safeLine1;
  lastDisplayLine2 = safeLine2;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(safeLine1);
  lcd.setCursor(0, 1);
  lcd.print(safeLine2);
}

// ---------------- PCF8574 / Keypad Helpers ----------------
void pcfWrite(byte value) {
  Wire.beginTransmission(PCF8574_ADDRESS);
  Wire.write(value);
  Wire.endTransmission();
}

byte pcfRead() {
  Wire.requestFrom(PCF8574_ADDRESS, (uint8_t)1);

  if (Wire.available()) {
    return Wire.read();
  }

  return 0xFF;
}

char readRawKeypad() {
  for (byte row = 0; row < ROWS; row++) {
    byte outputState = 0xFF;
    outputState &= ~(1 << rowPins[row]);

    pcfWrite(outputState);
    delayMicroseconds(300);

    byte inputState = pcfRead();

    for (byte col = 0; col < COLS; col++) {
      bool columnIsLow = !(inputState & (1 << colPins[col]));

      if (columnIsLow) {
        pcfWrite(0xFF);
        return keyMap[row][col];
      }
    }
  }

  pcfWrite(0xFF);
  return '\0';
}

char getKey() {
  static char lastRawKey = '\0';
  static char stableKey = '\0';
  static unsigned long changedAt = 0;

  char rawKey = readRawKeypad();

  if (rawKey != lastRawKey) {
    lastRawKey = rawKey;
    changedAt = millis();
  }

  if (millis() - changedAt < 35) {
    return '\0';
  }

  if (rawKey == stableKey) {
    return '\0';
  }

  stableKey = rawKey;
  return stableKey;
}

bool waitForKey(char expected, unsigned long timeoutMs) {
  unsigned long started = millis();

  while (millis() - started < timeoutMs) {
    char key = getKey();

    if (key != '\0') {
      Serial.print("Key pressed: ");
      Serial.println(key);
    }

    if (key == expected) {
      return true;
    }

    if (key == 'B') {
      return false;
    }

    delay(20);
  }

  return false;
}

// ---------------- Wi-Fi Helpers ----------------
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

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - lastWiFiReconnectAttempt < WIFI_RECONNECT_INTERVAL_MS) {
    return;
  }

  lastWiFiReconnectAttempt = millis();
  ensureWiFi(2500);
}

// ---------------- RFID Helpers ----------------
String readCardUid() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return "";
  }

  String uid = "";

  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) {
      uid += "0";
    }

    uid += String(rfid.uid.uidByte[i], HEX);
  }

  uid.toUpperCase();

  Serial.print("RFID UID: ");
  Serial.println(uid);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return uid;
}

// ---------------- Scale Helpers ----------------
long readAverageWeightGrams(int samples = 5) {
  float sum = 0;

  for (int i = 0; i < samples; i++) {
    sum += scale.get_units(1);
    delay(60);
  }

  return lround(sum / samples);
}

bool waitForScaleEmpty(
  unsigned long timeoutMs,
  const String& line1 = "Retire",
  const String& line2 = "tampinhas"
) {
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
    char key = getKey();

    if (key == 'B') {
      showMessage("Cancelado", "Professor");
      beepCancel();
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

  showMessage("Tempo esgotado", "Sem lancamento");
  beepError();
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

    long minValue = readings[0];
    long maxValue = readings[0];
    long sum = 0;

    for (int i = 0; i < samples; i++) {
      if (readings[i] < minValue) {
        minValue = readings[i];
      }

      if (readings[i] > maxValue) {
        maxValue = readings[i];
      }

      sum += readings[i];
    }

    long grams = sum / samples;

    Serial.print("Stable check | min: ");
    Serial.print(minValue);
    Serial.print(" max: ");
    Serial.print(maxValue);
    Serial.print(" avg: ");
    Serial.println(grams);

    if (grams <= 0) {
      return 0;
    }

    if (maxValue - minValue <= MAX_VARIATION_GRAMS) {
      return grams;
    }

    showMessage("Peso variando", "Aguarde...");
    delay(500);
  }

  return -1;
}

// ---------------- File System Helpers ----------------
void ensureDirectory(const char* path) {
  if (!LittleFS.exists(path)) {
    bool created = LittleFS.mkdir(path);
    Serial.print("Directory ");
    Serial.print(path);
    Serial.print(" created: ");
    Serial.println(created ? "yes" : "no");
  }
}

void ensureStorageDirectories() {
  ensureDirectory(PENDING_DIR);
  ensureDirectory(CACHE_DIR);
}

String cachePathForStudent(const String& rfidUid) {
  return String(CACHE_DIR) + "/student_" + rfidUid + ".json";
}

String makeTransactionUid() {
  return String(DEVICE_ID) + "-" + String(millis()) + "-" + String((uint32_t)esp_random(), HEX);
}

String makePendingPath() {
  return String(PENDING_DIR) + "/p" + String(millis(), HEX) + String((uint32_t)esp_random(), HEX) + ".json";
}

bool isPendingPath(const String& path) {
  return path.startsWith(String(PENDING_DIR) + "/p") && path.endsWith(".json");
}

// ---------------- Local Student Cache ----------------
void saveStudentCache(
  const String& rfidUid,
  const String& name,
  const String& classroom,
  const String& callNumber,
  int totalCredits
) {
  ensureDirectory(CACHE_DIR);

  String path = cachePathForStudent(rfidUid);
  String tempPath = path + ".tmp";

  File file = LittleFS.open(tempPath, "w");

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

  size_t written = serializeJson(doc, file);
  file.flush();
  file.close();

  if (written == 0) {
    LittleFS.remove(tempPath);
    Serial.println("Student cache write failed.");
    return;
  }

  LittleFS.remove(path);
  if (!LittleFS.rename(tempPath, path)) {
    LittleFS.remove(tempPath);
    Serial.println("Student cache rename failed.");
    return;
  }

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

// ---------------- HTTP / API Helpers ----------------
bool postJson(const String& path, const String& body, String& response) {
  if (!ensureWiFi(3000)) {
    return false;
  }

  HTTPClient http;
  http.setTimeout(5000);
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
  http.setTimeout(5000);
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

// ---------------- Robust Offline Queue ----------------
int countPendingTransactions() {
  ensureDirectory(PENDING_DIR);

  File queue = LittleFS.open(PENDING_DIR);

  if (!queue || !queue.isDirectory()) {
    Serial.println("Pending directory unavailable while counting.");
    return 0;
  }

  int count = 0;
  File file = queue.openNextFile();

  while (file) {
    String path = file.name();
    file.close();

    if (isPendingPath(path)) {
      count++;
    }

    file = queue.openNextFile();
  }

  queue.close();
  return count;
}

bool savePendingTransaction(
  const String& transactionUid,
  const String& rfidUid,
  long grams,
  String& savedPath
) {
  ensureDirectory(PENDING_DIR);

  savedPath = makePendingPath();
  String tempPath = savedPath + ".tmp";

  StaticJsonDocument<384> doc;
  doc["transaction_uid"] = transactionUid;
  doc["device_id"] = DEVICE_ID;
  doc["device_key"] = DEVICE_KEY;
  doc["rfid_uid"] = rfidUid;
  doc["weight_grams"] = grams;
  doc["credits"] = grams;
  doc["created_at_device"] = String(millis());

  File file = LittleFS.open(tempPath, "w");

  if (!file) {
    showMessage("Erro memoria", "Nao salvou");
    Serial.print("Failed to create pending temp file: ");
    Serial.println(tempPath);
    delay(2000);
    return false;
  }

  size_t written = serializeJson(doc, file);
  file.flush();
  file.close();

  if (written == 0 || !LittleFS.exists(tempPath)) {
    LittleFS.remove(tempPath);
    showMessage("Erro memoria", "Nao confirmou");
    Serial.print("Pending transaction was not written: ");
    Serial.println(tempPath);
    delay(2000);
    return false;
  }

  if (!LittleFS.rename(tempPath, savedPath) || !LittleFS.exists(savedPath)) {
    LittleFS.remove(tempPath);
    showMessage("Erro memoria", "Nao confirmou");
    Serial.print("Pending transaction rename failed: ");
    Serial.println(savedPath);
    delay(2000);
    return false;
  }

  Serial.print("Pending transaction saved and verified: ");
  Serial.println(savedPath);
  Serial.print("Pending count after save: ");
  Serial.println(countPendingTransactions());
  return true;
}

bool syncTransactionFile(const String& path) {
  File file = LittleFS.open(path, "r");

  if (!file) {
    Serial.print("Cannot open pending file: ");
    Serial.println(path);
    return false;
  }

  String body = file.readString();
  file.close();

  if (body.length() < 10) {
    Serial.print("Invalid pending file body, keeping file: ");
    Serial.println(path);
    return false;
  }

  String response;
  bool ok = postJson("/api/transactions/sync", body, response);

  if (ok) {
    if (LittleFS.remove(path)) {
      Serial.print("Synced and removed: ");
      Serial.println(path);
    } else {
      Serial.print("Synced, but could not remove local file: ");
      Serial.println(path);
    }
  } else {
    Serial.print("Sync failed, keeping pending file: ");
    Serial.println(path);
  }

  return ok;
}

void syncPendingTransactions(bool showResult = false) {
  int before = countPendingTransactions();

  if (!ensureWiFi(1500)) {
    if (showResult) {
      showMessage("Sem internet", String(before) + " pendentes");
      delay(1200);
    }

    return;
  }

  ensureDirectory(PENDING_DIR);

  File queue = LittleFS.open(PENDING_DIR);

  if (!queue || !queue.isDirectory()) {
    Serial.println("Pending directory unavailable while syncing.");
    return;
  }

  if (before > 0) {
    Serial.print("Syncing pending transactions. Found: ");
    Serial.println(before);
  }

  int synced = 0;
  File file = queue.openNextFile();

  while (file) {
    String path = file.name();
    file.close();

    if (isPendingPath(path) && syncTransactionFile(path)) {
      synced++;
    }

    file = queue.openNextFile();
  }

  queue.close();

  int after = countPendingTransactions();

  if (before > 0 || synced > 0 || after > 0) {
    Serial.print("Sync finished. Synced: ");
    Serial.print(synced);
    Serial.print(" | Remaining: ");
    Serial.println(after);
  }

  if (showResult || synced > 0) {
    if (after == 0) {
      showMessage("Sincronizado", String(synced) + " enviados");
      if (synced > 0) {
        beepSuccess();
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

void runBackgroundTasks() {
  maintainWiFi();

  if (millis() - lastBackgroundSync >= BACKGROUND_SYNC_INTERVAL_MS) {
    lastBackgroundSync = millis();

    if (WiFi.status() == WL_CONNECTED) {
      pingServer();
      syncPendingTransactions(false);
    }
  }
}

// ---------------- Setup and Main Workflow ----------------
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("Tampinha Magica firmware starting...");

  pinMode(BUZZER_PIN, OUTPUT);
  buzzerOff();

  Wire.begin(I2C_SDA, I2C_SCL);
  pcfWrite(0xFF);

  lcd.init();
  lcd.backlight();
  showMessage("Tampinha", "Magica");
  beepCardRead();

  if (!LittleFS.begin(true)) {
    showMessage("Erro memoria", "Local");
    Serial.println("LittleFS failed.");
    beepError();
    delay(2000);
  }

  ensureStorageDirectories();

  SPI.begin();
  rfid.PCD_Init();

  scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
  scale.set_scale(SCALE_CALIBRATION_FACTOR);

  showMessage("Preparando", "balanca...");

  unsigned long hxStarted = millis();

  while (!scale.is_ready() && millis() - hxStarted < 3000) {
    delay(20);
  }

  if (!scale.is_ready()) {
    showMessage("Erro HX711", "Verifique fios");
    Serial.println("HX711 not ready.");
    beepError();
    delay(2500);
  } else {
    // Discard initial readings while the HX711 and load cell stabilize.
    for (int i = 0; i < 25; i++) {
      scale.get_units(1);
      delay(40);
    }

    showMessage("Zerando", "balanca...");
    scale.tare(20);
    delay(300);

    Serial.print("Startup tare check: ");
    Serial.println(readAverageWeightGrams(5));

    delay(800);
  }
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    pingServer();
  }

  syncPendingTransactions(true);
  showMessage("Sistema pronto", "Aprox. cartao");
}

void loop() {
  runBackgroundTasks();
  showMessage("Sistema pronto", "Aprox. cartao");

  String uid = readCardUid();

  if (uid == "") {
    delay(80);
    return;
  }

  beepCardRead();

  String name;
  String classroom;
  String callNumber;
  int totalCredits = 0;

  if (!lookupStudent(uid, name, classroom, callNumber, totalCredits)) {
    if (WiFi.status() != WL_CONNECTED) {
      showMessage("Sem internet", "Cartao sem cache");
    } else {
      showMessage("Cartao nao", "cadastrado");
    }

    beepError();
    delay(2200);
    return;
  }

  showMessage(name, classroom + " N:" + callNumber);
  delay(1800);

  if (studentLoadedFromCache) {
    showMessage("Modo offline", "Aluno salvo");
    delay(1200);
  }

  if (!waitForScaleEmpty(30000, "Retire peso", "da balanca")) {
    showMessage("Balanca ocupada", "Tente depois");
    beepError();
    delay(1800);
    return;
  }

  showMessage("Zerando", "balanca...");
  scale.tare(10);
  delay(800);

  if (!waitForWeightPlaced(90000)) {
    return;
  }

  long grams = stableWeightGrams();

  if (grams < 0) {
    showMessage("Peso instavel", "Tente de novo");
    beepError();
    delay(1800);
    return;
  }

  if (grams == 0) {
    showMessage("Peso zerado", "Sem lancamento");
    beepError();
    delay(1800);
    return;
  }

  showMessage("Peso: " + String(grams) + "g", "A=OK B=Canc");

  if (!waitForKey('A', 30000)) {
    showMessage("Cancelado", "Professor");
    beepCancel();
    delay(1200);
    return;
  }

  String transactionUid = makeTransactionUid();
  String pendingPath = "";

  if (!savePendingTransaction(transactionUid, uid, grams, pendingPath)) {
    showMessage("Falha ao salvar", "Nao recolher");
    beepError();
    delay(2500);
    return;
  }

  showMessage("Credito salvo", "Sincronizando");

  if (syncTransactionFile(pendingPath)) {
    showMessage("Registrado!", "+" + String(grams) + " creditos");
    beepSuccess();
  } else {
    showMessage("Sem internet", "Credito salvo");
    beepOfflineSaved();
  }

  delay(2200);

  waitForScaleEmpty(90000, "Retire", "tampinhas");
}
