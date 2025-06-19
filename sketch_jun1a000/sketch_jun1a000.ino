/**************************************************************************************************
 * Improved Fingerprint Attendance System for ESP32
 * Version: 5.3 (Corrected, Refactored, and Fully Commented)
 *
 * Description:
 * A comprehensive fingerprint attendance system using an ESP32. It is designed to be
 * robust and user-friendly, with features for online and offline operation.
 *
 * Key Features:
 * - On-Demand WiFi Setup: Uses WiFiManager to create a web portal for WiFi configuration
 * only when a dedicated button is held down, allowing for fast boot-up in normal operation.
 * - Offline Logging: If no WiFi is available, attendance logs are saved to an SD card.
 * - Automatic Sync: Saved offline logs are automatically sent to the server once an
 * internet connection is established.
 * - Accurate Timestamps: Uses a DS3231/DS1307 Real-Time Clock (RTC) with a backup battery
 * to ensure timestamps are always accurate, even after power loss.
 * - Full Data Logging: Logs include the User ID, the full fingerprint template model
 * (Base64 encoded), and a Unix timestamp for complete record-keeping.
 * - Secure Server Communication: Uses HTTPS for secure data transmission.
 * - User-Friendly Interface: A 16x2 LCD provides clear instructions and feedback.
 *
 **************************************************************************************************/

// --- LIBRARIES ---
#include <LiquidCrystal.h>      // For the 16x2 LCD display
#include <Adafruit_Fingerprint.h> // For the fingerprint sensor
#include <HardwareSerial.h>     // For serial communication with the sensor
#include <WiFi.h>               // For WiFi connectivity
#include <WiFiManager.h>        // For the on-demand WiFi setup portal
#include <WiFiClientSecure.h>   // For HTTPS communication
#include <HTTPClient.h>         // To make HTTP requests
#include <ArduinoJson.h>        // To create and parse JSON data
#include <SPI.h>                // For SD card communication
#include <SD.h>                 // For reading from and writing to the SD card
#include <Wire.h>               // For I2C communication with the RTC
#include "RTClib.h"             // For the Real-Time Clock Module

// --- RTC OBJECT ---
// Use RTC_DS3231 for more accuracy, or RTC_DS1307 for the other common module.
RTC_DS3231 rtc;

// --- SERVER AND TIME CONFIGURATION ---
#define SERVER_HOST "https://192.168.1.6:7069" // The base URL of your backend server
#define NTP_SERVER "pool.ntp.org"               // Network Time Protocol server for initial time sync
#define GMT_OFFSET_SEC 3600 * 2                 // GMT offset for your timezone (e.g., UTC+2 for Egypt Standard Time)
#define DAYLIGHT_OFFSET_SEC 0                   // Daylight saving offset (0 if not applicable)

// --- FINGERPRINT SENSOR COMMAND CONSTANTS (for manual commands) ---
// These are used for manually constructing command packets when the library doesn't expose a function.
#define FINGERPRINT_STARTCODE         0xEF01
#define FINGERPRINT_COMMANDPACKET     0x1
#define FINGERPRINT_DATAPACKET        0x2
#define FINGERPRINT_ACKPACKET         0x7
#define FINGERPRINT_ENDDATAPACKET     0x8

// Command Codes
#define FINGERPRINT_UPCHAR            0x08 // Command to upload a template from a sensor buffer

// --- HARDWARE PIN DEFINITIONS ---
// LCD Pins (rs, en, d4, d5, d6, d7)
const int rs = 27, en = 26, d4 = 25, d5 = 33, d6 = 32, d7 = 14;
// Fingerprint Sensor RX/TX (connected to ESP32's Serial Port 2)
const int FINGERPRINT_RX = 16;
const int FINGERPRINT_TX = 17;
// Main operational button (for enrolling, menus, etc.)
const int BUTTON_PIN = 34;
// Button to trigger WiFiManager setup portal (connect to GPIO 35 and GND)
const int WIFI_SETUP_BUTTON_PIN = 35;
// SD Card Chip Select (CS) Pin
const int SD_CS_PIN = 5;

// --- GLOBAL OBJECTS ---
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);
HardwareSerial fingerSerial(2); // Use hardware serial port 2 for the sensor to avoid conflict with USB monitor
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);
WiFiClientSecure client; // Use a secure client for HTTPS connections

// --- STATE MANAGEMENT ---
// Enum to track the current menu state for the main button
enum class MenuState {
  MAIN_MENU,
  OPTIONS_MENU
};
MenuState currentMenuState = MenuState::MAIN_MENU;

// Struct to hold a local cache of user names fetched from the server
struct FingerprintData {
  uint16_t id;
  String name;
};
FingerprintData fingerprintCache[128]; // Cache for up to 128 users
const char* LOG_FILE = "/attendance_log.txt"; // Filename for offline logs on the SD card

// --- FUNCTION PROTOTYPES ---
// Core System & UI
void displayMessage(String line1, String line2 = "", int delayMs = 0);
void handleButton();
void handleWifiSetupButton();

