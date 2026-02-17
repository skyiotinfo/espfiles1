#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <time.h>
#include <TM1637Display.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include "RTClib.h"
#include <ESP8266HTTPClient.h>

void compareAndSyncTime();
void updateTable(String token, int st, int ds);
void updateackTable(String token, int ack);
int login_email(String email_a, String password_a);
bool isOnline();

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

#define MOTOR_PIN D8
#define CLK_PIN D3
#define DIO_PIN D4
#define OT_SENSOR_PIN D7    // same as reference
#define OT_ACTIVE_LEVEL LOW // ot_sensorstatus == 0 in ref

int ot_sensorcount = 0;
const int OT_TRIP_COUNT = 5;

const int input1 = D9;
int temp_count1 = 0;

TM1637Display display(CLK_PIN, DIO_PIN);

int motor_status_manual = 0;

unsigned long mili_now;
unsigned long lastExecutionTime = 0;
long EXECUTION_INTERVAL = 10000; // Second in milliseconds
uint32_t lastSavedStartUnix = 0;
uint32_t lastSavedStopUnix = 0;
int count = 0;

struct Schedule
{
  time_t unixTime; // scheduled unix time
  bool triggered;
};

uint8_t scheduleCount = 3;

struct sch
{
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

char WIFI_SSID[20] = "sm42";
char WIFI_PASS[20] = "chai1111";
const char *supabase_device_url = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.211";
//char WIFI_SSID[20] = "Airtel_9764005401";
//char WIFI_PASS[20] = "air46403";
char SUPABASE_URL[100] = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
char AUTH_URL[100] = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/auth/v1/token?grant_type=password";
char SUPABASE_KEY[300] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";
char USER_EMAIL[30] = "9999900002@gmail.com";
char USER_PASS[10] = "1234";

const long DRIFT_THRESHOLD = 30; // rtc drift (difference) threshold in seconds

unsigned long lastSync = 0;
const unsigned long SYNC_INTERVAL = 1 * 60 * 60 * 1000UL; // 6 hours

// Function to load schedules from EEPROM or set defaults
void loadSchedules()
{
  sch1 = {
      110001, // device_id
      1767225600, // start time
      1767225600, // stop time
      0, // startTime
      0, // stopTime
      10, // duration in minutes
      0, // schedule active
      0, // state on/off
      0, // ack
      0, // sch1_en
      0, // updated_by
      0 // local_state
  };
}

// Function to process OT sensor and handle motor state
void processOTSensor()
{
  int ot_sensorstatus = digitalRead(OT_SENSOR_PIN);

  if (digitalRead(MOTOR_PIN) == HIGH)
  {
    if (ot_sensorstatus == OT_ACTIVE_LEVEL)
    {
      ot_sensorcount++;

      Serial.print("OT Sensor Count: ");
      Serial.println(ot_sensorcount);

      if (ot_sensorcount >= OT_TRIP_COUNT)
      {
        Serial.println("OT Sensor → MOTOR OFF");
        digitalWrite(MOTOR_PIN, LOW);
        sch1.active = 0;
        sch1.local_state = 1;
        if(isOnline()){
          updateTable(USER_TOKEN, 0, 0);
          updateackTable(USER_TOKEN, 0);
          sch1.local_state=0;
        }
        ot_sensorcount = 0;
      }
    }
    else
    {
      ot_sensorcount = 0;
    }
  }
  else
  {
    ot_sensorcount = 0;
  }
}

// Function to save schedule to EEPROM if changed
void saveScheduleToEEPROM()
{
  if (sch1.startUnix != lastSavedStartUnix ||
      sch1.stopUnix != lastSavedStopUnix)
  {

    EEPROM.put(EEPROM_START_UNIX_ADDR, sch1.startUnix);
    EEPROM.put(EEPROM_STOP_UNIX_ADDR, sch1.stopUnix);
    EEPROM.put(EEPROM_APP_MANUAL_STATUS, appManualStop);
    EEPROM.commit();

    lastSavedStartUnix = sch1.startUnix;
    lastSavedStopUnix = sch1.stopUnix;

    Serial.println("Schedule changed → saved to EEPROM");
  }
  else
  {
    Serial.println("Schedule unchanged → EEPROM not written");
  }
}

// Function to load schedule from EEPROM and validate
void loadScheduleFromEEPROM()
{
  EEPROM.get(EEPROM_START_UNIX_ADDR, sch1.startUnix);
  EEPROM.get(EEPROM_STOP_UNIX_ADDR, sch1.stopUnix);
  EEPROM.get(EEPROM_LOCAL_STATE, sch1.updated_by);
  EEPROM.get(EEPROM_APP_MANUAL_STATUS, appManualStop);

  if (sch1.startUnix < 1000000000 || sch1.stopUnix < sch1.startUnix)
  {
    Serial.println("Invalid EEPROM data, using defaults");
    loadSchedules();
    appManualStop = 0;
  }

  sch1.active = 0;
  sch1.state = 0;

  // Track last saved values
  lastSavedStartUnix = sch1.startUnix;
  lastSavedStopUnix = sch1.stopUnix;
}

// Function to convert hour and minute to unix time (UTC) for today
uint32_t hourMinuteToUnixUTC(uint8_t hour, uint8_t minute)
{
  DateTime now = rtc.now(); // assumed UTC

  DateTime dt(
      now.year(),
      now.month(),
      now.day(),
      hour,
      minute,
      0);

  return dt.unixtime();
}
int lastAppState = -1;

// Function to check and execute schedule
void checkSch(uint32_t nowUnix)
{
  // START schedule
  if (sch1.active == 0 &&
      nowUnix >= sch1.startUnix &&
      nowUnix < sch1.stopUnix &&
      sch1.sch1_en == 1 &&
      appManualStop == 0)  
  {
    sch1.active = 1;
    digitalWrite(MOTOR_PIN, HIGH);
    Serial.println("SCHEDULE START");
    sch1.local_state = 1;
    sch1.state = 1;
    if(isOnline()) {
      updateTable(USER_TOKEN, 1, 1);
      updateackTable(USER_TOKEN, 0);
      sch1.local_state = 0;
    }

  }

  // STOP schedule
  if (sch1.active == 1 && nowUnix >= sch1.stopUnix)
  {
    sch1.active = 0;
    digitalWrite(MOTOR_PIN, LOW);
    sch1.state = 0;
    sch1.local_state = 1;
    Serial.println("SCHEDULE STOP");
    if(isOnline()){
      updateTable(USER_TOKEN, 0, 0);
      appManualStop = 0;
      sch1.local_state=0;
    }
  
  }
}

// Function to check if the device is online
bool isOnline()
{
  if(WiFi.status() != WL_CONNECTED) {
    return false;
  } 

  if(WiFi.status() == WL_CONNECTED) {
    https.begin(client, "https://api.skyiottech.com/time");
    https.setTimeout(5000);

    int code = https.GET();
    if (code != 200)
    {
      Serial.println("Failed to connect to the internet");
      https.end();
      return false;
    }
    return true;
  }
  return false;
}

// Function to update the database table
void updateTable(String token, int st, int ds)
{
  https.begin(client, supabase_device_url);
  https.setTimeout(3000);

  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");
  String payload = "{\"state\": " + String(st) + ", \"device_state\": " + String(ds) + "}";

  int httpCode = https.sendRequest("PATCH", payload);

  Serial.print("HTTP Code: ");
  Serial.println(httpCode);

  https.end();
}

// Function to update the database table
void updateackTable(String token, int ack)
{
  https.begin(client, supabase_device_url);
  https.setTimeout(3000);

  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");
  String payload = "{\"ack\": " + String(ack) + "}";
  int httpCode = https.sendRequest("PATCH", payload);

  Serial.print("HTTP Code: ");
  Serial.println(httpCode);

  https.end();
}

void heartbeat(String token, int value)
{
  https.begin(client, supabase_device_url);
  https.setTimeout(3000);

  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");
  String payload = "{\"heart_beat_count\": " + String(value) + "}";
  int httpCode = https.sendRequest("PATCH", payload);

  Serial.print("HTTP Code: ");
  Serial.println(httpCode);

  https.end();
}

// Function to get data from the database table
void getTableData(String token, String field)
{
  https.begin(client, supabase_device_url);
  https.setTimeout(3000);
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");

  int httpCode = https.GET();
  Serial.print("HTTP Code: ");
  Serial.println(httpCode);

  if (httpCode == 200)
  {
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
    Serial.print("App State: ");
    Serial.println(updated_sch1.state);

    if (sch1.local_state == 1){
       Serial.print("Local state change → skipping app command");
       Serial.println(sch1.local_state);
        updateTable(USER_TOKEN, sch1.state, sch1.state);
        sch1.local_state = 0;
        return;
    }

    if (updated_sch1.state == 1 && digitalRead(MOTOR_PIN) == LOW && sch1.ack==1)
    {
        digitalWrite(MOTOR_PIN, HIGH);
        sch1.state = 1;
        count = 0;
        updateTable(USER_TOKEN, 1, 1);
        updateackTable(USER_TOKEN, 0);
        Serial.println("Motor ON from App");
        appManualStop = 0;
        EEPROM.put(EEPROM_APP_MANUAL_STATUS, appManualStop);
        EEPROM.commit();
    }
    
  
    if (updated_sch1.state == 0 && digitalRead(MOTOR_PIN) == HIGH && sch1.ack==1)
    {
        digitalWrite(MOTOR_PIN, LOW);
        updateTable(USER_TOKEN, 0, 0);
        updateackTable(USER_TOKEN, 0);
        Serial.println("Motor OFF from App");
        sch1.active = 0;
        sch1.state = 0;
    }

    String schTime = doc[0]["sch1_start"];
    u_int16_t s1 = schTime.substring(0, 2).toInt();
    u_int16_t s2 = schTime.substring(3, 5).toInt();
    updated_sch1.startUnix = hourMinuteToUnixUTC(s1, s2);
    updated_sch1.stopUnix = updated_sch1.startUnix + (duration * 60);

    if (sch1.startUnix != updated_sch1.startUnix || sch1.stopUnix != updated_sch1.stopUnix)
    {
      sch1.startUnix = updated_sch1.startUnix;
      sch1.stopUnix = updated_sch1.stopUnix;
      sch1.active = 0;
      saveScheduleToEEPROM();
    }
    
    Serial.println();
  }
  https.end();
}

// Function to get internet time
bool getInternetUnixTime(time_t &unixTime)
{
  if (!isOnline())
    return false;

  https.begin(client, "https://api.skyiottech.com/time");
  https.setTimeout(3000);

  int code = https.GET();
  if (code != 200)
  {
    Serial.println("Failed to get internet time");
    https.end();
    return false;
  }

  StaticJsonDocument<512> doc;
  deserializeJson(doc, https.getString());
  Serial.println(https.getString());
  https.end();

  unixTime = doc["unix_time"];
  // compareAndSyncTime(); anupam
  return true;
}

// Function to get RTC unix time
time_t getRtcUnixTime()
{
  DateTime now = rtc.now();
  Serial.print("RTC Time: ");
  Serial.print(now.unixtime());
  return now.unixtime();
}

// Function to compare and sync time
void compareAndSyncTime()
{
  if (!rtc.isrunning())
  {
    Serial.println("RTC is NOT running, can't sync time.");
    return;
  }
  // anupam
  if (lastSync != 0 && millis() - lastSync < SYNC_INTERVAL)
  {
    Serial.println("Time sync not needed yet.");
    return;
  }

  time_t internetTime;
  if (!getInternetUnixTime(internetTime))
  {
    Serial.println("Failed to get internet time.");
    return;
  }

  time_t rtcTime = getRtcUnixTime();
  long drift = abs((long)(internetTime - rtcTime));

  Serial.print("RTC drift: ");
  Serial.println(drift);

  if (drift > DRIFT_THRESHOLD)
  {
    rtc.adjust(DateTime(internetTime));
    Serial.println("RTC resynced");
  }

  lastSync = millis();
}

void process_LocalEvents()
{
  processOTSensor();
  static int lastDay = -1;
  DateTime now = rtc.now();

  if (now.day() != lastDay)
  {
    appManualStop = 0;  // Reset app manual stop on new day
    lastDay = now.day();
    Serial.println("New day – app cancellation reset");
  }

  // ===== MANUAL BUTTON OVERRIDE =====
  if (digitalRead(input1) == LOW)
  {
      // If schedule is running and motor is ON → cancel schedule
      Serial.println("Manual Button Pressed");
      if (sch1.active == 1 && digitalRead(MOTOR_PIN) == HIGH)
      {
        digitalWrite(MOTOR_PIN, LOW); 
        sch1.local_state = 1;
        sch1.state = 0;
        if(isOnline()) {
          updateTable(USER_TOKEN, 0, 0);
          sch1.local_state = 0;
        }
      }

      // Toggle motor
      if (sch1.active == 0 && digitalRead(MOTOR_PIN) == HIGH)
      {
        digitalWrite(MOTOR_PIN, LOW);
        sch1.local_state = 1;
        sch1.state = 0;
        Serial.println("Motor OFF by Button");
        if(isOnline()) {
          updateTable(USER_TOKEN, 0, 0);
          sch1.local_state = 0;
        }
        delay(200);
      }
      else if (sch1.active == 0 && digitalRead(MOTOR_PIN) == LOW )
      {
        digitalWrite(MOTOR_PIN, HIGH);
        count = 0;
        sch1.local_state = 1;
        sch1.state = 1;
        Serial.println("Motor ON by Button");
        if(isOnline()) {
          updateTable(USER_TOKEN, 1, 1);
          sch1.local_state = 0;
        }
        delay(200);
      }
  }

  if (mili_now - lastExecutionTime >= EXECUTION_INTERVAL)
  {
    
    if(digitalRead(MOTOR_PIN) == HIGH){
      count += EXECUTION_INTERVAL / 1000; 
      Serial.println("Count: " + String(count));
      if(count >= (sch1.duration * 60)) { 
        sch1.active = 0;
        sch1.state = 0;
        digitalWrite(MOTOR_PIN, LOW);
        if(isOnline()) {
              updateTable(USER_TOKEN, 0, 0);
              updateackTable(USER_TOKEN, 0);
        }
        count = 0;
      }
    }

    Serial.println("Checking Local Events...");
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
    Serial.print("Schedule Start Time: ");
    Serial.print(dt.hour());
    Serial.print(":");
    Serial.println(dt.minute());
    Serial.print("Auth Timeout in seconds: ");
    Serial.println(authTimeout);
    if (authTimeout > 20)
    {
      authTimeout -= 10;
    }
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("WiFi Disconnected - Reconnecting...");
      Serial.println("Reconnected to the internet");
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      authTimeout = 0;
      login_status = 0;
    }
    if (WiFi.status() == WL_CONNECTED)
    {
      if (lastSync == 0)
      {
        compareAndSyncTime();
      }
      if (login_status == 0)
      {
        if(isOnline()) {
          int n1 = login_email(USER_EMAIL, USER_PASS);
          Serial.print("Login Process HTTP Code: ");
          Serial.println(n1);
          login_status = 1;
        } 
      }
      if (authTimeout <= 100)
      {
        Serial.println("Auth Token Timeout...");
        login_status = 0;
      }
      getTableData(USER_TOKEN, "");
    }
  }
}

