#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TM1637Display.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>
#include <DS1307RTC.h>
#include <TimeLib.h>

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
#include <FirebaseClient.h>
#include "ExampleFunctions.h"

// ===== Pins =====
#define CLK D3
#define DIO D4
#define OT_SENSOR D1
#define BUZZER D8
#define OT_STATUS D6
#define AUTO_STATUS D7
#define BUTTON D9

// ===== EEPROM for WiFi and Motor Schedule =====
#define EEPROM_SIZE 512
#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64
char ssid[MAX_SSID_LEN + 1];
char password[MAX_PASS_LEN + 1];

// ===== Firebase =====
#define API_KEY "AIzaSyBXcj6BMqeYVYmwKc37GU66qPaUSGbM7Tg"
#define DATABASE_URL "https://skyiot2-default-rtdb.firebaseio.com"
#define USER_EMAIL "anupam@gmail.com"
#define USER_PASSWORD "123456"

SSL_CLIENT ssl_client, stream_ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client), streamClient(stream_ssl_client);

UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD, 3000);
FirebaseApp app;
RealtimeDatabase Database;
AsyncResult streamResult;

String uid, streamPath;

// ===== Display =====
TM1637Display display(CLK, DIO);
uint8_t blank[] = {0x00, 0x00, 0x00, 0x00};

// ===== Motor & Sensor =====
int motor_status = 0, motor_time = 0, motor_duration = 30;
int ot_sensorstatus = 1;
int ot_sensorcount = 0;
bool manualMode = false;
int currentScheduleIndex = -1;
bool scheduleCompleted[3] = {false,false,false};

// ===== PZEM =====
PZEM004Tv30 pzem1(4, 14); 
float VOLTAGE=0, CURRENT=0, POWER=0;

// ===== NTP =====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800);
unsigned long lastNtpUpdate = 0;

// ===== Boot Safe Flag =====
bool bootCompleted = false;

// ===== Function Prototypes =====
void saveCredentials(const char* ssid, const char* password);
void loadCredentials();
void connectWiFi();
void initFirebase();
void readSchedule(int index,int &hour,int &min,int &duration);
int findMatchingSchedule(int h,int m);
void processData(AsyncResult &aResult);
void updateMotorStatusInFirebase(int status);
void sendHeartbeat();
void sendPZEMData();
float zeroIfNan(float v);
tmElements_t getTimeFromRTC();

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);

  // ===== Pins =====
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);
  pinMode(OT_STATUS, OUTPUT);
  pinMode(AUTO_STATUS, OUTPUT);
  pinMode(OT_SENSOR, INPUT_PULLUP);

  // ===== Display =====
  display.setBrightness(0x0f);
  display.setSegments(blank);

  // ===== Load WiFi =====
  loadCredentials();
  if (strlen(ssid)==0 || strlen(password)==0 || digitalRead(BUTTON)==LOW){
    WiFiManager wifiManager;
    wifiManager.setSaveConfigCallback([](){
      saveCredentials(WiFi.SSID().c_str(), WiFi.psk().c_str());
    });
    wifiManager.startConfigPortal("SetupWiFi","admin123");
    loadCredentials();
  }

  connectWiFi();

  // ===== Time =====
  timeClient.begin();
  timeClient.update();
  lastNtpUpdate = millis();

  // ===== Initialize Motor OFF =====
  motor_status = 0;
  motor_time = motor_duration * 60;
  digitalWrite(AUTO_STATUS, LOW);
  digitalWrite(BUZZER, LOW);
  display.showNumberDec(0, false);

  delay(500); // allow heap stabilization

  // ===== Firebase =====
  initFirebase();
}

unsigned long lastHeartbeat=0, lastPZEMSend=0;

