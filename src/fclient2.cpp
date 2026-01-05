#include <EEPROM.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TM1637Display.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
#include <FirebaseClient.h>
#include "ExampleFunctions.h"

// ===== WiFi & Firebase Credentials =====
#define WIFI_SSID "Anupam"
#define WIFI_PASSWORD "12345678"
#define API_KEY "AIzaSyBXcj6BMqeYVYmwKc37GU66qPaUSGbM7Tg"
#define DATABASE_URL "https://skyiot2-default-rtdb.firebaseio.com"
#define USER_EMAIL "anupam@gmail.com"
#define USER_PASSWORD "123456"

// ===== Pins =====
#define CLK D3
#define DIO D4
#define OT_SENSOR D1
#define UT_SENSOR D2
#define BUZZER D8
#define OT_STATUS D6
#define UT_STATUS D5
#define AUTO_STATUS D7
#define BUTTON D9 

// ===== Motor schedule EEPROM =====
#define SCHED1_ADDR_HOUR     10
#define SCHED1_ADDR_MIN      11
#define SCHED1_ADDR_DURATION 12
#define SCHED2_ADDR_HOUR     20
#define SCHED2_ADDR_MIN      21
#define SCHED2_ADDR_DURATION 22
#define SCHED3_ADDR_HOUR     30
#define SCHED3_ADDR_MIN      31
#define SCHED3_ADDR_DURATION 32
#define MOTOR_DURATION_ADDR 100

// ===== Globals =====
TM1637Display display(CLK, DIO);
uint8_t blank[] = {0x00, 0x00, 0x00, 0x00};

PZEM004Tv30 pzem1(D4, D5);  // TX,RX pins
float VOLTAGE=0, CURRENT=0, POWER=0;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800);

SSL_CLIENT ssl_client, stream_ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client), streamClient(stream_ssl_client);

UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD, 3000);
FirebaseApp app;
RealtimeDatabase Database;
AsyncResult streamResult;

int motor_status = 0, motor_time = 0, motor_duration = 30;
int ot_sensorstatus = 1, ut_sensorstatus = 1;
int ot_sensorcount = 0, ut_sensorcount = 0;
bool manualMode = false;
int currentScheduleIndex = -1;
bool scheduleCompleted[3] = {false,false,false};

String uid, streamPath;

// ===== Function Prototypes =====
void connectToWiFi();
void readSchedule(int index, int &hour, int &min, int &duration);
int findMatchingSchedule(int h, int m);
void processData(AsyncResult &aResult);
void updateMotorStatusInFirebase(int status);
void sendHeartbeat();
void sendPZEMData();
float zeroIfNan(float v);

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);

  // ===== Display =====
  display.setBrightness(0x0f);
  display.setSegments(blank);

  // ===== Pins =====
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);
  pinMode(OT_STATUS, OUTPUT);
  pinMode(UT_STATUS, OUTPUT);
  pinMode(AUTO_STATUS, OUTPUT);
  pinMode(OT_SENSOR, INPUT_PULLUP);
  pinMode(UT_SENSOR, INPUT_PULLUP);

  delay(500);

  // ===== Read motor duration from EEPROM =====
  motor_duration = EEPROM.read(MOTOR_DURATION_ADDR);
  if (motor_duration < 1 || motor_duration > 100) motor_duration = 30;
  motor_time = motor_duration * 60;

  digitalWrite(AUTO_STATUS, HIGH);
  digitalWrite(BUZZER, HIGH);
  delay(500);
  digitalWrite(BUZZER, LOW);

  timeClient.begin();
  connectToWiFi();
  if (WiFi.status() != WL_CONNECTED) manualMode = true;

  // ===== Firebase Initialization =====
  Firebase.printf("Firebase Client v%s\n", FIREBASE_CLIENT_VERSION);
  set_ssl_client_insecure_and_buffer(ssl_client);
  set_ssl_client_insecure_and_buffer(stream_ssl_client);

  Serial.println("Initializing Firebase app...");
  initializeApp(aClient, app, getAuth(user_auth), 20 * 1000, auth_debug_print);

  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  streamClient.setSSEFilters("get,put,patch,keep-alive,cancel,auth_revoked");

  uid = app.getUid().c_str();
  streamPath = "/board1/schedule/" + uid;
  Serial.print("Streaming path: "); Serial.println(streamPath);

  Database.get(streamClient, streamPath, processData, true, "streamTask");
}