// WiFi & Server
void startWifiManagerPortal();
void syncSensorWithServer();
bool sendFingerprintToServer(uint16_t id, const String& name);
int getNextAvailableIDFromServer();
bool logAttendanceToServer(uint16_t id, const String& base64Template, time_t timestamp);

// Fingerprint Operations
void scanForFingerprint();
void enrollNewFingerprint();
int getFingerprintImage(int step);
int createAndStoreModel(uint16_t id);
String getFingerprintTemplateAsBase64(uint16_t id);
String getFingerNameFromSerial(uint16_t id);

// RTC, SD Card, and Logging
void setupSDCard();
void setupRTC();
void syncRtcWithNtp();
void recordAttendance(uint16_t id, const String& base64Template);
void logAttendanceOffline(uint16_t id, const String& base64Template, time_t timestamp);
void syncOfflineLogs();

// Menu Actions
void runMainMenuAction();
void runOptionsMenuAction();
void showOptionsMenu();
void syncAndDisplayServerData();
void attemptToClearAllData();
bool confirmAdminPassword();

// Utility
String base64Encode(const uint8_t* data, size_t input_length);

// =================================================================================================
// SETUP FUNCTION - Runs once on boot
// =================================================================================================
void setup() {
  // Initialize Serial Monitor for debugging
  Serial.begin(115200);
  // Initialize I2C for RTC communication
  Wire.begin(); 
  while (!Serial); // Wait for Serial Monitor to connect

  // Initialize the LCD display
  lcd.begin(16, 2);
  displayMessage("System Starting", "Please wait...");

  // --- INITIALIZE HARDWARE ---
  setupSDCard();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // Setup the WiFiManager button pin. GPIO 35 is an input-only pin.
  pinMode(WIFI_SETUP_BUTTON_PIN, INPUT_PULLUP);

  // Initialize the fingerprint sensor
  fingerSerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  if (finger.verifyPassword()) {
    Serial.println("Fingerprint sensor found!");
  } else {
    displayMessage("Sensor Error", "Check wiring.", 10000);
    while (1) delay(1); // Halt on critical error, system cannot operate
  }
  
  // --- ATTEMPT TO CONNECT TO WIFI SILENTLY ON BOOT ---
  // Tries to connect to the last saved WiFi network without blocking for user input.
  WiFi.mode(WIFI_STA);
  WiFi.begin();

  displayMessage("Connecting...", "To saved WiFi");
  byte timeout = 10; // 5-second timeout (10 * 500ms)
  while (WiFi.status() != WL_CONNECTED && timeout > 0) {
    delay(500);
    Serial.print(".");
    timeout--;
  }

  // --- PERFORM ONLINE TASKS if connection was successful ---
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    displayMessage("WiFi Connected", WiFi.localIP().toString(), 2000);
    
    // Now that we are online, we can sync the RTC if its time is not set.
    setupRTC(); 

    // IMPORTANT: Allow insecure HTTPS connections.
    // This is needed if your local server uses a self-signed SSL certificate.
    // For a production environment with a valid certificate, this should be removed.
    client.setInsecure();

    // Perform all server-related synchronization tasks
    syncSensorWithServer();
    syncOfflineLogs();
  } else {
    // If we are here, the connection failed. Start in offline mode.
    Serial.println("\nCould not connect. Starting in offline mode.");
    displayMessage("Offline Mode", "Hold WiFi btn", 2000);
    // Initialize the RTC anyway; it will use its last known time if the backup battery is good.
    setupRTC();
  }

  // Ready for normal operation
  displayMessage("Add: Press Btn", "Scan: Place Finger");
}

// =================================================================================================
// MAIN LOOP - Runs continuously
// =================================================================================================
void loop() {
  handleButton();           // Checks the main operational button for user commands
  handleWifiSetupButton();  // Checks the on-demand WiFi setup button
  scanForFingerprint();     // Continuously checks for a finger on the sensor

  // This block handles periodic synchronization of offline logs.
  static unsigned long lastSyncAttempt = 0;
  // If WiFi is connected and it has been 5 minutes since the last attempt...
  if (WiFi.status() == WL_CONNECTED && (millis() - lastSyncAttempt > 300000)) { 
      lastSyncAttempt = millis(); // Reset the timer
      Serial.println("Periodic check for offline logs to sync...");
      syncOfflineLogs();
  }

  delay(50); // Small delay to prevent overwhelming the processor and to allow tasks to run
}

// =================================================================================================
// WIFI & DISPLAY FUNCTIONS
// =================================================================================================
/**
 * @brief Starts the WiFiManager configuration portal.
 * This is a blocking function that creates an Access Point ("FingerprintSetupAP").
 * A user can connect to this AP, and a captive portal will open on their device,
 * allowing them to select and enter credentials for the local WiFi network.
 */
