#define TINY_GSM_MODEM_GENERIC  // Use this instead of A7670
#include <TinyGsmClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <HTTPClient.h>

// ========== 4G / SIM CARD CREDENTIALS ==========
const char apn[]      = "JIO4G";           // APN for your SIM (e.g., "JIO4G", "airtelgprs", "internet")
const char gprsUser[] = "";                // Leave blank if not required
const char gprsPass[] = "";                // Leave blank if not required

// ========== FIREBASE CREDENTIALS ==========
const char* firebase_host = "fenceconnect-1bd91-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* deviceID = "FenceA";           // CHANGE THIS FOR EACH CUSTOMER

// ========== PIN DEFINITIONS ==========
const int RELAY_PIN = 23;    
const int BATTERY_PIN = 34;  

// ========== HARDWARE SERIAL (ESP32 Serial2) ==========
#define SerialAT Serial2

// ========== GLOBAL VARIABLES ==========
TinyGsm modem(SerialAT);
TinyGsmClient client(modem);
Preferences preferences;

bool fenceState = false;
float batteryVoltage = 0.0;

// Timer Variables (Default 6:00 AM ON, 6:00 PM OFF)
int turnOnHour = 6;
int turnOnMinute = 0;
int turnOffHour = 18;
int turnOffMinute = 0;

unsigned long lastTimeCheck = 0;

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  
  // Initialize Serial2 for A7670 4G (RX=16, TX=17)
  SerialAT.begin(115200, SERIAL_8N1, 16, 17);
  delay(3000);
  
  Serial.println("Connecting to 4G Network...");
  if (!modem.restart()) {
    Serial.println("Failed to restart modem.");
    while(true) {}
  }
  
  String modemInfo = modem.getModemInfo();
  Serial.print("Modem: "); Serial.println(modemInfo);
  
  Serial.print("Waiting for network...");
  if (!modem.waitForNetwork()) {
    Serial.println(" fail");
    while(true) {}
  }
  Serial.println(" OK");
  
  Serial.print("Connecting to ");
  Serial.print(apn);
  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    Serial.println(" fail");
    while(true) {}
  }
  Serial.println(" OK");
  
  Serial.print("GSM IP: ");
  Serial.println(modem.getLocalIP());
  
  // Load saved timer from memory
  preferences.begin("fence", false);
  turnOnHour = preferences.getInt("onHour", 6);
  turnOnMinute = preferences.getInt("onMin", 0);
  turnOffHour = preferences.getInt("offHour", 18);
  turnOffMinute = preferences.getInt("offMin", 0);
  preferences.end();
  
  Serial.println("Fence Controller Ready!");
  Serial.print("Device ID: ");
  Serial.println(deviceID);
}

void loop() {
  checkFirebaseForCommands(); // Checks Firebase for ON/OFF & Timer
  checkTimer();               // Checks if it's time to auto turn on/off
  readBattery();
  delay(500);
}

// ========== FIREBASE CLOUD READER ==========
void checkFirebaseForCommands() {
  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) return;
  if (!client.connected()) client.connect(firebase_host, 80);

  HTTPClient http;
  String url = "http://" + String(firebase_host) + "/devices/" + String(deviceID) + ".json";
  
  http.begin(client, url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      String cmd = doc["cmd"];
      String onHourStr = doc["onHour"];
      String onMinStr = doc["onMin"];
      String offHourStr = doc["offHour"];
      String offMinStr = doc["offMin"];
      String trig = doc["trig"];
      
      // 1. ON/OFF Logic (Your absolute stable logic)
      if (cmd == "ON" && !fenceState) {
        digitalWrite(RELAY_PIN, HIGH);
        fenceState = true;
        Serial.println("Fence: ON (4G)");
      } 
      else if (cmd == "OFF" && fenceState) {
        digitalWrite(RELAY_PIN, LOW);
        fenceState = false;
        Serial.println("Fence: OFF (4G)");
      }
      
      // 2. Timer Data Update (Always reads the latest numbers)
      turnOnHour = onHourStr.toInt();
      turnOnMinute = onMinStr.toInt();
      turnOffHour = offHourStr.toInt();
      turnOffMinute = offMinStr.toInt();
      
      // 3. Timer Save & Trigger Logic
      if (trig == "1") {
        preferences.begin("fence", false);
        preferences.putInt("onHour", turnOnHour);
        preferences.putInt("onMin", turnOnMinute);
        preferences.putInt("offHour", turnOffHour);
        preferences.putInt("offMin", turnOffMinute);
        preferences.end();
        Serial.println("TIMER SAVED TO MEMORY!");
        
        // Clear the trigger flag in Firebase (PATCH to set trig to 0)
        HTTPClient clearHttp;
        String clearUrl = "http://" + String(firebase_host) + "/devices/" + String(deviceID) + "/trig.json";
        clearHttp.begin(client, clearUrl);
        clearHttp.addHeader("Content-Type", "application/json");
        int clearCode = clearHttp.PATCH("0"); // Set to 0
        clearHttp.end();
        
        // Immediately check if the fence needs to turn ON
        checkTimer();
      }
    }
  }
  http.end();
}

// ========== AUTO TIMER LOGIC ==========
void checkTimer() {
  // Update the ESP32's clock via HTTP time API (since we don't have UDP NTP easily)
  if (millis() - lastTimeCheck > 60000) { // Check time every 60 seconds
    HTTPClient http;
    String url = "http://worldtimeapi.org/api/timezone/Asia/Kolkata";
    http.begin(client, url);
    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      StaticJsonDocument<512> doc;
      deserializeJson(doc, payload);
      String dt = doc["datetime"];
      // Format: 2026-08-08T15:30:00...
      int h = dt.substring(11, 13).toInt();
      int m = dt.substring(14, 16).toInt();
      
      if (h == turnOnHour && m == turnOnMinute && !fenceState) {
        digitalWrite(RELAY_PIN, HIGH);
        fenceState = true;
        Serial.println("Auto ON triggered by Timer");
      }
      else if (h == turnOffHour && m == turnOffMinute && fenceState) {
        digitalWrite(RELAY_PIN, LOW);
        fenceState = false;
        Serial.println("Auto OFF triggered by Timer");
      }
    }
    http.end();
    lastTimeCheck = millis();
  }
}

void readBattery() {
  static unsigned long lastRead = 0;
  if (millis() - lastRead > 5000) {
    float voltage = (analogRead(BATTERY_PIN) / 4095.0) * 3.3 * 2.0;
    batteryVoltage = voltage;
    lastRead = millis();
  }
}
