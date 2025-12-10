#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPSupabaseRealtime.h>
#include <EEPROM.h>
#include <time.h>
#include <TM1637Display.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include "RTClib.h"
#include <ESP8266httpClient.h>

RTC_DS1307 rtc;
WiFiClientSecure client;
HTTPClient https;

const char* gettime_url = "https://gzcpvuueeexndnvwoanw.supabase.co/functions/v1/bright-function/time";

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif

#define EEPROM_SIZE 128

#define MOTOR_PIN D8
#define CLK_PIN   D3
#define DIO_PIN   D4

const int ot_sensor = D1;
const int ot_status = D2;
const int auto_status = D7;
const int input1 = D9;
void GetTime();
void connectSupabase();
void displayIdle();
TM1637Display display(CLK_PIN, DIO_PIN);
String dt_payload = "";
DateTime rt1;
//PZEM004Tv30 pzem1(D2, D5);
int saddress = 20;
String read_eeprom_data(int addr, int duration, String read_data);

float zeroIfNan(float v) { if (isnan(v)) v = 0; return v; }
float VOLTAGE, CURRENT, POWER;
unsigned long lastVoltageRead = 0;
const unsigned long VOLTAGE_READ_INTERVAL = 2000;

SupabaseRealtime realtime;

unsigned long lastCheck = 0;
bool motorRunning = false;
unsigned long motorStartTime = 0;
unsigned long motorRunDuration = 0;

unsigned long scheduledRemainingMs = 0;
bool scheduleInProgress = false;

const int MANUAL_ADDR = 100;        
const int SCHEDULE_MAGIC_ADDR = 200; 
const int SCHEDULE_REMAIN_ADDR = 201; 
const int SCHEDULE_FLAG_ADDR = 205;  
const byte SCHEDULE_MAGIC = 0x42;

int manual_duration = 30;
bool manualStopRequested = false;

const unsigned long SCHEDULE_SAVE_INTERVAL_MS = 5000;
unsigned long lastScheduleSaveMs = 0;

const char* WIFI_SSID = "sm42";
const char* WIFI_PASS = "chai1111";

 
const char* SUPABASE_URL = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";
const char* USER_EMAIL = "1234567890@gmail.com";
const char* USER_PASS = "1234";
  

bool wifiWasConnected = false;
bool supabaseConnected = false;
unsigned long lastWifiAttempt = 0;

bool updateRTCfromJSON(const String& jsonStr) 
{
  StaticJsonDocument<256> doc;

  // Parse JSON
  DeserializationError error = deserializeJson(doc, jsonStr);
  if (error) {
    Serial.println("JSON Parse Failed!");
    return false;
  }

  // Extract datetime string
  String datetime = doc["datetime"];  
  // Format: "24/11/2025, 08:01:18"

  // Split into date & time
  int commaIndex = datetime.indexOf(',');
  String datePart = datetime.substring(0, commaIndex);         // "24/11/2025"
  String timePart = datetime.substring(commaIndex + 2);        // "08:01:18"

  // Extract day, month, year
  int day   = datePart.substring(0, 2).toInt();
  int month = datePart.substring(3, 5).toInt();
  int year  = datePart.substring(6, 10).toInt();

  // Extract hour, minute, second
  int hour   = timePart.substring(0, 2).toInt();
  int minute = timePart.substring(3, 5).toInt();
  int second = timePart.substring(6, 8).toInt();

  Serial.println("Extracted Date & Time:");
  Serial.printf("%02d/%02d/%04d %02d:%02d:%02d\n", day, month, year, hour, minute, second);

  // -------------------------
  // Set DS1307 RTC Time
  // -------------------------
  if(rtc.begin()==true){
    rtc.adjust(DateTime(year, month, day, hour, minute, second));
    return true;
  }else{
    Serial.println("RTC Not Found");
    return false;
  }
}

struct PumpMotorData {
  int mstate;
  int manual_duration;
  int sch1_en, sch2_en, sch3_en;
  char sch1_start[6];
  char sch2_start[6];
  char sch3_start[6];
  int sch1_duration;
  int sch2_duration;
  int sch3_duration;
};

PumpMotorData motorData;