void loop() {
  app.loop();

  // ===== Sensors =====
  ot_sensorstatus = digitalRead(OT_SENSOR);

  // ===== Update time =====
  if (!timeClient.update()) {
      if (millis() - lastNtpUpdate > 5000) {
          tmElements_t rtcTime = getTimeFromRTC();
          setTime(rtcTime.Hour, rtcTime.Minute, rtcTime.Second,
                  rtcTime.Day, rtcTime.Month, tmYearToCalendar(rtcTime.Year));
      }
  } else {
      lastNtpUpdate = millis();
      time_t epoch = timeClient.getEpochTime();
      tmElements_t tm;
      breakTime(epoch, tm);
      setTime(tm.Hour, tm.Minute, tm.Second, tm.Day, tm.Month, tmYearToCalendar(tm.Year));
  }

  int currentHour = hour();
  int currentMin = minute();
  int currentSec = currentHour*3600 + currentMin*60;

  // ===== Manual Button =====
  static bool lastButtonState = HIGH;
  bool currButtonState = digitalRead(BUTTON);
  if(lastButtonState==HIGH && currButtonState==LOW){
    if(motor_status==0){
      motor_status = 1;
      motor_time = motor_duration*60;
      digitalWrite(BUZZER,HIGH); digitalWrite(AUTO_STATUS,HIGH);
      currentScheduleIndex=-1;
      updateMotorStatusInFirebase(1);
    } else if(motor_status==1 && currentScheduleIndex==-1){
      motor_status=0;
      digitalWrite(BUZZER,LOW); digitalWrite(AUTO_STATUS,LOW);
      display.showNumberDec(0,false);
      motor_time=motor_duration*60;
      updateMotorStatusInFirebase(0);
    }
  }
  lastButtonState = currButtonState;

  // ===== Motor scheduling =====
  if(bootCompleted){  // Prevent motor start immediately after boot
    int matched = manualMode?-1:findMatchingSchedule(currentHour,currentMin);
    if(!manualMode && motor_status==0 && matched!=-1 && !scheduleCompleted[matched]){
      int h,m,d; readSchedule(matched,h,m,d);
      int remaining = (h*3600+m*60+d*60)-currentSec;
      if(remaining>0){
        motor_time = remaining;
        motor_status = 1;
        digitalWrite(BUZZER,HIGH); digitalWrite(AUTO_STATUS,HIGH);
        currentScheduleIndex = matched;
        scheduleCompleted[matched] = true;
        updateMotorStatusInFirebase(1);
      }
    }
  } else {
    bootCompleted = true; // First loop after boot completed
  }

  // ===== Motor handling =====
  if(motor_status==1){
    int remMin = motor_time/60, remSec = motor_time%60;
    display.showNumberDecEx(remMin*100 + remSec, 0b01000000);
    delay(1000);
    motor_time--;

    if(ot_sensorstatus==0){
      ot_sensorcount++;
      if(ot_sensorcount>=5){
        motor_status=0; ot_sensorcount=0;
        digitalWrite(BUZZER,LOW); digitalWrite(AUTO_STATUS,LOW);
        display.showNumberDec(0,false);
        if(currentScheduleIndex!=-1) scheduleCompleted[currentScheduleIndex]=true;
        updateMotorStatusInFirebase(0);
      }
    } else ot_sensorcount=0;

    if(motor_time<=0){
      motor_status=0;
      digitalWrite(BUZZER,LOW); digitalWrite(AUTO_STATUS,LOW);
      display.showNumberDec(0,false);
      if(currentScheduleIndex!=-1) scheduleCompleted[currentScheduleIndex]=true;
      updateMotorStatusInFirebase(0);
    }
  }

  // ===== Heartbeat =====
  if(millis()-lastHeartbeat>=30000){
    sendHeartbeat();
    lastHeartbeat=millis();
  }

  // ===== PZEM =====
  if(millis()-lastPZEMSend>=30000){
    VOLTAGE = zeroIfNan(pzem1.voltage());
    CURRENT = zeroIfNan(pzem1.current());
    POWER = zeroIfNan(pzem1.power());
    sendPZEMData();
    lastPZEMSend=millis();
  }
}

// ===== Functions =====
void saveCredentials(const char* ssid, const char* password){
  EEPROM.begin(EEPROM_SIZE);
  for(int i=0;i<EEPROM_SIZE;i++) EEPROM.write(i,0);
  for(int i=0; i<MAX_SSID_LEN && ssid[i]; i++) EEPROM.write(i,ssid[i]);
  for(int i=0; i<MAX_PASS_LEN && password[i]; i++) EEPROM.write(MAX_SSID_LEN+i,password[i]);
  EEPROM.commit(); EEPROM.end();
}

