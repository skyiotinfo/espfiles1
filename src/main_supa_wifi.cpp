#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>
#include <TM1637Display.h>
#include <Wire.h>
#include "RTClib.h"
#include <ESP8266HTTPClient.h>
#include <espnow.h>
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif

void compareAndSyncTime();
int login_email(String email_a, String password_a);
void updateTable(String token, int st, int ds);
void updateackTable(String token, int ack);
void updateTable1(String token, int st1, int ds1);
void updateackTable1(String token, int ack1);
void heartbeat(String token, int value);
void getTableData(String token, String field);
void getTableData1(String token, String field);
bool getInternetUnixTime(time_t &unixTime);
time_t getRtcUnixTime();

RTC_DS1307 rtc;


#define CLK_PIN   D3
#define DIO_PIN   D4


TM1637Display display(CLK_PIN, DIO_PIN);


unsigned long mili_now;
unsigned long lastExecutionTime = 0;
unsigned long lastSync = 0;
const unsigned long EXECUTION_INTERVAL = 10000; 
unsigned long lastEspNowSend = 0;
const unsigned long ESPNOW_INTERVAL = 4000;
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 30000;
const unsigned long SYNC_INTERVAL = 1 * 60 * 60 * 1000UL; 
volatile bool httpBusy = false;

int prevMotor1State = -1;
int prevMotor1Ack = -1;
int prevMotor1SchEn = -1;
String prevMotor1Start = "";
uint32_t prevMotor1Duration = 0;

int prevMotor2State = -1;
int prevMotor2Ack = -1;
int prevMotor2SchEn = -1;
String prevMotor2Start = "";
uint32_t prevMotor2Duration = 0;



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


char WIFI_SSID[20] = "Anupam";
char WIFI_PASS[20] = "12345678";
char SUPABASE_URL[100] = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
char AUTH_URL[100] = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/auth/v1/token?grant_type=password";
char SUPABASE_KEY[300] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";
char USER_EMAIL[30] = "9999900005@gmail.com";
char USER_PASS[10] = "123456";


const long DRIFT_THRESHOLD = 30;


int lastAppState = -1;
int lastAppState1 = -1;


struct MotorData {
  uint32_t sch1_duration;
  int state;
  int ack;
  int sch1_en;
  String sch1_start;
};
volatile bool newFeedback = false;



MotorData motor1Data;
MotorData motor2Data;


uint8_t wifiMacAddress[] = {0x84, 0x0D, 0x8E, 0xB8, 0x47, 0xB9}; // Device 2 MAC
uint8_t partnerMacAddress[] = {0x8C, 0x4F, 0x00, 0xE1, 0xE0, 0x75}; // Device 1 MAC

typedef struct esp_now_command {
  uint32_t currentUnixTime;
  int motor1AppState;
  uint32_t motor1StartUnix;
  uint32_t motor1StopUnix;
  int motor1Ack;
  int motor1SchEn;
  int motor2AppState;
  uint32_t motor2StartUnix;
  uint32_t motor2StopUnix;
  int motor2Ack;
  int motor2SchEn;
} esp_now_command;

typedef struct esp_now_feedback {
  int motor1PhysicalState;
  int motor1DeviceState;
  uint8_t motor1Cancellation;
  int motor2State;
  int motor2DeviceState;
  uint8_t motor2Cancellation;
  int heartBeatCount;
} esp_now_feedback;

esp_now_command commandData;
esp_now_feedback receivedFeedback;

void initESP_NOW();
void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len);
void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus);
void sendCommandToDevice1();
void processFeedback();


void initESP_NOW() {
WiFi.mode(WIFI_STA);
WiFi.disconnect();   

if (esp_now_init() != 0) {
  Serial.println("ESP-NOW initialization failed");
  return;
}

esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
esp_now_register_recv_cb(OnDataRecv);
esp_now_register_send_cb(OnDataSent);

esp_now_add_peer(partnerMacAddress, ESP_NOW_ROLE_COMBO, 1, NULL, 0);

Serial.println("ESP-NOW Initialized");

}

void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  if (len == sizeof(receivedFeedback)) {
      memcpy(&receivedFeedback, incomingData, sizeof(receivedFeedback));
      newFeedback = true;   
  }
}

void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
  if (sendStatus == 0) {
    Serial.println("ESP-NOW Send success");
  } else {
    Serial.println("ESP-NOW Send failed");
  }
}


