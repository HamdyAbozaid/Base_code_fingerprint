#include <LiquidCrystal.h>
#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define WIFI_SSID "Alla"
#define WIFI_PASSWORD "GREA@G&R6"

const int rs = 19, en = 23, d4 = 32, d5 = 33, d6 = 25, d7 = 26;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

HardwareSerial mySerial(2);
Adafruit_Fingerprint finger(&mySerial);

const int addButtonPin = 18;

struct FingerprintData {
  uint16_t id;
  String name;
};
FingerprintData fingerprints[128];
uint16_t nextID = 0;

enum MenuState {
  MAIN_MENU,
  SHOW_MENU_CHOICES
};

MenuState menuState = MAIN_MENU;

unsigned long buttonPressStart = 0;
bool buttonHeld = false;
bool waitingForSecondPress = false;

void setupWiFi();
void displayMainMenu();
bool addFingerprint();
void enrollWithRetry();
void getFingerName(uint16_t id);
void sendToServer(uint16_t id, String name);
void scanFingerprint();
void showMenuChoices();
void printFingerprintsFromServer();
void clearServerData();
bool confirmPassword();

void setup() {
  Serial.begin(115200);
  lcd.begin(16, 2);
  lcd.print("Initializing...");
  pinMode(addButtonPin, INPUT_PULLUP);

  mySerial.begin(57600, SERIAL_8N1, 16, 17);
  finger.begin(57600);

  if (!finger.verifyPassword()) {
    lcd.setCursor(0, 1);
    lcd.print("Sensor not found!");
    while (1) delay(1);
    }

  if (WiFi.status() == WL_CONNECTED) {
  lcd.print("WiFi Connected");
  delay(1000);
  }

  lcd.clear();
  setupWiFi();
  nextID = getNextAvailableIDFromServer();
  nextID = synchronizeIDs();

  displayMainMenu();
}

bool syncedOnce = false;

void loop() {
  static bool menuShown = false;
  int buttonState = digitalRead(addButtonPin);

  if (WiFi.status() == WL_CONNECTED && !syncedOnce) {
    Serial.println("🔁 Syncing Sensor to Server...");
    syncSensorToServer();
    syncedOnce = true;
  } else if (WiFi.status() != WL_CONNECTED) {
    syncedOnce = false; // لما النت يفصل، نرجّع السماح بالمزامنة
  }


  if (buttonState == LOW) {
    if (!buttonHeld) {
      buttonPressStart = millis();
      buttonHeld = true;
    } else {
      unsigned long heldTime = millis() - buttonPressStart;

      if (!menuShown && heldTime > 5000) {
        showMenuChoices();
        menuState = SHOW_MENU_CHOICES;
        menuShown = true;
        delay(500);
      }

      if (menuState == SHOW_MENU_CHOICES && heldTime > 25000) {
        if (confirmPassword()) {
          clearServerData();
          finger.emptyDatabase();
          lcd.clear();
          lcd.print("All Deleted");
        } else {
          lcd.clear();
          lcd.print("Wrong Password");
        }
        delay(2000);
        displayMainMenu();
        menuState = MAIN_MENU;
        menuShown = false;
        buttonHeld = false;
      }
    }
  } else {
    if (buttonHeld) {
      unsigned long heldTime = millis() - buttonPressStart;

      if (menuState == MAIN_MENU && heldTime < 5000) {
        enrollWithRetry();
        displayMainMenu();
      } else if (menuState == SHOW_MENU_CHOICES && heldTime < 2000) {
        printFingerprintsFromServer();
        displayMainMenu();
        menuState = MAIN_MENU;
        menuShown = false;
      }

      buttonHeld = false;
    }
  }

  scanFingerprint();
  delay(100);
}

void setupWiFi() {
  lcd.clear();
  lcd.print("Connecting WiFi");
  lcd.setCursor(0, 1);
  int dots = 0;
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    lcd.print(".");
    delay(300);
    dots++;
    if (dots > 14) {
      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
      dots = 0;
    }
  }
  lcd.clear();
  lcd.print("WiFi Connected");
  delay(1000);
}

void displayMainMenu() {
  lcd.clear();
  //lcd.setCursor(1, 0);
  lcd.print("Add : Press The ");
  lcd.setCursor(5, 1);
  lcd.print("Button"); 
}

