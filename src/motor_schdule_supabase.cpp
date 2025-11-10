#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPSupabaseRealtime.h>
#include <EEPROM.h>
#include <time.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

#define EEPROM_SIZE 512
#define MOTOR_PIN D8  

SupabaseRealtime realtime;

struct ScheduleData {
  bool motor_state;
  char motor1_time[6];
  int motor1_duration;
  char motor2_time[6];
  int motor2_duration;
};

ScheduleData scheduleData;
unsigned long lastCheck = 0;
bool motorRunning = false;
unsigned long motorStartTime = 0;
unsigned long motorRunDuration = 0;

const char* WIFI_SSID = "Anupam";
const char* WIFI_PASS = "12345678";

const char* SUPABASE_URL = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";
const char* USER_EMAIL = "user2@demo.com";
const char* USER_PASS = "123456";

void saveScheduleToEEPROM() {
  EEPROM.put(0, scheduleData);
  EEPROM.commit();
  Serial.println("Saved schedule to EEPROM.");
}

void loadScheduleFromEEPROM() {
  EEPROM.get(0, scheduleData);
  Serial.println("Loaded schedule from EEPROM:");
  Serial.printf("  Motor state: %d\n", scheduleData.motor_state);
  Serial.printf("  Motor1: %s (%d min)\n", scheduleData.motor1_time, scheduleData.motor1_duration);
  Serial.printf("  Motor2: %s (%d min)\n", scheduleData.motor2_time, scheduleData.motor2_duration);
}

void clearEEPROM() {
  for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
  EEPROM.commit();
  Serial.println("EEPROM cleared!");
}

bool timeMatches(const char* scheduledTime, const char* currentTime) {
  return strcmp(scheduledTime, currentTime) == 0;
}

void startMotorForDuration(unsigned long durationMs) {
  digitalWrite(MOTOR_PIN, HIGH);
  motorRunning = true;
  motorStartTime = millis();
  motorRunDuration = durationMs;
  Serial.printf("Motor started for %lu ms\n", durationMs);
}

void handleMotorRun() {
  if (motorRunning && (millis() - motorStartTime >= motorRunDuration)) {
    digitalWrite(MOTOR_PIN, LOW);
    motorRunning = false;
    Serial.println("Motor stopped after duration.");
  }
}

void HandleChanges(String result) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, result);
  if (error) {
    Serial.println("JSON parse error");
    return;
  }

  String tableName = doc["table"];
  JsonObject record = doc["record"];

  if (tableName == "motor_schedule") {
    scheduleData.motor_state = record["motor_state"].as<bool>();
    strlcpy(scheduleData.motor1_time, record["motor1_time"] | "", sizeof(scheduleData.motor1_time));
    scheduleData.motor1_duration = record["motor1_duration"] | 0;
    strlcpy(scheduleData.motor2_time, record["motor2_time"] | "", sizeof(scheduleData.motor2_time));
    scheduleData.motor2_duration = record["motor2_duration"] | 0;

    saveScheduleToEEPROM();

    Serial.println("Updated schedule from Supabase:");
    Serial.printf("  Motor1: %s (%d min)\n", scheduleData.motor1_time, scheduleData.motor1_duration);
    Serial.printf("  Motor2: %s (%d min)\n", scheduleData.motor2_time, scheduleData.motor2_duration);
    Serial.printf("  Manual Motor State: %d\n", scheduleData.motor_state);

    if (scheduleData.motor_state) {
      digitalWrite(MOTOR_PIN, HIGH);
      motorRunning = false; 
    } else {
      digitalWrite(MOTOR_PIN, LOW);
    }
  }
}

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void initTime() {
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov"); 
  Serial.print("Syncing time via NTP");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\n Time synchronized!");
  Serial.printf(" Current Time: %02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min);
}

void connectSupabase() {
  realtime.begin(SUPABASE_URL, SUPABASE_KEY, HandleChanges);
  realtime.login_email(USER_EMAIL, USER_PASS);
  realtime.addChangesListener("motor_schedule", "*", "public", "");
  realtime.listen();
  Serial.println(" Connected to Supabase Realtime");
}

void setup() {
  Serial.begin(115200);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  EEPROM.begin(EEPROM_SIZE);
  loadScheduleFromEEPROM();

  connectWiFi();
  initTime();
  connectSupabase();

  Serial.println(" Setup complete, listening for Supabase changes...");
}

void loop() {
  realtime.loop();
  handleMotorRun();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(" WiFi lost, reconnecting...");
    connectWiFi();
    initTime();
    connectSupabase();
  }

  if (millis() - lastCheck > 10000) {
    lastCheck = millis();

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char currentTime[6];
      sprintf(currentTime, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
      Serial.printf(" Time: %s\n", currentTime);

      if (!motorRunning) { 
        if (timeMatches(scheduleData.motor1_time, currentTime)) {
          startMotorForDuration(scheduleData.motor1_duration * 60000UL);
        } else if (timeMatches(scheduleData.motor2_time, currentTime)) {
          startMotorForDuration(scheduleData.motor2_duration * 60000UL);
        }
      }

      if (scheduleData.motor_state && !motorRunning) {
        digitalWrite(MOTOR_PIN, HIGH);
      } else if (!scheduleData.motor_state && !motorRunning) {
        digitalWrite(MOTOR_PIN, LOW);
      }

    } else {
      Serial.println(" Time not yet available!");
    }
  }
}
