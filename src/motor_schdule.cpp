#include <EEPROM.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TM1637Display.h>
#include <ArduinoJson.h>

const char* ssid = "Anupam";
const char* password = "12345678";
const char* firebaseHost = "sky2-9d324-default-rtdb.firebaseio.com";
const char* firebaseHost1 = "anupam-32ea7-default-rtdb.firebaseio.com";
String getPath = "/user/fTdRgWx4YNVtEVTOzO1GAltFv9G3/motorSchedule.json";
String postPath = "/user/uid/1001/motorLog.json";


WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800); 


#define CLK D3
#define DIO D4
TM1637Display display(CLK, DIO);
uint8_t blank[] = { 0x00, 0x00, 0x00, 0x00 };


const int ot_sensor = D1;
const int ut_sensor = D2;
const int buzzer = D8;
const int ot_status = D6;
const int ut_status = D5;
const int auto_status = D7;
const int input1 = D9;


#define SCHED1_ADDR_HOUR 10
#define SCHED1_ADDR_MIN 11
#define SCHED1_ADDR_DURATION 12
#define SCHED2_ADDR_HOUR 20
#define SCHED2_ADDR_MIN 21
#define SCHED2_ADDR_DURATION 22
#define SCHED3_ADDR_HOUR 30
#define SCHED3_ADDR_MIN 31
#define SCHED3_ADDR_DURATION 32
#define MOTOR_DURATION_ADDR 100

int motor_status = 0;
int motor_time = 0;
int motor_duration = 30;
int ot_sensorstatus = 1;
int ut_sensorstatus = 1;
int ot_sensorcount = 0;
int ut_sensorcount = 0;

bool scheduleChecked = false;
bool manualMode = false;
int currentScheduleIndex = -1;
bool scheduleCompleted[3] = {false, false, false};

unsigned long lastFirebaseFetchTime = 0;
const unsigned long firebaseFetchInterval = 5 * 60 * 1000;

unsigned long lastWiFiReconnectAttempt = 0;
const unsigned long wifiReconnectInterval = 10 * 60 * 1000; 

void connectToWiFi();
void getScheduleFromFirebase();
void postRunToFirebase(int duration);
int getDurationToPost();

void readSchedule(int index, int &hour, int &min, int &duration) {
  int baseAddr = 0;
  switch(index) {
    case 0: baseAddr = SCHED1_ADDR_HOUR; break;
    case 1: baseAddr = SCHED2_ADDR_HOUR; break;
    case 2: baseAddr = SCHED3_ADDR_HOUR; break;
    default: hour = -1; min = -1; duration = -1; return;
  }
  hour = EEPROM.read(baseAddr);
  min = EEPROM.read(baseAddr + 1);
  duration = EEPROM.read(baseAddr + 2);
  if (duration < 1 || duration > 100) duration = 30;
}

int findMatchingSchedule(int currentHour, int currentMin) {
  int nowSec = currentHour * 3600 + currentMin * 60;
  for (int i = 0; i < 3; i++) {
    int h, m, d;
    readSchedule(i, h, m, d);
    if (h < 0 || m < 0) continue;

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
  EEPROM.begin(512);
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
        Serial.print("Updated motor duration: ");
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

  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 2 * 60 * 1000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    manualMode = false;
    getScheduleFromFirebase();
    scheduleChecked = true;
    lastFirebaseFetchTime = millis();
  } else {
    Serial.println("\nWiFi NOT connected. Entering manual mode.");
    manualMode = true;
    lastWiFiReconnectAttempt = millis();
  }
}