void enrollWithRetry() {
  lcd.clear();
  lcd.print("Place Finger");
  delay(1000);
  for (int i = 1; i <= 3; i++) {   // وجهين فقط بدل 3 محاولات
    lcd.clear();
    lcd.print("Attempt " + String(i));
    if (addFingerprint()) {
      lcd.clear();
      lcd.print("Success!");
      delay(2000);
      return;
    }
    delay(1500);
  }
  lcd.clear();
  lcd.print("Failed 3 times");
  delay(2000);
}

bool addFingerprint() {
  int p = -1; // متغير لتخزين نتيجة وظائف المستشعر

  lcd.clear();
  lcd.print("Place Finger (1/3)"); // توجيه أول للمستخدم
  Serial.println("Place finger on sensor for first view (1/3)...");

  // الخطوة 1: التقاط الصورة الأولى وتخزينها في المخزن رقم 1
  p = -1; // إعادة تهيئة المتغير
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      // لا يوجد إصبع، ننتظر
      delay(50);
    } else if (p == FINGERPRINT_OK) {
      Serial.println("Image taken.");
    } else {
      // أي خطأ آخر في التقاط الصورة
      lcd.clear();
      lcd.print("Error! Retrying...");
      Serial.print("Error getting image: "); Serial.println(p);
      delay(1000);
    }
  }
  
  // تحويل الصورة إلى قالب وتخزينها في المخزن 1
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    lcd.clear();
    lcd.print("Image 1 failed");
    Serial.print("Image to Template 1 failed: "); Serial.println(p);
    delay(2000);
    return false;
  }
  Serial.println("Image 1 converted.");

  lcd.clear();
  lcd.print("Lift Finger"); // توجيه لرفع الإصبع
  lcd.setCursor(0, 1);
  lcd.print("Then Reposition");
  Serial.println("Lift your finger, then reposition for second view...");
  delay(1000); // إعطاء وقت كافي لرفع الإصبع

  // التأكد من أن الإصبع قد رفع
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    delay(50);
  }
  delay(500); // انتظار قصير للتأكد من خلو المستشعر

  lcd.clear();
  lcd.print("Place Finger (2/3)"); // توجيه للوجه الثاني
  Serial.println("Place finger on sensor for second view (2/3)...");

  // الخطوة 2: التقاط الصورة الثانية وتخزينها في المخزن رقم 2
  p = -1; // إعادة تهيئة المتغير
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      // لا يوجد إصبع، ننتظر
      delay(50);
    } else if (p == FINGERPRINT_OK) {
      Serial.println("Image taken.");
    } else {
      // أي خطأ آخر في التقاط الصورة
      lcd.clear();
      lcd.print("Error! Retrying...");
      Serial.print("Error getting image: "); Serial.println(p);
      delay(1000);
    }
  }
  
  // تحويل الصورة الثانية إلى قالب ومحاولة دمجها مع الأولى
  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    lcd.clear();
    lcd.print("Image 2 failed");
    Serial.print("Image to Template 2 failed: "); Serial.println(p);
    delay(2000);
    return false;
  }
  Serial.println("Image 2 converted.");

  // دمج القالبين لإنشاء نموذج بصمة نهائي
  p = finger.createModel();
  if (p == FINGERPRINT_OK) {
    Serial.println("Fingerprint models matched and merged.");
  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
    // خطأ في استقبال البيانات (المشكلة هنا بتكون غالبا في عدم تطابق الصورتين كويس)
    lcd.clear();
    lcd.print("No Match. Try again"); // توجيه واضح للمستخدم
    Serial.println("Could not create model: no match between images.");
    delay(2000);
    return false;
  } else {
    // أي خطأ آخر
    lcd.clear();
    lcd.print("Model creation fail");
    Serial.print("Create model failed: "); Serial.println(p);
    delay(2000);
    return false;
  }

  // الآن نطلب صورة ثالثة لتحسين الجودة إذا أردت
  // هذه الخطوة اختيارية لكنها تحسن الكفاءة بشكل كبير
  lcd.clear();
  lcd.print("Lift Finger (Final)");
  lcd.setCursor(0,1);
  lcd.print("Then Reposition");
  Serial.println("Lift finger for final view (3/3)...");
  delay(1000);
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    delay(50);
  }
  delay(500);

  lcd.clear();
  lcd.print("Place Finger (3/3)");
  Serial.println("Place finger on sensor for final view (3/3)...");

  p = -1; // إعادة تهيئة المتغير
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      delay(50);
    } else if (p == FINGERPRINT_OK) {
      Serial.println("Final image taken.");
    } else {
      lcd.clear();
      lcd.print("Error! Retrying...");
      Serial.print("Error getting final image: "); Serial.println(p);
      delay(1000);
    }
  }

  // الآن قم بتخزين الصورة الثالثة في المخزن المؤقت ودمجها مع النموذج الموجود
  p = finger.image2Tz(1); // استخدم المخزن 1 مرة أخرى لتحديث القالب
  if (p != FINGERPRINT_OK) {
      lcd.clear();
      lcd.print("Final Img fail");
      Serial.print("Image to Template final failed: "); Serial.println(p);
      delay(2000);
      return false;
  }
  Serial.println("Final image converted.");
  
  // دمج القالب الجديد (المخزن 1) مع النموذج المخزن بالفعل (المخزن 2)
  // هنا احنا بنستفيد من خاصية المستشعر إنه يقدر يعمل merge لقالبين
  // أو ممكن تخزنها مباشرة
  
  // لو كنت عايز تحسن الجودة أكتر ممكن تستخدم createModel مرة تانية لو المستشعر بيدعم
  // أو ممكن تخزن النموذج مباشرة بعد أول دمج ناجح لو مش محتاج 3 وجوه
  
  // احنا هنكمل بالطريقة اللي بتخزن مباشرة
  
  uint16_t id = nextID; // استخدم الـ ID المتاح التالي

  // تخزين النموذج النهائي في قاعدة بيانات المستشعر بالـ ID المتاح
  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println("Fingerprint stored successfully.");
  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
    lcd.clear();
    lcd.print("Store Error!");
    Serial.println("Store model failed: Packet recieve error (full or bad ID).");
    delay(2000);
    return false;
  } else {
    lcd.clear();
    lcd.print("Store Failed!");
    Serial.print("Store model failed: "); Serial.println(p);
    delay(2000);
    return false;
  }

  // البحث عن الـ ID المتاح التالي للاستخدام مستقبلاً
  nextID = findNextAvailableID();

  // الحصول على اسم البصمة من المستخدم عبر السيريال
  getFingerName(id);

  // إرسال بيانات البصمة (الـ ID والاسم) إلى السيرفر
  sendToServer(id, fingerprints[id].name);

  lcd.clear();
  lcd.print("Added ID: " + String(id));
  lcd.setCursor(0, 1);
  lcd.print(fingerprints[id].name);
  Serial.println("Added fingerprint ID: " + String(id) + ", Name: " + fingerprints[id].name);
  delay(3000);
  return true;
}