void startWifiManagerPortal() {
    WiFiManager wm;
    wm.setConfigPortalTimeout(180); // Portal will close after 3 minutes of inactivity

    displayMessage("Connect to AP:", "FingerprintSetup");
    
    if (!wm.startConfigPortal("FingerprintSetupAP")) {
        Serial.println("Failed to connect and hit timeout.");
        displayMessage("Config Failed", "Check credentials", 2000);
    } else {
        // If we get here, the user has successfully configured the WiFi.
        Serial.println("WiFi configured successfully!");
        displayMessage("WiFi Configured!", WiFi.localIP().toString(), 2000);
        
        // Now that we are online, perform all the necessary sync tasks.
        setupRTC();
        syncSensorWithServer();
        syncOfflineLogs();
    }
    
    // After the portal closes, return to the main operational screen.
    displayMessage("Add: Press Btn", "Scan: Place Finger");
}

/**
 * @brief Displays a two-line message on the LCD.
 * @param line1 Text for the first row.
 * @param line2 (Optional) Text for the second row.
 * @param delayMs (Optional) Time in milliseconds to show the message. 0 means it stays until overwritten.
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
 * @brief Handles long presses for the dedicated WiFi setup button on GPIO 35.
 * A long press is required to prevent accidental activation.
 */
void handleWifiSetupButton() {
    static unsigned long pressStartTime = 0;
    static bool isBeingPressed = false;

    // Read the button state. It's pulled up, so LOW means pressed.
    bool isPressed = (digitalRead(WIFI_SETUP_BUTTON_PIN) == LOW);

    if (isPressed && !isBeingPressed) {
        // Button was just pressed
        pressStartTime = millis();
        isBeingPressed = true;
    } 
    else if (isPressed && isBeingPressed) {
        // Button is being held down. Check if it's been held for more than 5 seconds.
        if (millis() - pressStartTime > 5000) {
            Serial.println("WiFi setup button long-pressed. Starting portal...");
            startWifiManagerPortal();
            // IMPORTANT: Reset the state to prevent this from running repeatedly
            // while the button is still held down.
            isBeingPressed = false; 
        }
    } 
    else if (!isPressed) {
        // Button is not pressed, reset the state.
        isBeingPressed = false;
    }
}

/**
 * @brief Handles the main operational button with multiple press durations.
 * - Short Press (<5s): Main action (Enroll or Show Data).
 * - Long Press (>5s): Enter options menu.
 * - Very Long Press (>10s): Destructive action (Clear All Data).
 */