unsigned long lastHeartbeat = 0;
unsigned long lastPZEMSend = 0;

void loop() {
  app.loop();

  ot_sensorstatus = digitalRead(OT_SENSOR);
  ut_sensorstatus = digitalRead(UT_SENSOR);

  timeClient.update();
  int currentHour = timeClient.getHours();
  int currentMin = timeClient.getMinutes();
  int currentSec = currentHour*3600 + currentMin*60;

  // ===== WiFi reconnect =====
  static unsigned long lastWiFiAttempt = 0;
  if (manualMode && millis() - lastWiFiAttempt > 600000) {
    lastWiFiAttempt = millis();
    connectToWiFi();
    if (WiFi.status() == WL_CONNECTED) manualMode = false;
  }

  // ===== Reset scheduleCompleted flags =====
  for (int i=0;i<3;i++){
    int h,m,d;
    readSchedule(i,h,m,d);
    int startSec = h*3600 + m*60;
    int endSec = startSec + d*60;
    if (currentSec >= endSec || currentSec < startSec) scheduleCompleted[i] = false;
  }

  // ===== Manual Button =====
  static bool lastButtonState = HIGH;
  bool currButtonState = digitalRead(BUTTON);
  if (lastButtonState == HIGH && currButtonState == LOW) {
    if (motor_status == 0) {
      Serial.println("Manual Start Triggered!");
      motor_status = 1;
      motor_time = motor_duration*60;
      digitalWrite(BUZZER,HIGH);
      digitalWrite(AUTO_STATUS,HIGH);
      currentScheduleIndex = -1;
      updateMotorStatusInFirebase(1);
    } else if (motor_status == 1 && currentScheduleIndex==-1) {
      Serial.println("Manual Stop Triggered!");
      motor_status = 0;
      digitalWrite(BUZZER,LOW);
      digitalWrite(AUTO_STATUS,LOW);
      display.showNumberDec(0,false);
      motor_time = motor_duration*60;
      updateMotorStatusInFirebase(0);
    }
  }
  lastButtonState = currButtonState;

  // ===== Check schedules =====
  int matched = manualMode?-1:findMatchingSchedule(currentHour,currentMin);
  if (!manualMode && motor_status==0 && matched!=-1 && !scheduleCompleted[matched]) {
    int h,m,d;
    readSchedule(matched,h,m,d);
    int startSec = h*3600+m*60;
    int endSec = startSec+d*60;
    int remaining = endSec - currentSec;
    if (remaining>0){
      motor_time = remaining;
      motor_status = 1;
      digitalWrite(BUZZER,HIGH);
      digitalWrite(AUTO_STATUS,HIGH);
      currentScheduleIndex = matched;
      scheduleCompleted[matched] = true;
      updateMotorStatusInFirebase(1);
    }
  }

  // ===== Motor handling =====
  if (motor_status==1){
    int remMin = motor_time/60;
    int remSec = motor_time%60;
    int dispTime = remMin*100 + remSec;
    display.showNumberDecEx(dispTime,0b01000000);
    delay(1000);
    motor_time--;

    if (ot_sensorstatus==0){
      ot_sensorcount++;
      if (ot_sensorcount>=5){
        Serial.println("OT Sensor Stopping Motor");
        motor_status=0;
        ot_sensorcount=0;
        digitalWrite(BUZZER,LOW);
        digitalWrite(AUTO_STATUS,LOW);
        display.showNumberDec(0,false);
        motor_time = motor_duration*60;
        if (currentScheduleIndex!=-1) scheduleCompleted[currentScheduleIndex]=true;
        updateMotorStatusInFirebase(0);
      }
    } else ot_sensorcount=0;

    if (motor_time<=0){
      Serial.println("Motor Time Up");
      motor_status=0;
      digitalWrite(BUZZER,LOW);
      digitalWrite(AUTO_STATUS,LOW);
      display.showNumberDec(0,false);
      if (currentScheduleIndex!=-1) scheduleCompleted[currentScheduleIndex]=true;
      updateMotorStatusInFirebase(0);
    }
  }

  // ===== UT sensor auto start =====
  if (ut_sensorstatus==0){
    ut_sensorcount++;
    if (ut_sensorcount>=20 && motor_status==0){
      Serial.println("UT sensor triggered start");
      motor_status=1;
      motor_time = motor_duration*60;
      digitalWrite(BUZZER,HIGH);
      digitalWrite(AUTO_STATUS,HIGH);
      currentScheduleIndex=-1;
      updateMotorStatusInFirebase(1);
    }
  } else ut_sensorcount=0;

  // ===== Heartbeat =====
  if (millis() - lastHeartbeat >= 30000){
    sendHeartbeat();
    lastHeartbeat = millis();
  }

  // ===== PZEM send =====
  if (millis() - lastPZEMSend >= 30000){
    VOLTAGE = zeroIfNan(pzem1.voltage());
    CURRENT = zeroIfNan(pzem1.current());
    POWER = zeroIfNan(pzem1.power());
    sendPZEMData();
    lastPZEMSend = millis();
  }
}

