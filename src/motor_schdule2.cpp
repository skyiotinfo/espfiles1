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

// Firebase config
const char* firebaseHost   = "sky2-9d324-default-rtdb.firebaseio.com";
const char* firebaseHost1  = "anupam-32ea7-default-rtdb.firebaseio.com";
String getPath             = "/user/fTdRgWx4YNVtEVTOzO1GAltFv9G3/motorSchedule.json";
String postPath            = "/user/uid/1001/motorLog.json";

// NTP Client for IST
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800);


// TM1637 4‑digit display
#define CLK D3
#define DIO D4
TM1637Display display(CLK, DIO);
uint8_t blank[] = {0x00, 0x00, 0x00, 0x00};

// IO pin definitions
const int ot_sensor = D1;
const int ut_sensor = D2;
const int buzzer    = D8;
const int ot_status = D6;
const int ut_status = D5;
const int auto_status = D7;
const int input1    = D9; // Also used for Motor duration / WiFi reset

// EEPROM layout
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

// State variables
int motor_status        = 0;
int motor_time          = 0;
int motor_duration      = 30;
int ot_sensorstatus     = 1;
int ut_sensorstatus     = 1;
int ot_sensorcount      = 0;
int ut_sensorcount      = 0;

bool manualMode                 = false;
int currentScheduleIndex       = -1;
bool scheduleCompleted[3]      = {false, false, false};
unsigned long lastFirebaseFetchTime = 0;
const unsigned long firebaseFetchInterval = 5 * 60 * 1000;
unsigned long lastWiFiReconnectAttempt = 0;
const unsigned long wifiReconnectInterval = 10 * 60 * 1000;

void connectToWiFi();
void getScheduleFromFirebase();
void postRunToFirebase(int duration);
int getDurationToPost();
void readSchedule(int index, int &hour, int &min, int &duration);
int findMatchingSchedule(int currentHour, int currentMin);


void readSchedule(int index, int &hour, int &min, int &duration) {
  int base;
  if (index == 0) {
    base = SCHED1_ADDR_HOUR;
  } else if (index == 1) {
    base = SCHED2_ADDR_HOUR;
  } else {
    base = SCHED3_ADDR_HOUR;
  }
  hour = EEPROM.read(base);
  min = EEPROM.read(base + 1);
  duration = EEPROM.read(base + 2);
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


void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  display.setBrightness(0x0f);
  display.setSegments(blank);

  pinMode(input1, INPUT_PULLUP);
  pinMode(buzzer, OUTPUT);
  pinMode(ot_status, OUTPUT);
  pinMode(ut_status, OUTPUT);
  pinMode(auto_status, OUTPUT);
  pinMode(ot_sensor, INPUT_PULLUP);
  pinMode(ut_sensor, INPUT_PULLUP);
  delay(500);

  // Allow manual setting of motor_duration via long button press
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
  digitalWrite(buzzer, HIGH);
  delay(500);
  digitalWrite(buzzer, LOW);

  timeClient.begin();

  // WiFiManager setup for Wi‑Fi configuration portal
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("Setup_WiFi", "admin123")) {
    Serial.println("Failed to connect via portal; rebooting...");
    delay(3000);
    ESP.restart();
  }
  Serial.println("WiFi connected.");

  manualMode = false;
  getScheduleFromFirebase();
  lastFirebaseFetchTime = millis();


}

