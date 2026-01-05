
#include <PZEM004Tv30.h>
#include <EEPROM.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TM1637Display.h>
#include <ArduinoJson.h>

const char* firebaseHost   = "sky2-9d324-default-rtdb.firebaseio.com";
const char* firebaseHost1  = "anupam-32ea7-default-rtdb.firebaseio.com";
String getPath             = "/user/fTdRgWx4YNVtEVTOzO1GAltFv9G3/motorSchedule.json";
String statusPostPath      = "/user/uid/1001/motorStatusLog.json";  
String HeartbeatPostPath   = "/user/uid/1001/heartbeat.json";  

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800);

                      
unsigned long lastVoltageRead = 0;
const unsigned long VOLTAGE_READ_INTERVAL = 2000;
                      

const uint8_t seg_full[] = {
  SEG_A | SEG_E | SEG_F | SEG_G,                 
  SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,         
  SEG_D | SEG_E | SEG_F,                         
  SEG_D | SEG_E | SEG_F                          
};

PZEM004Tv30 pzem1(4, 14); 

float zeroIfNan(float v);
float VOLTAGE, CURRENT, POWER;

#define CLK D3
#define DIO D4
TM1637Display display(CLK, DIO);
uint8_t blank[] = {0x00, 0x00, 0x00, 0x00};

const int ot_sensor       = D1;
// const int ut_sensor       = D2;
const int buzzer          = D8;
const int ot_status       = D6;
// const int ut_status       = D5;
const int auto_status     = D7;
const int input1          = D9;  


#define EEPROM_SIZE           512
#define SCHED1_ADDR_HOUR       10
#define SCHED1_ADDR_MIN        11
#define SCHED1_ADDR_DURATION   12
#define SCHED2_ADDR_HOUR       20
#define SCHED2_ADDR_MIN        21
#define SCHED2_ADDR_DURATION   22
#define SCHED3_ADDR_HOUR       30
#define SCHED3_ADDR_MIN        31
#define SCHED3_ADDR_DURATION   32
#define MOTOR_DURATION_ADDR   100

int motor_status = 0;
int prev_motor_status = 0;
int motor_time = 0;
int motor_duration = 30;

int ot_sensorstatus = 1;
int ut_sensorstatus = 1;
int ot_sensorcount = 0;
int ut_sensorcount = 0;

bool manualMode = false;
int currentScheduleIndex = -1;
bool scheduleCompleted[3] = {false, false, false};

unsigned long lastFirebaseFetchTime = 0;
const unsigned long firebaseFetchInterval = 2 * 60 * 1000;
const unsigned long wifiReconnectInterval = 10 * 60 * 1000;
unsigned long lastWiFiReconnectAttempt = 0;

const unsigned long HEARTBEAT_INTERVAL = 30000;
unsigned long lastHeartbeatTime = 0;

unsigned long lastMotorTimeUpdate = 0;

unsigned long lastVoltageSend = 0;
const unsigned long VOLTAGE_SEND_INTERVAL = 30000;

bool wifiReconnectInProgress = false;
unsigned long wifiReconnectStartTime = 0;
int wifiReconnectTries = 0;


void connectToWiFi();
void getScheduleFromFirebase();
int getDurationToPost();
void postMotorStatusChange(int status);
void readSchedule(int index, int &hour, int &min, int &duration);
int findMatchingSchedule(int currentHour, int currentMin);
void sendHeartbeat();
void sendVoltageData();

void readSchedule(int index, int &hour, int &min, int &duration) {
  int base = (index == 0) ? SCHED1_ADDR_HOUR : (index == 1) ? SCHED2_ADDR_HOUR : SCHED3_ADDR_HOUR;
  hour = EEPROM.read(base);
  min = EEPROM.read(base + 1);
  duration = EEPROM.read(base + 2);
  if (duration < 1 || duration > 100) duration = 30;
}