uint32_t hourMinuteToUnixUTC(uint8_t hour, uint8_t minute) {
  DateTime now = rtc.now();
  
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

void sendCommandToDevice1() {
  DateTime now = rtc.now();
  commandData.currentUnixTime = now.unixtime();
  
  commandData.motor1AppState = motor1Data.state;
  commandData.motor1Ack = motor1Data.ack;
  commandData.motor1SchEn = motor1Data.sch1_en;
  
  commandData.motor2AppState = motor2Data.state;
  commandData.motor2Ack = motor2Data.ack;
  commandData.motor2SchEn = motor2Data.sch1_en;
  
  if (motor1Data.sch1_start.length() >= 5) {
    uint16_t hour = motor1Data.sch1_start.substring(0, 2).toInt();
    uint16_t minute = motor1Data.sch1_start.substring(3, 5).toInt();
    commandData.motor1StartUnix = hourMinuteToUnixUTC(hour, minute);
    commandData.motor1StopUnix = commandData.motor1StartUnix + (motor1Data.sch1_duration * 60);
  }
  
  if (motor2Data.sch1_start.length() >= 5) {
    uint16_t hour = motor2Data.sch1_start.substring(0, 2).toInt();
    uint16_t minute = motor2Data.sch1_start.substring(3, 5).toInt();
    commandData.motor2StartUnix = hourMinuteToUnixUTC(hour, minute);
    commandData.motor2StopUnix = commandData.motor2StartUnix + (motor2Data.sch1_duration * 60);
  }
  
  esp_now_send(partnerMacAddress, (uint8_t *)&commandData, sizeof(commandData));
  Serial.println("Command sent to Device 1");
}

void processFeedback() {
  if (WiFi.status() == WL_CONNECTED && login_status == 1) {
    updateTable(USER_TOKEN, receivedFeedback.motor1PhysicalState, receivedFeedback.motor1DeviceState);
    
    updateTable1(USER_TOKEN, receivedFeedback.motor2State, receivedFeedback.motor2DeviceState);
    
    if (receivedFeedback.motor1Cancellation) {
      updateackTable(USER_TOKEN, 0);
    }
    
    if (receivedFeedback.motor2Cancellation) {
      updateackTable1(USER_TOKEN, 0);
    }
    
  }
}



bool isOnline() {
  return WiFi.status() == WL_CONNECTED;
}


void updateTable(String token, int st, int ds) {
    httpBusy = true;
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.244";
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
  httpBusy = false;
}

void updateackTable(String token, int ack) {
    httpBusy = true;
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.244";
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
  httpBusy = false;
}

void updateTable1(String token, int st1, int ds1) {
    httpBusy = true;
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.309";
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
  httpBusy = false;
}

void updateackTable1(String token, int ack1) {
    httpBusy = true;
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.309";
  https.begin(client, supabaseUrl);
  https.setTimeout(3000);
  
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(token));
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");
  
  String payload = "{\"ack\": " + String(ack1) + "}";
  https.sendRequest("PATCH", payload);
  https.end();
  httpBusy = false;
}

void heartbeat(String token, int value) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Heartbeat skipped (WiFi not connected)");
    return;
  }
httpBusy = true;
  String payload = "{\"heart_beat_count\": " + String(value) + "}";

  const char* url1 = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.244";

  https.begin(client, url1);
  https.setTimeout(3000);
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + token);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");

  int httpCode1 = https.sendRequest("PATCH", payload);
  Serial.print("Heartbeat ID 244 HTTP Code: ");
  Serial.println(httpCode1);

  https.end();
  

  const char* url2 = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.309";

  https.begin(client, url2);
  https.setTimeout(3000);
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + token);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");

  int httpCode2 = https.sendRequest("PATCH", payload);
  Serial.print("Heartbeat ID 309 HTTP Code: ");
  Serial.println(httpCode2);

  https.end();
  httpBusy = false;
}


void getTableData(String token, String field) {
    httpBusy = true;
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.244";
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

    motor1Data.sch1_duration = doc[0]["sch1_duration"];
    motor1Data.state = doc[0]["state"];
    motor1Data.ack = doc[0]["ack"];
    motor1Data.sch1_en = doc[0]["sch1_en"];
    motor1Data.sch1_start = doc[0]["sch1_start"].as<String>();
    
    Serial.print("Motor1 State: "); Serial.println(motor1Data.state);
    Serial.print("Motor1 Schedule: "); Serial.println(motor1Data.sch1_start);
  }
  https.end();
  httpBusy = false;

}

void getTableData1(String token, String field) {
    httpBusy = true;
  const char* supabaseUrl = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/pump_motor?id=eq.309";
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

    motor2Data.sch1_duration = doc[0]["sch1_duration"];
    motor2Data.state = doc[0]["state"];
    motor2Data.ack = doc[0]["ack"];
    motor2Data.sch1_en = doc[0]["sch1_en"];
    motor2Data.sch1_start = doc[0]["sch1_start"].as<String>();
    
    Serial.print("Motor2 State: "); Serial.println(motor2Data.state);
    Serial.print("Motor2 Schedule: "); Serial.println(motor2Data.sch1_start);
  }
  https.end();
  httpBusy = false;
}


