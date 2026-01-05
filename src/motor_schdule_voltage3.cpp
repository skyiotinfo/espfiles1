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
#include <Wire.h>
#include <DS1307RTC.h>
#include <TimeLib.h>

// ----------------- Firebase -----------------
const char* firebaseHost   = "sky2-9d324-default-rtdb.firebaseio.com";
const char* firebaseHost1  = "anupam-32ea7-default-rtdb.firebaseio.com";
String getPath             = "/user/fTdRgWx4YNVtEVTOzO1GAltFv9G3/motorSchedule.json";
String statusPostPath      = "/user/uid/1001/motorStatusLog.json";  
String HeartbeatPostPath   = "/user/uid/1001/heartbeat.json";  

// ----------------- NTP -----------------
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800); // 19800 = IST offset

// ----------------- PZEM -----------------
PZEM004Tv30 pzem1(4, 14); 
float VOLTAGE, CURRENT, POWER;
unsigned long lastVoltageRead = 0;
const unsigned long VOLTAGE_READ_INTERVAL = 2000;

// ----------------- Display -----------------
#define CLK D3
#define DIO D4
TM1637Display display(CLK, DIO);
uint8_t blank[] = {0x00, 0x00, 0x00, 0x00};

// ----------------- Pins -----------------
const int ot_sensor       = D1;
const int buzzer          = D8;
const int ot_status       = D6;
const int auto_status     = D7;
const int input1          = D9;
const int wifiBtn         = A0;  // WiFi config button

// ----------------- EEPROM -----------------
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

// ----------------- Motor -----------------
int motor_status = 0;
int prev_motor_status = 0;
int motor_time = 0;
int motor_duration = 30;
int currentScheduleIndex = -1;
bool scheduleCompleted[3] = {false, false, false};

// ----------------- Others -----------------
bool manualMode = true; // start in manual mode (RTC)
unsigned long lastFirebaseFetchTime = 0;
const unsigned long firebaseFetchInterval = 60 * 1000;
const unsigned long wifiReconnectInterval = 10000; 
unsigned long lastWiFiReconnectAttempt = 0;
const unsigned long HEARTBEAT_INTERVAL = 30000;
unsigned long lastHeartbeatTime = 0;
unsigned long lastMotorTimeUpdate = 0;
unsigned long lastVoltageSend = 0;
const unsigned long VOLTAGE_SEND_INTERVAL = 30000;
bool wifiReconnectInProgress = false;
unsigned long otLowSince = 0;

// ----------------- Helper Functions -----------------
float zeroIfNan(float v) {
  if (isnan(v)) v = 0;
  return v;
}

void readVoltage() {
  VOLTAGE = zeroIfNan(pzem1.voltage());  
  CURRENT = zeroIfNan(pzem1.current());  
  POWER = zeroIfNan(pzem1.power());  
}

void stopMotor() {
  motor_status = 0;
  digitalWrite(buzzer, LOW);
  digitalWrite(auto_status, LOW);
  display.showNumberDec(0, false);
  motor_time = motor_duration * 60;
  if (currentScheduleIndex != -1) scheduleCompleted[currentScheduleIndex] = true;
  Serial.println("Motor stopped");
}

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
    int startSec = h * 3600 + m * 60;
    int endSec = startSec + d * 60;
    if (nowSec >= startSec && nowSec < endSec) {
      return i;
    }
  }
  return -1;
}

void getCurrentTime(int &hour, int &min) {
  if (!manualMode && WiFi.status() == WL_CONNECTED) {
    timeClient.update();
    hour = timeClient.getHours();
    min  = timeClient.getMinutes();
  } else {
    tmElements_t tm;
    if (RTC.read(tm)) {
      hour = tm.Hour;
      min  = tm.Minute;
    } else {
      hour = 0;
      min  = 0;
    }
  }
}

int getDurationToPost() {
  if (currentScheduleIndex != -1) {
    int h, m, d;
    readSchedule(currentScheduleIndex, h, m, d);
    return d;
  }
  return motor_duration;
}