void loop() {
  ot_sensorstatus = digitalRead(ot_sensor);
  ut_sensorstatus = digitalRead(ut_sensor);

  timeClient.update();
  int currH = timeClient.getHours();
  int currM = timeClient.getMinutes();
  int currSec = currH * 3600 + currM * 60;

  // Attempt reconnection if in manual mode
  if (manualMode && millis() - lastWiFiReconnectAttempt >= wifiReconnectInterval) {
    Serial.println("Trying to reconnect WiFi...");
    lastWiFiReconnectAttempt = millis();
    connectToWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi reconnected.");
      manualMode = false;
      getScheduleFromFirebase();
      lastFirebaseFetchTime = millis();
    }
  }

  // Periodically fetch schedule from Firebase
  if (!manualMode && millis() - lastFirebaseFetchTime >= firebaseFetchInterval) {
    connectToWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      getScheduleFromFirebase();
      lastFirebaseFetchTime = millis();
    } else {
      Serial.println("WiFi lost; switching to manual mode.");
      manualMode = true;
      lastWiFiReconnectAttempt = millis();
    }
  }

  int matchedSchedule = manualMode ? -1 : findMatchingSchedule(currH, currM);

  // Reset scheduleCompleted after each period expires
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
    } else if (motor_status == 1 && currentScheduleIndex == -1) {
      Serial.println("Manual STOP");
      motor_status = 0;
      digitalWrite(buzzer, LOW);
      digitalWrite(auto_status, LOW);
      display.showNumberDec(0, false);
      motor_time = motor_duration * 60;
    }
  }
  lastBtnState = currBtnState;

  // Auto‑start motor based on schedule
  if (!manualMode && motor_status == 0 && matchedSchedule != -1 && !scheduleCompleted[matchedSchedule]) {
    int h, m, d;
    readSchedule(matchedSchedule, h, m, d);
    int endSec = h * 3600 + m * 60 + d * 60;
    int nowSec = currH * 3600 + currM * 60;
    int remaining = endSec - nowSec;

    if (remaining > 0) {
      Serial.printf("Auto‑starting schedule %d for %d seconds\n", matchedSchedule + 1, remaining);
      motor_status = 1;
      motor_time = remaining;
      digitalWrite(buzzer, HIGH);
      digitalWrite(auto_status, HIGH);
      currentScheduleIndex = matchedSchedule;
      scheduleCompleted[matchedSchedule] = true;
    }
  }

  // Motor running logic
  if (motor_status == 1) {
    int remMin = motor_time / 60;
    int remSec = motor_time % 60;
    int disp = remMin * 100 + remSec;
    display.showNumberDecEx(disp, 0b01000000);
    motor_time--;

    // OT sensor triggers motor stop
    if (ot_sensorstatus == 0) {
      ot_sensorcount++;
      if (ot_sensorcount >= 5) {
        Serial.println("OT sensor triggered STOP");
        motor_status = 0;
        ot_sensorcount = 0;
        digitalWrite(buzzer, LOW);
        digitalWrite(auto_status, LOW);
        display.showNumberDec(0, false);
        motor_time = motor_duration * 60;
        if (currentScheduleIndex != -1) scheduleCompleted[currentScheduleIndex] = true;
        if (!manualMode) postRunToFirebase(getDurationToPost());
      }
    } else {
      ot_sensorcount = 0;
    }

    // Timeout reached
    if (motor_time <= 0) {
      Serial.println("Motor run completed by timer");
      motor_status = 0;
      digitalWrite(buzzer, LOW);
      digitalWrite(auto_status, LOW);
      display.showNumberDec(0, false);
      motor_time = motor_duration * 60;
      if (currentScheduleIndex != -1) scheduleCompleted[currentScheduleIndex] = true;
      if (!manualMode) postRunToFirebase(getDurationToPost());
    }
  }

  // UT sensor auto‑start motor if motor is stopped
  if (ut_sensorstatus == 0) {
    ut_sensorcount++;
    if (ut_sensorcount >= 20 && motor_status == 0) {
      Serial.println("UT sensor triggered START");
      motor_status = 1;
      motor_time = motor_duration * 60;
      digitalWrite(buzzer, HIGH);
      digitalWrite(auto_status, HIGH);
      currentScheduleIndex = -1;
    }
  } else {
    ut_sensorcount = 0;
  }

  delay(500);
}

void connectToWiFi() {
  WiFi.begin();
  Serial.print("Reconnecting WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi reconnected.");
  } else {
    Serial.println("\nWiFi reconnect failed.");
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

void getScheduleFromFirebase() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    String url = String("https://") + firebaseHost + getPath;
    Serial.println("GET: " + url);
    if (https.begin(client, url)) {
      int code = https.GET();
      if (code == 200) {
        String payload = https.getString();
        StaticJsonDocument<512> doc;
        if (!deserializeJson(doc, payload) && doc.containsKey("schedules")) {
          JsonArray arr = doc["schedules"];
          for (int i = 0; i < 3 && i < arr.size(); i++) {
            int sh = arr[i]["startHour"] | 0;
            int sm = arr[i]["startMin"] | 0;
            int dur = arr[i]["duration"] | 30;
            int base = (i == 0 ? SCHED1_ADDR_HOUR : i == 1 ? SCHED2_ADDR_HOUR : SCHED3_ADDR_HOUR);
            EEPROM.write(base, sh);
            EEPROM.write(base+1, sm);
            EEPROM.write(base+2, dur);
            EEPROM.commit();
            Serial.printf("Saved schedule %d: %02d:%02d for %d min\n", i+1, sh, sm, dur);
          }
        } else {
          Serial.println("JSON parse error or missing 'schedules'");
        }
      } else {
        Serial.printf("GET failed: %d\n", code);
      }
      https.end();
    } else {
      Serial.println("GET begin failed");
    }
  }
}

void postRunToFirebase(int duration) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    String url = String("https://") + firebaseHost1 + postPath;
    if (https.begin(client, url)) {
      https.addHeader("Content-Type", "application/json");
      timeClient.update();
      char ts[6];
      sprintf(ts, "%02d:%02d", timeClient.getHours(), timeClient.getMinutes());
      String body = "{\"fullTime\":\"" + String(ts) + "\",\"duration\":" + String(duration) + "}";
      int code = https.POST(body);
      if (code > 0) {
        Serial.printf("POST success: %d\n", code);
      } else {
        Serial.printf("POST failed: %s\n", https.errorToString(code).c_str());
      }
      https.end();
    } else {
      Serial.println("POST begin failed");
    }
  } else {
    Serial.println("WiFi offline; cannot POST.");
  }
}