// Function to handle login process
int _login_process()
{
  int httpCode;
  JsonDocument doc;
  Serial.println("Beginning to login..");
  https.setTimeout(3000);

  if (https.begin(client, AUTH_URL))
  {
    https.addHeader("apikey", SUPABASE_KEY);
    https.addHeader("Content-Type", "application/json");
    String loginMethod = "email";

    String query = "{\"" + loginMethod + "\": \"" + phone_or_email + "\", \"password\": \"" + password + "\"}";
    httpCode = https.POST(query);

    if (httpCode > 0)
    {
      String data = https.getString();
      deserializeJson(doc, data);
      if (doc.containsKey("access_token") && !doc["access_token"].isNull() && doc["access_token"].is<String>() && !doc["access_token"].as<String>().isEmpty())
      {
        USER_TOKEN = doc["access_token"].as<String>();
        authTimeout = doc["expires_in"].as<int>();
        Serial.println("Login Success");
      }
      else
      {
        Serial.println("Login Failed: Invalid access token in response");
      }
    }
    else
    {
      Serial.println(phone_or_email);
      Serial.println(password);

      Serial.print("Login Failed : ");
      Serial.println(httpCode);
    }

    https.end();
    loginTime = millis();
  }
  else
  {
    return -100;
  }

  return httpCode;
}

// Function to login using email and password
int login_email(String email_a, String password_a)
{
  useAuth = true;
  loginMethod = "email";
  phone_or_email = email_a;
  password = password_a;

  int httpCode = 0;
  while (httpCode <= 0)
  {
    httpCode = _login_process();
  }
  return httpCode;
  https.end();
}

// Arduino setup function
void setup()
{
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  pinMode(input1, INPUT_PULLUP);
  pinMode(OT_SENSOR_PIN, INPUT_PULLUP);

  display.setBrightness(0x0f);
  display.clear();
  client.setInsecure();
  bool rtc_status = rtc.begin();
  delay(1000);
  if (rtc_status == true)
  {
    Serial.println("RTC Found and Set");
  }
  else
  {
    Serial.println("RTC Not Found");
    sch1.sch1_en = 0; 
  }
  loadSchedules();
  loadScheduleFromEEPROM();
}

// Arduino loop function
void loop()
{
  mili_now = millis();
  process_LocalEvents();
  delay(500);
}
