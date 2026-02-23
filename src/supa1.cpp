#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <time.h>
#include <TM1637Display.h>
#include <Wire.h>
#include "RTClib.h"
#include <ESP8266HTTPClient.h>
#include <LoRa.h>
#include <SPI.h>

// Function declarations
void compareAndSyncTime();
void updateTable(String token, int st, int ds);
void updateackTable(String token, int ack);
void updateTable1(String token, int st1, int ds1);
void updateackTable1(String token, int ack1);
int login_email(String email_a, String password_a);

// Constants
const int device_id = 10100101;

RTC_DS1307 rtc;

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif

// EEPROM Configuration
#define EEPROM_SIZE 256
#define EEPROM_START_UNIX_ADDR   0   // Motor 1 schedule
#define EEPROM_STOP_UNIX_ADDR    4
#define EEPROM_START_UNIX1_ADDR  8   // Motor 2 schedule
#define EEPROM_STOP_UNIX1_ADDR   12

// Pin Definitions
#define MOTOR1_PIN D8        // Only Motor 1 has physical motor
#define LORA_STATUS_LED LED_BUILTIN  // For LORA device status LED
#define CLK_PIN   D3
#define DIO_PIN   D4
#define OT_SENSOR_PIN D1
#define MOTOR1_BUTTON D9

// OT Sensor Configuration
#define OT_ACTIVE_LEVEL LOW
int ot_sensorcount = 0;
const int OT_TRIP_COUNT = 3;

// Display
TM1637Display display(CLK_PIN, DIO_PIN);

// Motor status
bool motor1_status_manual = 0;
bool motor2_status_manual = 0;

// Timing variables
unsigned long mili_now;
unsigned long lastExecutionTime = 0;
unsigned long lastLoraExecutionTime = 0;
const unsigned long EXECUTION_INTERVAL = 10000;     // 10 seconds for Motor 1
const unsigned long LORA_INTERVAL = 5000;          // 5 seconds for LORA updates
const unsigned long POST_SCHEDULE_LORA_TIME = 120000; // 120 seconds of "full" signals

// Schedule storage
uint32_t lastSavedStartUnix = 0;
uint32_t lastSavedStopUnix = 0;
uint32_t lastSavedStartUnix1 = 0;
uint32_t lastSavedStopUnix1 = 0;

// Schedule structures
struct Schedule {
  time_t unixTime;
  bool triggered;
};

uint8_t scheduleCount = 3;

// Motor 1 structure (physical motor)
struct sch {
  uint32_t startUnix;
  uint32_t stopUnix;
  uint8_t startTime;
  uint8_t stopTime;
  bool active;
  int state;
  int ack;
  int sch1_en;
};

// Motor 2 structure (LORA only)
struct sche {
  uint32_t startUnix1;
  uint32_t stopUnix1;
  uint8_t startTime1;
  uint8_t stopTime1;
  bool active1;
  int state1;
  int ack1;
  int sch1_en1;
  bool sendPostScheduleFull;  // Flag to send "full" signals after schedule
  unsigned long postScheduleEndTime; // When to stop sending "full" signals
};

// Motor instances
sch sch1;
sch updated_sch1;
sche sche1;
sche updated_sche1;

// Login and network variables
int login_status = 0;
int login_timeout = 0;
String phone_or_email;
String password;
String data;
String loginMethod;
String filter;
int authTimeout = 0;
unsigned long loginTime;
bool useAuth;
String USER_TOKEN;

// Network clients
WiFiClientSecure client;
HTTPClient https;
time_t internetTime;

// WiFi and Supabase credentials
char WIFI_SSID[20] = "Anupam";
char WIFI_PASS[20] = "12345678";
char SUPABASE_URL[100] = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
char AUTH_URL[100] = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/auth/v1/token?grant_type=password";
char SUPABASE_KEY[300] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";
char USER_EMAIL[30] = "9999900002@gmail.com";
char USER_PASS[10] = "1234";

// Time sync variables
const long DRIFT_THRESHOLD = 30;  // rtc drift (difference) threshold in seconds
unsigned long lastSync = 0;
const unsigned long SYNC_INTERVAL = 1 * 60 * 60 * 1000UL; // 1 hour in milliseconds

// Schedule cancellation flags
bool scheduleCancelledByApp = false;
bool scheduleCancelledByApp1 = false;

// LORA Configuration
#define LORA_SS 15
#define LORA_RST 16
#define LORA_DIO0 2
#define LORA_FREQUENCY 433920000