void init_motordata(){
  motorData.mstate=0;
  motorData.manual_duration=0;
  motorData.sch1_en=0;
  motorData.sch2_en=0;
  motorData.sch3_en=0;   
  motorData.sch1_duration=30;
  motorData.sch2_duration=30;
  motorData.sch3_duration=30;
  strcpy(motorData.sch1_start,"08:00");
  strcpy(motorData.sch2_start,"08:00");
  strcpy(motorData.sch3_start,"08:00");
}

String write_eeprom_data(String data1, int duration, String write_data) {
    if(write_data == "m_on") {
      EEPROM.put(0, 1);
      EEPROM.commit();
    }
    if(write_data == "m_off") {
      EEPROM.put(0, 0);
      EEPROM.commit();
    }

    if(write_data == "auto_on") {
      EEPROM.put(1, 1);
      EEPROM.commit();
    }
    if(write_data == "auto_off") {
      EEPROM.put(1, 0);
      EEPROM.commit();
    }

    if(write_data == "m_duration") {
      EEPROM.put(3, duration);
      EEPROM.commit();
    }

    if(write_data == "s1_on") {
      EEPROM.put(11, 1);
      EEPROM.commit();
    }
    if(write_data == "s1_off") {
      EEPROM.put(11, 0);
      EEPROM.commit();
    }

    if(write_data == "s2_on") {
      EEPROM.put(12, 1);
      EEPROM.commit();
    }
    if(write_data == "s2_off") {
      EEPROM.put(12, 0);
      EEPROM.commit();
    }

    if(write_data == "s3_on") {
      EEPROM.put(13, 1);
      EEPROM.commit();
    }
    if(write_data == "s3_off") {
      EEPROM.put(13, 0);
      EEPROM.commit();
    }

    if(write_data == "sch1_update") {
      for (int i = 0; i < data1.length(); i++){
        EEPROM.write(20 + i, data1[i]);
        EEPROM.write(20 + data1.length(), '\0');
        EEPROM.commit();
        Serial.println("Written String: " + data1);
      }
      EEPROM.put(3, duration);
      EEPROM.commit();
    }
    return "d1";
}

String read_eeprom_data(int addr, int duration, String read_data) {
    String val1="";
    if(read_data == "m_on_status") {
      EEPROM.get(1, val1);
      return val1;
    }

    if(read_data == "auto_on_status") {
      EEPROM.get(1, val1);
      return val1;
    }

    if(read_data == "m_duration_status") {
      EEPROM.get(3, val1);
      return val1;
    }

    if(read_data == "s1_on_status") {
      EEPROM.get(11, val1);
      return val1;
    }

    if(read_data == "s1_off_status") {
      EEPROM.get(11, val1);
      return val1;
    }

    if(read_data == "s2_on_status") {
      EEPROM.get(12, val1);
      return val1;
    }
    if(read_data == "s2_off_status") {
      EEPROM.get(12, val1);
      return val1;
    }

    if(read_data == "s3_on_status") {
      EEPROM.get(13, val1);
      return val1;
    }

    if(read_data == "s3_off_status") {
      EEPROM.get(13, val1);
      return val1;
    }

    if(read_data == "sch1_data") {
      String sdata = "";
      char ch;
      while (true) {
        ch = EEPROM.read(saddress++);
        if (ch == '\0') break;
        sdata += ch;
      }
      return sdata;
    }
    return "d1";
}

void displayTankFull() {
  uint8_t FF_segments[] = {
    SEG_A | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_E | SEG_F | SEG_G,
    0, 0
  };
  display.setSegments(FF_segments);
}

void displayIdle() { display.showNumberDec(0, true); }

void displayMotorRunning() {
  if (!motorRunning) return displayIdle();

  unsigned long elapsedMs = millis() - motorStartTime;
  unsigned long elapsedMin = elapsedMs / 60000UL;
  unsigned long remainingMin = (motorRunDuration + 59999UL) / 60000UL; 
  if (motorRunDuration > elapsedMs) {
    unsigned long remMs = motorRunDuration - elapsedMs;
    remainingMin = (remMs + 59999UL) / 60000UL;
  } else {
    remainingMin = 0;
  }
  if (remainingMin > 99) remainingMin = 99;
  display.showNumberDec(remainingMin, true);
}