void handleButton() {
  static unsigned long buttonPressTime = 0;
  static bool buttonWasPressed = false;

  bool isPressed = (digitalRead(BUTTON_PIN) == LOW);

  if (isPressed && !buttonWasPressed) {
    // Button was just pressed down.
    buttonPressTime = millis();
    buttonWasPressed = true;
  } else if (!isPressed && buttonWasPressed) {
    // Button was just released.
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
    // Button is being held down. Check for long press actions.
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

/**
 * @brief Action for a short press on the main menu (enroll new fingerprint).
 */
void runMainMenuAction() {
  enrollNewFingerprint();
  displayMessage("Add: Press Btn", "Scan: Place Finger");
}

/**
 * @brief Action for a short press on the options menu (sync and show server data).
 */
void runOptionsMenuAction() {
  syncAndDisplayServerData();
  displayMessage("Add: Press Btn", "Scan: Place Finger");
  currentMenuState = MenuState::MAIN_MENU;
}

// =================================================================================================
// FINGERPRINT OPERATIONS
// =================================================================================================
/**
 * @brief Continuously scans for a finger. If found, identifies it and logs the attendance.
 */
void scanForFingerprint() {
  // Step 1: Check if a finger is present and get an image.
  if (finger.getImage() != FINGERPRINT_OK) return;

  // Step 2: Convert the image to a storable template (characteristic file).
  if (finger.image2Tz() != FINGERPRINT_OK) return;

  // Step 3: Search the sensor's internal database for a match. This is very fast.
  if (finger.fingerFastSearch() != FINGERPRINT_OK) {
      displayMessage("Access Denied", "Finger not found", 1500);
      return;
  }

  // If we get here, a match was found!
  uint16_t id = finger.fingerID;
  String name = fingerprintCache[id].name;
  if (name.isEmpty()) {
    name = "Name not synced";
  }
  
  displayMessage("Welcome!", "ID: " + String(id) + " " + name, 2000);
  
  // Get the full template model for complete logging.
  String base64Template = getFingerprintTemplateAsBase64(id);
  if (!base64Template.isEmpty()) {
      // Log the attendance record (decides automatically whether to go online or offline).
      recordAttendance(id, base64Template);
  } else {
      displayMessage("Log Failed", "Template Error", 2000);
  }
  
  // Return to the main screen after the process is complete.
  displayMessage("Add: Press Btn", "Scan: Place Finger");
}

/**
 * @brief Manages the full, guided process of enrolling a new user.
 */
void enrollNewFingerprint() {
  // Enrolling requires an internet connection to get a new ID from the server.
  if (WiFi.status() != WL_CONNECTED) {
    displayMessage("Enroll Failed", "Need WiFi", 2000);
    return;
  }
  
  displayMessage("Getting ID...", "From server");
  int newId = getNextAvailableIDFromServer();

  if (newId < 0 || newId >= 128) {
    displayMessage("Enroll Failed", "No available ID", 2000);
    return;
  }
  
  Serial.printf("Starting enrollment for new ID: %d\n", newId);
  displayMessage("Place finger", "On the sensor");

  // Get the first fingerprint image
  if (getFingerprintImage(1) != FINGERPRINT_OK) return;
  displayMessage("Lift finger", "");
  delay(1000);
  while (finger.getImage() != FINGERPRINT_NOFINGER) delay(10); // Wait for finger to be removed

  // Get the second fingerprint image for verification
  displayMessage("Place again", "Same finger");
  if (getFingerprintImage(2) != FINGERPRINT_OK) return;

  // Create a final model from the two images and store it on the sensor
  if (createAndStoreModel(newId) != FINGERPRINT_OK) return;

  // Get a name for the user via the Serial Monitor
  String name = getFingerNameFromSerial(newId);
  
  // Send the new user data (ID and name) to the server database
  if (sendFingerprintToServer(newId, name)) {
    // If successful, update our local cache and show success message
    fingerprintCache[newId] = { (uint16_t)newId, name };
    displayMessage("Enroll Success!", "ID: " + String(newId), 2000);
  } else {
    // If server communication fails, roll back the change by deleting the print from the sensor
    // to maintain consistency between the sensor and the server.
    displayMessage("Enroll Failed", "Server error", 2000);
    finger.deleteModel(newId);
  }
}

/**
 * @brief Helper function for enrollment. Captures one fingerprint image and converts it.
 * @param step The enrollment step (1 or 2).
 * @return FINGERPRINT_OK on success, or an error code.
 */
int getFingerprintImage(int step) {
  int p = -1;
  while (p != FINGERPRINT_OK) { // Loop until a good image is captured
    p = finger.getImage();
    if (p != FINGERPRINT_OK && p != FINGERPRINT_NOFINGER) {
      displayMessage("Imaging Error", "", 1500);
      return p;
    }
  }

  // Convert image to a template and store in one of the sensor's character buffers (1 or 2)
  p = finger.image2Tz(step);
  if (p != FINGERPRINT_OK) {
    displayMessage("Processing Error", "", 1500);
  }
  return p;
}

/**
 * @brief Helper function for enrollment. Creates a final model and stores it in flash.
 * @param id The ID (location) to store the fingerprint under in the sensor's flash.
 * @return FINGERPRINT_OK on success, or an error code.
 */
int createAndStoreModel(uint16_t id) {
  displayMessage("Creating model...", "");
  // Combine the two character files into a single template
  int p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    displayMessage("Fingers do not", "match. Try again.", 2000);
    return p;
  }

  // Store the final template in the sensor's flash memory at the specified ID
  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) {
    displayMessage("Storage Error", "Slot may be full.", 2000);
  }
  return p;
}

/**
 * @brief Retrieves the fingerprint template for a given ID and encodes it in Base64.
 * This function uses a more direct, packet-based communication method because the standard
 * Adafruit library does not provide a public function to get the full template data from a
 * specific flash location into a user-accessible byte array.
 * @param id The ID of the fingerprint template to retrieve from the sensor's flash.
 * @return A String containing the Base64 encoded template, or an empty string on failure.
 */
