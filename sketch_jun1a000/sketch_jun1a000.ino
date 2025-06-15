// --- LIBRARIES ---
#include <LiquidCrystal.h>
#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WiFiClientSecure.h> // Required for HTTPS
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h> // For SD Card
#include <SD.h>  // For SD Card
#include <time.h> // For NTP Time

// --- WIFI AND SERVER CONFIGURATION ---
#define WIFI_SSID "Alla"
#define WIFI_PASSWORD "GREA@G&R6"
#define SERVER_HOST "https://192.168.1.12:7069" // Use your server's host address
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 3600 * 2 // For EET (Egypt Standard Time, UTC+2)
#define DAYLIGHT_OFFSET_SEC 0   // No daylight saving for simplicity

// --- HARDWARE PIN DEFINITIONS ---
// LCD Pins
const int rs = 19, en = 23, d4 = 32, d5 = 33, d6 = 25, d7 = 26;
// Fingerprint Sensor RX/TX
const int FINGERPRINT_RX = 16;
const int FINGERPRINT_TX = 17;
// Button Pin
const int BUTTON_PIN = 18;
// SD Card CS Pin
const int SD_CS_PIN = 5; // Standard CS pin for many ESP32 boards, change if needed

// --- GLOBAL OBJECTS ---
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);
HardwareSerial fingerSerial(2); // Use UART2 for the sensor
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);
WiFiClientSecure client; // Use a secure client for HTTPS

// --- STATE MANAGEMENT ---
enum class MenuState {
  MAIN_MENU,
  OPTIONS_MENU
};
MenuState currentMenuState = MenuState::MAIN_MENU;

// For storing names locally after fetching from the server
struct FingerprintData {
  uint16_t id;
  String name;
};
FingerprintData fingerprintCache[128]; // Max 128 fingerprints
const char* LOG_FILE = "/attendance_log.txt";

// --- FUNCTION PROTOTYPES ---
void setupWiFi();
void displayMessage(String line1, String line2 = "", int delayMs = 0);
void handleButton();
void runMainMenuAction();
void runOptionsMenuAction();
void enrollNewFingerprint();
int getFingerprintImage(int step);
int createAndStoreModel(uint16_t id);
String getFingerNameFromSerial(uint16_t id);
void scanForFingerprint();
void showOptionsMenu();
void syncAndDisplayServerData();
void attemptToClearAllData();
bool confirmAdminPassword();
void syncSensorWithServer();
bool sendFingerprintToServer(uint16_t id, const String& name);
int getNextAvailableIDFromServer();

// New function prototypes for offline logging
void setupSDCard();
void setupTime();
void recordAttendance(uint16_t id);
bool logAttendanceToServer(uint16_t id, time_t timestamp);
void logAttendanceOffline(uint16_t id, time_t timestamp);
void syncOfflineLogs();

// =================================================================================================
// SETUP FUNCTION
// =================================================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial); // Wait for serial monitor to open

  // --- INITIALIZE LCD ---
  lcd.begin(16, 2);
  displayMessage("System Starting", "Please wait...");

  // --- INITIALIZE SD CARD ---
  setupSDCard();

  // --- INITIALIZE BUTTON ---
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // --- INITIALIZE FINGERPRINT SENSOR ---
  fingerSerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  if (finger.verifyPassword()) {
    Serial.println("Fingerprint sensor found!");
  } else {
    displayMessage("Sensor Error", "Check wiring.", 10000);
    while (1) delay(1); // Halt on error
  }

  // --- CONNECT TO WIFI ---
  setupWiFi();
  
  // --- CONFIGURE TIME FROM NTP ---
  setupTime();

  // IMPORTANT: Allow insecure HTTPS connections for local development server
  client.setInsecure();

  // --- INITIAL SYNC WITH SERVER ---
  syncSensorWithServer();

  // --- SYNC ANY PENDING OFFLINE LOGS ---
  syncOfflineLogs();

  displayMessage("Add: Press Btn", "Scan: Place Finger");
}

