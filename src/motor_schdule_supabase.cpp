// FULL UPDATED CODE WITH SCHEDULE RESUME + OT CANCEL LOGIC (updated copy)
// --------------------------------------------------------------------
// Merged fixes: persistent scheduledRemainingMs, periodic checkpointing,
// restore on boot, OT cancel clears persisted schedule, ArduinoJson fix, etc.

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

// NEW: Resume scheduled run after power failure
unsigned long scheduledRemainingMs = 0;
bool scheduleInProgress = false;

// Filesystem / EEPROM layout (addresses)
const int MANUAL_ADDR = 100;        // keep existing manual duration byte
const int SCHEDULE_MAGIC_ADDR = 200; // single byte magic
const int SCHEDULE_REMAIN_ADDR = 201; // unsigned long (4 or 8 bytes depending)
const int SCHEDULE_FLAG_ADDR = 205;  // single byte flag
const byte SCHEDULE_MAGIC = 0x42;

int manual_duration = 30;
bool manualStopRequested = false;

// Save interval to avoid flashing EEPROM too often
const unsigned long SCHEDULE_SAVE_INTERVAL_MS = 5000;
unsigned long lastScheduleSaveMs = 0;

// ---------- SAVE / LOAD ----------
void saveMotorToEEPROM() {
  EEPROM.put(0, motorData);
  EEPROM.commit();
}

void loadMotorFromEEPROM() {
  // Ensure struct version fits in EEPROM size
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

// Persist scheduled state: magic (1 byte), remaining (unsigned long), flag (1 byte)
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
    // no saved schedule
    scheduledRemainingMs = 0;
    scheduleInProgress = false;
    return;
  }
  EEPROM.get(SCHEDULE_REMAIN_ADDR, scheduledRemainingMs);
  byte flag = EEPROM.read(SCHEDULE_FLAG_ADDR);
  scheduleInProgress = (flag == 1);
  // Sanity checks
  if (scheduledRemainingMs > 24UL * 3600UL * 1000UL) { // more than 24 hrs -> ignore
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

// ---------- TIME MATCH ----------
bool timeMatches(const char* scheduledTime, const char* currentTime) {
  // scheduledTime expected "HH:MM"
  if (scheduledTime == nullptr || scheduledTime[0] == '\0') return false;
  return strcmp(scheduledTime, currentTime) == 0;
}

// ---------- DISPLAY ----------
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
  unsigned long remainingMin = (motorRunDuration + 59999UL) / 60000UL; // round up display
  if (motorRunDuration > elapsedMs) {
    unsigned long remMs = motorRunDuration - elapsedMs;
    remainingMin = (remMs + 59999UL) / 60000UL;
  } else {
    remainingMin = 0;
  }
  if (remainingMin > 99) remainingMin = 99;
  display.showNumberDec(remainingMin, true);
}

// ---------- MOTOR CONTROL ----------
void startMotorForDuration(unsigned long durationMs) {
  manualStopRequested = false;
  motorRunning = true;
  motorStartTime = millis();
  motorRunDuration = durationMs;
  digitalWrite(MOTOR_PIN, HIGH);
  displayMotorRunning();

  // Mark as state true so remote/persisted state reflects motor running
  motorData.state = true;
  saveMotorToEEPROM();
}

void stopMotorImmediate() {
  motorRunning = false;
  motorRunDuration = 0;
  digitalWrite(MOTOR_PIN, LOW);
  displayIdle();
  // If we stopped manually, clear scheduled state if it was scheduled
  clearScheduledState();
}

// UPDATED: Track remaining scheduled time, checkpoint to EEPROM periodically
void handleMotorRun() {
  if (manualStopRequested) {
    if (motorRunning) stopMotorImmediate();
    manualStopRequested = false;
    return;
  }

  if (motorRunning) {
    unsigned long elapsedMs = millis() - motorStartTime;

    // NEW: Store remaining time for resume only if scheduleInProgress
    if (scheduleInProgress) {
      if (motorRunDuration > elapsedMs)
        scheduledRemainingMs = motorRunDuration - elapsedMs;
      else
        scheduledRemainingMs = 0;

      // checkpoint to EEPROM periodically
      if (millis() - lastScheduleSaveMs >= SCHEDULE_SAVE_INTERVAL_MS) {
        saveScheduledState();
      }
    }

    if (elapsedMs >= motorRunDuration) {
      // run finished
      stopMotorImmediate();
      motorData.state = false;
      saveMotorToEEPROM();

      scheduleInProgress = false;
      scheduledRemainingMs = 0;
      clearScheduledState();
    }
  }
}

// ---------- SUPABASE CALLBACK ----------
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

  // If remote says state true and local is not running, start motor for appropriate duration
  // Note: remote doesn't tell how long — scheduled runs will start from schedule checks.
  if (motorData.state && !motorRunning) {
    // Do not forcibly start motor unless schedule logic or manual button requested.
    // Keep pin state consistent if not running
    digitalWrite(MOTOR_PIN, HIGH);
  } else if (!motorData.state && !motorRunning) {
    digitalWrite(MOTOR_PIN, LOW);
  }
}

// ---------- WIFI / TIME / SUPABASE ----------
const char* WIFI_SSID = "Anupam";
const char* WIFI_PASS = "12345678";

const char* SUPABASE_URL = "https://gzcpvuueeexndnvwoanw.supabase.co";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Imd6Y3B2dXVlZWV4bmRudndvYW53Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjE1NzA2OTIsImV4cCI6MjA3NzE0NjY5Mn0.lW_6KKWeUF1l7qq4RAQvJsAmrdQetLenL5O8LYH62Ek";
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

// ---------- SETUP ----------
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
  loadScheduledState(); // <- load persisted schedule info

  if (connectWiFiBlocking()) {
    wifiWasConnected = true;
    initTime();
    connectSupabase();
  }

  // NEW: Resume scheduled run after power restore only if OT sensor is HIGH (no tank full)
  if (scheduleInProgress && scheduledRemainingMs > 0) {
    if (digitalRead(ot_sensor) == HIGH) {
      startMotorForDuration(scheduledRemainingMs);
      // ensure saved flag is present
      saveScheduledState();
    } else {
      // Tank is full; do not start. Keep persisted schedule so it may be resumed later.
      Serial.println("Persisted scheduled run found but OT sensor is LOW - not starting.");
    }
  }
}

// ---------- LOOP ----------
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

  // ---------- NEW: OT cancels schedule permanently ----------
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
        clearScheduledState(); // also clear persisted schedule
      }
    }
  }

  // ---------- MANUAL BUTTON ----------
  if (buttonState == LOW) {
    delay(50);
    while (digitalRead(input1) == LOW) delay(20);

    if (digitalRead(ot_sensor) == HIGH) {
      motorData.state = !motorData.state;

      if (motorData.state && !motorRunning) {
        // start manual run for manual_duration
        scheduleInProgress = false;
        scheduledRemainingMs = 0;
        clearScheduledState(); // manual run should not be resumed as scheduled
        startMotorForDuration((unsigned long)manual_duration * 60000UL);
      } else if (!motorData.state) {
        // stop requested
        manualStopRequested = true;
        stopMotorImmediate();
      }

      saveMotorToEEPROM();
    }
  }

  digitalWrite(ot_status, ot_sensorstatus == LOW ? HIGH : LOW);

  // ---------- SCHEDULE CHECK ----------
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