void startMotorForDuration(unsigned long durationMs) {
  manualStopRequested = false;
  motorRunning = true;
  motorStartTime = millis();
  motorRunDuration = durationMs;
  digitalWrite(MOTOR_PIN, HIGH);
  displayMotorRunning();
  motorData.mstate = true;
  write_eeprom_data("1", 1, "m_on");
}

void stopMotorImmediate() {
  motorRunning = false;
  motorRunDuration = 0;
  digitalWrite(MOTOR_PIN, LOW);
  displayIdle();
}

void handleMotorRun() {
  if (manualStopRequested) {
    if (motorRunning) stopMotorImmediate();
    manualStopRequested = false;
    return;
  }

  if (motorRunning) {
    unsigned long elapsedMs = millis() - motorStartTime;

    if (scheduleInProgress) {
      if (motorRunDuration > elapsedMs)
        scheduledRemainingMs = motorRunDuration - elapsedMs;
      else
        scheduledRemainingMs = 0;

      if (millis() - lastScheduleSaveMs >= SCHEDULE_SAVE_INTERVAL_MS) {
      }
    }

    if (elapsedMs >= motorRunDuration) {
      stopMotorImmediate();
      motorData.mstate = false;

      scheduleInProgress = false;
      scheduledRemainingMs = 0;
    }
  }
}

void HandleLocalChanges(String result) {
  if (result.length() < 6) return;

  StaticJsonDocument<8192> doc;
  DeserializationError err = deserializeJson(doc, result);
  if (err) {
    Serial.print("JSON parse failed: ");
    Serial.println(err.c_str());
    return;
  }

  if (!doc.containsKey("table") || !doc.containsKey("record")) return;
  JsonObject record = doc["record"].as<JsonObject>();

  motorData.mstate = record["state"].as<bool>();
  motorData.sch1_en = record["sch1_en"].as<bool>();
  motorData.sch2_en = record["sch2_en"].as<bool>();
  motorData.sch3_en = record["sch3_en"].as<bool>();

  strlcpy(motorData.sch1_start, record["sch1_start"] | "", 6);
  strlcpy(motorData.sch2_start, record["sch2_start"] | "", 6);
  strlcpy(motorData.sch3_start, record["sch3_start"] | "", 6);

  motorData.sch1_duration = record["sch1_duration"] | 0;
  motorData.sch2_duration = record["sch2_duration"] | 0;
  motorData.sch3_duration = record["sch3_duration"] | 0;



  if (motorData.mstate && !motorRunning) {

    digitalWrite(MOTOR_PIN, HIGH);
  } else if (!motorData.mstate && !motorRunning) {
    digitalWrite(MOTOR_PIN, LOW);
  }

  Serial.print(rt1.day());
  Serial.print("/");
  Serial.print(rt1.month());
  Serial.print("/");
  Serial.print(rt1.year());
  Serial.print("  ");

  Serial.print(rt1.hour());
  Serial.print(":");
  Serial.print(rt1.minute());
  Serial.print(":");
  Serial.println(rt1.second());
}

bool connectWiFiBlocking() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(300);
    Serial.print(".");
  }
  return WiFi.status() == WL_CONNECTED;
}

void wifiReconnectNonBlocking() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      GetTime();
      connectSupabase();
    }
    return;
  }

  wifiWasConnected = false;
  supabaseConnected = false;

  if (millis() - lastWifiAttempt > 10000) {
    lastWifiAttempt = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void GetTime() {
    client.setInsecure();
    https.begin(client, gettime_url);
    // Add your headers
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Imd6Y3B2dXVlZWV4bmRudndvYW53Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjE1NzA2OTIsImV4cCI6MjA3NzE0NjY5Mn0.lW_6KKWeUF1l7qq4RAQvJsAmrdQetLenL5O8LYH62Ek");
    int httpCode = https.GET();
    Serial.print("Time Response");
    Serial.println(httpCode);
    dt_payload = https.getString();
    updateRTCfromJSON(dt_payload);
    Serial.println("RAW:");
    Serial.println(dt_payload);
}

void connectSupabase() {
  Serial.println("Connecting to Supabase...");
  if (WiFi.status() != WL_CONNECTED) return;
  realtime.begin(SUPABASE_URL, SUPABASE_KEY, HandleLocalChanges);
  realtime.login_email(USER_EMAIL, USER_PASS);
  realtime.addChangesListener("pump_motor", "*", "public", "");
  realtime.listen();
  supabaseConnected = true;
}

void Init_Localtimer() {
  Wire.begin(D6, D5);   // SDA, SCL for NodeMCU

  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  if (!rtc.isrunning()) {
    Serial.println("RTC not running, setting time...");
    rtc.adjust(DateTime(2025, 11, 22, 15, 30, 00)); // Set time from your PC
  }
} 

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  pinMode(input1, INPUT_PULLUP);
  pinMode(ot_sensor, INPUT_PULLUP);
  pinMode(ot_status, OUTPUT);
  pinMode(auto_status, OUTPUT);
  display.setBrightness(0x0f);
  display.clear();

  if (connectWiFiBlocking()) {
    wifiWasConnected = true;
    GetTime();
    //connectSupabase();
  }

  /*
  
  if (scheduleInProgress && scheduledRemainingMs > 0) {
    if (digitalRead(ot_sensor) == HIGH) {
      startMotorForDuration(scheduledRemainingMs);
      saveScheduledState();
    } else {
      Serial.println("Persisted scheduled run found but OT sensor is LOW - not starting.");
    }
  } */
}