// =================================================================================================
// MAIN LOOP
// =================================================================================================
void loop() {
  handleButton();      // Check for button presses to navigate menus or enroll
  scanForFingerprint(); // Continuously scan for a known fingerprint

  // Periodically try to sync offline logs if WiFi is available
  static unsigned long lastSyncAttempt = 0;
  if (WiFi.status() == WL_CONNECTED && (millis() - lastSyncAttempt > 300000)) { // Every 5 minutes
      lastSyncAttempt = millis();
      Serial.println("Periodic check for offline logs to sync...");
      syncOfflineLogs();
  }

  delay(50);             // Small delay to prevent overwhelming the processor
}

// =================================================================================================
// WIFI & DISPLAY FUNCTIONS
// =================================================================================================
/**
 * @brief Connects to the configured WiFi network.
 */
void setupWiFi() {
  displayMessage("Connecting WiFi", "...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    lcd.setCursor(dots, 1);
    lcd.print(".");
    dots = (dots + 1) % 16;
    if (dots == 0) {
      lcd.setCursor(0, 1);
      lcd.print("                ");
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  displayMessage("WiFi Connected", WiFi.localIP().toString(), 2000);
}

/**
 * @brief Displays a two-line message on the LCD and optionally clears it after a delay.
 */
void displayMessage(String line1, String line2, int delayMs) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  if (delayMs > 0) {
    delay(delayMs);
  }
}

// =================================================================================================
// BUTTON & MENU LOGIC
// =================================================================================================

/**
 * @brief Handles short and long presses of the button to trigger actions.
 */
void handleButton() {
  static unsigned long buttonPressTime = 0;
  static bool buttonWasPressed = false;

  bool isPressed = (digitalRead(BUTTON_PIN) == LOW);

  if (isPressed && !buttonWasPressed) {
    buttonPressTime = millis();
    buttonWasPressed = true;
  } else if (!isPressed && buttonWasPressed) {
    unsigned long pressDuration = millis() - buttonPressTime;
    if (pressDuration < 5000) { // Short Press (< 5 seconds)
      if (currentMenuState == MenuState::MAIN_MENU) {
        runMainMenuAction();
      } else if (currentMenuState == MenuState::OPTIONS_MENU) {
        runOptionsMenuAction();
      }
    }
    buttonWasPressed = false;
  } else if (isPressed && buttonWasPressed) {
    unsigned long pressDuration = millis() - buttonPressTime;
    if (pressDuration > 5000 && pressDuration < 5100) { // Long press (triggered once around 5s)
      if (currentMenuState == MenuState::MAIN_MENU) {
        showOptionsMenu();
      }
    } else if (pressDuration > 10000 && pressDuration < 10100) { // Very long press (triggered once around 10s)
      if (currentMenuState == MenuState::OPTIONS_MENU) {
        attemptToClearAllData();
      }
    }
  }
}

void runMainMenuAction() {
  enrollNewFingerprint();
  displayMessage("Add: Press Btn", "Scan: Place Finger");
}

void runOptionsMenuAction() {
  syncAndDisplayServerData();
  displayMessage("Add: Press Btn", "Scan: Place Finger");
  currentMenuState = MenuState::MAIN_MENU;
}

// =================================================================================================
// FINGERPRINT ENROLLMENT PROCESS
// =================================================================================================

void enrollNewFingerprint() {
  displayMessage("Getting ID...", "From server");
  int newId = getNextAvailableIDFromServer();

  if (newId < 0 || newId >= 128) {
    displayMessage("Enroll Failed", "No available ID", 2000);
    return;
  }
  
  Serial.printf("Starting enrollment for new ID: %d\n", newId);
  displayMessage("Place finger", "On the sensor");

  if (getFingerprintImage(1) != FINGERPRINT_OK) return;

  displayMessage("Place again", "Same finger");
  
  if (getFingerprintImage(2) != FINGERPRINT_OK) return;

  if (createAndStoreModel(newId) != FINGERPRINT_OK) return;

  String name = getFingerNameFromSerial(newId);
  
  if (sendFingerprintToServer(newId, name)) {
    fingerprintCache[newId] = { (uint16_t)newId, name };
    displayMessage("Enroll Success!", "ID: " + String(newId), 2000);
  } else {
    displayMessage("Enroll Failed", "Server error", 2000);
    finger.deleteModel(newId);
  }
}

int getFingerprintImage(int step) {
  int p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p != FINGERPRINT_OK && p != FINGERPRINT_NOFINGER) {
      displayMessage("Imaging Error", "", 1500);
      return p;
    }
  }

  p = finger.image2Tz(step);
  if (p != FINGERPRINT_OK) {
    displayMessage("Processing Error", "", 1500);
  }
  return p;
}