// ===== Functions =====
void connectToWiFi(){
  if (WiFi.status()!=WL_CONNECTED){
    Serial.print("Connecting to WiFi...");
    WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
    unsigned long start = millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-start<15000){
      delay(500); Serial.print(".");
    }
    Serial.println();
    if (WiFi.status()==WL_CONNECTED) Serial.println("WiFi connected!");
    else Serial.println("WiFi not connected.");
  }
}

void readSchedule(int index, int &hour, int &min, int &duration){
  int base=0;
  switch(index){
    case 0: base=SCHED1_ADDR_HOUR; break;
    case 1: base=SCHED2_ADDR_HOUR; break;
    case 2: base=SCHED3_ADDR_HOUR; break;
    default: hour=-1; min=-1; duration=-1; return;
  }
  hour = EEPROM.read(base);
  min = EEPROM.read(base+1);
  duration = EEPROM.read(base+2);
  if (duration<1||duration>100) duration=30;
}

int findMatchingSchedule(int h,int m){
  int nowSec = h*3600+m*60;
  for(int i=0;i<3;i++){
    int sh,sm,d;
    readSchedule(i,sh,sm,d);
    if (sh<0||sm<0) continue;
    int startSec = sh*3600+sm*60;
    int endSec = startSec+d*60;
    if (nowSec>=startSec && nowSec<endSec) return i;
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
        Serial.print("Motor status via Firebase: "); Serial.println(status);
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
  String writePath = streamPath+"/motor_status_node";
  Database.set<object_t>(aClient,writePath,json,processData,"setTask");
}

void sendHeartbeat(){
  if(WiFi.status()!=WL_CONNECTED) return;
  object_t json; JsonWriter writer; writer.create(json,"last_active",(long)timeClient.getEpochTime());
  String path = "/board1/heartbeat/"+uid;
  Database.set<object_t>(aClient,path,json,processData,"heartbeatTask");
}

void sendPZEMData(){
  if(WiFi.status()!=WL_CONNECTED) return;
  object_t json; JsonWriter writer; 
  writer.create(json,"voltage",VOLTAGE);
  writer.create(json,"current",CURRENT);
  writer.create(json,"power",POWER);
  String path="/board1/pzem/"+uid;
  Database.set<object_t>(aClient,path,json,processData,"pzemTask");
}

float zeroIfNan(float v){ return isnan(v)?0:v; }
