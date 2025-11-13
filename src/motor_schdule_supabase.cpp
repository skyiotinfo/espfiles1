#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPSupabaseRealtime.h>
#include <EEPROM.h>
#include <time.h>
#include <TM1637Display.h>

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif

#define EEPROM_SIZE 512

#define MOTOR_PIN D8       
#define CLK_PIN   D3
#define DIO_PIN   D4

const int ot_sensor = D1;     
const int ot_status = D6;   
const int auto_status = D7;  
const int input1 = D9;       

TM1637Display display(CLK_PIN, DIO_PIN);

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
  Serial.printf("  Manual State: %d\n", motorData.state ? 1 : 0);
  Serial.printf("  SCH1: %s (%d min, en=%d)\n", motorData.sch1_start, motorData.sch1_duration, motorData.sch1_en ? 1 : 0);
  Serial.printf("  SCH2: %s (%d min, en=%d)\n", motorData.sch2_start, motorData.sch2_duration, motorData.sch2_en ? 1 : 0);
  Serial.printf("  SCH3: %s (%d min, en=%d)\n", motorData.sch3_start, motorData.sch3_duration, motorData.sch3_en ? 1 : 0);
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

void stopMotorImmediate() {
  digitalWrite(MOTOR_PIN, LOW);
  motorRunning = false;
  Serial.println("Motor stopped.");
}

void handleMotorRun() {
  if (motorRunning && (millis() - motorStartTime >= motorRunDuration)) {
    stopMotorImmediate();
  }
}

void HandleChanges(String result) {
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, result);
  if (error) {
    Serial.println("⚠️ JSON parse error");
    return;
  }

  String tableName = doc["table"].as<String>();
  JsonObject record = doc["record"].as<JsonObject>();

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

    Serial.println("Updated schedule from Supabase:");
    Serial.printf("  SCH1: %s (%d min, en=%d)\n", motorData.sch1_start, motorData.sch1_duration, motorData.sch1_en ? 1 : 0);
    Serial.printf("  SCH2: %s (%d min, en=%d)\n", motorData.sch2_start, motorData.sch2_duration, motorData.sch2_en ? 1 : 0);
    Serial.printf("  SCH3: %s (%d min, en=%d)\n", motorData.sch3_start, motorData.sch3_duration, motorData.sch3_en ? 1 : 0);
    Serial.printf("  Manual Motor State: %d\n", motorData.state ? 1 : 0);

    if (motorData.state && !motorRunning) {
      digitalWrite(MOTOR_PIN, HIGH);
    } else if (!motorData.state && !motorRunning) {
      digitalWrite(MOTOR_PIN, LOW);
    }
  }
}

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void initTime() {
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing time via NTP");
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 24 * 3600 && attempts < 30) { 
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    attempts++;
  }
  if (now >= 24 * 3600) {
    struct tm *ti = localtime(&now);
    Serial.printf("\nTime synchronized: %02d:%02d\n", ti->tm_hour, ti->tm_min);
  } else {
    Serial.println("\nTime sync failed or still pending.");
  }
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

  EEPROM.begin(EEPROM_SIZE);

  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW); 

  pinMode(input1, INPUT_PULLUP);    
  pinMode(ot_sensor, INPUT_PULLUP); 
  pinMode(ot_status, OUTPUT);
  pinMode(auto_status, OUTPUT);

  display.setBrightness(0x0f);
  display.clear();

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

  int ot_sensorstatus = digitalRead(ot_sensor);  
  int buttonState = digitalRead(input1);        

  if (ot_sensorstatus == LOW) {
    if (motorRunning || motorData.state) {
      Serial.println("Tank full detected — stopping motor!");
      stopMotorImmediate();
      motorData.state = false;  
      saveMotorToEEPROM();
    }
  }

  if (buttonState == LOW) {
    Serial.println("Manual button pressed!");
    delay(50); 
    while (digitalRead(input1) == LOW) delay(20); 

    if (ot_sensorstatus == HIGH) {
      motorData.state = !motorData.state;
      if (motorData.state) {
        Serial.println("Manual motor ON");
        digitalWrite(MOTOR_PIN, HIGH);
      } else {
        Serial.println("Manual motor OFF");
        stopMotorImmediate();
      }
      saveMotorToEEPROM();
    } else {
      Serial.println("Tank full — manual ON blocked!");
      stopMotorImmediate();
      motorData.state = false;
      saveMotorToEEPROM();
    }
  }

  digitalWrite(ot_status, ot_sensorstatus == LOW ? HIGH : LOW);

  if (millis() - lastCheck > 10000) {
    lastCheck = millis();

    time_t now = time(nullptr);
    struct tm *ti = localtime(&now);
    char currentTime[6] = {0};
    if (ti) snprintf(currentTime, sizeof(currentTime), "%02d:%02d", ti->tm_hour, ti->tm_min);

    Serial.printf("Time: %s | MotorRunning: %d | Manual: %d | TankFull: %d\n",
                  currentTime, motorRunning, motorData.state, ot_sensorstatus == LOW);

    if (ot_sensorstatus == HIGH && !motorRunning) {
      if (motorData.sch1_en && timeMatches(motorData.sch1_start, currentTime))
        startMotorForDuration((unsigned long)motorData.sch1_duration * 60000UL);
      else if (motorData.sch2_en && timeMatches(motorData.sch2_start, currentTime))
        startMotorForDuration((unsigned long)motorData.sch2_duration * 60000UL);
      else if (motorData.sch3_en && timeMatches(motorData.sch3_start, currentTime))
        startMotorForDuration((unsigned long)motorData.sch3_duration * 60000UL);
    }

    if (motorData.state && !motorRunning && ot_sensorstatus == HIGH) {
      digitalWrite(MOTOR_PIN, HIGH);
    } else if ((!motorData.state || ot_sensorstatus == LOW) && !motorRunning) {
      digitalWrite(MOTOR_PIN, LOW);
    }
  }

  delay(50);
}