int findMatchingSchedule(int currentHour, int currentMin) {
  int nowSec = currentHour * 3600 + currentMin * 60;
  for (int i = 0; i < 3; i++) {
    int h, m, d;
    readSchedule(i, h, m, d);
    if (h < 0) continue;
    int startSec = h * 3600 + m * 60;
    int endSec = startSec + d * 60;
    if (nowSec >= startSec && nowSec < endSec) {
      return i;
    }
  }
  return -1;
}
void readVoltage() {
//   VOLTAGE = zeroIfNan(pzem1.voltage());  
//   Serial.printf("Voltage: %.2f V\n", VOLTAGE);
//   CURRENT = zeroIfNan(pzem1.current());  
//   Serial.printf("Current: %.2f V\n", CURRENT);
//   POWER = zeroIfNan(pzem1.power());  
//   Serial.printf("Power: %.2f V\n", POWER);
}
void voltage() {
  
}




void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  display.setBrightness(0x0f);
  display.setSegments(blank);

  pinMode(input1, INPUT_PULLUP);
  pinMode(buzzer, OUTPUT);
  pinMode(ot_status, OUTPUT);
  //pinMode(ut_status, OUTPUT);
  pinMode(auto_status, OUTPUT);
  pinMode(ot_sensor, INPUT_PULLUP);
  //pinMode(ut_sensor, INPUT_PULLUP);
  delay(500);

  if (digitalRead(input1) == LOW) {
    int temp_count = 100;
    int new_duration = EEPROM.read(MOTOR_DURATION_ADDR);
    if (new_duration < 5 || new_duration > 100) new_duration = 30;

    while (temp_count-- > 0) {
      if (digitalRead(input1) == LOW) {
        new_duration += 5;
        if (new_duration > 100) new_duration = 5;
        EEPROM.write(MOTOR_DURATION_ADDR, new_duration);
        EEPROM.commit();
        display.showNumberDec(new_duration, false);
        Serial.print("Set motor duration to: ");
        Serial.println(new_duration);
        delay(300);
      } else {
        break;
      }
    }
  }

  motor_duration = EEPROM.read(MOTOR_DURATION_ADDR);
  if (motor_duration < 1 || motor_duration > 100) motor_duration = 30;
  motor_time = motor_duration * 60;

  digitalWrite(auto_status, HIGH);
  digitalWrite(buzzer, LOW);

  timeClient.begin();

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("Setup_WiFi", "admin123")) {
    Serial.println("Failed to connect via portal. Switching to manual mode...");
    manualMode = true;
  }
  else {
    Serial.println("WiFi connected.");
    manualMode = false;
  }


  manualMode = false;
  getScheduleFromFirebase();
  lastFirebaseFetchTime = millis();
  prev_motor_status = motor_status;
}


void loop() {
  
  VOLTAGE = pzem1.voltage();
  VOLTAGE = zeroIfNan(VOLTAGE);
  CURRENT = pzem1.current();
  CURRENT = zeroIfNan(CURRENT);
  POWER = pzem1.power();
  POWER = zeroIfNan(POWER);
  ot_sensorstatus = digitalRead(ot_sensor);
  //ut_sensorstatus = digitalRead(ut_sensor);

  timeClient.update();
  int currH = timeClient.getHours();
  int currM = timeClient.getMinutes();
  int currSec = currH * 3600 + currM * 60;
 

  if (WiFi.status() != WL_CONNECTED && !wifiReconnectInProgress) {
    Serial.println("Starting non-blocking WiFi reconnect...");
    WiFi.begin();
    wifiReconnectInProgress = true;
    wifiReconnectStartTime = millis();
    wifiReconnectTries = 0;
  }

  if (wifiReconnectInProgress && millis() - wifiReconnectStartTime >= 500) {
    wifiReconnectStartTime = millis();
    wifiReconnectTries++;

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi reconnected.");
      wifiReconnectInProgress = false;
      manualMode = false;
      getScheduleFromFirebase();
      lastFirebaseFetchTime = millis();
    } else if (wifiReconnectTries >= 20) {
      Serial.println("WiFi reconnect failed.");
      wifiReconnectInProgress = false;
      manualMode = true;
      lastWiFiReconnectAttempt = millis();
    }
  }

  if ((millis() - lastHeartbeatTime) >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeatTime = millis();
  }