void getFingerName(uint16_t id) {
  lcd.clear();
  lcd.print("Enter name:");
  lcd.setCursor(0, 1);
  lcd.print("Type in Serial");
  Serial.println("Please type a name and press Enter:");
  String name = "";
  unsigned long start = millis();
  while (millis() - start < 30000) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (name.length() > 0) {
          fingerprints[id].id = id;
          fingerprints[id].name = name;
          return;
        }
      } else {
        name += c;
      }
    }
    delay(50);
  }
  fingerprints[id].id = id;
  fingerprints[id].name = "User_" + String(id);
}

void scanFingerprint() {
  int p = finger.getImage();
  if (p == FINGERPRINT_OK) {
    if (finger.image2Tz() != FINGERPRINT_OK) return;
    p = finger.fingerFastSearch();
    if (p == FINGERPRINT_OK) {
      uint16_t id = finger.fingerID;
      lcd.clear();
      lcd.print("ID: " + String(id));
      lcd.setCursor(0, 1);
      lcd.print("Name: " + fingerprints[id].name);
      Serial.println("Found ID: " + String(id) + ", Name: " + fingerprints[id].name);
      delay(2000);
      displayMainMenu();
    }
    else {
            // في حالة عدم تطابق صوره البصمة مع أي بصمة مخزّنة
            lcd.clear();
            lcd.print("Fingerprint Not"); // طباعة رسالة "البصمة غير موجودة"
            lcd.setCursor(5, 1);
            lcd.print("Found"); // طباعة "تم العثور عليها"
            Serial.println("Fingerprint Not Found!"); // طباعة على Serial Monitor
            delay(2000); // انتظر لمدة 2 ثانية
            displayMainMenu(); // عرض القائمة الرئيسية
        }
  }
}

void showMenuChoices() {
  lcd.clear();
  lcd.print("1.Show Data");
  lcd.setCursor(0, 1);
  lcd.print("2.Del All:Hold 5s");
}

void printFingerprintsFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    lcd.clear();
    lcd.print("WiFi Not Conn");
    delay(1500);
    return;
  }

  HTTPClient http;
  String url = "https://192.168.1.12:7069/api/SensorData";
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("Server Data:");
    Serial.println(payload);

    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      lcd.clear();
      lcd.print("Data in Serial");
      for (JsonObject item : doc.as<JsonArray>()) {
        int id = item["id"];
        const char* name = item["name"];
        Serial.printf("ID: %d, Name: %s\n", id, name);
        delay(100);
      }
    } else {
      lcd.clear();
      lcd.print("JSON Error");
    }
  } else {
    lcd.clear();
    lcd.print("GET Error");
  }
  http.end();
  delay(3000);
}

void clearServerData() {
  if (WiFi.status() != WL_CONNECTED) {
    lcd.clear();
    lcd.print("WiFi Not Conn");
    delay(1500);
    return;
  }
  HTTPClient http;
  String url = "https://192.168.1.12:7069/api/SensorData/clear";
  http.begin(url);
  int httpCode = http.POST("");
  if (httpCode == 200) {
    lcd.clear();
    lcd.print("Data Cleared");
    Serial.println("Server data cleared");
  } else {
    lcd.clear();
    lcd.print("Clear Fail");
    Serial.printf("Clear error: %d\n", httpCode);
  }
  http.end();
  delay(3000);
}

bool confirmPassword() {
  lcd.clear();
  lcd.print("Enter Password:");
  lcd.setCursor(0, 1);
  lcd.print("Type in Serial");

  Serial.println("Enter admin password:");
  String input = "";
  unsigned long startTime = millis();

  while (millis() - startTime < 30000) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        return input == "admin";
      } else {
        input += c;
      }
    }
    delay(10);
  }

  return false;
}

uint16_t findNextAvailableID() {
  Serial.println("Scanning fingerprint storage...");
  uint16_t usedCount = 0;

  for (uint16_t id = 0; id < 128; id++) {
    if (finger.loadModel(id) == FINGERPRINT_OK) {
      Serial.print("ID in use: ");
      Serial.println(id);
      usedCount++;
    } else {
      Serial.print("ID empty: ");
      Serial.println(id);
    }
  }

  Serial.print("Total used IDs: ");
  Serial.println(usedCount);

  // Return the first available ID
  for (uint16_t id = 0; id < 128; id++) {
    if (finger.loadModel(id) != FINGERPRINT_OK) {
      Serial.print("Next available ID is: ");
      Serial.println(id);
      return id;
    }
  }

  // fallback if all full
  Serial.println("All IDs are full, using fallback.");
  return finger.getTemplateCount();
}

uint16_t getNextAvailableIDFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot check server IDs.");
    return finger.getTemplateCount(); // fallback
  }

  HTTPClient http;
  String url = "https://192.168.1.12:7069/api/SensorData";
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.printf("Failed to fetch server data. HTTP code: %d\n", httpCode);
    http.end();
    return finger.getTemplateCount(); // fallback
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("JSON parsing failed");
    return finger.getTemplateCount(); // fallback
  }

  bool usedIDs[128] = { false };
  for (JsonObject item : doc.as<JsonArray>()) {
    int id = item["id"];
    const char* name = item["name"];
    if (id >= 0 && id < 128) {
      usedIDs[id] = true;
      Serial.printf("ID from server: %d, Name: %s\n", id, name);
    }
  }

  for (int i = 0; i < 128; i++) {
    if (!usedIDs[i]) {
      Serial.printf("First available ID from server: %d\n", i);
      return i;
    }
  }

  Serial.println("All IDs are used on server.");
  return finger.getTemplateCount(); // fallback
}

uint16_t synchronizeIDs() {
  bool serverIDs[128] = { false };
  bool sensorIDs[128] = { false };

  // 1. Get IDs from server
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("https://192.168.1.12:7069/api/SensorData");
    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      DynamicJsonDocument doc(4096);
      DeserializationError err = deserializeJson(doc, payload);
      if (!err) {
        for (JsonObject item : doc.as<JsonArray>()) {
          int id = item["id"];
          if (id >= 0 && id < 128) {
            serverIDs[id] = true;
          }
        }
      } else {
        Serial.println("Failed to parse server JSON.");
      }
    } else {
      Serial.printf("Failed to get server data: %d\n", httpCode);
    }
    http.end();
  }

  // 2. Get IDs from fingerprint sensor
  for (uint16_t id = 0; id < 128; id++) {
    if (finger.loadModel(id) == FINGERPRINT_OK) {
      sensorIDs[id] = true;
    }
  }

  // 3. Synchronize both
  for (uint16_t id = 0; id < 128; id++) {
    if (sensorIDs[id] && !serverIDs[id]) {
      // Fingerprint on sensor but not on server — remove it
      Serial.printf("Deleting ID %d from sensor (not on server)\n", id);
      finger.deleteModel(id);
    } else if (!sensorIDs[id] && serverIDs[id]) {
      // Fingerprint on server but not on sensor — log only, can't add from ESP32
      Serial.printf("ID %d exists on server but not in sensor.\n", id);
    }
  }

  // 4. Return first available ID
  for (uint16_t id = 0; id < 128; id++) {
    if (!serverIDs[id] && !sensorIDs[id]) {
      Serial.printf("Next available synchronized ID: %d\n", id);
      return id;
    }
  }

  // Fallback if all IDs are used
  return finger.getTemplateCount();
}

