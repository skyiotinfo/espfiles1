#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <time.h>
#include <TM1637Display.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include "RTClib.h"
#include <ESP8266httpClient.h>

void compareAndSyncTime();
void updateTable(String token, int st);
int login_process(String email, String pass);
int login_email(String email_a, String password_a);
int login_process();

const int device_id = 10100101;

RTC_DS1307 rtc;

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif

#define EEPROM_SIZE 128

#define MOTOR_PIN D8
#define CLK_PIN   D3
#define DIO_PIN   D4

const int auto_status = D7;
const int input1 = D9;
int temp_count1 = 0;

TM1637Display display(CLK_PIN, DIO_PIN);

unsigned long mili_now;
unsigned long lastExecutionTime = 0;
const unsigned long EXECUTION_INTERVAL = 10000; // Second in milliseconds

struct Schedule {
  time_t unixTime;   // scheduled unix time
  bool triggered;
};

uint8_t scheduleCount = 3;

struct sch {
  uint32_t startUnix;   // when to turn ON
  uint32_t stopUnix;    // when to turn OFF
  uint8_t startTime;
  uint8_t stopTime;
  bool active;          // runtime state
  int state;
};

sch sch1;
sch updated_sch1;
sch sch2;
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

//char WIFI_SSID[20] = "sm42";
//char WIFI_PASS[20] = "chai1111";
char WIFI_SSID[20] = "Airtel_9764005401";
char WIFI_PASS[20] = "air46403";
char SUPABASE_URL[100] = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
char AUTH_URL[100] = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/auth/v1/token?grant_type=password";
char SUPABASE_KEY[300] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";
char USER_EMAIL[30] = "9999900002@gmail.com";
char USER_PASS[10]= "1234";

const long DRIFT_THRESHOLD = 30;  // rtc drift (difference) threshold in seconds
unsigned long lastSync = 0;
const unsigned long SYNC_INTERVAL = 1 * 60 * 60 * 1000UL; // 6 hours

// Function to load schedules from EEPROM or set defaults
void loadSchedules() {
  sch1 = {
    1767225600,  // start time
    1767225600,  // stop time
    0,
    0,
    false,
    0
    };
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
  
    if(sch1.state == 1 && digitalRead(MOTOR_PIN) == LOW){
      digitalWrite(MOTOR_PIN, HIGH);
      updateTable(USER_TOKEN, 1);
    } 

    if(sch1.state == 0 && digitalRead(MOTOR_PIN) == HIGH){
      digitalWrite(MOTOR_PIN, LOW);
      updateTable(USER_TOKEN, 0);
    }

    // START condition
    if (!sch1.active && nowUnix >= sch1.startUnix && nowUnix < sch1.stopUnix) {
      sch1.active = true;
      Serial.println();
      Serial.println("SCHEDULE START");
      digitalWrite(MOTOR_PIN, HIGH);
      sch1.state = 1;
      updateTable(USER_TOKEN, 1);
      // turn motor ON
    }

    // STOP condition
    if (sch1.active && nowUnix >= sch1.stopUnix) {
      sch1.active = false;
      Serial.println();
      Serial.println("SCHEDULE STOP");
      digitalWrite(MOTOR_PIN, LOW);
      sch1.state = 0;
      updateTable(USER_TOKEN, 0);
      // turn motor OFF
    }
}

// Function to check if the device is online
bool isOnline() {
  return WiFi.status() == WL_CONNECTED;
}

// Function to update the database table
void updateTable(String token, int st){
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.211";
  https.begin(client, supabaseUrl);
  https.setTimeout(3000);

  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");

  String payload = "{\"state\": " + String(st) + "}";

  int httpCode = https.sendRequest("PATCH", payload);

  Serial.print("HTTP Code: ");
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

    int httpCode=https.GET();
    Serial.print("HTTP Code: ");
    Serial.println(httpCode);
    if (httpCode == 200) {
        StaticJsonDocument<512> doc;
        deserializeJson(doc, https.getString());
        uint32_t duration = doc[0]["sch1_duration"];
        updated_sch1.state = doc[0]["state"];
        String schTime = doc[0]["sch1_start"];
        u_int16_t s1 = schTime.substring(0, 2).toInt();
        u_int16_t s2 = schTime.substring(3, 5).toInt();
        updated_sch1.startUnix = hourMinuteToUnixUTC(s1, s2);
        updated_sch1.stopUnix = updated_sch1.startUnix + (duration * 60);
        
        sch1.startUnix = updated_sch1.startUnix;
        sch1.stopUnix = updated_sch1.stopUnix;  
        sch1.state = updated_sch1.state;        
        https.end();
        Serial.println();
    }
    https.end();
}

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
  compareAndSyncTime();
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

  if (millis() - lastSync < SYNC_INTERVAL){
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

// Function to process local events
void process_LocalEvents(){    
  // Scheduled Task Execution - Runs every EXECUTION_INTERVAL

  if(digitalRead(input1) == LOW){
      digitalWrite(OUTPUT, HIGH);

  } 
  
  if (mili_now - lastExecutionTime >= EXECUTION_INTERVAL) {
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
    if(authTimeout>20){
      authTimeout -= 10;
    }
    if(WiFi.status() != WL_CONNECTED)
    {   
      Serial.println("WiFi Disconnected - Reconnecting...");
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      authTimeout = 0;
      login_status=0;
    }
    if(WiFi.status() == WL_CONNECTED){
        if(login_status==0){
          if(getInternetUnixTime(internetTime)==true){
            int n1=login_email(USER_EMAIL, USER_PASS);
            Serial.print("Login Process HTTP Code: "); 
            Serial.println(n1);
            login_status=1;
          }
        }   
        if(authTimeout <= 100){
            Serial.println("Auth Token Timeout...");
            login_status=0;
        }
        if(getInternetUnixTime(internetTime)==true){
          getTableData(USER_TOKEN, "");
        }
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
void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  pinMode(input1, INPUT_PULLUP);
  pinMode(auto_status, OUTPUT);
  display.setBrightness(0x0f);
  display.clear();
  client.setInsecure();
  bool rtc_status = rtc.begin();
  delay(1000);
  if(rtc_status==true){
    Serial.println("RTC Found and Set");
  }else{
    Serial.println("RTC Not Found");
  }
  loadSchedules();
}

// Arduino loop function
void loop() {
  mili_now = millis();
  process_LocalEvents();
  delay(500);
}