void loadCredentials(){
  EEPROM.begin(EEPROM_SIZE);
  for(int i=0;i<MAX_SSID_LEN;i++) ssid[i]=EEPROM.read(i);
  ssid[MAX_SSID_LEN]='\0';
  for(int i=0;i<MAX_PASS_LEN;i++) password[i]=EEPROM.read(MAX_SSID_LEN+i);
  password[MAX_PASS_LEN]='\0';
  EEPROM.end();
}

void connectWiFi(){
  WiFi.begin(ssid,password);
  unsigned long start=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<15000){ delay(500); Serial.print("."); }
  if(WiFi.status()==WL_CONNECTED) Serial.println("\nWiFi Connected!");
  else { Serial.println("\nFailed to connect. Rebooting..."); delay(2000); ESP.restart(); }
}
void initFirebase(){
  Firebase.printf("Firebase Client v%s\n",FIREBASE_CLIENT_VERSION);

  set_ssl_client_insecure_and_buffer(ssl_client);
  set_ssl_client_insecure_and_buffer(stream_ssl_client);

  initializeApp(aClient, app, getAuth(user_auth), 20*1000, auth_debug_print);
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  uid = app.getUid().c_str();
  streamPath = "/board1/schedule/" + uid;
}

void readSchedule(int index,int &hour,int &min,int &duration){
  int base=10*index;
  hour = EEPROM.read(base);
  min = EEPROM.read(base+1);
  duration = EEPROM.read(base+2);
  if(duration<1||duration>100) duration=30;
}

int findMatchingSchedule(int h,int m){
  int nowSec = h*3600+m*60;
  for(int i=0;i<3;i++){
    int sh,sm,d;
    readSchedule(i,sh,sm,d);
    int startSec = sh*3600+sm*60;
    int endSec = startSec+d*60;
    if(nowSec>=startSec && nowSec<endSec) return i;
  }
  return -1;
}

void processData(AsyncResult &aResult){
  if(!aResult.isResult()) return;
  if(aResult.available()){
    RealtimeDatabaseResult &stream = aResult.to<RealtimeDatabaseResult>();
    if(stream.isStream()){
      String eventPath = stream.dataPath().c_str();
      if(eventPath.endsWith("/motor_status_node")){
        int status = stream.to<int>();
        if(status==1 && motor_status==0){
          motor_status=1; motor_time=motor_duration*60;
          digitalWrite(BUZZER,HIGH); digitalWrite(AUTO_STATUS,HIGH);
          currentScheduleIndex=-1;
        } else if(status==0 && motor_status==1){
          motor_status=0; digitalWrite(BUZZER,LOW); digitalWrite(AUTO_STATUS,LOW);
          display.showNumberDec(0,false); motor_time=motor_duration*60;
        }
      }
    }
  }
}

void updateMotorStatusInFirebase(int status){
  if(WiFi.status()!=WL_CONNECTED) return;
  object_t json; JsonWriter writer; writer.create(json,"motor_status_node",status);
  Database.set<object_t>(aClient, streamPath+"/motor_status_node", json, processData, "setTask");
}

void sendHeartbeat(){
  if(WiFi.status()!=WL_CONNECTED) return;
  object_t json; JsonWriter writer; writer.create(json,"last_active",(long)timeClient.getEpochTime());
  Database.set<object_t>(aClient,"/board1/heartbeat/"+uid,json,processData,"heartbeatTask");
}

void sendPZEMData(){
  if(WiFi.status()!=WL_CONNECTED) return;
  object_t json; JsonWriter writer;
  writer.create(json,"voltage",VOLTAGE);
  writer.create(json,"current",CURRENT);
  writer.create(json,"power",POWER);
  Database.set<object_t>(aClient,"/board1/pzem/"+uid,json,processData,"pzemTask");
}

float zeroIfNan(float v){ return isnan(v)?0:v; }

tmElements_t getTimeFromRTC(){
  tmElements_t tm;
  if(!RTC.read(tm)){
    Serial.println("RTC read failed! Using default safe time.");
    tm.Hour=0; tm.Minute=0; tm.Second=0;
    tm.Day=1; tm.Month=1; tm.Year=CalendarYrToTm(2025);
  }
  return tm;
}
