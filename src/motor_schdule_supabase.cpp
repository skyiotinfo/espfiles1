#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPSupabaseRealtime.h>
#include <EEPROM.h>
#include <time.h>
#include <TM1637Display.h>
#include <PZEM004Tv30.h>

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  //#include <SoftwareSerial.h>
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
PZEM004Tv30 pzem1(4, 14); 

float zeroIfNan(float v);
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

float zeroIfNan(float v) {
  if (isnan(v)) v = 0;
  return v;
}

void readVoltage() {
  VOLTAGE = zeroIfNan(pzem1.voltage());  
  CURRENT = zeroIfNan(pzem1.current());  
  POWER = zeroIfNan(pzem1.power());  
}
const char* WIFI_SSID = "Anupam";
const char* WIFI_PASS = "12345678";

const char* SUPABASE_URL = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";
const char* USER_EMAIL = "1234567890@gmail.com";
const char* USER_PASS = "1234";

unsigned long lastWifiAttempt = 0;
bool wifiWasConnected = false;
bool supabaseConnected = false;

const int MANUAL_ADDR = 100;
int manual_duration = 30; 

bool manualStopRequested = false;

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

void writeManualDurationToEEPROM(int mins) {
  if (mins < 0) mins = 0;
  if (mins > 255) mins = 255; 
  EEPROM.write(MANUAL_ADDR, (byte)mins);
  if (EEPROM.commit()) {
    Serial.printf("Manual duration (%d min) saved to EEPROM @%d\n", mins, MANUAL_ADDR);
  } else {
    Serial.println("ERROR! Manual duration EEPROM commit failed");
  }
}

void readManualDurationFromEEPROM() {
  byte v = EEPROM.read(MANUAL_ADDR);
  if (v == 0xFF) { 
    manual_duration = 30;
  } else {
    manual_duration = (int)v;
  }

  if (manual_duration < 1 || manual_duration > 100) manual_duration = 30;
  Serial.printf("Manual duration loaded: %d min\n", manual_duration);
}

bool timeMatches(const char* scheduledTime, const char* currentTime) {
  return strcmp(scheduledTime, currentTime) == 0;
}

void displayTankFull() {
  uint8_t FF_segments[] = {
    SEG_A | SEG_E | SEG_F | SEG_G, 
    SEG_A | SEG_E | SEG_F | SEG_G,  
    0,
    0
  };
  display.setSegments(FF_segments);
}

void displayMotorRunning() {
  if (!motorRunning) {
    displayIdle();
    return;
  }

  unsigned long elapsed = (millis() - motorStartTime) / 60000UL;
  unsigned long remaining = 0;

  if (motorRunDuration > (elapsed * 60000UL)) {
    remaining = (motorRunDuration / 60000UL) - elapsed;
  }

  if (remaining > 99) remaining = 99;

  display.showNumberDec(remaining, true);
}

void displayIdle() {
  display.showNumberDec(0, true);
}

void startMotorForDuration(unsigned long durationMs) {
  manualStopRequested = false;

  digitalWrite(MOTOR_PIN, HIGH);
  motorRunning = true;
  motorStartTime = millis();
  motorRunDuration = durationMs;

  displayMotorRunning();  

  Serial.printf("Motor started for %lu ms\n", durationMs);
}

void stopMotorImmediate() {
  digitalWrite(MOTOR_PIN, LOW);
  motorRunning = false;
  motorRunDuration = 0; 

  displayIdle();  

  Serial.println("Motor stopped.");
}

void handleMotorRun() {
  if (manualStopRequested) {
    if (motorRunning) {
      stopMotorImmediate();
    }
    return;
  }

  if (motorRunning && (millis() - motorStartTime >= motorRunDuration)) {
    motorData.state = false;
    saveMotorToEEPROM();

    int lowCount = 0;
    for (int i = 0; i < 10; ++i) {
      if (digitalRead(ot_sensor) == LOW) lowCount++;
      delay(500);
    }
    Serial.printf("OT confirm after duration: lowCount=%d\n", lowCount);

    stopMotorImmediate();
  }
}





void HandleChanges(String result) {
  if (result.length() < 6) return;

  StaticJsonDocument<8192> doc;
  DeserializationError error = deserializeJson(doc, result);
  if (error) {
    Serial.print("JSON error: ");
    Serial.println(error.c_str());
    return;
  }

  if (!doc.containsKey("table") || !doc.containsKey("record")) return;

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

    if (motorData.state && !motorRunning)
      digitalWrite(MOTOR_PIN, HIGH);
    else if (!motorData.state && !motorRunning)
      digitalWrite(MOTOR_PIN, LOW);
  }
}

bool connectWiFiBlocking() {
  Serial.println("Connecting to WiFi (up to 30s)...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long startAttempt = millis();
  const unsigned long timeout = 30000;

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < timeout) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\nWiFi NOT connected! Continuing...");
    return false;
  }
}