void loop() {
  unsigned long now = millis();
  if(rtc.begin()==true){
    rt1 = rtc.now();
  }
  if (supabaseConnected && WiFi.status() == WL_CONNECTED)
  //realtime.loop();
  //handleMotorRun();
  //wifiReconnectNonBlocking();
  int ot_sensorstatus = digitalRead(ot_sensor);
  int buttonState = digitalRead(input1);
  /*
  if (ot_sensorstatus == LOW) displayTankFull();
  else if (motorRunning) displayMotorRunning();
  else displayIdle();
  if (ot_sensorstatus == LOW) {
    if (motorRunning || motorData.state) {
      int lowCount = 0;
      for (int i = 0; i < 10; i++) {
        if (digitalRead(ot_sensor) == LOW) lowCount++;
        delay(500);
      }

      if (lowCount >= 5) {
        stopMotorImmediate();
        motorData.state = false;
        saveMotorToEEPROM();
        scheduleInProgress = false;
        scheduledRemainingMs = 0;
        clearScheduledState(); 
      }
    }
  }

  if (buttonState == LOW) {
    delay(50);
    while (digitalRead(input1) == LOW) delay(20);

    if (digitalRead(ot_sensor) == HIGH) {
      motorData.state = !motorData.state;

      if (motorData.state && !motorRunning) {
        scheduleInProgress = false;
        scheduledRemainingMs = 0;
        clearScheduledState(); 
        startMotorForDuration((unsigned long)manual_duration * 60000UL);
      } else if (!motorData.state) {
        manualStopRequested = true;
        stopMotorImmediate();
      }

      saveMotorToEEPROM();
    }
  }

  digitalWrite(ot_status, ot_sensorstatus == LOW ? HIGH : LOW);

  if (millis() - lastCheck > 10000) {
    lastCheck = millis();

    time_t nowt = time(nullptr);
    struct tm *ti = localtime(&nowt);
    char currentTime[6];
    snprintf(currentTime, 6, "%02d:%02d", ti->tm_hour, ti->tm_min);

    if (!motorRunning && ot_sensorstatus == HIGH) {

      if (motorData.sch1_en && timeMatches(motorData.sch1_start, currentTime)) {
        scheduledRemainingMs = (unsigned long)motorData.sch1_duration * 60000UL;
        scheduleInProgress = true;
        startMotorForDuration(scheduledRemainingMs);
        saveScheduledState();
      }

      else if (motorData.sch2_en && timeMatches(motorData.sch2_start, currentTime)) {
        scheduledRemainingMs = (unsigned long)motorData.sch2_duration * 60000UL;
        scheduleInProgress = true;
        startMotorForDuration(scheduledRemainingMs);
        saveScheduledState();
      }

      else if (motorData.sch3_en && timeMatches(motorData.sch3_start, currentTime)) {
        scheduledRemainingMs = (unsigned long)motorData.sch3_duration * 60000UL;
        scheduleInProgress = true;
        startMotorForDuration(scheduledRemainingMs);
        saveScheduledState();
      }
    }
  }

  */
  Serial.println("....------.");
  delay(200);
}