int createAndStoreModel(uint16_t id) {
  displayMessage("Creating model...", "");
  int p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    displayMessage("Fingers do not", "match. Try again.", 2000);
    return p;
  }

  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) {
    displayMessage("Storage Error", "Slot may be full.", 2000);
  }
  return p;
}

String getFingerNameFromSerial(uint16_t id) {
  displayMessage("Enter name in", "Serial Monitor");
  Serial.println("Please enter a name for ID " + String(id) + " and press Enter:");
  
  unsigned long startTime = millis();
  String name = "";
  while (millis() - startTime < 30000) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (name.length() > 0) break;
      } else {
        name += c;
      }
    }
  }
  
  if (name.length() == 0) {
    name = "User_" + String(id);
    Serial.println("Timeout. Using default name: " + name);
  }
  
  return name;
}

// =================================================================================================
// CORE OPERATIONS (SCAN, MENU, CLEAR)
// =================================================================================================

/**
 * @brief Scans for a fingerprint, displays info, and logs the attendance.
 */
void scanForFingerprint() {
  if (finger.getImage() != FINGERPRINT_OK) return;
  if (finger.image2Tz() != FINGERPRINT_OK) return;
  if (finger.fingerFastSearch() != FINGERPRINT_OK) return;

  // Found a match!
  uint16_t id = finger.fingerID;
  String name = fingerprintCache[id].name;
  if (name.isEmpty()) {
    name = "Name not synced";
  }
  
  displayMessage("Welcome!", "ID: " + String(id) + " " + name, 2000);
  
  // Record the attendance log (online or offline)
  recordAttendance(id);
  
  // Return to the main screen
  displayMessage("Add: Press Btn", "Scan: Place Finger");
}

void showOptionsMenu() {
  currentMenuState = MenuState::OPTIONS_MENU;
  displayMessage("1.Show Data(short)", "2.Del All(hold 10s)");
}

void syncAndDisplayServerData() {
  if (WiFi.status() != WL_CONNECTED) {
    displayMessage("WiFi Error", "Not connected", 2000);
    return;
  }

  displayMessage("Getting data...", "From server");

  HTTPClient http;
  String url = String(SERVER_HOST) + "/api/SensorData";
  http.begin(client, url);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();
    
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.println("Failed to parse server data.");
      displayMessage("JSON Error", "", 2000);
      return;
    }
    
    displayMessage("Data in Serial", "Monitor", 2000);
    Serial.println("--- Fingerprint Data from Server ---");
    for (JsonObject item : doc.as<JsonArray>()) {
      int id = item["id"];
      const char* name = item["name"];
      Serial.printf("ID: %d, Name: %s\n", id, name);
      if(id >= 0 && id < 128) {
        fingerprintCache[id] = {(uint16_t)id, String(name)};
      }
    }
    Serial.println("------------------------------------");

  } else {
    http.end();
    Serial.printf("Failed to get data. HTTP Error: %d\n", httpCode);
    displayMessage("Server Error", "Code: " + String(httpCode), 2000);
  }
}

void attemptToClearAllData() {
  if (confirmAdminPassword()) {
    displayMessage("Clearing Data...", "");
    
    HTTPClient http;
    String url = String(SERVER_HOST) + "/api/SensorData/clear";
    http.begin(client, url);
    int httpCode = http.POST("");
    http.end();

    if (httpCode == HTTP_CODE_OK) {
      Serial.println("Server data cleared successfully.");
      displayMessage("Server Cleared", "Clearing sensor...");
      
      if (finger.emptyDatabase() == FINGERPRINT_OK) {
        Serial.println("Fingerprint sensor cleared.");
        displayMessage("All Data Deleted", "", 2000);
      } else {
        Serial.println("Failed to clear sensor.");
        displayMessage("Sensor Clear Fail", "", 2000);
      }
      
      for (int i = 0; i < 128; i++) {
        fingerprintCache[i] = {0, ""};
      }
      // Also clear any pending offline logs
      SD.remove(LOG_FILE);

    } else {
      Serial.printf("Failed to clear server data. HTTP Error: %d\n", httpCode);
      displayMessage("Server Clear Fail", "Code: " + String(httpCode), 2000);
    }

  } else {
    displayMessage("Wrong Password", "Operation Canceled", 2000);
  }
  
  displayMessage("Add: Press Btn", "Scan: Place Finger");
  currentMenuState = MenuState::MAIN_MENU;
}