// LORA Data
String loraDeviceId = "210001";  // Changed to 210001
int loraVstate1 = 1;  // 1 = full (default)
int loraVstate2 = 1;  // 1 = full (default)
bool loraInitialized = false;

// App state tracking
int lastAppState = -1;
int lastAppState1 = -1;

// Day change tracking
int lastDay = -1;
int lastDay1 = -1;

// ==================== MOTOR 1 FUNCTIONS (Physical Motor) ====================

// Function to load schedules from EEPROM or set defaults
void loadSchedules() {
  sch1 = {
    1767225600,  // start time
    1767225600,  // stop time
    0,
    0,
    false,
    0,
    0,
    0
  };
}

void saveScheduleToEEPROM() {
  if (sch1.startUnix != lastSavedStartUnix ||
      sch1.stopUnix != lastSavedStopUnix) {

    EEPROM.put(EEPROM_START_UNIX_ADDR, sch1.startUnix);
    EEPROM.put(EEPROM_STOP_UNIX_ADDR, sch1.stopUnix);
    EEPROM.commit();

    lastSavedStartUnix = sch1.startUnix;
    lastSavedStopUnix = sch1.stopUnix;
    updateackTable(USER_TOKEN, 1);

    Serial.println("Motor1 schedule changed → saved to EEPROM");
  } else {
    Serial.println("Motor1 schedule unchanged → EEPROM not written");
  }
}

void loadScheduleFromEEPROM() {
  EEPROM.get(EEPROM_START_UNIX_ADDR, sch1.startUnix);
  EEPROM.get(EEPROM_STOP_UNIX_ADDR, sch1.stopUnix);

  if (sch1.startUnix < 1000000000 || sch1.stopUnix < sch1.startUnix) {
    Serial.println("Invalid EEPROM data for Motor1, using defaults");
    loadSchedules();
    scheduleCancelledByApp = false;
  }

  sch1.active = false;

  // Track last saved values
  lastSavedStartUnix = sch1.startUnix;
  lastSavedStopUnix = sch1.stopUnix;
}

// Function to convert hour and minute to unix time (UTC) for today
uint32_t hourMinuteToUnixUTC(uint8_t hour, uint8_t minute) {
  DateTime now = rtc.now(); // assumed UTC
 
  DateTime dt(
    now.year(),
    now.month(),
    now.day(),
    hour,
    minute,
    0
  );
 
  return dt.unixtime();
}

// Function to check and execute schedule
void checkSch(uint32_t nowUnix) {
    // START schedule
    if (!sch1.active &&
        !scheduleCancelledByApp &&
        nowUnix >= sch1.startUnix &&
        nowUnix < sch1.stopUnix && sch1.ack==1 && sch1.sch1_en==1) {

        sch1.active = true;
        digitalWrite(MOTOR1_PIN, HIGH);
        Serial.println("MOTOR1 SCHEDULE START");
        updateTable(USER_TOKEN, 1, 1);
        updateackTable(USER_TOKEN, 1);
    }

    // STOP schedule
    if (sch1.active && nowUnix >= sch1.stopUnix) {
        sch1.active = false;
        digitalWrite(MOTOR1_PIN, LOW);
        sch1.state = 0;
        Serial.println("MOTOR1 SCHEDULE STOP");
        updateTable(USER_TOKEN, 0, 0);
    }
}

// ==================== MOTOR 2 FUNCTIONS (LORA Only) ====================

void loadSchedules1() {
  sche1 = {
    1767225600,  // startUnix1
    1767225600,  // stopUnix1
    0,           // startTime1
    0,           // stopTime1
    false,       // active1
    0,           // state1
    0,           // ack1
    0,           // sch1_en1
    false,       // sendPostScheduleFull
    0            // postScheduleEndTime
  };
}

void saveScheduleToEEPROM1() {
  if (sche1.startUnix1 != lastSavedStartUnix1 ||
      sche1.stopUnix1 != lastSavedStopUnix1) {

    EEPROM.put(EEPROM_START_UNIX1_ADDR, sche1.startUnix1);
    EEPROM.put(EEPROM_STOP_UNIX1_ADDR, sche1.stopUnix1);
    EEPROM.commit();

    lastSavedStartUnix1 = sche1.startUnix1;
    lastSavedStopUnix1 = sche1.stopUnix1;
    updateackTable1(USER_TOKEN, 1);

    Serial.println("Motor2 schedule saved to EEPROM");
  }
}

