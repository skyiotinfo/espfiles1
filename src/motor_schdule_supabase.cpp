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

struct PumpMotorData {
  bool state;                
  bool sch1_en, sch2_en, sch3_en;
  char sch1_start[6];
  char sch2_start[6];
  char sch3_start[6];
  int sch1_duration;
  int sch2_duration;
  int sch3_duration;
};

PumpMotorData motorData;

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

void saveMotorToEEPROM() {
  EEPROM.put(0, motorData);
  EEPROM.commit();
  Serial.println("Saved motor data to EEPROM.");
}

void loadMotorFromEEPROM() {
  EEPROM.get(0, motorData);
  Serial.println("Loaded motor data from EEPROM:");
  Serial.printf("  Manual State: %d\n", motorData.state);
  Serial.printf("  SCH1: %s (%d min, en=%d)\n", motorData.sch1_start, motorData.sch1_duration, motorData.sch1_en);
  Serial.printf("  SCH2: %s (%d min, en=%d)\n", motorData.sch2_start, motorData.sch2_duration, motorData.sch2_en);
  Serial.printf("  SCH3: %s (%d min, en=%d)\n", motorData.sch3_start, motorData.sch3_duration, motorData.sch3_en);
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

  if (tableName == "pump_motor") {
    motorData.state = record["state"].as<bool>();
    motorData.sch1_en = record["sch1_en"].as<bool>();
    motorData.sch2_en = record["sch2_en"].as<bool>();
    motorData.sch3_en = record["sch3_en"].as<bool>();

    strlcpy(motorData.sch1_start, record["sch1_start"] | "", sizeof(motorData.sch1_start));
    strlcpy(motorData.sch2_start, record["sch2_start"] | "", sizeof(motorData.sch2_start));
    strlcpy(motorData.sch3_start, record["sch3_start"] | "", sizeof(motorData.sch3_start));

    motorData.sch1_duration = record["sch1_duration"] | 0;
    motorData.sch2_duration = record["sch2_duration"] | 0;
    motorData.sch3_duration = record["sch3_duration"] | 0;

    saveMotorToEEPROM();

    Serial.println("Updated from Supabase:");
    Serial.printf("  SCH1: %s (%d min, en=%d)\n", motorData.sch1_start, motorData.sch1_duration, motorData.sch1_en);
    Serial.printf("  SCH2: %s (%d min, en=%d)\n", motorData.sch2_start, motorData.sch2_duration, motorData.sch2_en);
    Serial.printf("  SCH3: %s (%d min, en=%d)\n", motorData.sch3_start, motorData.sch3_duration, motorData.sch3_en);
    Serial.printf("  Manual Motor State: %d\n", motorData.state);

    if (motorData.state) {
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
  Serial.println("\nTime synchronized!");
}

void connectSupabase() {
  realtime.begin(SUPABASE_URL, SUPABASE_KEY, HandleChanges);
  realtime.login_email(USER_EMAIL, USER_PASS);
  realtime.addChangesListener("pump_motor", "*", "public", "");
  realtime.listen();
  Serial.println("Connected to Supabase Realtime");
}

void setup() {
  Serial.begin(115200);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  EEPROM.begin(EEPROM_SIZE);
  loadMotorFromEEPROM();

  connectWiFi();
  initTime();
  connectSupabase();

  Serial.println("Setup complete, listening for Supabase changes...");
}

void loop() {
  realtime.loop();
  handleMotorRun();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
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
      Serial.printf("Time: %s\n", currentTime);

      if (!motorRunning) {
        if (motorData.sch1_en && timeMatches(motorData.sch1_start, currentTime)) {
          startMotorForDuration(motorData.sch1_duration * 60000UL);
        } else if (motorData.sch2_en && timeMatches(motorData.sch2_start, currentTime)) {
          startMotorForDuration(motorData.sch2_duration * 60000UL);
        } else if (motorData.sch3_en && timeMatches(motorData.sch3_start, currentTime)) {
          startMotorForDuration(motorData.sch3_duration * 60000UL);
        }
      }

      if (motorData.state && !motorRunning) {
        digitalWrite(MOTOR_PIN, HIGH);
      } else if (!motorData.state && !motorRunning) {
        digitalWrite(MOTOR_PIN, LOW);
      }

    } else {
      Serial.println("Time not yet available!");
    }
  }
}
