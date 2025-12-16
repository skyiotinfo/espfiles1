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

const char* gettime_url = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/functions/v1/bright-function/time";

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
String eeprom_readString(int address);
TM1637Display display(CLK_PIN, DIO_PIN);
String dt_payload = "";
DateTime rt1;
//PZEM004Tv30 pzem1(D2, D5);
int saddress = 20;
String eeprom_read_data(int addr, int duration, String read_data);
int eeprom_read_int(int addr, int duration, String read_data);

float zeroIfNan(float v) { if (isnan(v)) v = 0; return v; }
float VOLTAGE, CURRENT, POWER;
int current_hour = 12;
int current_minute = 20;
unsigned long lastVoltageRead = 0;
const unsigned long VOLTAGE_READ_INTERVAL = 2000;
unsigned long lastExecutionTime = 0;
const unsigned long EXECUTION_INTERVAL = 20000; // 20 Second in milliseconds
unsigned long now;

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

int motor_duration = 30;
bool manualStopRequested = false;

const unsigned long SCHEDULE_SAVE_INTERVAL_MS = 5000;
unsigned long lastScheduleSaveMs = 0;

//const char* WIFI_SSID = "sm42";
//const char* WIFI_PASS = "chai1111";

const char* WIFI_SSID = "Airtel_9764005401";
const char* WIFI_PASS = "air46403";

 
const char* SUPABASE_URL = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";
const char* USER_EMAIL = "1234567890@gmail.com";
const char* USER_PASS = "1234";
  

bool wifiWasConnected = false;
bool supabaseConnected = false;
unsigned long lastWifiAttempt = 0;

void init_time() {
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for time sync");
  time_t now;
  while ((now = time(nullptr)) < 100000) {
    delay(100);
    Serial.print(".");
  }

  // Convert to DateTime
  struct tm* t = localtime(&now);
  DateTime ntpTime(
    t->tm_year + 1900,
    t->tm_mon + 1,
    t->tm_mday,
    t->tm_hour,
    t->tm_min,
    t->tm_sec
  );

  // Update DS1307
  Serial.println("\nDS1307 updated from NTP!");
  if(rtc.begin()==true){
    rtc.adjust(ntpTime);
    char yearStr[4];
    sprintf(yearStr, "%04d", rtc.now().year());
    Serial.println(yearStr);
    char monthStr[2];
    sprintf(monthStr, "%02d", rtc.now().day());
    Serial.println(monthStr);
    char dayStr[2];
    sprintf(dayStr, "%02d", rtc.now().second());
    Serial.println(dayStr);
  }else{
    Serial.println("RTC Not Found");
  }
}

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

struct c_serverdata {
  int mstate;
  int motor_duration;
  int m_auto;
  int sch1_en, sch2_en, sch3_en;
  char sch1_start[10];
  char sch2_start[10];
  char sch3_start[10];
  int day;
  int month;
  int year;
  int hour;
  int minute;
  int sch1_duration;
  int sch2_duration;
  int sch3_duration;
};

struct d_data {
  int mstate;
  int motor_duration;
  int m_auto;
  int sch1_en, sch2_en, sch3_en;
  char sch1_start[20];
  char sch2_start[20];
  char sch3_start[20];
  int day;
  int month;
  int year;
  int hour;
  int minute;
  int second;
  int sch1_duration;
  int sch2_duration;
  int sch3_duration;
  int manual_state;
};

d_data device_data;
c_serverdata server_data;

time_t toUnix(
  int year, int month, int day,
  int hour, int minute, int second
) {
  struct tm t;
  t.tm_year = year - 1900; // years since 1900
  t.tm_mon  = month - 1;   // 0–11
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min  = minute;
  t.tm_sec  = second;
  t.tm_isdst = 0;
  return mktime(&t);
}