void loadScheduleFromEEPROM1() {
  EEPROM.get(EEPROM_START_UNIX1_ADDR, sche1.startUnix1);
  EEPROM.get(EEPROM_STOP_UNIX1_ADDR, sche1.stopUnix1);

  if (sche1.startUnix1 < 1000000000 || sche1.stopUnix1 < sche1.startUnix1) {
    Serial.println("Invalid EEPROM data for Motor2, using defaults");
    loadSchedules1();
    scheduleCancelledByApp1 = false;
  }

  sche1.active1 = false;
  sche1.sendPostScheduleFull = false;
  lastSavedStartUnix1 = sche1.startUnix1;
  lastSavedStopUnix1 = sche1.stopUnix1;
}

void checkSch1(uint32_t nowUnix) {
  // Check if schedule should start
  if (!sche1.active1 &&
      !scheduleCancelledByApp1 &&
      nowUnix >= sche1.startUnix1 &&
      nowUnix < sche1.stopUnix1 &&
      sche1.ack1 == 1 &&
      sche1.sch1_en1 == 1) {

    sche1.active1 = true;
    sche1.sendPostScheduleFull = false; // Reset post-schedule flag
    loraVstate1 = 0;  // EMPTY signal
    loraVstate2 = 0;  // EMPTY signal
    
    Serial.println("MOTOR2 SCHEDULE START - Sending EMPTY signals via LORA");
    updateTable1(USER_TOKEN, 1, 1);
    updateackTable1(USER_TOKEN, 1);
  }

  // Check if schedule should end
  if (sche1.active1 && nowUnix >= sche1.stopUnix1) {
    sche1.active1 = false;
    sche1.state1 = 0;
    sche1.sendPostScheduleFull = true;
    sche1.postScheduleEndTime = millis() + POST_SCHEDULE_LORA_TIME;
    loraVstate1 = 1;  // FULL signal
    loraVstate2 = 1;  // FULL signal
    
    Serial.println("MOTOR2 SCHEDULE STOP - Will send FULL signals for 120 seconds");
    updateTable1(USER_TOKEN, 0, 0);
  }
}

// ==================== LORA FUNCTIONS ====================

void initializeLORA() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  LoRa.setSyncWord(0xA2);
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(62.5E3);
  
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("LORA initialization failed!");
    loraInitialized = false;
    return;
  }
  
  loraInitialized = true;
  Serial.println("LORA Initialized OK!");
}

void sendLORAData() {
  if (!loraInitialized) {
    Serial.println("LORA not initialized!");
    return;
  }

  // Determine what signal to send based on Motor2 state
  if (sche1.active1) {
    // During schedule time: send EMPTY signals continuously
    loraVstate1 = 0;
    loraVstate2 = 0;
    Serial.println("LORA: Sending EMPTY signal (schedule active)");
  } 
  else if (sche1.sendPostScheduleFull && millis() < sche1.postScheduleEndTime) {
    // After schedule: send FULL signals for 120 seconds
    loraVstate1 = 1;
    loraVstate2 = 1;
    Serial.println("LORA: Sending FULL signal (post-schedule)");
  } 
  else if (sche1.sendPostScheduleFull && millis() >= sche1.postScheduleEndTime) {
    // Stop sending post-schedule signals after 120 seconds
    sche1.sendPostScheduleFull = false;
    
    // Now use app state
    if (sche1.state1 == 1) {
      loraVstate1 = 0;
      loraVstate2 = 0;
      Serial.println("LORA: Sending EMPTY signal (app ON)");
    } else {
      loraVstate1 = 1;
      loraVstate2 = 1;
      Serial.println("LORA: Sending FULL signal (app OFF)");
    }
  } 
  else {
    // Normal operation based on app state
    if (sche1.state1 == 1) {
      loraVstate1 = 0;
      loraVstate2 = 0;
      Serial.println("LORA: Sending EMPTY signal (app ON)");
    } else {
      loraVstate1 = 1;
      loraVstate2 = 1;
      Serial.println("LORA: Sending FULL signal (app OFF)");
    }
  }

  // Send LORA packet
  LoRa.beginPacket();
  LoRa.print(loraDeviceId);
  LoRa.print(loraVstate1);
  LoRa.print(loraVstate2);
  LoRa.endPacket();

  // Print what was sent
  Serial.print("LORA Packet Sent: ");
  Serial.print(loraDeviceId);
  Serial.print(loraVstate1);
  Serial.println(loraVstate2);

  // Blink LED to indicate transmission
  digitalWrite(LORA_STATUS_LED, HIGH);
  delay(50);
  digitalWrite(LORA_STATUS_LED, LOW);
}