unsigned long now = millis();

  if (now - lastVoltageRead >= VOLTAGE_READ_INTERVAL) {
    lastVoltageRead = now;
    readVoltage();
  }
 

  if (now - lastVoltageSend >= VOLTAGE_SEND_INTERVAL) {
    lastVoltageSend = now;
    sendVoltageData();
  }
  if (!manualMode && millis() - lastFirebaseFetchTime >= firebaseFetchInterval && motor_status == 0) {
    getScheduleFromFirebase();
    lastFirebaseFetchTime = millis();
  }

  for (int i = 0; i < 3; i++) {
    int h, m, d;
    readSchedule(i, h, m, d);
    int startSec = h * 3600 + m * 60;
    int endSec = startSec + d * 60;
    if (currSec >= endSec || currSec < startSec) {
      scheduleCompleted[i] = false;
    }
  }

  static bool lastBtnState = HIGH;
  bool currBtnState = digitalRead(input1);
if (lastBtnState == HIGH && currBtnState == LOW) {
    if (motor_status == 0) {
        Serial.println("Manual START");
        motor_status = 1;
        motor_time = motor_duration * 60;
        digitalWrite(buzzer, HIGH);
        digitalWrite(auto_status, HIGH);
        currentScheduleIndex = -1;  
    } else {
        Serial.println("Manual STOP");
        motor_status = 0;
        digitalWrite(buzzer, LOW);
        digitalWrite(auto_status, LOW);
        display.showNumberDec(0, false);
        motor_time = motor_duration * 60;

        
        if (currentScheduleIndex != -1) {
            scheduleCompleted[currentScheduleIndex] = true;
            currentScheduleIndex = -1;
        }
    }
}

  lastBtnState = currBtnState;

  if (motor_status != prev_motor_status) {
    if (WiFi.status() == WL_CONNECTED) {
      postMotorStatusChange(motor_status);
    }
    prev_motor_status = motor_status;
  }

  int matchedSchedule = manualMode ? -1 : findMatchingSchedule(currH, currM);
  if (!manualMode && motor_status == 0 && matchedSchedule != -1 && !scheduleCompleted[matchedSchedule]) {
    int h, m, d;
    readSchedule(matchedSchedule, h, m, d);
    int endSec = h * 3600 + m * 60 + d * 60;
    int nowSec = currH * 3600 + currM * 60;
    int remaining = endSec - nowSec;
    if (remaining > 0) {
      motor_status = 1;
      motor_time = remaining;
      digitalWrite(buzzer, HIGH);
      digitalWrite(auto_status, HIGH);
      currentScheduleIndex = matchedSchedule;
      scheduleCompleted[matchedSchedule] = true;
    }
  }

  if (motor_status == 1) {
    if (millis() - lastMotorTimeUpdate >= 1000) {
      lastMotorTimeUpdate = millis();
      motor_time--;
    }

    int remMin = motor_time / 60;
    int remSec = motor_time % 60;
    int disp = remMin * 100 + remSec;
    display.showNumberDecEx(disp, 0b01000000);

 
    if (ot_sensorstatus == 0) {
      ot_sensorcount++;
      if (ot_sensorcount >= 50) {
        motor_status = 0;
        ot_sensorcount = 0;
        digitalWrite(buzzer, LOW);
        digitalWrite(auto_status, LOW);
        display.showNumberDec(0, false);
        delay(300);
        display.clear();
        display.setSegments(seg_full);
        motor_time = motor_duration * 60;
        if (currentScheduleIndex != -1) scheduleCompleted[currentScheduleIndex] = true;
      }
    } else {
      ot_sensorcount = 0;
    }

    if (motor_time <= 0) {
      motor_status = 0;
      digitalWrite(buzzer, LOW);
      digitalWrite(auto_status, LOW);
      display.showNumberDec(0, false);
      motor_time = motor_duration * 60;
      if (currentScheduleIndex != -1) scheduleCompleted[currentScheduleIndex] = true;
    }
  }

  if (ut_sensorstatus == 0) {
    ut_sensorcount++;
    if (ut_sensorcount >= 20 && motor_status == 0) {
      motor_status = 1;
      motor_time = motor_duration * 60;
      digitalWrite(buzzer, HIGH);
      digitalWrite(auto_status, HIGH);
      currentScheduleIndex = -1;
    }
  } else {
    ut_sensorcount = 0;
  }

  delay(10); 
}
float zeroIfNan(float v) 
{
  if (isnan(v)) 
  v = 0;
  return v;
}