// ----------------- Firebase Functions -----------------
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
        for (size_t i = 0; i < 3 && i < arr.size(); i++) {
          int sh = arr[i]["startHour"] | 0;
          int sm = arr[i]["startMin"] | 0;
          int dur = arr[i]["duration"] | 30;
          int base = (i == 0 ? SCHED1_ADDR_HOUR : i == 1 ? SCHED2_ADDR_HOUR : SCHED3_ADDR_HOUR);
          bool updated = false;
          if (EEPROM.read(base) != sh) { EEPROM.write(base, sh); updated = true; }
          if (EEPROM.read(base + 1) != sm) { EEPROM.write(base + 1, sm); updated = true; }
          if (EEPROM.read(base + 2) != dur) { EEPROM.write(base + 2, dur); updated = true; }
          if (updated) EEPROM.commit();
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

// ----------------- Setup -----------------
void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  display.setBrightness(0x0f);
  display.setSegments(blank);

  pinMode(input1, INPUT_PULLUP);
  pinMode(buzzer, OUTPUT);
  pinMode(ot_status, OUTPUT);
  pinMode(auto_status, OUTPUT);
  pinMode(ot_sensor, INPUT_PULLUP);
  pinMode(A0, INPUT);


  delay(500);

  motor_duration = EEPROM.read(MOTOR_DURATION_ADDR);
  if (motor_duration < 1 || motor_duration > 100) motor_duration = 30;
  motor_time = motor_duration * 60;
  digitalWrite(auto_status, HIGH);
  digitalWrite(buzzer, LOW);

  timeClient.begin();

  // WiFi connection will only trigger on button press
  manualMode = true;

  getScheduleFromFirebase();
  lastFirebaseFetchTime = millis();
  prev_motor_status = motor_status;
}

// ----------------- Loop -----------------
void loop() {
    int value = analogRead(A0);  // Read voltage from A0
  Serial.println(value);        // Print the raw value (0-1023)
  delay(500);
  unsigned long now = millis();

  // Read voltage periodically
  if (now - lastVoltageRead >= VOLTAGE_READ_INTERVAL) {
    lastVoltageRead = now;
    readVoltage();
  }

  // OT Sensor
  int ot_sensorstatus = digitalRead(ot_sensor);
  if (ot_sensorstatus == 0) {
    if (otLowSince == 0) otLowSince = now;
    if (now - otLowSince >= 2000) {
      stopMotor();
      otLowSince = 0;
    }
  } else otLowSince = 0;

  // Motor time countdown
  if (motor_status == 1 && now - lastMotorTimeUpdate >= 1000) {
    lastMotorTimeUpdate = now;
    motor_time--;
    int remMin = motor_time / 60;
    int remSec = motor_time % 60;
    int disp = remMin * 100 + remSec;
    display.showNumberDecEx(disp, 0b01000000);
    if (motor_time <= 0) stopMotor();
  }

  // Manual motor button
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
      stopMotor();
      currentScheduleIndex = -1;
    }
  }
  lastBtnState = currBtnState;

  // WiFi config button (A0)
  static bool lastWifiBtnState = false;
static unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 200;

int analogVal = analogRead(A0);
bool currWifiBtnState = (analogVal < 100); // pressed if < 100 (tweak as needed)

if (currWifiBtnState != lastWifiBtnState) {
  lastDebounceTime = millis(); // reset debounce
}

if ((millis() - lastDebounceTime) > debounceDelay) {
  if (currWifiBtnState && !lastWifiBtnState) {
    Serial.println("WiFi Config Button Pressed!");
    WiFiManager wm;
    wm.setConfigPortalTimeout(0);
    if (wm.startConfigPortal("Setup_WiFi", "admin123")) {
      Serial.println("WiFi connected successfully.");
      manualMode = false;
      timeClient.begin();
    } else {
      Serial.println("WiFi portal exited without connection.");
      manualMode = true;
    }
  }
}

lastWifiBtnState = currWifiBtnState;
  // WiFi reconnect
  if (WiFi.status() != WL_CONNECTED && now - lastWiFiReconnectAttempt >= wifiReconnectInterval) {
    Serial.println("Attempting WiFi reconnect...");
    WiFi.begin();
    lastWiFiReconnectAttempt = now;
  }

  // Heartbeat
  if (now - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeatTime = now;
  }

  // Send voltage periodically
  if (now - lastVoltageSend >= VOLTAGE_SEND_INTERVAL) {
    lastVoltageSend = now;
    sendVoltageData();
  }

  // Get schedule from Firebase if online
  if (!manualMode && WiFi.status() == WL_CONNECTED && motor_status == 0 &&
      now - lastFirebaseFetchTime >= firebaseFetchInterval) {
    getScheduleFromFirebase();
    lastFirebaseFetchTime = now;
  }

  // Get current time (RTC fallback)
  int currH, currM;
  getCurrentTime(currH, currM);

  // Find schedule
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

  // Reset scheduleCompleted flags
  for (int i = 0; i < 3; i++) {
    int h, m, d;
    readSchedule(i, h, m, d);
    int startSec = h * 3600 + m * 60;
    int endSec = startSec + d * 60;
    int nowSec = currH * 3600 + currM * 60;
    if (nowSec < startSec || nowSec >= endSec) scheduleCompleted[i] = false;
  }

  // Post motor status change
  if (motor_status != prev_motor_status && WiFi.status() == WL_CONNECTED) {
    postMotorStatusChange(motor_status);
    prev_motor_status = motor_status;
  }

  delay(10);
}
