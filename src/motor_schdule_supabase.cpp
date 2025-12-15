#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPSupabaseRealtime.h>
#include <EEPROM.h>
#include <time.h>
#include <TM1637Display.h>
#include <PZEM004Tv30.h>

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
void initTime();
void connectSupabase();
void displayIdle();
TM1637Display display(CLK_PIN, DIO_PIN);
PZEM004Tv30 pzem1(D2, D5);

float zeroIfNan(float v) { if (isnan(v)) v = 0; return v; }
float VOLTAGE, CURRENT, POWER;
unsigned long lastVoltageRead = 0;
const unsigned long VOLTAGE_READ_INTERVAL = 2000;

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

unsigned long scheduledRemainingMs = 0;
bool scheduleInProgress = false;

const int MANUAL_ADDR = 100;        
const int SCHEDULE_MAGIC_ADDR = 200; 
const int SCHEDULE_REMAIN_ADDR = 201; 
const int SCHEDULE_FLAG_ADDR = 205;  
const byte SCHEDULE_MAGIC = 0x42;

int manual_duration = 30;
bool manualStopRequested = false;

const unsigned long SCHEDULE_SAVE_INTERVAL_MS = 5000;
unsigned long lastScheduleSaveMs = 0;

void saveMotorToEEPROM() {
  EEPROM.put(0, motorData);
  EEPROM.commit();
}

void loadMotorFromEEPROM() {
  EEPROM.get(0, motorData);
}

void writeManualDurationToEEPROM(int mins) {
  mins = constrain(mins, 0, 255);
  EEPROM.write(MANUAL_ADDR, (byte)mins);
  EEPROM.commit();
}

void readManualDurationFromEEPROM() {
  byte v = EEPROM.read(MANUAL_ADDR);
  if (v == 0xFF) manual_duration = 30;
  else manual_duration = v;
  if (manual_duration < 1 || manual_duration > 100) manual_duration = 30;
}

void saveScheduledState() {
  EEPROM.write(SCHEDULE_MAGIC_ADDR, SCHEDULE_MAGIC);
  EEPROM.put(SCHEDULE_REMAIN_ADDR, scheduledRemainingMs);
  EEPROM.write(SCHEDULE_FLAG_ADDR, scheduleInProgress ? 1 : 0);
  EEPROM.commit();
  lastScheduleSaveMs = millis();
}

void loadScheduledState() {
  byte magic = EEPROM.read(SCHEDULE_MAGIC_ADDR);
  if (magic != SCHEDULE_MAGIC) {
    scheduledRemainingMs = 0;
    scheduleInProgress = false;
    return;
  }
  EEPROM.get(SCHEDULE_REMAIN_ADDR, scheduledRemainingMs);
  byte flag = EEPROM.read(SCHEDULE_FLAG_ADDR);
  scheduleInProgress = (flag == 1);
  if (scheduledRemainingMs > 24UL * 3600UL * 1000UL) { 
    scheduledRemainingMs = 0;
    scheduleInProgress = false;
  }
}

void clearScheduledState() {
  EEPROM.write(SCHEDULE_MAGIC_ADDR, 0xFF);
  EEPROM.put(SCHEDULE_REMAIN_ADDR, (unsigned long)0);
  EEPROM.write(SCHEDULE_FLAG_ADDR, 0);
  EEPROM.commit();
  scheduledRemainingMs = 0;
  scheduleInProgress = false;
}

bool timeMatches(const char* scheduledTime, const char* currentTime) {
  if (scheduledTime == nullptr || scheduledTime[0] == '\0') return false;
  return strcmp(scheduledTime, currentTime) == 0;
}

void displayTankFull() {
  uint8_t FF_segments[] = {
    SEG_A | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_E | SEG_F | SEG_G,
    0, 0
  };
  display.setSegments(FF_segments);
}

void displayIdle() { display.showNumberDec(0, true); }