bool confirmAdminPassword() {
  displayMessage("Enter Password", "in Serial Monitor");
  Serial.println("ADMIN ACTION: Enter password to confirm data deletion:");
  
  String input = "";
  unsigned long startTime = millis();
  while (millis() - startTime < 30000) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (input == "admin") {
          Serial.println("Password correct.");
          return true;
        } else {
          Serial.println("Incorrect password.");
          return false;
        }
      } else {
        input += c;
      }
    }
  }
  Serial.println("Password entry timed out.");
  return false;
}

// =================================================================================================
// SD CARD & OFFLINE LOGGING
// =================================================================================================

/**
 * @brief Initializes the SD card module.
 */
void setupSDCard() {
    Serial.println("Initializing SD card...");
    displayMessage("Init SD Card...", "");
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("SD Card initialization failed!");
        displayMessage("SD Card Error!", "Check connection", 5000);
    } else {
        Serial.println("SD card initialized.");
        displayMessage("SD Card OK", "", 1000);
    }
}

/**
 * @brief Configures and synchronizes time from an NTP server.
 */
void setupTime() {
    Serial.println("Setting up time...");
    displayMessage("Syncing time...", "");
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
        displayMessage("Time Sync Failed", "Check NTP server", 2000);
        return;
    }
    Serial.println("Time synchronized successfully.");
    displayMessage("Time Synced", "", 1000);
}

/**
 * @brief Main function to handle an attendance event. Decides whether to log online or offline.
 */
void recordAttendance(uint16_t id) {
    time_t now = time(nullptr);

    if (now < 1000000000) { // Simple check for a valid Unix timestamp
        Serial.println("Cannot log attendance: Time not synced.");
        displayMessage("Log Failed", "Time not synced", 2000);
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (!logAttendanceToServer(id, now)) {
            Serial.println("Server log failed. Saving offline.");
            displayMessage("Server Error", "Logging offline", 1500);
            logAttendanceOffline(id, now);
        } else {
            Serial.println("Attendance logged to server.");
            displayMessage("Log Sent", "To server", 1500);
        }
    } else {
        Serial.println("No WiFi. Saving attendance offline.");
        logAttendanceOffline(id, now);
    }
}

/**
 * @brief Logs an attendance record to the SD card.
 */
void logAttendanceOffline(uint16_t id, time_t timestamp) {
    File logFile = SD.open(LOG_FILE, FILE_APPEND);
    if (!logFile) {
        Serial.println("Failed to open log file for appending.");
        displayMessage("SD Write Error", "", 2000);
        return;
    }
    
    if (logFile.printf("%u,%lu\n", id, timestamp)) {
        Serial.printf("Successfully wrote log to SD: ID %u, Time %lu\n", id, timestamp);
        displayMessage("Log Saved", "Offline", 1500);
    } else {
        Serial.println("Failed to write to log file.");
        displayMessage("SD Write Error", "", 2000);
    }
    logFile.close();
}

/**
 * @brief Reads logs from the SD card and sends them to the server.
 */