// ==================== DATABASE FUNCTIONS ====================

// Function to check if the device is online
bool isOnline() {
  return WiFi.status() == WL_CONNECTED;
}

void updateTable(String token, int st, int ds){
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.211";
  https.begin(client, supabaseUrl);
  https.setTimeout(3000);

  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");
  String payload = "{\"state\": " + String(st) + ", \"device_state\": " + String(ds) + "}";

  int httpCode = https.sendRequest("PATCH", payload);

  Serial.print("Motor1 Update HTTP Code: ");
  Serial.println(httpCode);

  https.end();
}

// Function to update the database table
void updateackTable(String token, int ack){
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.211";
  https.begin(client, supabaseUrl);
  https.setTimeout(3000);

  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");
  String payload = "{\"ack\": " + String(ack) + "}";  
  int httpCode = https.sendRequest("PATCH", payload);

  Serial.print("Motor1 Ack Update HTTP Code: ");
  Serial.println(httpCode);

  https.end();
}

void updateTable1(String token, int st1, int ds1) {
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.245";
  https.begin(client, supabaseUrl);
  https.setTimeout(3000);
  
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");
  
  String payload = "{\"state\": " + String(st1) + ", \"device_state\": " + String(ds1) + "}";
  int httpCode = https.sendRequest("PATCH", payload);
  
  Serial.print("Motor2 Update HTTP Code: ");
  Serial.println(httpCode);
  https.end();
}

void updateackTable1(String token, int ack1) {
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.245";
  https.begin(client, supabaseUrl);
  https.setTimeout(3000);
  
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");
  
  String payload = "{\"ack\": " + String(ack1) + "}";
  https.sendRequest("PATCH", payload);
  https.end();
}

void heartbeat(String token, int value){
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.211";
  https.begin(client, supabaseUrl);
  https.setTimeout(3000);

  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");
  String payload = "{\"heart_beat_count\": " + String(value) + "}";  
  int httpCode = https.sendRequest("PATCH", payload);

  Serial.print("Heartbeat HTTP Code: ");
  Serial.println(httpCode);

  https.end();
}

// Function to get data from the database table
void getTableData(String token, String field){
    const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.211";
    https.begin(client, supabaseUrl);
    https.setTimeout(3000);
    https.addHeader("apikey", SUPABASE_KEY);
    https.addHeader("Authorization", "Bearer " + String(token));
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Prefer", "return=minimal");

    int httpCode = https.GET();
    Serial.print("Motor1 GET HTTP Code: ");
    Serial.println(httpCode);
    
    if (httpCode == 200) {
        StaticJsonDocument<512> doc;
        deserializeJson(doc, https.getString());

        uint32_t duration = doc[0]["sch1_duration"];
        updated_sch1.state = doc[0]["state"];
        sch1.ack = doc[0]["ack"];
        sch1.sch1_en = doc[0]["sch1_en"];
        bool appOffPressed = (lastAppState == 1 && updated_sch1.state == 0);
        lastAppState = updated_sch1.state;

        if (!sch1.active) {
          if (updated_sch1.state == 1 && digitalRead(MOTOR1_PIN) == LOW ) {
            digitalWrite(MOTOR1_PIN, HIGH);
            updateTable(USER_TOKEN, 1, 1);
            Serial.println("Motor1 ON from App");
          }

          if (updated_sch1.state == 0 && digitalRead(MOTOR1_PIN) == HIGH) {
            digitalWrite(MOTOR1_PIN, LOW);
            updateTable(USER_TOKEN, 0, 0);
            Serial.println("Motor1 OFF from App");
          }
        }
        
        // Parse schedule times from server
        String schTime = doc[0]["sch1_start"];
        u_int16_t s1 = schTime.substring(0, 2).toInt();
        u_int16_t s2 = schTime.substring(3, 5).toInt();
        updated_sch1.startUnix = hourMinuteToUnixUTC(s1, s2);
        updated_sch1.stopUnix = updated_sch1.startUnix + (duration * 60);
        
        // If a new schedule window is received (future schedule), allow it
        if (scheduleCancelledByApp && updated_sch1.startUnix > rtc.now().unixtime()) {
            scheduleCancelledByApp = false;
            Serial.println("Motor1: New schedule detected – cancellation cleared");
        }

        if (sch1.active && appOffPressed) {
            sch1.active = false;
            scheduleCancelledByApp = true;
            digitalWrite(MOTOR1_PIN, LOW);
            updateackTable(USER_TOKEN, 0);
            Serial.println("Motor1 Schedule cancelled by App OFF");
        }

        if (!sch1.active && (sch1.startUnix != updated_sch1.startUnix || sch1.stopUnix != updated_sch1.stopUnix)) {
            sch1.startUnix = updated_sch1.startUnix;
            sch1.stopUnix = updated_sch1.stopUnix;
            saveScheduleToEEPROM();
        }
        Serial.println();
    }
    https.end();
}

