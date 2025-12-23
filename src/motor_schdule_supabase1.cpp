#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPSupabaseRealtime.h>
#include <ESPSupabase.h>
#include <EEPROM.h>
#include <time.h>
#include <TM1637Display.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include "RTClib.h"
#include <ESP8266httpClient.h>

RTC_DS1307 rtc;

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
Supabase db;
  

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
const char* TOKEN = "eyJhbGciOiJIUzI1NiIsImtpZCI6IkVEVFJka0dxODdpbVJwV2oiLCJ0eXAiOiJKV1QifQ.eyJpc3MiOiJodHRwczovL2ZrZ2ZkZ3dwcXFmeGhueXV3dHdlLnN1cGFiYXNlLmNvL2F1dGgvdjEiLCJzdWIiOiJjNmJjNjM4My00NmUyLTRkZjQtYTgxYS0zZDI1ZTc1ZDYzZTciLCJhdWQiOiJhdXRoZW50aWNhdGVkIiwiZXhwIjoxNzY2NDI3OTcxLCJpYXQiOjE3NjY0MjQzNzEsImVtYWlsIjoiMTIzNDU2Nzg5MEBnbWFpbC5jb20iLCJwaG9uZSI6IiIsImFwcF9tZXRhZGF0YSI6eyJwcm92aWRlciI6ImVtYWlsIiwicHJvdmlkZXJzIjpbImVtYWlsIl19LCJ1c2VyX21ldGFkYXRhIjp7ImVtYWlsX3ZlcmlmaWVkIjp0cnVlfSwicm9sZSI6ImF1dGhlbnRpY2F0ZWQiLCJhYWwiOiJhYWwxIiwiYW1yIjpbeyJtZXRob2QiOiJwYXNzd29yZCIsInRpbWVzdGFtcCI6MTc2NjQyNDM3MX1dLCJzZXNzaW9uX2lkIjoiZDg3YzgwMWEtZWI4Zi00ZGRmLTk4OTYtMGU1MzE5ODhlYTAyIiwiaXNfYW5vbnltb3VzIjpmYWxzZX0.hOG9jTyImzG9JbCO-L1qIAMIwP6Hwbbmgw7fWmg6VgY";
  

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

void updateTable(String token, int st){
  WiFiClientSecure client;
  client.setInsecure(); // skip SSL validation (OK for ESP)

  HTTPClient https;
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.167";
  client.setInsecure(); // skip SSL validation (OK for ESP)
  https.begin(client, supabaseUrl);

  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");

  StaticJsonDocument<16> doc;
  doc["state"] = st;  
 
  String payload;
  serializeJson(doc, payload);

  //String payload = R"({
  //  "state": st
  //})";

  int httpCode = https.sendRequest("PATCH", payload);

  Serial.print("HTTP Code: ");
  Serial.println(httpCode); 

  https.end();
}

void process_LocalEvents(){
  if (digitalRead(input1) == LOW && device_data.mstate == 0) {
    device_data.mstate = 1;
    device_data.manual_state = 1;
    StaticJsonDocument<16> doc; 
    digitalWrite(MOTOR_PIN, HIGH);
    Serial.println("Motor Turned ON via Local Button");
    realtime.end();
    updateTable(realtime.update_d(),1);
    delay(500);
    //db.begin(SUPABASE_URL, SUPABASE_KEY);
    //db.login_email(USER_EMAIL, USER_PASS);
    //int code = db.update("pump_motor").eq("id","167").doUpdate("{\"state\":1}");
    //Serial.println(code);
  }
  else if (digitalRead(input1) == LOW && device_data.mstate == 1) {
    device_data.mstate = 0;
    device_data.manual_state = 0;
    digitalWrite(MOTOR_PIN, LOW);
    Serial.println("Motor Turned OFF via Local Button");
    realtime.end();
    updateTable(realtime.update_d(),0);
    delay(500);
    //StaticJsonDocument<16> doc; 
    //db.begin(SUPABASE_URL, SUPABASE_KEY);
    //db.login_email(USER_EMAIL, USER_PASS);
    //int code = db.update("pump_motor").eq("id","167").doUpdate("{\"state\":0}");
    //Serial.println(code);
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
        if( ((ts1-ts2)/60 <= device_data.sch1_duration) && ((ts1-ts2) > 0) && digitalRead(MOTOR_PIN) != HIGH){
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
  device_data.manual_state = 0;
    String sch1=eeprom_read_data(20,30,"sch_data");
    String sch2=eeprom_read_data(40,30,"sch_data");
    String sch3=eeprom_read_data(60,30,"sch_data");
    Serial.print("Sch1 Data :");
    Serial.print(sch1);
    Serial.print("==");
    Serial.print("Sch2 Data :");
    Serial.print(sch2);
    Serial.print("==");
    Serial.print("Sch3 Data :");
    Serial.print(sch3);
    Serial.println("");
    device_data.hour=sch1.toInt();
    device_data.minute=sch1.substring(3,5).toInt();
    device_data.second=0;
    int s1_duration = eeprom_read_int(15,0,"s1_duration");
    device_data.sch1_duration=s1_duration;
    Serial.print("Sch1 Duration :");
    Serial.println(device_data.sch1_duration);
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
      EEPROM.put(15, duration);
      EEPROM.commit();
      Serial.println("Written String: " + data1);
    }

    if(write_data == "sch2_update") {
      for (int i = 0; i < data1.length(); i++){
        EEPROM.write(40 + i, data1[i]);
      }
      EEPROM.write(40 + data1.length(), '\0');
      EEPROM.commit();
      EEPROM.put(16, duration);
      EEPROM.commit();
      Serial.println("Written String: " + data1);
    }

    if(write_data == "sch3_update") {
      for (int i = 0; i < data1.length(); i++){
        EEPROM.write(60 + i, data1[i]);
      }
      EEPROM.write(60 + data1.length(), '\0');
      EEPROM.commit();
      EEPROM.put(17, duration);
      EEPROM.commit();
      Serial.println("Written String: " + data1);
    }

    return "d1";
}

String eeprom_read_data(int addr, int duration, String read_data) {
    String val1="";

    if(read_data == "sch_data") {
      String sdata = "";
      char ch;
      while (true) {
        ch = EEPROM.read(saddress++);
        if (ch == '\0') break;
        sdata += ch;
      }
      return sdata;
    }
    return "999";
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

    if(read_data == "s2_on_status") {
      EEPROM.get(12, val1);
      return val1;
    }

    if(read_data == "s3_on_status") {
      EEPROM.get(13, val1);
      return val1;
    }

    if(read_data == "s1_duration") {
      EEPROM.get(15, val1);
      return val1;
    }

    if(read_data == "s1_duration") {
      EEPROM.get(16, val1);
      return val1;
    }

    if(read_data == "s1_duration") {
      EEPROM.get(17, val1);
      return val1;
    }

    return 999;
}

// line 481 - This is referance code .....
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
        device_data.manual_state=1;
        digitalWrite(MOTOR_PIN, HIGH);
      }else{
        device_data.mstate=0;
        device_data.manual_state=0;
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
  bool rtc_status = rtc.begin();
  delay(2000);
  if(rtc_status==true){
    //rtc.adjust(DateTime(2000, 1, 1, 12, 0, 0));
    Serial.println("RTC Found and Set");
  }else{
    Serial.println("RTC Not Found");
  }
  init_devicedata();

}

void loop() {
  now = millis();
  process_LocalEvents();
  realtime.loop();
  delay(200);
}