void process_localevents(){
  if (digitalRead(input1) == LOW && device_data.mstate == 0) {
    device_data.mstate = 1;
    device_data.manual_state = 1;
    digitalWrite(MOTOR_PIN, HIGH);
    Serial.println("Motor Turned ON via Local Button");
  }
  else if (digitalRead(input1) == LOW && device_data.mstate == 1) {
    device_data.mstate = 0;
    device_data.manual_state = 0;
    digitalWrite(MOTOR_PIN, LOW);
    Serial.println("Motor Turned OFF via Local Button");
  }

  if (now - lastExecutionTime >= EXECUTION_INTERVAL) {
    lastExecutionTime = now;

    if(WiFi.status() != WL_CONNECTED)
    {
      supabaseConnected=false;
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    if(WiFi.status() == WL_CONNECTED){
      if(supabaseConnected==false){
          connectSupabase();
          init_time();
      }

    }

    Serial.println("Checking scheduled tasks...");
    Serial.println("Current Time: " + String(rtc.now().hour()) + ":" + String(rtc.now().minute()));
    Serial.println("Scheduled Time: " + String(device_data.hour) + ":" + String(device_data.minute));
    if(rtc.isrunning()){
        time_t ts1 = toUnix(rtc.now().year(), rtc.now().month(), rtc.now().day(), rtc.now().hour(), rtc.now().minute(), rtc.now().second());
        Serial.println("RTC is running. Current Unix Time: " + String(ts1));
        time_t ts2 = toUnix(rtc.now().year(), rtc.now().month(), rtc.now().day(), device_data.hour, device_data.minute, 0);
        time_t ts3 = toUnix(2025, 12, 15, 10, 50, 55);
        Serial.println(ts1-ts2);  // Unix timestamp0
        Serial.println(ts1-ts3);  // Unix timestamp
        Serial.println(device_data.sch1_duration);
        if( ((ts1-ts2)/60 <= device_data.sch1_duration) && ((ts1-ts2)/60 > 0) && digitalRead(MOTOR_PIN) != HIGH){
          device_data.mstate = 1;  
          digitalWrite(MOTOR_PIN, HIGH);
          Serial.println("Motor Turned ON via Schedule 1");
        }

        if( ((ts1-ts2)/60 >= device_data.sch1_duration) && digitalRead(MOTOR_PIN) == HIGH){
          if(device_data.manual_state!=1){
            device_data.mstate = 0;  
            digitalWrite(MOTOR_PIN, LOW);
            Serial.println("Motor Turned OFF - Schedule 1 Ended or Not Time Yet");
          }else{
            Serial.println("Device Manually Running..");
          }
        }
    }
  }
}

void init_devicedata(){
  device_data.mstate=0;
  device_data.m_auto=0;
  device_data.motor_duration=0;
  device_data.sch1_en=0;
  device_data.sch2_en=0;
  device_data.sch3_en=0;   
  device_data.sch1_duration=10;
  device_data.sch2_duration=10;
  device_data.sch3_duration=10;
  device_data.day=0;
  device_data.month=0;
  device_data.year=0;
  device_data.hour=0;
  device_data.minute=0;
  device_data.second=0;
    String s=eeprom_read_data(20,30,"sch1_data");
    Serial.println(s);
    Serial.println(s.substring(0,2));
    device_data.hour=s.toInt();
    Serial.println(s.substring(3,5));
    device_data.minute=s.substring(3,5).toInt();
    device_data.second=0;
    int s1 = eeprom_read_int(3,0,"m_duration");
    device_data.sch1_duration=s1;
  String sch1_start_str = String(device_data.sch1_start);
  Serial.println("Sch1 Start String: " + sch1_start_str);
  Serial.println(s1);
  device_data.manual_state = 0;
}

void init_serverdata(){
  server_data.mstate=0;
  server_data.m_auto=0;
  server_data.motor_duration=0;
  server_data.sch1_en=0;
  server_data.sch2_en=0;
  server_data.sch3_en=0;   
  server_data.sch1_duration=30;
  server_data.sch2_duration=30;
  server_data.sch3_duration=30;
  strcpy(server_data.sch1_start,"0700");
  strcpy(server_data.sch2_start,"0000");
  strcpy(server_data.sch3_start,"0000");

  EEPROM.get(0, server_data.mstate);
  EEPROM.get(1, server_data.m_auto);
  EEPROM.get(3, server_data.motor_duration);  
  EEPROM.get(11, server_data.sch1_en);
  EEPROM.get(12, server_data.sch2_en);
  EEPROM.get(13, server_data.sch3_en);
}

String eeprom_write_data(String data1, int duration, String write_data) {
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
      }
      EEPROM.write(20 + data1.length(), '\0');
      EEPROM.commit();
      Serial.println("Written String: " + data1);
      EEPROM.put(3, duration);
      EEPROM.commit();
    }

    if(write_data == "sch2_update") {
      for (int i = 0; i < data1.length(); i++){
        EEPROM.write(40 + i, data1[i]);
      }
      EEPROM.write(40 + data1.length(), '\0');
      EEPROM.commit();
      Serial.println("Written String: " + data1);
      EEPROM.put(4, duration);
      EEPROM.commit();
    }

    if(write_data == "sch3_update") {
      for (int i = 0; i < data1.length(); i++){
        EEPROM.write(60 + i, data1[i]);
      }
      EEPROM.write(60 + data1.length(), '\0');
      EEPROM.commit();
      Serial.println("Written String: " + data1);
      EEPROM.put(5, duration);
      EEPROM.commit();
    }

    return "d1";
}