void getTableData1(String token, String field) {
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.245";
  https.begin(client, supabaseUrl);
  https.setTimeout(3000);
  
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");

  int httpCode = https.GET();
  Serial.print("Motor2 GET HTTP Code: ");
  Serial.println(httpCode);
  
  if (httpCode == 200) {
    StaticJsonDocument<512> doc;
    deserializeJson(doc, https.getString());

    uint32_t duration1 = doc[0]["sch1_duration"];
    updated_sche1.state1 = doc[0]["state"];
    sche1.ack1 = doc[0]["ack"];
    sche1.sch1_en1 = doc[0]["sch1_en"];
    
    bool appOffPressed1 = (lastAppState1 == 1 && updated_sche1.state1 == 0);
    lastAppState1 = updated_sche1.state1;

    // Motor 2 doesn't control physical motor, just update state
    // Send LORA signal immediately when app state changes
    if (sche1.state1 != updated_sche1.state1) {
      sche1.state1 = updated_sche1.state1;
      
      // If turning ON, send EMPTY signal
      if (sche1.state1 == 1) {
        loraVstate1 = 0;
        loraVstate2 = 0;
        Serial.println("Motor2 App ON - Sending EMPTY signal");
      } 
      // If turning OFF, send FULL signal
      else if (sche1.state1 == 0) {
        loraVstate1 = 1;
        loraVstate2 = 1;
        Serial.println("Motor2 App OFF - Sending FULL signal");
      }
      
      // Send immediate LORA update
      sendLORAData();
    }

    // Parse schedule times
    String schTime1 = doc[0]["sch1_start"];
    uint16_t s1 = schTime1.substring(0, 2).toInt();
    uint16_t s2 = schTime1.substring(3, 5).toInt();
    updated_sche1.startUnix1 = hourMinuteToUnixUTC(s1, s2);
    updated_sche1.stopUnix1 = updated_sche1.startUnix1 + (duration1 * 60);

    // If new schedule window is received, allow it
    if (scheduleCancelledByApp1 && updated_sche1.startUnix1 > rtc.now().unixtime()) {
      scheduleCancelledByApp1 = false;
      Serial.println("Motor2: New schedule detected - cancellation cleared");
    }

    // Handle schedule cancellation by app
    if (sche1.active1 && appOffPressed1) {
      sche1.active1 = false;
      scheduleCancelledByApp1 = true;
      sche1.sendPostScheduleFull = true;
      sche1.postScheduleEndTime = millis() + POST_SCHEDULE_LORA_TIME;
      loraVstate1 = 1;
      loraVstate2 = 1;
      updateackTable1(USER_TOKEN, 0);
      Serial.println("Motor2 Schedule cancelled by App OFF - Will send FULL signals for 120s");
    }

    // Update schedule if changed
    if (!sche1.active1 && (sche1.startUnix1 != updated_sche1.startUnix1 || sche1.stopUnix1 != updated_sche1.stopUnix1)) {
      sche1.startUnix1 = updated_sche1.startUnix1;
      sche1.stopUnix1 = updated_sche1.stopUnix1;
      saveScheduleToEEPROM1();
    }
  }
  https.end();
}

// ==================== TIME SYNC FUNCTIONS ====================

// Function to get internet time
bool getInternetUnixTime(time_t &unixTime) {
  if (!isOnline()) return false;
 
  https.begin(client, "https://api.skyiottech.com/time");
  https.setTimeout(3000);
 
  int code = https.GET();
  if (code != 200) {
    Serial.println("Failed to get internet time");
    https.end();
    return false;
  }
 
  StaticJsonDocument<512> doc;
  deserializeJson(doc, https.getString());
  Serial.println(https.getString());
  https.end();
 
  unixTime = doc["unix_time"];
  return true;
}
 