String getFingerprintTemplateAsBase64(uint16_t id) {
  // Step 1: Command the sensor to load the template from its flash memory into its internal buffer (CharBuffer1).
  if (finger.loadModel(id) != FINGERPRINT_OK) {
    Serial.printf("Failed to load model for ID #%u from flash.\n", id);
    return "";
  }
  Serial.printf("Template for ID #%u loaded into sensor buffer.\n", id);

  // Step 2: Manually command the sensor to UPLOAD the template from the character buffer to the ESP32.
  // This is where the manual packet construction happens.
  uint8_t packet[] = {
    (uint8_t)(FINGERPRINT_STARTCODE >> 8), (uint8_t)FINGERPRINT_STARTCODE, 
    0xFF, 0xFF, 0xFF, 0xFF, // Default address
    FINGERPRINT_COMMANDPACKET, 
    0x00, 0x04, // Packet length (4 bytes: command, buffer id, 2 for checksum)
    FINGERPRINT_UPCHAR, 1 // Command to upload from CharBuffer1
  };

  // Calculate checksum
  uint16_t sum = 0;
  for (int i = 6; i < 11; i++) {
    sum += packet[i];
  }
  
  // Send the command packet
  fingerSerial.write(packet, 11);
  fingerSerial.write((uint8_t)(sum >> 8));
  fingerSerial.write((uint8_t)(sum & 0xFF));

  // Step 3: Wait for and read the data packets containing the template.
  const int templateSize = 512; // A full fingerprint model is 512 bytes.
  uint8_t fingerprintTemplate[templateSize];
  int index = 0;
  bool endOfData = false;
  uint32_t startTime = millis();

  while (!endOfData && (millis() - startTime < 2000)) { // 2-second timeout
    if (fingerSerial.available() < 9) { // Wait for at least a packet header
      delay(1);
      continue;
    }
    
    // Read the packet header to determine what type of packet it is.
    if (fingerSerial.read() != (uint8_t)(FINGERPRINT_STARTCODE >> 8)) continue;
    if (fingerSerial.read() != (uint8_t)(FINGERPRINT_STARTCODE & 0xFF)) continue;
    fingerSerial.read(); fingerSerial.read(); fingerSerial.read(); fingerSerial.read(); // Skip address
    
    uint8_t packet_type = fingerSerial.read();
    uint16_t length = ((uint16_t)fingerSerial.read()) << 8;
    length |= fingerSerial.read();

    if (packet_type == FINGERPRINT_ACKPACKET) {
      // It's just the ACK, the data will follow. We can ignore it for this implementation.
      continue;
    }

    if (packet_type == FINGERPRINT_DATAPACKET || packet_type == FINGERPRINT_ENDDATAPACKET) {
      if (index + length - 2 > templateSize) {
          Serial.println("Error: Template data exceeds buffer size.");
          return "";
      }
      // Read the data payload from the packet (length - 2 to exclude checksum).
      fingerSerial.readBytes(fingerprintTemplate + index, length - 2);
      index += (length - 2);
      
      // The last packet is marked as an ENDDATAPACKET.
      if (packet_type == FINGERPRINT_ENDDATAPACKET) {
        endOfData = true;
      }
    } else {
        // Unexpected packet type.
        Serial.printf("Unexpected packet type: 0x%X\n", packet_type);
        return "";
    }
  }

  if (endOfData) {
      Serial.printf("Full template data received successfully (%d bytes).\n", index);
      // Step 4: Encode the complete template to Base64.
      return base64Encode(fingerprintTemplate, index);
  } else {
      Serial.println("Error: Timed out or failed to receive full template.");
      return "";
  }
}

/**
 * @brief Helper function for enrollment. Prompts user to enter a name via the Serial Monitor.
 * @param id The ID of the fingerprint being named.
 * @return The name entered by the user, or a default name on timeout.
 */
String getFingerNameFromSerial(uint16_t id) {
  displayMessage("Enter name in", "Serial Monitor");
  Serial.println("Please enter a name for ID " + String(id) + " and press Enter:");
  
  unsigned long startTime = millis();
  String name = "";
  while (millis() - startTime < 30000) { // 30-second timeout
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
// RTC, SD CARD & OFFLINE LOGGING
// =================================================================================================
/**
 * @brief Initializes the SD card module.
 */
void setupSDCard() {
    Serial.println("Initializing SD card...");
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("SD Card initialization failed! Check connection.");
        displayMessage("SD Card Error!", "Check connection", 5000);
    } else {
        Serial.println("SD card initialized successfully.");
    }
}

/**
 * @brief Initializes the RTC module and syncs its time via NTP if power was lost.
 */
void setupRTC() {
    Serial.println("Initializing RTC...");
    if (!rtc.begin()) {
        Serial.println("Couldn't find RTC module! Check wiring.");
        displayMessage("RTC Error!", "Check wiring", 5000);
        return;
    }

    // If the RTC has lost power (e.g., battery died or this is the first boot)...
    if (rtc.lostPower()) {
        Serial.println("RTC has lost power. Attempting to sync time.");
        // ...and if we have an internet connection...
        if (WiFi.status() == WL_CONNECTED) {
            // ...then sync the RTC time from the internet.
            displayMessage("RTC power lost", "Syncing time...", 2000);
            syncRtcWithNtp();
        } else {
            // Otherwise, we can't set the time and must rely on it being set later.
            Serial.println("Cannot sync RTC time, no WiFi connection available.");
            displayMessage("RTC Time Not Set", "No WiFi", 2000);
        }
    } else {
        Serial.println("RTC is running on battery backup.");
        displayMessage("RTC OK", "", 1000);
    }
}

/**
 * @brief Connects to an NTP server to get the current UTC time, adjusts for timezone, and sets the RTC.
 * This function assumes WiFi is already connected.
 */
void syncRtcWithNtp() {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10000)) { // 10-second timeout to get time
        // Set the RTC module with the fetched time
        rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
        Serial.println("RTC time has been successfully synced with NTP server.");
        displayMessage("RTC Time Synced", "", 2000);
    } else {
        Serial.println("Failed to obtain time from NTP server.");
        displayMessage("NTP Sync Failed", "", 2000);
    }
}

/**
 * @brief Main function to handle an attendance event. Gets a timestamp from the RTC and decides
 * whether to log the data to the server (online) or the SD card (offline).
 * @param id The user ID from the fingerprint scan.
 * @param base64Template The Base64 encoded fingerprint template for the record.
 */
