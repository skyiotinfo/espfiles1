#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <time.h>
#include <TM1637Display.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include "RTClib.h"
#include <ESP8266HTTPClient.h>
#include <LoRa.h>
#include <SPI.h>

// -------------------- LoRa Definitions --------------------
#define LORA_SS     D5      // GPIO14
#define LORA_RST    D6      // GPIO12
#define LORA_DIO0   D0      // GPIO16
#define LORA_DEVICE_ID "103201"

#define LOW_WATER_PIN  D1   // GPIO5 – LOW = water absent, HIGH = water present
#define HIGH_WATER_PIN D2   // GPIO4 (optional, for info)

// -------------------- Original Pin Definitions --------------------
#define MOTOR_PIN   D8      // Not used directly – kept for compatibility
#define CLK_PIN     D3
#define DIO_PIN     D4
#define OT_SENSOR_PIN D7
#define OT_ACTIVE_LEVEL LOW
#define INPUT_BUTTON D9

// -------------------- Constants & Global Variables --------------------
const int device_id = 110001;
RTC_DS1307 rtc;

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

#define EEPROM_SIZE 128
#define DEVICE_ID 0
#define EEPROM_START_UNIX_ADDR 10
#define EEPROM_STOP_UNIX_ADDR 25
#define EEPROM_LOCAL_STATE 40
#define EEPROM_APP_MANUAL_STATUS 42
#define EEPROM_UPDATED_BY 44

int ot_sensorcount = 0;
const int OT_TRIP_COUNT = 5;

TM1637Display display(CLK_PIN, DIO_PIN);

int motor_status_manual = 0;
unsigned long mili_now;
unsigned long lastExecutionTime = 0;
long EXECUTION_INTERVAL = 10000;
uint32_t lastSavedStartUnix = 0;
uint32_t lastSavedStopUnix = 0;
int count = 0;

// Motor state tracking (true = motor ON)
bool motorRunning = false;

struct Schedule {
  time_t unixTime;
  bool triggered;
};
uint8_t scheduleCount = 3;

struct sch {
  int device_id;
  uint32_t startUnix;
  uint32_t stopUnix;
  uint8_t startTime;
  uint8_t stopTime;
  int duration;
  int active;
  int state;
  int ack;
  int sch1_en;
  int updated_by;
  int local_state;
};

sch sch1;
sch updated_sch1;
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

WiFiClientSecure client;
HTTPClient https;
time_t internetTime;
int appManualStop = 0;

char WIFI_SSID[20] = "riyaz";
char WIFI_PASS[20] = "12345678";
const char *supabase_device_url = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?device_id=eq.110001";
char SUPABASE_URL[100] = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
char AUTH_URL[100] = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/auth/v1/token?grant_type=password";
char SUPABASE_KEY[300] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";
char USER_EMAIL[30] = "9630852741@gmail.com";
char USER_PASS[10] = "123456";

const long DRIFT_THRESHOLD = 30;
unsigned long lastSync = 0;
const unsigned long SYNC_INTERVAL = 1 * 60 * 60 * 1000UL;

// -------------------- Function Prototypes --------------------
void compareAndSyncTime();
void updateTable(String token, int st, int ds);
void updateackTable(String token, int ack);
int login_email(String email_a, String password_a);
bool isOnline();
void loadSchedules();
void processOTSensor();
void saveScheduleToEEPROM();
void loadScheduleFromEEPROM();
uint32_t hourMinuteToUnixUTC(uint8_t hour, uint8_t minute);
void checkSch(uint32_t nowUnix);
bool isWaterPresent();
void sendLoraCommand(String cmd);
void motorOn();
void motorOff();
void heartbeat(String token, int value);
void getTableData(String token, String field);
bool getInternetUnixTime(time_t &unixTime);
time_t getRtcUnixTime();
void process_LocalEvents();
int _login_process();

// -------------------- Water Sensor Check --------------------
bool isWaterPresent() {
  return digitalRead(LOW_WATER_PIN) == HIGH;   // HIGH = water present
}

// -------------------- LoRa Command Sender --------------------
void sendLoraCommand(String cmd) {
  LoRa.beginPacket();
  LoRa.print(cmd);
  LoRa.endPacket();
  Serial.print("LoRa sent: ");
  Serial.println(cmd);
  delay(100);   // brief pause after transmission
}