bool getInternetUnixTime(time_t &unixTime) {
  if (!isOnline()) return false;
  httpBusy = true;
  https.begin(client, "https://api.skyiottech.com/time");
  https.setTimeout(3000);
  
  int code = https.GET();
  if (code != 200) {
    Serial.println("Failed to get internet time");
    https.end();
    httpBusy = false;
    return false;
  }
  
  StaticJsonDocument<512> doc;
  deserializeJson(doc, https.getString());
  Serial.println(https.getString());
  https.end();
  httpBusy = false;
  unixTime = doc["unix_time"];
  return true;
}

time_t getRtcUnixTime() {
  DateTime now = rtc.now();
  Serial.print("RTC Time: ");  
  Serial.print(now.unixtime());
  return now.unixtime();
}

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

int login_email(String email_a, String password_a) {
  useAuth = true;
  loginMethod = "email";
  phone_or_email = email_a;
  password = password_a;
  return _login_process();
}


void processMainEvents() {
    Serial.println("\n=== Processing Main Events ===");
    
    DateTime now = rtc.now();
    
    display.showNumberDecEx(now.hour() * 100 + now.minute(), 0b01000000, true);
    
    Serial.print("Current Time: ");
    Serial.print(now.hour());
    Serial.print(":");
    Serial.print(now.minute());
    Serial.print(":");
    Serial.print(now.second());
    Serial.println();
    
    if (WiFi.status() != WL_CONNECTED) {  
      Serial.println("WiFi Disconnected - Reconnecting...");
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      authTimeout = 0;
      login_status = 0;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      if (millis() - lastSync > SYNC_INTERVAL) {
        compareAndSyncTime();
    }

      
      if (login_status == 0) {
        int n1 = login_email(USER_EMAIL, USER_PASS);
        Serial.print("Login Process HTTP Code: ");
        Serial.println(n1);
        if (n1 > 0) {
            login_status = 1;
        }  
    }   
      
      if (authTimeout <= 100) {
        Serial.println("Auth Token Timeout...");
        login_status = 0;
      }
      
      if (login_status == 1) {
        getTableData(USER_TOKEN, "");
        getTableData1(USER_TOKEN, "");
        

      }


      bool changed = false;

if (motor1Data.state != prevMotor1State ||
    motor1Data.ack != prevMotor1Ack ||
    motor1Data.sch1_en != prevMotor1SchEn ||
    motor1Data.sch1_start != prevMotor1Start ||
    motor1Data.sch1_duration != prevMotor1Duration) {

    changed = true;

    prevMotor1State = motor1Data.state;
    prevMotor1Ack = motor1Data.ack;
    prevMotor1SchEn = motor1Data.sch1_en;
    prevMotor1Start = motor1Data.sch1_start;
    prevMotor1Duration = motor1Data.sch1_duration;
}

if (motor2Data.state != prevMotor2State ||
    motor2Data.ack != prevMotor2Ack ||
    motor2Data.sch1_en != prevMotor2SchEn ||
    motor2Data.sch1_start != prevMotor2Start ||
    motor2Data.sch1_duration != prevMotor2Duration) {

    changed = true;

    prevMotor2State = motor2Data.state;
    prevMotor2Ack = motor2Data.ack;
    prevMotor2SchEn = motor2Data.sch1_en;
    prevMotor2Start = motor2Data.sch1_start;
    prevMotor2Duration = motor2Data.sch1_duration;
}

if (changed && !httpBusy) {
    sendCommandToDevice1();
}


    }
  }



void setup() {
  Serial.begin(115200);
  
  display.setBrightness(0x0f);
  display.clear();
  
  Wire.begin(D2, D1); 
  bool rtc_status = rtc.begin();
  delay(1000);
  
  if (rtc_status == true) {
    Serial.println("RTC Found");
  } else {
    Serial.println("RTC Not Found - Check connections");
  }
  
  initESP_NOW();
  
  commandData.motor1AppState = 0;
  commandData.motor1StartUnix = 0;
  commandData.motor1StopUnix = 0;
  commandData.motor1Ack = 0;
  commandData.motor1SchEn = 0;
  commandData.motor2AppState = 0;
  commandData.motor2StartUnix = 0;
  commandData.motor2StopUnix = 0;
  commandData.motor2Ack = 0;
  commandData.motor2SchEn = 0;
  
  client.setInsecure();
  
  Serial.println("Device 2 (WiFi+RTC+Display) setup complete");
}

void loop() {

  unsigned long now = millis();

  if (newFeedback) {
    newFeedback = false;
    processFeedback();
  }

if (now - lastHeartbeat >= HEARTBEAT_INTERVAL && !httpBusy) {
    lastHeartbeat = now;
    heartbeat(USER_TOKEN, 10);
}


  if (now - lastExecutionTime >= EXECUTION_INTERVAL && !httpBusy) {
    lastExecutionTime = now;
    processMainEvents();
  }

  delay(10);
}   