// Function to get RTC unix time
time_t getRtcUnixTime() {
  DateTime now = rtc.now();
  Serial.print("RTC Time: ");  
  Serial.print(now.unixtime());
  return now.unixtime();
}
 
// Function to compare and sync time
void compareAndSyncTime() {
  if(!rtc.isrunning()){
    Serial.println("RTC is NOT running, can't sync time.");
    return;
  }
 
  if (lastSync != 0 && millis() - lastSync < SYNC_INTERVAL){
    Serial.println("Time sync not needed yet.");
    return;
  }
 
  time_t internetTime;
  if (!getInternetUnixTime(internetTime)){
    Serial.println("Failed to get internet time.");
    return;
  }
 
  time_t rtcTime = getRtcUnixTime();
  long drift = abs((long)(internetTime - rtcTime));
 
  Serial.print("RTC drift: ");
  Serial.println(drift);
 
  if (drift > DRIFT_THRESHOLD) {
    rtc.adjust(DateTime(internetTime));
    Serial.println("RTC resynced");
  }
 
  lastSync = millis();
}

// ==================== MAIN PROCESSING FUNCTIONS ====================

void process_LocalEvents(){    
  static int lastDay = -1;
  DateTime now = rtc.now();

  if (now.day() != lastDay) {
      scheduleCancelledByApp = false;
      lastDay = now.day();
      Serial.println("Motor1: New day – app cancellation reset");
  }

  // Motor 1 Manual Button
  if (digitalRead(MOTOR1_BUTTON) == LOW) {
    delay(50); // debounce

    if (digitalRead(MOTOR1_BUTTON) == LOW) {
      Serial.println("Motor1 Button Pressed");

      // If schedule is running and motor is ON → cancel schedule
      if (sch1.active && digitalRead(MOTOR1_PIN) == HIGH) {
        sch1.active = false;
        scheduleCancelledByApp = true;
        Serial.println("Motor1 Schedule cancelled by Manual Button");
        updateackTable(USER_TOKEN, 0);
      }

      // Toggle motor
      if (digitalRead(MOTOR1_PIN) == HIGH) {
        digitalWrite(MOTOR1_PIN, LOW);
        motor1_status_manual = 0;
        updateTable(USER_TOKEN, 0, 0);
        Serial.println("Motor1 OFF by Button");
      } else {
        digitalWrite(MOTOR1_PIN, HIGH);
        motor1_status_manual = 1;
        updateTable(USER_TOKEN, 1, 1);
        Serial.println("Motor1 ON by Button");
      }
      delay(500); // prevent multiple triggers
    }
  }

  if (mili_now - lastExecutionTime >= EXECUTION_INTERVAL) {
    Serial.println("\n=== Processing Motor1 Events ===");
    lastExecutionTime = mili_now;
    
    checkSch(rtc.now().unixtime());
    
    DateTime dt(sch1.startUnix);
    Serial.print("Current Date and Time: ");
    Serial.print(rtc.now().hour());
    Serial.print(":");
    Serial.print(rtc.now().minute());
    Serial.print(":");
    Serial.print(rtc.now().second());
    Serial.println();
    
    Serial.print("Motor1 Schedule Start Time: ");
    Serial.print(dt.hour());
    Serial.print(":");
    Serial.println(dt.minute());
    
    Serial.print("Auth Timeout in seconds: ");
    Serial.println(authTimeout);
    
    heartbeat(USER_TOKEN, 10);
    
    if(authTimeout>20){
      authTimeout -= 10;
    }
    
    if(WiFi.status() != WL_CONNECTED) {  
      Serial.println("WiFi Disconnected - Reconnecting...");
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      authTimeout = 0;
      login_status = 0;
    }
    
    if(WiFi.status() == WL_CONNECTED) {
      if (lastSync == 0) {
        compareAndSyncTime();   
      }
      
      if(login_status==0) {
        int n1 = login_email(USER_EMAIL, USER_PASS);
        Serial.print("Login Process HTTP Code: ");
        Serial.println(n1);
        login_status = 1;
      }   
      
      if(authTimeout <= 100) {
        Serial.println("Auth Token Timeout...");
        login_status = 0;
      }

      getTableData(USER_TOKEN, "");
    }
  }
}