// -------------------- Motor Control via LoRa --------------------
void motorOn() {
  if (!isWaterPresent()) {
    Serial.println("Motor start blocked: No water (low sensor active)");
    return;
  }
  if (motorRunning) {
    Serial.println("Motor already ON");
    return;
  }
  sendLoraCommand(LORA_DEVICE_ID + "11");
  motorRunning = true;
  sch1.state = 1;
  sch1.active = 1;
  sch1.local_state = 1;
  if (isOnline()) {
    updateTable(USER_TOKEN, 1, 1);
    updateackTable(USER_TOKEN, 0);
    sch1.local_state = 0;
  }
  Serial.println("Motor ON command sent via LoRa");
}

void motorOff() {
  if (!motorRunning) {
    Serial.println("Motor already OFF");
    return;
  }
  sendLoraCommand(LORA_DEVICE_ID + "00");
  motorRunning = false;
  sch1.state = 0;
  sch1.active = 0;
  sch1.local_state = 1;
  if (isOnline()) {
    updateTable(USER_TOKEN, 0, 0);
    updateackTable(USER_TOKEN, 0);
    sch1.local_state = 0;
  }
  Serial.println("Motor OFF command sent via LoRa");
}

// -------------------- Original Functions (modified) --------------------
void loadSchedules() {
  sch1 = {
    110001,        // device_id
    1767225600,    // start time
    1767225600,    // stop time
    0, 0, 10, 0, 0, 0, 0, 0, 0
  };
}

void processOTSensor() {
  int ot_sensorstatus = digitalRead(OT_SENSOR_PIN);
  if (motorRunning) {
    if (ot_sensorstatus == OT_ACTIVE_LEVEL) {
      ot_sensorcount++;
      Serial.print("OT Sensor Count: ");
      Serial.println(ot_sensorcount);
      if (ot_sensorcount >= OT_TRIP_COUNT) {
        Serial.println("OT Sensor → Motor OFF");
        motorOff();
        sch1.active = 0;
        sch1.local_state = 1;
        if (isOnline()) {
          updateTable(USER_TOKEN, 0, 0);
          updateackTable(USER_TOKEN, 0);
          sch1.local_state = 0;
        }
        ot_sensorcount = 0;
      }
    } else {
      ot_sensorcount = 0;
    }
  } else {
    ot_sensorcount = 0;
  }
}

void saveScheduleToEEPROM() {
  if (sch1.startUnix != lastSavedStartUnix || sch1.stopUnix != lastSavedStopUnix) {
    EEPROM.put(EEPROM_START_UNIX_ADDR, sch1.startUnix);
    EEPROM.put(EEPROM_STOP_UNIX_ADDR, sch1.stopUnix);
    EEPROM.put(EEPROM_APP_MANUAL_STATUS, appManualStop);
    EEPROM.commit();
    lastSavedStartUnix = sch1.startUnix;
    lastSavedStopUnix = sch1.stopUnix;
    Serial.println("Schedule changed → saved to EEPROM");
  } else {
    Serial.println("Schedule unchanged → EEPROM not written");
  }
}

void loadScheduleFromEEPROM() {
  EEPROM.get(EEPROM_START_UNIX_ADDR, sch1.startUnix);
  EEPROM.get(EEPROM_STOP_UNIX_ADDR, sch1.stopUnix);
  EEPROM.get(EEPROM_LOCAL_STATE, sch1.updated_by);
  EEPROM.get(EEPROM_APP_MANUAL_STATUS, appManualStop);
  if (sch1.startUnix < 1000000000 || sch1.stopUnix < sch1.startUnix) {
    Serial.println("Invalid EEPROM data, using defaults");
    loadSchedules();
    appManualStop = 0;
  }
  sch1.active = 0;
  sch1.state = 0;
  lastSavedStartUnix = sch1.startUnix;
  lastSavedStopUnix = sch1.stopUnix;
}

uint32_t hourMinuteToUnixUTC(uint8_t hour, uint8_t minute) {
  DateTime now = rtc.now();
  DateTime dt(now.year(), now.month(), now.day(), hour, minute, 0);
  return dt.unixtime();
}

int lastAppState = -1;

void checkSch(uint32_t nowUnix) {
  // START schedule
  if (sch1.active == 0 && nowUnix >= sch1.startUnix && nowUnix < sch1.stopUnix && sch1.sch1_en == 1 && appManualStop == 0) {
    motorOn();      // will check water & send LoRa start
    sch1.active = 1;
    Serial.println("SCHEDULE START");
  }
  // STOP schedule
  if (sch1.active == 1 && nowUnix >= sch1.stopUnix) {
    motorOff();     // send LoRa stop
    sch1.active = 0;
    Serial.println("SCHEDULE STOP");
  }
}

bool isOnline() {
  if (WiFi.status() != WL_CONNECTED) return false;
  https.begin(client, "https://api.skyiottech.com/time");
  https.setTimeout(5000);
  int code = https.GET();
  https.end();
  return (code == 200);
}