String eeprom_read_data(int addr, int duration, String read_data) {
    String val1="";
    if(read_data == "m_on_status") {
      EEPROM.get(1, val1);
      return val1;
    }

    if(read_data == "auto_on_status") {
      EEPROM.get(1, val1);
      return val1;
    }

    if(read_data == "m_duration") {
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

int eeprom_read_int(int addr, int duration, String read_data) {
    int val1=0;
    if(read_data == "m_on_status") {
      EEPROM.get(1, val1);
      return val1;
    }

    if(read_data == "auto_on_status") {
      EEPROM.get(1, val1);
      return val1;
    }

    if(read_data == "m_duration") {
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
    return 10;
}

String eeprom_readString(int address) {
  String data = "";
  char ch;
  while (true) {
    ch = EEPROM.read(address++);
    if (ch == '\0') break;
    data += ch;
  }
  return data;
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
  device_data.mstate = 1;
  eeprom_write_data("1", 1, "m_on");
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
      device_data.mstate = 0;

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

  device_data.mstate = record["state"].as<bool>();
  device_data.sch1_en = record["sch1_en"].as<bool>();
  device_data.sch2_en = record["sch2_en"].as<bool>();
  device_data.sch3_en = record["sch3_en"].as<bool>();

  strlcpy(device_data.sch1_start, record["sch1_start"] | "", 6);
  strlcpy(device_data.sch2_start, record["sch2_start"] | "", 6);
  strlcpy(device_data.sch3_start, record["sch3_start"] | "", 6);

  device_data.sch1_duration = record["sch1_duration"] | 0;
  device_data.sch2_duration = record["sch2_duration"] | 0;
  device_data.sch3_duration = record["sch3_duration"] | 0;



  if (device_data.mstate && !motorRunning) {

    digitalWrite(MOTOR_PIN, HIGH);
  } else if (!device_data.mstate && !motorRunning) {
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

void process_RemoteEvents(String result)
{
  JsonDocument doc;
  deserializeJson(doc, result);

  String tableName = doc["table"];
  String event = doc["type"];
  JsonObject record = doc["record"];
  String changes = doc["record"];

  Serial.println("====================");
  Serial.print("Table: ");
  Serial.println(tableName);
  Serial.print("Event: ");
  Serial.println(event);

  if (tableName == "pump_motor") {
    Serial.print("Motor State: ");
    Serial.println(record["state"].as<int>());
    int state = record["state"].as<int>();
    if (state != device_data.mstate){
      if (device_data.mstate==0){
        device_data.mstate=1;
        digitalWrite(MOTOR_PIN, HIGH);
      }else{
        device_data.mstate=0;
        digitalWrite(MOTOR_PIN, LOW);
      }
    }
    Serial.print("Motor Duration: ");
    Serial.println(record["sch1_duration"].as<int>());
    Serial.print("Schedule 1 Enabled: ");
    Serial.println(record["sch1_en"].as<bool>());
    Serial.print("Schedule 1 Start: ");
    int d = record["sch1_duration"].as<int>();
    String s = record["sch1_start"].as<const char*>();
    eeprom_write_data(s,d,"sch1_update");
    Serial.println(s);
    Serial.println(s.substring(0,2));
    device_data.hour=s.toInt();
    Serial.println(s.substring(3,5));
    device_data.minute=s.substring(3,5).toInt();
    device_data.second=0;
    device_data.sch1_duration=d;
    Serial.print("Duration:");
    Serial.println(d);
    Serial.print("Schedule 2 Enabled: ");
    Serial.println(record["sch2_en"].as<bool>());
    Serial.print("Schedule 2 Start: ");
    Serial.println(record["sch2_end"].as<const char*>());
    Serial.println("--------------------");
  }
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
  int tcount=0;
  
  if(WiFi.status() != WL_CONNECTED){
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED)
    {
      delay(200);
      Serial.print(".");
      if(tcount>=60){
        return;
      }
    }
    connectSupabase();
    Serial.println("\nConnected!");
  }

}

void GetTime() {
    client.setInsecure();
    https.begin(client, gettime_url);
    // Add your headers
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU");
    int httpCode = https.GET();
    Serial.print("Response Code: ");
    if(httpCode==200){
      Serial.println(httpCode);
      dt_payload = https.getString();
      updateRTCfromJSON(dt_payload);
      Serial.println("RAW:");
      Serial.println(dt_payload);
      } else {
      Serial.println("Error on HTTP request");  
    }
    https.end();
}

void connectSupabase() {
  Serial.println("Connecting to Supabase...");
  if (WiFi.status() != WL_CONNECTED) return;
  if(supabaseConnected==false)
  {
    realtime.begin(SUPABASE_URL, SUPABASE_KEY, process_RemoteEvents);
    realtime.login_email(USER_EMAIL, USER_PASS);
    realtime.addChangesListener("pump_motor", "*", "public", "");
    realtime.listen();
    supabaseConnected = true;
  }

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
  if(rtc.begin()==true){
    rtc.adjust(DateTime(2000, 1, 1, 12, 0, 0));
    Serial.println("RTC Found and Set");
  }else{
    Serial.println("RTC Not Found");
  }
  init_devicedata();
  Serial.println(eeprom_readString(20));

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
  now = millis();
  process_localevents();
  realtime.loop();
  Serial.println("....------.");
  Serial.println(".....");
  delay(200);
}