void syncSensorToServer() {
  uint16_t templateCount = finger.getTemplateCount();
  Serial.println("🔁 Syncing Sensor to Server...");

  for (uint16_t id = 0; id < 128; id++) {
    if (finger.loadModel(id) == FINGERPRINT_OK) {
      String name = fingerprints[id].name;

      lcd.clear();
      lcd.print("Sync ID: " + String(id));

      if (name == "") {
        // لا يوجد اسم => نحذفه من السيرفر والسنسور
        Serial.printf("⚠️ ID %d has no name. Deleting...\n", id);
        lcd.setCursor(0, 1);
        lcd.print("Deleting ID: " + String(id));

        // حذف من السيرفر
        HTTPClient http;
        String url = "https://192.168.1.12:7069/api/SensorData/" + String(id);
        http.begin(url);
        int httpCode = http.sendRequest("DELETE");

        if (httpCode == 200 || httpCode == 204) {
          Serial.printf("🗑️ Deleted ID %d from Server\n", id);
        } else {
          Serial.printf("⚠️ Could not delete ID %d from Server (HTTP %d)\n", id, httpCode);
        }
        http.end();

        // حذف من السنسور
        if (finger.deleteModel(id) == FINGERPRINT_OK) {
          Serial.printf("✅ Deleted ID %d from Sensor\n", id);
        } else {
          Serial.printf("❌ Failed to delete ID %d from Sensor\n", id);
        }

      } else {
        // الاسم موجود → نرسل للسيرفر
        Serial.printf("🟡 Found ID %d, Name: %s\n", id, name.c_str());

        HTTPClient http;
        String url = "https://192.168.1.12:7069/api/SensorData";
        http.begin(url);
        http.addHeader("Content-Type", "application/json");

        DynamicJsonDocument doc(256);
        doc["id"] = id;
        doc["name"] = name;

        String jsonStr;
        serializeJson(doc, jsonStr);
        int httpCode = http.POST(jsonStr);

        if (httpCode == 200 || httpCode == 201) {
          Serial.printf("✅ Synced ID %d\n", id);
          lcd.setCursor(0, 1);
          lcd.print("Synced & Deleted");
        } else {
          Serial.printf("❌ Failed to sync ID %d, HTTP code: %d\n", id, httpCode);
          lcd.setCursor(0, 1);
          lcd.print("Sync Failed");
        }

        http.end();
      }

      delay(1500); // راحة بسيطة لكل ID
    }
  }
} 

int getFingerprintIDFromServer() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("https://192.168.1.12:7069/api/generate-id"); // استبدل بالرابط الفعلي
    int httpCode = http.GET();

    if (httpCode == 200) {
      String payload = http.getString();
      Serial.println("Received ID: " + payload);
      http.end();
      return payload.toInt();  // لازم API يرجع رقم فقط
    } else {
      Serial.print("HTTP error: ");
      Serial.println(httpCode);
      http.end();
      return -1;
    }
  } else {
    Serial.println("WiFi not connected");
    return -1;
  }
}

void sendToServer(uint16_t id, String name) {
  if (WiFi.status() != WL_CONNECTED) {
    lcd.clear();
    lcd.print("WiFi Not Conn");
    Serial.println("WiFi not connected, saved locally.");
    return;
  }

  HTTPClient http;
  String url = "https://192.168.1.12:7069/api/SensorData";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(256);
  doc["id"] = id;
  doc["name"] = name;

  String jsonStr;
  serializeJson(doc, jsonStr);

  int httpCode = http.POST(jsonStr);

  if (httpCode == 200 || httpCode == 201) {
    lcd.clear();
    lcd.print("Sent to Server");
    Serial.println("Data sent: " + jsonStr);
  } else {
    lcd.clear();
    lcd.print("Send Fail");
    Serial.printf("Send error: %d\n", httpCode);
  }

  http.end();
  delay(1000);
}