int getDurationToPost() {
  if (currentScheduleIndex != -1) {
    int h, m, d;
    readSchedule(currentScheduleIndex, h, m, d);
    return d;
  }
  return motor_duration;
}

void getScheduleFromFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String("https://") + firebaseHost + getPath;
  if (https.begin(client, url)) {
    int code = https.GET();
    if (code == 200) {
      String payload = https.getString();
      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, payload);
      if (!err && doc.containsKey("schedules")) {
        JsonArray arr = doc["schedules"];
        for (int i = 0; i < 3 && i < arr.size(); i++) {
          int sh = arr[i]["startHour"] | 0;
          int sm = arr[i]["startMin"] | 0;
          int dur = arr[i]["duration"] | 30;
          int base = (i == 0 ? SCHED1_ADDR_HOUR : i == 1 ? SCHED2_ADDR_HOUR : SCHED3_ADDR_HOUR);
bool updated = false;

if (EEPROM.read(base) != sh) {
  EEPROM.write(base, sh);
  updated = true;
}
if (EEPROM.read(base + 1) != sm) {
  EEPROM.write(base + 1, sm);
  updated = true;
}
if (EEPROM.read(base + 2) != dur) {
  EEPROM.write(base + 2, dur);
  updated = true;
}

if (updated) {
  EEPROM.commit();
  Serial.println("Schedule updated in EEPROM");
} else {
  Serial.println("No EEPROM change needed");
}

          Serial.printf("Saved schedule %d: %02d:%02d for %d min\n", i+1, sh, sm, dur);
        }
      }
    }
    https.end();
  }
}

void postMotorStatusChange(int status) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String("https://") + firebaseHost1 + statusPostPath;
  if (!https.begin(client, url)) return;
  https.addHeader("Content-Type", "application/json");

  timeClient.update();
  unsigned long epoch = timeClient.getEpochTime();
  time_t raw = (time_t)epoch;
  struct tm * tm_info = localtime(&raw);
  char timestamp[40];
  strftime(timestamp, sizeof(timestamp), "%d %B %Y %I:%M %p", tm_info); 

  String body = "{\"motor_status\":" + String(status) + ",\"timestamp\":\"" + String(timestamp) + "\"}";
  https.PUT(body);
  https.end();
}

void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String("https://") + firebaseHost1 + HeartbeatPostPath;
  if (!https.begin(client, url)) return;
  https.addHeader("Content-Type", "application/json");

  timeClient.update();
  unsigned long epoch = timeClient.getEpochTime();
  time_t raw = (time_t)epoch;
  struct tm * tm_info = localtime(&raw);
  char timestamp[40];
  strftime(timestamp, sizeof(timestamp), "%d %B %Y %I:%M %p", tm_info); 

  String body = "{\"last_active\":\"" + String(timestamp) + "\"}";
  https.PUT(body);
  https.end();
}


void sendVoltageData() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = String("https://") + firebaseHost1 + "/user/uid/1001/pzem.json";
  if (!https.begin(client, url)) return;
  https.addHeader("Content-Type", "application/json");

  String body = "{\"voltage\":" + String(VOLTAGE, 2) +
                ",\"current\":" + String(CURRENT, 2) +
                ",\"power\":"   + String(POWER, 2) + "}";

  https.PUT(body);
  https.end();

  Serial.println("PZEM data sent: " + body);
}