void displayMotorRunning() {
  if (!motorRunning) return displayIdle();

  unsigned long elapsedMs = millis() - motorStartTime;
  unsigned long elapsedMin = elapsedMs / 60000UL;
  unsigned long remainingMin = (motorRunDuration + 59999UL) / 60000UL; 
  if (motorRunDuration > elapsedMs) {
    unsigned long remMs = motorRunDuration - elapsedMs;
    remainingMin = (remMs + 59999UL) / 60000UL;
  } else {
    remainingMin = 0;
  }
  if (remainingMin > 99) remainingMin = 99;
  display.showNumberDec(remainingMin, true);
}

void startMotorForDuration(unsigned long durationMs) {
  manualStopRequested = false;
  motorRunning = true;
  motorStartTime = millis();
  motorRunDuration = durationMs;
  digitalWrite(MOTOR_PIN, HIGH);
  displayMotorRunning();

  motorData.state = true;
  saveMotorToEEPROM();
}

void stopMotorImmediate() {
  motorRunning = false;
  motorRunDuration = 0;
  digitalWrite(MOTOR_PIN, LOW);
  displayIdle();
  clearScheduledState();
}

void handleMotorRun() {
  if (manualStopRequested) {
    if (motorRunning) stopMotorImmediate();
    manualStopRequested = false;
    return;
  }

  if (motorRunning) {
    unsigned long elapsedMs = millis() - motorStartTime;

    if (scheduleInProgress) {
      if (motorRunDuration > elapsedMs)
        scheduledRemainingMs = motorRunDuration - elapsedMs;
      else
        scheduledRemainingMs = 0;

      if (millis() - lastScheduleSaveMs >= SCHEDULE_SAVE_INTERVAL_MS) {
        saveScheduledState();
      }
    }

    if (elapsedMs >= motorRunDuration) {
      stopMotorImmediate();
      motorData.state = false;
      saveMotorToEEPROM();

      scheduleInProgress = false;
      scheduledRemainingMs = 0;
      clearScheduledState();
    }
  }
}

void HandleChanges(String result) {
  if (result.length() < 6) return;

  StaticJsonDocument<8192> doc;
  DeserializationError err = deserializeJson(doc, result);
  if (err) {
    Serial.print("JSON parse failed: ");
    Serial.println(err.c_str());
    return;
  }

  if (!doc.containsKey("table") || !doc.containsKey("record")) return;
  JsonObject record = doc["record"].as<JsonObject>();

  motorData.state = record["state"].as<bool>();
  motorData.sch1_en = record["sch1_en"].as<bool>();
  motorData.sch2_en = record["sch2_en"].as<bool>();
  motorData.sch3_en = record["sch3_en"].as<bool>();

  strlcpy(motorData.sch1_start, record["sch1_start"] | "", 6);
  strlcpy(motorData.sch2_start, record["sch2_start"] | "", 6);
  strlcpy(motorData.sch3_start, record["sch3_start"] | "", 6);

  motorData.sch1_duration = record["sch1_duration"] | 0;
  motorData.sch2_duration = record["sch2_duration"] | 0;
  motorData.sch3_duration = record["sch3_duration"] | 0;

  saveMotorToEEPROM();


  if (motorData.state && !motorRunning) {

    digitalWrite(MOTOR_PIN, HIGH);
  } else if (!motorData.state && !motorRunning) {
    digitalWrite(MOTOR_PIN, LOW);
  }
}

const char* WIFI_SSID = "Airtel_9764005401";
const char* WIFI_PASS = "air46402";

 
const char* SUPABASE_URL = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";
const char* USER_EMAIL = "1234567890@gmail.com";
const char* USER_PASS = "1234";
  

bool wifiWasConnected = false;
bool supabaseConnected = false;
unsigned long lastWifiAttempt = 0;

bool connectWiFiBlocking() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(300);
  }
  return WiFi.status() == WL_CONNECTED;
}

void wifiReconnectNonBlocking() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      initTime();
      connectSupabase();
    }
    return;
  }

  wifiWasConnected = false;
  supabaseConnected = false;

  if (millis() - lastWifiAttempt > 10000) {
    lastWifiAttempt = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void initTime() {
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 24 * 3600 && attempts++ < 30) {
    delay(500);
    now = time(nullptr);
  }
}

void connectSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;

  realtime.begin(SUPABASE_URL, SUPABASE_KEY, HandleChanges);
  realtime.login_email(USER_EMAIL, USER_PASS);
  realtime.addChangesListener("pump_motor", "*", "public", "");
  realtime.listen();

  supabaseConnected = true;
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
  readManualDurationFromEEPROM();
  loadScheduledState(); 

  if (connectWiFiBlocking()) {
    wifiWasConnected = true;
    initTime();
    connectSupabase();
  }

  if (scheduleInProgress && scheduledRemainingMs > 0) {
    if (digitalRead(ot_sensor) == HIGH) {
      startMotorForDuration(scheduledRemainingMs);
      saveScheduledState();
    } else {
      Serial.println("Persisted scheduled run found but OT sensor is LOW - not starting.");
    }
  }
}

void loop() {
  unsigned long now = millis();

  if (now - lastVoltageRead > VOLTAGE_READ_INTERVAL) {
    lastVoltageRead = now;
    VOLTAGE = zeroIfNan(pzem1.voltage());
    CURRENT = zeroIfNan(pzem1.current());
    POWER = zeroIfNan(pzem1.power());
  }

  if (supabaseConnected && WiFi.status() == WL_CONNECTED)
    realtime.loop();

  handleMotorRun();
  wifiReconnectNonBlocking();

  int ot_sensorstatus = digitalRead(ot_sensor);
  int buttonState = digitalRead(input1);

  if (ot_sensorstatus == LOW) displayTankFull();
  else if (motorRunning) displayMotorRunning();
  else displayIdle();

  if (ot_sensorstatus == LOW) {
    if (motorRunning || motorData.state) {
      int lowCount = 0;
      for (int i = 0; i < 10; i++) {
        if (digitalRead(ot_sensor) == LOW) lowCount++;
        delay(500);
      }

      if (lowCount >= 5) {
        stopMotorImmediate();
        motorData.state = false;
        saveMotorToEEPROM();

        scheduleInProgress = false;
        scheduledRemainingMs = 0;
        clearScheduledState(); 
      }
    }
  }

  if (buttonState == LOW) {
    delay(50);
    while (digitalRead(input1) == LOW) delay(20);

    if (digitalRead(ot_sensor) == HIGH) {
      motorData.state = !motorData.state;

      if (motorData.state && !motorRunning) {
        scheduleInProgress = false;
        scheduledRemainingMs = 0;
        clearScheduledState(); 
        startMotorForDuration((unsigned long)manual_duration * 60000UL);
      } else if (!motorData.state) {
        manualStopRequested = true;
        stopMotorImmediate();
      }

      saveMotorToEEPROM();
    }
  }

  digitalWrite(ot_status, ot_sensorstatus == LOW ? HIGH : LOW);

  if (millis() - lastCheck > 10000) {
    lastCheck = millis();

    time_t nowt = time(nullptr);
    struct tm *ti = localtime(&nowt);
    char currentTime[6];
    snprintf(currentTime, 6, "%02d:%02d", ti->tm_hour, ti->tm_min);

    if (!motorRunning && ot_sensorstatus == HIGH) {

      if (motorData.sch1_en && timeMatches(motorData.sch1_start, currentTime)) {
        scheduledRemainingMs = (unsigned long)motorData.sch1_duration * 60000UL;
        scheduleInProgress = true;
        startMotorForDuration(scheduledRemainingMs);
        saveScheduledState();
      }

      else if (motorData.sch2_en && timeMatches(motorData.sch2_start, currentTime)) {
        scheduledRemainingMs = (unsigned long)motorData.sch2_duration * 60000UL;
        scheduleInProgress = true;
        startMotorForDuration(scheduledRemainingMs);
        saveScheduledState();
      }

      else if (motorData.sch3_en && timeMatches(motorData.sch3_start, currentTime)) {
        scheduledRemainingMs = (unsigned long)motorData.sch3_duration * 60000UL;
        scheduleInProgress = true;
        startMotorForDuration(scheduledRemainingMs);
        saveScheduledState();
      }
    }
  }

  delay(50);
}