void process_LoraEvents() {
  static int lastDay1 = -1;
  DateTime now = rtc.now();

  if (now.day() != lastDay1) {
    scheduleCancelledByApp1 = false;
    lastDay1 = now.day();
    Serial.println("Motor2: New day - app cancellation reset");
  }

  // Process Motor 2 schedule check
  checkSch1(rtc.now().unixtime());

  // Send LORA data periodically
  if (mili_now - lastLoraExecutionTime >= LORA_INTERVAL) {
    Serial.println("\n=== Processing Motor2 LORA Events ===");
    lastLoraExecutionTime = mili_now;
    
    // Display current time
    Serial.print("Current Time: ");
    Serial.print(now.hour());
    Serial.print(":");
    Serial.print(now.minute());
    Serial.print(":");
    Serial.println(now.second());
    
    // Get Motor2 data from database
    if (WiFi.status() == WL_CONNECTED && login_status == 1) {
      getTableData1(USER_TOKEN, "");
    }
    
    // Send LORA data
    sendLORAData();
  }
}

// ==================== LOGIN FUNCTIONS ====================

// Function to handle login process
int _login_process() {
  int httpCode;
  JsonDocument doc;
  Serial.println("Beginning to login..");
  https.setTimeout(3000);
 
  if (https.begin(client, AUTH_URL)) {
    https.addHeader("apikey", SUPABASE_KEY);
    https.addHeader("Content-Type", "application/json");
    String loginMethod = "email";
 
    String query = "{\"" + loginMethod + "\": \"" + phone_or_email + "\", \"password\": \"" + password + "\"}";
    httpCode = https.POST(query);
 
    if (httpCode > 0) {
      String data = https.getString();
      deserializeJson(doc, data);
      if (doc.containsKey("access_token") && !doc["access_token"].isNull() && doc["access_token"].is<String>() && !doc["access_token"].as<String>().isEmpty()) {
        USER_TOKEN = doc["access_token"].as<String>();
        authTimeout = doc["expires_in"].as<int>();
        Serial.println("Login Success");
      } else {
        Serial.println("Login Failed: Invalid access token in response");
      }
    } else {
      Serial.println(phone_or_email);
      Serial.println(password);
      Serial.print("Login Failed : ");
      Serial.println(httpCode);
    }
 
    https.end();
    loginTime = millis();
  } else {
    return -100;
  }
 
  return httpCode;
}
 
// Function to login using email and password
int login_email(String email_a, String password_a) {
  useAuth = true;
  loginMethod = "email";
  phone_or_email = email_a;
  password = password_a;
 
  int httpCode = 0;
  while (httpCode <= 0) {
    httpCode = _login_process();
  }
  return httpCode;
}

// ==================== SETUP AND LOOP ====================

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  pinMode(MOTOR1_PIN, OUTPUT);
  digitalWrite(MOTOR1_PIN, LOW);
  pinMode(MOTOR1_BUTTON, INPUT_PULLUP);
  pinMode(LORA_STATUS_LED, OUTPUT);
  display.setBrightness(0x0f);
  display.clear();
  client.setInsecure();
  
  // Initialize RTC
  bool rtc_status = rtc.begin();
  delay(1000);
  
  if(rtc_status == true){
    Serial.println("RTC Found");
    
    // Check if RTC is running
    if (rtc.isrunning()) {
      Serial.println("RTC is running");
      
      // Display current RTC time
      DateTime now = rtc.now();
      Serial.print("RTC Time: ");
      Serial.print(now.year());
      Serial.print("/");
      Serial.print(now.month());
      Serial.print("/");
      Serial.print(now.day());
      Serial.print(" ");
      Serial.print(now.hour());
      Serial.print(":");
      Serial.print(now.minute());
      Serial.print(":");
      Serial.println(now.second());
    } else {
      Serial.println("RTC is NOT running - Setting to compile time");
      // Set RTC to compile time
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  } else {
    Serial.println("RTC Not Found - Check connections");
  }
  
  // Load schedules from EEPROM
  loadScheduleFromEEPROM();   // Motor 1
  loadScheduleFromEEPROM1();  // Motor 2
  
  // Initialize LORA
  initializeLORA();
}

void loop() {
  mili_now = millis();
  
  // Process Motor 1 events (physical motor)
  process_LocalEvents();
  
  // Process Motor 2 events (LORA only)
  process_LoraEvents();
  
  delay(100);
}