void syncOfflineLogs() {
    if (WiFi.status() != WL_CONNECTED) return;

    Serial.println("Starting offline log sync process...");
    displayMessage("Syncing Logs...", "");

    File logFile = SD.open(LOG_FILE, FILE_READ);
    if (!logFile || logFile.size() == 0) {
        if (logFile) logFile.close();
        Serial.println("No offline logs to sync.");
        displayMessage("No Offline Logs", "", 1500);
        return;
    }

    const char* tempLogFile = "/temp_log.txt";
    SD.remove(tempLogFile);
    File tempFile = SD.open(tempLogFile, FILE_WRITE);

    if (!tempFile) {
        Serial.println("Failed to create temporary log file. Aborting sync.");
        logFile.close();
        return;
    }

    int syncedCount = 0;
    int failedCount = 0;

    while (logFile.available()) {
        String line = logFile.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        int commaIndex = line.indexOf(',');
        if (commaIndex == -1) continue;

        uint16_t id = line.substring(0, commaIndex).toInt();
        time_t timestamp = strtoul(line.substring(commaIndex + 1).c_str(), NULL, 10);
        
        if (logAttendanceToServer(id, timestamp)) {
            syncedCount++;
        } else {
            tempFile.println(line);
            failedCount++;
        }
    }
    
    logFile.close();
    tempFile.close();

    SD.remove(LOG_FILE);
    SD.rename(tempLogFile, LOG_FILE);
    
    Serial.printf("Log sync finished. Synced: %d, Failed (re-saved): %d\n", syncedCount, failedCount);
    if (syncedCount > 0) {
       displayMessage("Synced " + String(syncedCount) + " logs", "", 2000);
    } else if (failedCount == 0) {
       displayMessage("Logs are up", "to date.", 2000);
    }
}

// =================================================================================================
// SERVER COMMUNICATION & SYNC
// =================================================================================================
void syncSensorWithServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sync failed: WiFi not connected.");
    displayMessage("Sync Failed", "No WiFi", 2000);
    return;
  }

  Serial.println("Syncing sensor with server...");
  displayMessage("Syncing...", "");

  bool serverHasID[128] = {false};
  HTTPClient http;
  String url = String(SERVER_HOST) + "/api/SensorData";
  http.begin(client, url);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, payload).code() == DeserializationError::Ok) {
      for (JsonObject item : doc.as<JsonArray>()) {
        int id = item["id"];
        if (id >= 0 && id < 128) {
          serverHasID[id] = true;
          fingerprintCache[id] = {(uint16_t)id, String(item["name"].as<const char*>())};
        }
      }
    }
  }
  http.end();
  
  Serial.println("Checking for orphaned fingerprints on sensor...");
  for (int id = 0; id < 128; id++) {
    if (finger.loadModel(id) == FINGERPRINT_OK) {
      if (!serverHasID[id]) {
        Serial.printf("ID %d is on sensor but not server. Deleting from sensor.\n", id);
        finger.deleteModel(id);
      }
    }
  }
  
  Serial.println("Sync complete.");
  displayMessage("Sync Complete", "", 1500);
}

int getNextAvailableIDFromServer() {
  if (WiFi.status() != WL_CONNECTED) return -1;

  HTTPClient http;
  String url = String(SERVER_HOST) + "/api/SensorData/generate-id";
  http.begin(client, url);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    int id = http.getString().toInt();
    http.end();
    return id;
  } else {
    Serial.printf("Failed to get next ID. HTTP Error: %d\n", httpCode);
    http.end();
    return -1;
  }
}

bool sendFingerprintToServer(uint16_t id, const String& name) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = String(SERVER_HOST) + "/api/SensorData";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(256);
  doc["id"] = id;
  doc["name"] = name;
  String jsonPayload;
  serializeJson(doc, jsonPayload);

  int httpCode = http.POST(jsonPayload);
  http.end();
  
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    Serial.println("Fingerprint sent to server successfully.");
    return true;
  } else {
    Serial.printf("Failed to send fingerprint to server. HTTP Error: %d\n", httpCode);
    return false;
  }
}

/**
 * @brief Sends a single attendance log to the server.
 */
bool logAttendanceToServer(uint16_t id, time_t timestamp) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    // NOTE: You need to create this API endpoint on your server.
    String url = String(SERVER_HOST) + "/api/AttendanceLogs";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");

    DynamicJsonDocument doc(256);
    doc["fingerprintId"] = id;
    doc["timestamp"] = timestamp;
    String jsonPayload;
    serializeJson(doc, jsonPayload);

    Serial.println("Sending log to server: " + jsonPayload);
    int httpCode = http.POST(jsonPayload);
    http.end();
    
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
        Serial.printf("Server accepted log. Response: %d\n", httpCode);
        return true;
    } else {
        Serial.printf("Failed to send log to server. HTTP Error: %d\n", httpCode);
        return false;
    }
}