void recordAttendance(uint16_t id, const String& base64Template) {
    if (!rtc.now().isValid()){
        Serial.println("Cannot log attendance: RTC time is not set.");
        displayMessage("Log Failed", "Time not set", 2000);
        return;
    }

    // Get the current time from the RTC module as a Unix timestamp (seconds since 1970).
    time_t now = rtc.now().unixtime();

    // Check for internet connection and act accordingly.
    if (WiFi.status() == WL_CONNECTED) {
        if (!logAttendanceToServer(id, base64Template, now)) {
            // If the server log fails for any reason (e.g., server down),
            // save it to the SD card as a fallback so no data is lost.
            Serial.println("Server log failed. Saving attendance offline as a backup.");
            displayMessage("Server Error", "Logging offline", 1500);
            logAttendanceOffline(id, base64Template, now);
        } else {
            Serial.println("Attendance logged successfully to server.");
            displayMessage("Log Sent", "To server", 1500);
        }
    } else {
        // If there is no WiFi, log it directly to the SD card.
        Serial.println("No WiFi connection. Saving attendance offline.");
        logAttendanceOffline(id, base64Template, now);
    }
}

/**
 * @brief Logs an attendance record to the SD card in a CSV format.
 * @param id The user ID.
 * @param base64Template The Base64 encoded fingerprint template.
 * @param timestamp The Unix timestamp of the event.
 */
void logAttendanceOffline(uint16_t id, const String& base64Template, time_t timestamp) {
    // Open the log file in append mode. The file will be created if it doesn't exist.
    File logFile = SD.open(LOG_FILE, FILE_APPEND);
    if (!logFile) {
        Serial.println("Failed to open log file for appending.");
        displayMessage("SD Write Error", "", 2000);
        return;
    }
    
    // Write the log entry as a single CSV line: "id,base64template,timestamp"
    if (logFile.printf("%u,%s,%lu\n", id, base64Template.c_str(), timestamp)) {
        Serial.printf("Successfully wrote offline log to SD card for ID %u\n", id);
        displayMessage("Log Saved", "Offline", 1500);
    } else {
        Serial.println("Failed to write to log file.");
        displayMessage("SD Write Error", "", 2000);
    }
    logFile.close();
}

/**
 * @brief Reads logs from the SD card and attempts to send them to the server.
 * This uses a temporary file to ensure that logs are not lost if the sync process is interrupted
 * (e.g., by power loss or WiFi disconnection). This makes the syncing process very robust.
 */
void syncOfflineLogs() {
    if (WiFi.status() != WL_CONNECTED) return; // Cannot sync if offline.

    Serial.println("Starting offline log synchronization process...");
    displayMessage("Syncing Logs...", "");

    File logFile = SD.open(LOG_FILE, FILE_READ);
    if (!logFile || logFile.size() == 0) {
        if (logFile) logFile.close();
        Serial.println("No offline logs found to sync.");
        displayMessage("No Offline Logs", "", 1500);
        return;
    }

    // Create a temporary file. Logs that fail to sync will be written here.
    const char* tempLogFile = "/temp_log.txt";
    SD.remove(tempLogFile); // Remove any old temp file first.
    File tempFile = SD.open(tempLogFile, FILE_WRITE);

    if (!tempFile) {
        Serial.println("Failed to create temporary log file. Aborting sync.");
        logFile.close();
        return;
    }

    int syncedCount = 0;
    int failedCount = 0;

    // Read the main log file line by line.
    while (logFile.available()) {
        String line = logFile.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        // Parse the CSV line to extract the three data fields.
        int firstComma = line.indexOf(',');
        int lastComma = line.lastIndexOf(',');
        if (firstComma == -1 || lastComma == -1 || firstComma == lastComma) continue; // Skip malformed lines

        uint16_t id = line.substring(0, firstComma).toInt();
        String base64Template = line.substring(firstComma + 1, lastComma);
        time_t timestamp = strtoul(line.substring(lastComma + 1).c_str(), NULL, 10);
        
        // Attempt to send the parsed log to the server.
        if (logAttendanceToServer(id, base64Template, timestamp)) {
            syncedCount++; // Success
        } else {
            // If it fails, write it to the temporary file to be tried again later.
            tempFile.println(line);
            failedCount++;
        }
    }
    
    logFile.close();
    tempFile.close();

    // Replace the old log file with the temporary file (which now only contains unsynced logs).
    SD.remove(LOG_FILE);
    SD.rename(tempLogFile, LOG_FILE);
    
    Serial.printf("Log sync finished. Synced: %d, Failed (re-saved for next time): %d\n", syncedCount, failedCount);
    if (syncedCount > 0) {
       displayMessage("Synced " + String(syncedCount) + " logs", "", 2000);
    } else if (failedCount == 0) {
       displayMessage("Logs are up", "to date.", 2000);
    }
}

// =================================================================================================
// MENU ACTIONS
// =================================================================================================
/**
 * @brief Displays the options menu on the LCD when the main button is long-pressed.
 */