void loop() {
  ot_sensorstatus = digitalRead(ot_sensor);
  ut_sensorstatus = digitalRead(ut_sensor);

  timeClient.update();
  int currentHour = timeClient.getHours();
  int currentMin = timeClient.getMinutes();
  int currentSec = currentHour * 3600 + currentMin * 60;


  if (manualMode && millis() - lastWiFiReconnectAttempt >= wifiReconnectInterval) {
    Serial.println("Attempting WiFi reconnection...");
    lastWiFiReconnectAttempt = millis();
    connectToWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi reconnected. Exiting manual mode.");
      manualMode = false;
      getScheduleFromFirebase();
      lastFirebaseFetchTime = millis();
    }
  }


  if (!manualMode && millis() - lastFirebaseFetchTime >= firebaseFetchInterval) {
    connectToWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      getScheduleFromFirebase();
      lastFirebaseFetchTime = millis();
    } else {
      Serial.println("Lost WiFi. Entering manual mode.");
      manualMode = true;
      lastWiFiReconnectAttempt = millis();
    }
  }

  int matchedSchedule = manualMode ? -1 : findMatchingSchedule(currentHour, currentMin);

  for (int i = 0; i < 3; i++) {
    int h, m, d;
    readSchedule(i, h, m, d);
    int startSec = h * 3600 + m * 60;
    int endSec = startSec + d * 60;
    if (currentSec >= endSec || currentSec < startSec) {
      scheduleCompleted[i] = false;
    }
  }

  static bool lastButtonState = HIGH;
  bool currentButtonState = digitalRead(input1);
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    if (motor_status == 0) {
      Serial.println("Manual Start Triggered!");
      motor_status = 1;
      motor_time = motor_duration * 60;
      digitalWrite(buzzer, HIGH);
      digitalWrite(auto_status, HIGH);
      currentScheduleIndex = -1;
    } else if (motor_status == 1 && currentScheduleIndex == -1) {
      Serial.println("Manual Stop Triggered!");
      motor_status = 0;
      digitalWrite(buzzer, LOW);
      digitalWrite(auto_status, LOW);
      display.showNumberDec(0, false);
      motor_time = motor_duration * 60;
    }
  }
  lastButtonState = currentButtonState;

  if (!manualMode && motor_status == 0 && matchedSchedule != -1 && !scheduleCompleted[matchedSchedule]) {
    int h, m, d;
    readSchedule(matchedSchedule, h, m, d);
    int startSec = h * 3600 + m * 60;
    int endSec = startSec + d * 60;
    int nowSec = currentHour * 3600 + currentMin * 60;
    int remaining = endSec - nowSec;

    if (remaining > 0) {
      Serial.printf("Resuming motor for schedule %d for %d seconds\n", matchedSchedule + 1, remaining);
      motor_time = remaining;
      motor_status = 1;
      digitalWrite(buzzer, HIGH);
      digitalWrite(auto_status, HIGH);
      currentScheduleIndex = matchedSchedule;
      scheduleCompleted[matchedSchedule] = true;
    }
  }

  if (motor_status == 1) {
    int remainingMin = motor_time / 60;
    int remainingSec = motor_time % 60;
    int displayTime = remainingMin * 100 + remainingSec;
    display.showNumberDecEx(displayTime, 0b01000000);
    motor_time--;

    if (ot_sensorstatus == 0) {
      ot_sensorcount++;
      if (ot_sensorcount >= 5) {
        Serial.println("OT Sensor Stopping Motor");
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

    if (motor_time <= 0) {
      motor_status = 0;
      digitalWrite(buzzer, LOW);
      digitalWrite(auto_status, LOW);
      display.showNumberDec(0, false);
      motor_time = motor_duration * 60;
      if (currentScheduleIndex != -1) scheduleCompleted[currentScheduleIndex] = true;
      if (!manualMode) postRunToFirebase(getDurationToPost());
    }
  }

  if (ut_sensorstatus == 0) {
    ut_sensorcount++;
    if (ut_sensorcount >= 20 && motor_status == 0) {
      Serial.println("UT Sensor Trigger Start");
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

int getDurationToPost() {
  int durationToPost = motor_duration;
  if (currentScheduleIndex != -1) {
    int h, m, d;
    readSchedule(currentScheduleIndex, h, m, d);
    durationToPost = d;
  }
  return durationToPost;
}

void connectToWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
  } else {
    Serial.println("\nWiFi NOT connected.");
  }
}

void getScheduleFromFirebase() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    String url = String("https://") + firebaseHost + getPath;
    Serial.println("Requesting URL: " + url);
    if (https.begin(client, url)) {
      int httpCode = https.GET();
      if (httpCode == 200) {
        String payload = https.getString();
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, payload);
        if (!error && doc.containsKey("schedules")) {
          JsonArray schedules = doc["schedules"].as<JsonArray>();
          for (int i = 0; i < 3; i++) {
            if (i < schedules.size()) {
              int startHour = schedules[i]["startHour"] | 0;
              int startMin = schedules[i]["startMin"] | 0;
              int duration = schedules[i]["duration"] | 30;
              int baseAddr = (i == 0) ? SCHED1_ADDR_HOUR : (i == 1) ? SCHED2_ADDR_HOUR : SCHED3_ADDR_HOUR;
              EEPROM.write(baseAddr, startHour);
              EEPROM.write(baseAddr + 1, startMin);
              EEPROM.write(baseAddr + 2, duration);
              EEPROM.commit();
              Serial.printf("Saved Schedule %d: %02d:%02d for %d mins\n", i+1, startHour, startMin, duration);
            }
          }
        } else {
          Serial.println("Error parsing or missing 'schedules'");
        }
      } else {
        Serial.println("GET Failed: " + https.errorToString(httpCode));
      }
      https.end();
    } else {
      Serial.println("Unable to connect to Firebase for GET");
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
      int stopHour = timeClient.getHours();
      int stopMin = timeClient.getMinutes();
      char fullTime[6];
      sprintf(fullTime, "%02d:%02d", stopHour, stopMin);
      String postData = "{\"fullTime\":\"" + String(fullTime) + "\",\"duration\":" + String(duration) + "}";
      int httpCode = https.POST(postData);
      if (httpCode > 0) {
        Serial.printf("POST Success: %d\n", httpCode);
      } else {
        Serial.printf("POST Failed: %s\n", https.errorToString(httpCode).c_str());
      }
      https.end();
    } else {
      Serial.println("Unable to connect to Firebase for POST");
    }
  } else {
    Serial.println("WiFi not connected. POST not sent.");
  }
}