void wifiReconnectNonBlocking() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.println("WiFi restored.");
      initTime();
      connectSupabase();
    }
    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    supabaseConnected = false;
    Serial.println("WiFi lost.");
  }

  unsigned long now = millis();
  if (now - lastWifiAttempt > 10000UL) {
    lastWifiAttempt = now;
    Serial.println("Attempting WiFi reconnect...");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void initTime() {
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing time");
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
    Serial.printf("\nTime synced: %02d:%02d\n", ti->tm_hour, ti->tm_min);
  } else {
    Serial.println("\nTime sync failed.");
  }
}

void connectSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("Connecting to Supabase...");
  realtime.begin(SUPABASE_URL, SUPABASE_KEY, HandleChanges);
  realtime.login_email(USER_EMAIL, USER_PASS);
  realtime.addChangesListener("pump_motor", "*", "public", "");
  realtime.listen();

  supabaseConnected = true;
  Serial.println("Supabase connected.");
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

  

  bool wifiOK = connectWiFiBlocking();
  if (wifiOK) {
    wifiWasConnected = true;
    initTime();
    connectSupabase();
  }

  Serial.println("Setup complete.");
}

void loop() {
    
  VOLTAGE = pzem1.voltage();
  VOLTAGE = zeroIfNan(VOLTAGE);
  CURRENT = pzem1.current();
  CURRENT = zeroIfNan(CURRENT);
  POWER = pzem1.power();
  POWER = zeroIfNan(POWER);

  // Serial.printf("Voltage        : %.2f\ V\n", VOLTAGE);
  // Serial.printf("Current        : %.2f\ A\n", CURRENT);
  // Serial.printf("Power Active   : %.2f\ W\n", POWER);
  
  unsigned long now = millis();
    if(now - lastVoltageRead > VOLTAGE_READ_INTERVAL){
    lastVoltageRead = now;
    readVoltage();
    Serial.printf("PZEM: VOLTAGE=%.2f V, CURRENT=%.2f A, POWER=%.2f W\n", VOLTAGE, CURRENT, POWER);
  }
  if (supabaseConnected && WiFi.status() == WL_CONNECTED) {
    realtime.loop();
  }

  handleMotorRun();

  wifiReconnectNonBlocking();

  int ot_sensorstatus = digitalRead(ot_sensor);
  int buttonState = digitalRead(input1);

  if (ot_sensorstatus == LOW) {
    displayTankFull();
  } else if (motorRunning) {
    displayMotorRunning();
  } else {
    displayIdle();
  }

  if (ot_sensorstatus == LOW) {
    if (motorRunning || motorData.state) {
      Serial.println("Tank full — stopping motor!");
      int lowCount = 0;
      for (int i = 0; i < 10; ++i) {
        if (digitalRead(ot_sensor) == LOW) lowCount++;
        delay(500);
      }
      Serial.printf("OT checks lowCount=%d\n", lowCount);
      if (lowCount >= 5) {
        stopMotorImmediate();
        motorData.state = false;
        saveMotorToEEPROM();
      } else {
        Serial.println("OT NOT confirmed - ignoring.");
      }
    }
  }

  if (buttonState == LOW) {
    Serial.println("Manual button pressed!");
    delay(50);
    while (digitalRead(input1) == LOW) delay(20);

    int otstatus_now = digitalRead(ot_sensor);

    if (otstatus_now == HIGH) {
      motorData.state = !motorData.state;

      if (motorData.state) {
        manualStopRequested = false;
        if (!motorRunning) {
          unsigned long durMs = (unsigned long)manual_duration * 60000UL;
          startMotorForDuration(durMs);
        }
      } else {
        manualStopRequested = true;
        stopMotorImmediate();
      }
      saveMotorToEEPROM();
    } else {
      Serial.println("Tank full — manual ON blocked!");
      manualStopRequested = true;
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

    if (ot_sensorstatus == HIGH && !motorRunning) {
      if (motorData.sch1_en && timeMatches(motorData.sch1_start, currentTime)) {
        startMotorForDuration((unsigned long)motorData.sch1_duration * 60000UL);
      } else if (motorData.sch2_en && timeMatches(motorData.sch2_start, currentTime)) {
        startMotorForDuration((unsigned long)motorData.sch2_duration * 60000UL);
      } else if (motorData.sch3_en && timeMatches(motorData.sch3_start, currentTime)) {
        startMotorForDuration((unsigned long)motorData.sch3_duration * 60000UL);
      }
    }

    if (motorData.state && !motorRunning && ot_sensorstatus == HIGH) {
      digitalWrite(MOTOR_PIN, HIGH);
    } else if ((!motorData.state || ot_sensorstatus == LOW) && !motorRunning) {
      digitalWrite(MOTOR_PIN, LOW);
    }

    
    
  }

  delay(50);
}

// float zeroIfNan(float v) 
// {
//   if (isnan(v)) 
//   v = 0;
//   return v;
// }