void updateTable(String token, int st, int ds) {
  https.begin(client, supabase_device_url);
  https.setTimeout(3000);
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");
  String payload = "{\"state\": " + String(st) + ", \"device_state\": " + String(ds) + "}";
  int httpCode = https.sendRequest("PATCH", payload);
  Serial.print("HTTP Code: "); Serial.println(httpCode);
  https.end();
}

void updateackTable(String token, int ack) {
  https.begin(client, supabase_device_url);
  https.setTimeout(3000);
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");
  String payload = "{\"ack\": " + String(ack) + "}";
  int httpCode = https.sendRequest("PATCH", payload);
  Serial.print("HTTP Code: "); Serial.println(httpCode);
  https.end();
}

void heartbeat(String token, int value) {
  https.begin(client, supabase_device_url);
  https.setTimeout(3000);
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");
  String payload = "{\"heart_beat_count\": " + String(value) + "}";
  int httpCode = https.sendRequest("PATCH", payload);
  Serial.print("HTTP Code: "); Serial.println(httpCode);
  https.end();
}

void getTableData(String token, String field) {
  https.begin(client, supabase_device_url);
  https.setTimeout(3000);
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  int httpCode = https.GET();
  if (httpCode == 200) {
    StaticJsonDocument<512> doc;
    deserializeJson(doc, https.getString());
    heartbeat(token, 10);
    uint32_t duration = doc[0]["sch1_duration"];
    updated_sch1.state = doc[0]["state"];
    sch1.ack = doc[0]["ack"];
    sch1.sch1_en = doc[0]["sch1_en"];
    int sync_duration = doc[0]["sync_duration"];
    EXECUTION_INTERVAL = sync_duration * 1000UL;
    bool appOffPressed = (lastAppState == 1 && updated_sch1.state == 0);
    lastAppState = updated_sch1.state;
    Serial.print("App State: "); Serial.println(updated_sch1.state);
    if (sch1.local_state == 1) {
      Serial.println("Local state change → skipping app command");
      updateTable(USER_TOKEN, sch1.state, sch1.state);
      sch1.local_state = 0;
      https.end();
      return;
    }
    // App wants to turn ON
    if (updated_sch1.state == 1 && !motorRunning && sch1.ack == 1) {
      motorOn();   // water check inside
      if (motorRunning) {
        count = 0;
        updateackTable(USER_TOKEN, 0);
        appManualStop = 0;
        EEPROM.put(EEPROM_APP_MANUAL_STATUS, appManualStop);
        EEPROM.commit();
      }
      Serial.println("Motor ON from App");
    }
    // App wants to turn OFF
    if (updated_sch1.state == 0 && motorRunning && sch1.ack == 1) {
      motorOff();
      updateackTable(USER_TOKEN, 0);
      Serial.println("Motor OFF from App");
      sch1.active = 0;
    }
    String schTime = doc[0]["sch1_start"];
    u_int16_t s1 = schTime.substring(0, 2).toInt();
    u_int16_t s2 = schTime.substring(3, 5).toInt();
    updated_sch1.startUnix = hourMinuteToUnixUTC(s1, s2);
    updated_sch1.stopUnix = updated_sch1.startUnix + (duration * 60);
    if (sch1.startUnix != updated_sch1.startUnix || sch1.stopUnix != updated_sch1.stopUnix) {
      sch1.startUnix = updated_sch1.startUnix;
      sch1.stopUnix = updated_sch1.stopUnix;
      sch1.active = 0;
      saveScheduleToEEPROM();
    }
  }
  https.end();
}

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
  https.end();
  unixTime = doc["unix_time"];
  return true;
}

time_t getRtcUnixTime() {
  DateTime now = rtc.now();
  Serial.print("RTC Time: "); Serial.println(now.unixtime());
  return now.unixtime();
}

void compareAndSyncTime() {
  if (!rtc.isrunning()) {
    Serial.println("RTC is NOT running, can't sync time.");
    return;
  }
  if (lastSync != 0 && millis() - lastSync < SYNC_INTERVAL) {
    Serial.println("Time sync not needed yet.");
    return;
  }
  time_t internetTime;
  if (!getInternetUnixTime(internetTime)) {
    Serial.println("Failed to get internet time.");
    return;
  }
  time_t rtcTime = getRtcUnixTime();
  long drift = abs((long)(internetTime - rtcTime));
  Serial.print("RTC drift: "); Serial.println(drift);
  if (drift > DRIFT_THRESHOLD) {
    rtc.adjust(DateTime(internetTime));
    Serial.println("RTC resynced");
  }
  lastSync = millis();
}