void showOptionsMenu() {
  currentMenuState = MenuState::OPTIONS_MENU;
  displayMessage("1.Show Data(short)", "2.Del All(hold 10s)");
}

/**
 * @brief Fetches all user data (ID and name) from the server and prints it to the Serial Monitor.
 * Also updates the local name cache.
 */
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
    
    // Use the modern JsonDocument
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.printf("Failed to parse server data. Error: %s\n", error.c_str());
      displayMessage("JSON Error", "", 2000);
      return;
    }
    
    displayMessage("Data in Serial", "Monitor", 2000);
    Serial.println("\n--- Fingerprint Data from Server ---");
    for (JsonObject item : doc.as<JsonArray>()) {
      int id = item["id"];
      const char* name = item["name"];
      Serial.printf("ID: %d, Name: %s\n", id, name);
      // Update our local name cache for faster display after scanning.
      if(id >= 0 && id < 128) {
        fingerprintCache[id] = {(uint16_t)id, String(name)};
      }
    }
    Serial.println("------------------------------------");

  } else {
    http.end();
    Serial.printf("Failed to get data from server. HTTP Error: %d\n", httpCode);
    displayMessage("Server Error", "Code: " + String(httpCode), 2000);
  }
}

/**
 * @brief Manages the process of clearing ALL data from the server, sensor, and SD card.
 * This is a highly destructive action that requires admin password confirmation.
 */
void attemptToClearAllData() {
  if (WiFi.status() != WL_CONNECTED){
      displayMessage("Clear Failed", "Need WiFi", 2000);
      return;
  }
  // Require password confirmation before proceeding.
  if (confirmAdminPassword()) {
    displayMessage("Clearing Data...", "Please wait");
    
    // Step 1: Send request to server to clear its user and log databases.
    HTTPClient http;
    String url = String(SERVER_HOST) + "/api/SensorData/clear"; // This endpoint must be implemented on your server.
    http.begin(client, url);
    int httpCode = http.POST(""); // A POST request to a 'clear' endpoint is common.
    http.end();

    if (httpCode == HTTP_CODE_OK) {
      Serial.println("Server data cleared successfully.");
      displayMessage("Server Cleared", "Clearing sensor...");
      
      // Step 2: Clear the fingerprint sensor's internal flash memory.
      if (finger.emptyDatabase() == FINGERPRINT_OK) {
        Serial.println("Fingerprint sensor's internal database has been cleared.");
        displayMessage("All Data Deleted", "", 2000);
      } else {
        Serial.println("Failed to clear fingerprint sensor.");
        displayMessage("Sensor Clear Fail", "", 2000);
      }
      
      // Step 3: Clear the local name cache in memory.
      for (int i = 0; i < 128; i++) {
        fingerprintCache[i] = {0, ""};
      }
      // Step 4: Delete the offline log file from the SD card.
      SD.remove(LOG_FILE);

    } else {
      Serial.printf("Failed to clear server data. HTTP Error: %d\n", httpCode);
      displayMessage("Server Clear Fail", "Code: " + String(httpCode), 2000);
    }

  } else {
    displayMessage("Wrong Password", "Operation Canceled", 2000);
  }
  
  // Return to the main menu state.
  displayMessage("Add: Press Btn", "Scan: Place Finger");
  currentMenuState = MenuState::MAIN_MENU;
}

/**
 * @brief Prompts for an admin password via Serial Monitor to confirm a destructive action.
 * @return True if the correct password ("admin") is entered, false otherwise.
 */
bool confirmAdminPassword() {
  displayMessage("Enter Password", "in Serial Monitor");
  Serial.println("\n!!!!!!!!!!!!!!!!!! ADMIN ACTION !!!!!!!!!!!!!!!!!!");
  Serial.println("Enter admin password to confirm ALL data deletion:");
  
  String input = "";
  unsigned long startTime = millis();
  while (millis() - startTime < 30000) { // 30-second timeout
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (input == "admin") { // The password is hardcoded here.
          Serial.println("Password correct. Proceeding with deletion.");
          return true;
        } else {
          Serial.println("Incorrect password. Aborting.");
          return false;
        }
      } else {
        input += c;
      }
    }
  }
  Serial.println("Password entry timed out. Aborting.");
  return false;
}

// =================================================================================================
// SERVER COMMUNICATION
// =================================================================================================
/**
 * @brief Synchronizes the sensor's user list with the server's list.
 * It fetches the user list from the server and deletes any users from the physical sensor
 * that are no longer present on the server, ensuring consistency.
 */
void syncSensorWithServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sync failed: WiFi not connected.");
    displayMessage("Sync Failed", "No WiFi", 2000);
    return;
  }

  Serial.println("Syncing on-device sensor with server database...");
  displayMessage("Syncing...", "");

  // 1. Get all user IDs from the server and store them in a boolean array for fast lookup.
  bool serverHasID[128] = {false};
  HTTPClient http;
  String url = String(SERVER_HOST) + "/api/SensorData";
  http.begin(client, url);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    if (deserializeJson(doc, payload).code() == DeserializationError::Ok) {
      for (JsonObject item : doc.as<JsonArray>()) {
        int id = item["id"];
        if (id >= 0 && id < 128) {
          serverHasID[id] = true;
          // While we're here, update the local name cache.
          fingerprintCache[id] = {(uint16_t)id, String(item["name"].as<const char*>())};
        }
      }
    }
  }
  http.end();
  
  // 2. Iterate through all possible IDs on the sensor and delete any "orphaned" fingerprints.
  Serial.println("Checking for orphaned fingerprints on the physical sensor...");
  for (int id = 0; id < 128; id++) {
    // Check if a template exists at this ID on the sensor.
    if (finger.loadModel(id) == FINGERPRINT_OK) {
      // If it exists on the sensor but not on the server...
      if (!serverHasID[id]) {
        // ...then it's an orphan and should be deleted to maintain sync.
        Serial.printf("ID %d is on sensor but not server. Deleting from sensor.\n", id);
        finger.deleteModel(id);
      }
    }
  }
  
  Serial.println("Sensor-server sync complete.");
  displayMessage("Sync Complete", "", 1500);
}

/**
 * @brief Asks the server for the next available ID for a new user enrollment.
 * @return The next free ID (e.g., 5), or -1 on error.
 */
int getNextAvailableIDFromServer() {
  if (WiFi.status() != WL_CONNECTED) return -1;

  HTTPClient http;
  // This API endpoint must be implemented on your server.
  // It should find the lowest unused ID from 0-127 and return it as plain text.
  String url = String(SERVER_HOST) + "/api/SensorData/generate-id";
  http.begin(client, url);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    int id = http.getString().toInt();
    http.end();
    return id;
  } else {
    Serial.printf("Failed to get next available ID from server. HTTP Error: %d\n", httpCode);
    http.end();
    return -1;
  }
}

/**
 * @brief Sends a newly enrolled user's data (ID and name) to the server to be stored.
 * @param id The ID of the user.
 * @param name The name of the user.
 * @return True on success, false on failure.
 */
bool sendFingerprintToServer(uint16_t id, const String& name) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = String(SERVER_HOST) + "/api/SensorData";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  // Create the JSON payload.
  JsonDocument doc;
  doc["id"] = id;
  doc["name"] = name;
  String jsonPayload;
  serializeJson(doc, jsonPayload);

  int httpCode = http.POST(jsonPayload);
  http.end();
  
  // A 201 (Created) status is also a success for POST requests.
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    Serial.println("New user fingerprint data sent to server successfully.");
    return true;
  } else {
    Serial.printf("Failed to send fingerprint data to server. HTTP Error: %d\n", httpCode);
    return false;
  }
}

/**
 * @brief Sends a single attendance log to the server.
 * @param id The user ID.
 * @param base64Template The Base64 encoded fingerprint template.
 * @param timestamp The Unix timestamp of the attendance event.
 * @return True on success, false on failure.
 */
bool logAttendanceToServer(uint16_t id, const String& base64Template, time_t timestamp) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    // This API endpoint must exist on your server to receive attendance logs.
    String url = String(SERVER_HOST) + "/api/AttendanceLogs";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");

    // Create a larger JSON document to accommodate the Base64 template.
    JsonDocument doc;
    doc["fingerprintId"] = id;
    doc["fingerprintModel"] = base64Template;
    doc["timestamp"] = timestamp;
    String jsonPayload;
    serializeJson(doc, jsonPayload);

    Serial.println("Sending attendance log to server...");
    int httpCode = http.POST(jsonPayload);
    
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
        Serial.printf("Server accepted attendance log. Response code: %d\n", httpCode);
        http.end();
        return true;
    } else {
        Serial.printf("Failed to send log to server. HTTP Error: %d\n", httpCode);
        String responseBody = http.getString();
        Serial.println("Server response: " + responseBody);
        http.end();
        return false;
    }
}

// =================================================================================================
// UTILITY FUNCTIONS
// =================================================================================================
/**
 * @brief Encodes a block of binary data into a Base64 string.
 * @param data A pointer to the byte array to encode.
 * @param input_length The number of bytes in the data array to encode.
 * @return A String containing the Base64 encoded data.
 */
String base64Encode(const uint8_t* data, size_t input_length) {
    const char* b64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String ret;
    ret.reserve(((input_length + 2) / 3) * 4); // Pre-allocate memory for performance
    int i = 0;
    int j = 0;
    uint8_t char_array_3[3];
    uint8_t char_array_4[4];

    while (input_length--) {
        char_array_3[i++] = *(data++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for(i = 0; (i <4) ; i++)
                ret += b64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for(j = i; j < 3; j++)
            char_array_3[j] = '\0';

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

        for(j = 0; (j < i + 1) ; j++)
            ret += b64_chars[char_array_4[j]];

        while((i++ < 3))
            ret += '=';
    }

    return ret;
}