void process_LocalEvents() {
  processOTSensor();
  static int lastDay = -1;
  DateTime now = rtc.now();
  if (now.day() != lastDay) {
    appManualStop = 0;
    lastDay = now.day();
    Serial.println("New day – app cancellation reset");
  }
  // Manual button
  if (digitalRead(INPUT_BUTTON) == LOW) {
    Serial.println("Manual Button Pressed");
    delay(50); // debounce
    if (sch1.active == 1 && motorRunning) {
      // Cancel schedule
      motorOff();
      sch1.active = 0;
    }
    // Toggle motor
    if (!motorRunning) {
      motorOn();   // checks water
    } else {
      motorOff();
    }
    delay(200);
  }
  // Duration counting & auto stop
  if (motorRunning) {
    if (mili_now - lastExecutionTime >= EXECUTION_INTERVAL) {
      count += EXECUTION_INTERVAL / 1000;
      Serial.println("Count: " + String(count));
      if (count >= (sch1.duration * 60)) {
        motorOff();
        count = 0;
      }
    }
  }
  // Scheduled checks
  if (mili_now - lastExecutionTime >= EXECUTION_INTERVAL) {
    Serial.println("Checking Local Events...");
    lastExecutionTime = mili_now;
    checkSch(rtc.now().unixtime());
    DateTime dt(sch1.startUnix);
    Serial.print("Current: "); Serial.print(rtc.now().hour()); Serial.print(":"); Serial.print(rtc.now().minute()); Serial.print(":"); Serial.println(rtc.now().second());
    Serial.print("Schedule Start: "); Serial.print(dt.hour()); Serial.print(":"); Serial.println(dt.minute());
    if (authTimeout > 20) authTimeout -= 10;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi Disconnected - Reconnecting...");
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      authTimeout = 0;
      login_status = 0;
    }
    if (WiFi.status() == WL_CONNECTED) {
      if (lastSync == 0) compareAndSyncTime();
      if (login_status == 0) {
        if (isOnline()) {
          int n1 = login_email(USER_EMAIL, USER_PASS);
          login_status = 1;
        }
      }
      if (authTimeout <= 100) {
        login_status = 0;
      }
      getTableData(USER_TOKEN, "");
    }
  }
}

int _login_process() {
  int httpCode;
  JsonDocument doc;
  https.setTimeout(3000);
  if (https.begin(client, AUTH_URL)) {
    https.addHeader("apikey", SUPABASE_KEY);
    https.addHeader("Content-Type", "application/json");
    String query = "{\"email\": \"" + phone_or_email + "\", \"password\": \"" + password + "\"}";
    httpCode = https.POST(query);
    if (httpCode > 0) {
      String data = https.getString();
      deserializeJson(doc, data);
      if (doc.containsKey("access_token") && !doc["access_token"].isNull()) {
        USER_TOKEN = doc["access_token"].as<String>();
        authTimeout = doc["expires_in"].as<int>();
        Serial.println("Login Success");
      } else {
        Serial.println("Login Failed: Invalid token");
      }
    } else {
      Serial.print("Login Failed : "); Serial.println(httpCode);
    }
    https.end();
    loginTime = millis();
  } else {
    return -100;
  }
  return httpCode;
}

int login_email(String email_a, String password_a) {
  useAuth = true;
  phone_or_email = email_a;
  password = password_a;
  int httpCode = 0;
  while (httpCode <= 0) {
    httpCode = _login_process();
  }
  return httpCode;
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  pinMode(MOTOR_PIN, OUTPUT);        // kept for compatibility, not used for motor
  digitalWrite(MOTOR_PIN, LOW);
  pinMode(INPUT_BUTTON, INPUT_PULLUP);
  pinMode(OT_SENSOR_PIN, INPUT_PULLUP);
  pinMode(LOW_WATER_PIN, INPUT_PULLUP);
  pinMode(HIGH_WATER_PIN, INPUT_PULLUP);

  display.setBrightness(0x0f);
  display.clear();
  client.setInsecure();

  // Initialize RTC
  if (!rtc.begin()) {
    Serial.println("RTC Not Found");
    sch1.sch1_en = 0;
  } else {
    Serial.println("RTC Found");
  }

  // Initialize LoRa
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  LoRa.setSyncWord(0xA2);
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(62.5E3);
  while (!LoRa.begin(433E6)) {
    Serial.println("LoRa init failed, retrying...");
    delay(1000);
  }
  Serial.println("LoRa Initialized OK!");

  loadSchedules();
  loadScheduleFromEEPROM();
  motorRunning = false;  // assume motor off at start
}

// -------------------- Main Loop --------------------
void loop() {
  mili_now = millis();
  process_LocalEvents();
  delay(500);
}