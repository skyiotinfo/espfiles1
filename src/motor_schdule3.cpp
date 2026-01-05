/* Full corrected sketch
   - Prevent repeated auto-restarts
   - Keep motor state when WiFi disconnects
   - Fetch schedules on reconnect / once per minute when online
   - Per-schedule blocked flag set when motor stopped manually or by OT
   - Block cleared only after schedule end (so it won't restart mid-window)
   - Safe timestamping using NTP or RTC fallback
   - Uses SoftwareSerial for PZEM (no deprecation warning)
*/

#include <PZEM004Tv30.h>
#include <SoftwareSerial.h>
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
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800); // IST offset

// ----------------- PZEM (SoftwareSerial) -----------------
SoftwareSerial pzemSerial(D5, D6); // RX, TX pins (swap if needed)
PZEM004Tv30 pzem1(pzemSerial);
float VOLTAGE = 0, CURRENT = 0, POWER = 0;
unsigned long lastVoltageRead = 0;
const unsigned long VOLTAGE_READ_INTERVAL = 2000;

// ----------------- Display -----------------
#define CLK D3
#define DIO D4
TM1637Display display(CLK, DIO);
uint8_t blank[] = {0x00,0x00,0x00,0x00};

// ----------------- Pins -----------------
const int ot_sensor       = D1;  // over-temperature or external sensor
const int buzzer          = D8;
const int ot_status       = D7;
const int auto_status     = D2;
const int input1          = D0;  // manual button (use a safe pin)
const int wifiBtnThreshold = 100;

// ----------------- EEPROM -----------------
#define EEPROM_SIZE 512
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

// ----------------- Variables -----------------
int motor_status = 0;          // 0 stopped, 1 running
int prev_motor_status = 0;
int motor_time = 0;            // remaining seconds
int motor_duration = 30;       // default minutes
int currentScheduleIndex = -1;

bool scheduleCompleted[3] = {false, false, false}; // marks if schedule executed naturally
bool scheduleBlocked[3]   = {false, false, false}; // marks if schedule was blocked (manual/OT stop)
int numSchedules = 0;

bool manualMode = true;   // start in manual mode to be safe
unsigned long lastFirebaseFetchTime = 0;
const unsigned long firebaseFetchInterval = 60 * 1000UL; // 1 minute
const unsigned long wifiReconnectInterval = 10 * 1000UL; // 10s
unsigned long lastWiFiReconnectAttempt = 0;

const unsigned long HEARTBEAT_INTERVAL = 30 * 1000UL;
unsigned long lastHeartbeatTime = 0;

const unsigned long VOLTAGE_SEND_INTERVAL = 30 * 1000UL;
unsigned long lastVoltageSend = 0;

unsigned long lastMotorTimeUpdate = 0;
unsigned long lastVoltageReadMillis = 0;

unsigned long otLowSince = 0;

// track wifi connection state to handle reconnect-only fetch
bool wifiConnectedLast = false;

// WiFiManager
WiFiManager wm;

// ----------------- Helpers -----------------
float zeroIfNan(float v){ return isnan(v) ? 0.0f : v; }

String getTimestampSafe() {
  char buf[64] = "unknown";
  // Try NTP first (only meaningful if WiFi connected)
  if (WiFi.status() == WL_CONNECTED) {
    if (timeClient.update()) {
      time_t raw = (time_t) timeClient.getEpochTime();
      struct tm *tm_info = localtime(&raw);
      if (tm_info != nullptr) {
        strftime(buf, sizeof(buf), "%d %b %Y %I:%M:%S %p", tm_info);
        return String(buf);
      }
    }
  }
  // Fallback to RTC
  tmElements_t tm;
  if (RTC.read(tm)) {
    struct tm tm_info;
    tm_info.tm_sec  = tm.Second;
    tm_info.tm_min  = tm.Minute;
    tm_info.tm_hour = tm.Hour;
    tm_info.tm_mday = tm.Day;
    tm_info.tm_mon  = tm.Month - 1;
    // tm.Year is years since 1970? TimeLib uses years since 2000 via tmYearToY2k
    tm_info.tm_year = tmYearToY2k(tm.Year) + 100; // convert to years since 1900
    if (strftime(buf, sizeof(buf), "%d %b %Y %I:%M:%S %p", &tm_info) > 0) {
      return String(buf);
    }
  }
  return String("unknown");
}

void safeWriteEEPROM(int addr, uint8_t val){
  if (EEPROM.read(addr) != val) {
    EEPROM.write(addr, val);
  }
}

// ----------------- Voltage / PZEM -----------------
void readVoltage() {
  VOLTAGE = zeroIfNan(pzem1.voltage());
  CURRENT = zeroIfNan(pzem1.current());
  POWER   = zeroIfNan(pzem1.power());
}

// ----------------- Schedule helpers -----------------
void readSchedule(int idx, int &h, int &m, int &d) {
  int base = (idx == 0) ? SCHED1_ADDR_HOUR : (idx == 1) ? SCHED2_ADDR_HOUR : SCHED3_ADDR_HOUR;
  h = EEPROM.read(base);
  m = EEPROM.read(base + 1);
  d = EEPROM.read(base + 2);
  if (h > 23 || h == 255) h = 0;
  if (m > 59 || m == 255) m = 0;
  if (d < 1 || d > 240 || d == 255) d = 30;
}

int findMatchingSchedule(int ch, int cm, int &remainingSec) {
  int nowSec = ch*3600 + cm*60;
  for (int i=0;i<numSchedules;i++){
    int h,m,d; readSchedule(i,h,m,d);
    int startSec = h*3600 + m*60;
    int endSec = startSec + d*60;
    if (nowSec >= startSec && nowSec < endSec) {
      remainingSec = endSec - nowSec;
      return i;
    }
  }
  remainingSec = 0;
  return -1;
}

// ----------------- Firebase functions -----------------
void getScheduleFromFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  String url = String("https://") + firebaseHost + getPath;
  if (!https.begin(client, url)) return;
  int code = https.GET();
  if (code == 200) {
    String payload = https.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc.containsKey("schedules")) {
      JsonArray arr = doc["schedules"];
      numSchedules = arr.size();
      for (int i=0;i<3 && i<arr.size(); i++){
        int sh = arr[i]["startHour"] | 0;
        int sm = arr[i]["startMin"]  | 0;
        int du = arr[i]["duration"]  | 30;
        int base = (i==0? SCHED1_ADDR_HOUR : (i==1? SCHED2_ADDR_HOUR : SCHED3_ADDR_HOUR));
        safeWriteEEPROM(base, (uint8_t)sh);
        safeWriteEEPROM(base+1, (uint8_t)sm);
        safeWriteEEPROM(base+2, (uint8_t)du);
      }
      EEPROM.commit();
      Serial.println("Schedules updated from Firebase");
    } else {
      Serial.println("No 'schedules' key or JSON error");
    }
  } else {
    Serial.printf("getScheduleFromFirebase failed code=%d\n", code);
  }
  https.end();
}

void postMotorStatusChange(int status) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("postMotorStatusChange: offline, skipping");
    return;
  }
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  String url = String("https://") + firebaseHost1 + statusPostPath;
  if (!https.begin(client, url)) return;
  https.addHeader("Content-Type", "application/json");
  String ts = getTimestampSafe();
  String body = "{\"motor_status\":" + String(status) + ",\"timestamp\":\"" + ts + "\"}";
  int code = https.PUT(body);
  Serial.printf("Motor status PUT code: %d\n", code);
  https.end();
}

void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("sendHeartbeat: offline, skipping");
    return;
  }
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  String url = String("https://") + firebaseHost1 + HeartbeatPostPath;
  if (!https.begin(client, url)) return;
  https.addHeader("Content-Type", "application/json");
  String ts = getTimestampSafe();
  String body = "{\"last_active\":\"" + ts + "\"}";
  int code = https.PUT(body);
  Serial.printf("Heartbeat PUT code: %d\n", code);
  https.end();
}

void sendVoltageData() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  String url = String("https://") + firebaseHost1 + "/user/uid/1001/pzem.json";
  if (!https.begin(client, url)) return;
  https.addHeader("Content-Type", "application/json");
  String body = "{\"voltage\":" + String(VOLTAGE,2) + ",\"current\":" + String(CURRENT,2) + ",\"power\":" + String(POWER,2) + "}";
  int code = https.PUT(body);
  Serial.printf("PZEM data PUT code: %d\n", code);
  https.end();
}

// ----------------- Motor control -----------------
void stopMotor(bool blocked=true) {
  motor_status = 0;
  digitalWrite(buzzer, LOW);
  digitalWrite(auto_status, LOW);
  display.showNumberDec(0, false);
  motor_time = motor_duration * 60;
  if (currentScheduleIndex != -1) {
    scheduleCompleted[currentScheduleIndex] = true;
    if (blocked) scheduleBlocked[currentScheduleIndex] = true;
  }
  Serial.println("Motor stopped (local)");
  // post status if connected
  postMotorStatusChange(motor_status);
  currentScheduleIndex = -1;
}

void startMotorWithSeconds(int seconds, int scheduleIndex = -1) {
  motor_status = 1;
  motor_time = seconds;
  digitalWrite(buzzer, HIGH);
  digitalWrite(auto_status, HIGH);
  currentScheduleIndex = scheduleIndex;
  Serial.printf("Motor started for %d seconds (schedule %d)\n", seconds, scheduleIndex);
  postMotorStatusChange(motor_status);
}

// ----------------- Setup -----------------
void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  display.setBrightness(0x0f);
  display.setSegments(blank);

  // Safe pin defaults
  pinMode(input1, INPUT_PULLUP);
  pinMode(buzzer, OUTPUT);
  pinMode(auto_status, OUTPUT);
  pinMode(ot_status, OUTPUT);
  pinMode(ot_sensor, INPUT_PULLUP);

  digitalWrite(buzzer, LOW);
  digitalWrite(auto_status, LOW);
  digitalWrite(ot_status, LOW);

  // Load motor duration
  int dur = EEPROM.read(MOTOR_DURATION_ADDR);
  if (dur <= 0 || dur > 240 || dur == 255) {
    motor_duration = 30;
    EEPROM.write(MOTOR_DURATION_ADDR, motor_duration);
    EEPROM.commit();
  } else motor_duration = dur;
  motor_time = motor_duration * 60;

  // start pzem serial & NTP
  pzemSerial.begin(9600);
  timeClient.begin();

  // Try to auto-connect WiFi with saved creds
  if (WiFi.SSID() != "") {
    Serial.println("Trying saved WiFi...");
    WiFi.begin();
    unsigned long start = millis();
    while (millis() - start < 5000 && WiFi.status() != WL_CONNECTED) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connected at setup");
      manualMode = false;
      wifiConnectedLast = true;
      getScheduleFromFirebase();
    } else {
      Serial.println("No saved WiFi or connect failed");
      wifiConnectedLast = false;
    }
  }

  lastFirebaseFetchTime = millis();
  lastHeartbeatTime = millis();
  lastVoltageSend = millis();
  prev_motor_status = motor_status;
}

// ----------------- Loop -----------------
void loop() {
  unsigned long now = millis();

  // Read PZEM periodically
  if (now - lastVoltageReadMillis >= VOLTAGE_READ_INTERVAL) {
    lastVoltageReadMillis = now;
    readVoltage();
  }

  // OT sensor (immediate stop if low for 2s)
  int otVal = digitalRead(ot_sensor);
  if (otVal == LOW) {
    if (otLowSince == 0) otLowSince = now;
    if (now - otLowSince >= 2000) {
      Serial.println("OT sensor triggered -> stop");
      stopMotor(true); // blocked so schedule won't restart
      otLowSince = 0;
    }
  } else otLowSince = 0;

  // Handle manual button (edge detect)
  static bool lastBtn = HIGH;
  bool curBtn = digitalRead(input1);
  if (lastBtn == HIGH && curBtn == LOW) {
    if (motor_status == 0) {
      // Manual start
      startMotorWithSeconds(motor_duration * 60, -1);
      manualMode = true; // treat as manual
      Serial.println("Manual START pressed");
    } else {
      // Manual stop
      stopMotor(true);
      manualMode = true;
      Serial.println("Manual STOP pressed");
    }
  }
  lastBtn = curBtn;

  // Manage WiFi reconnect attempts (non-blocking)
  if (WiFi.status() != WL_CONNECTED && (now - lastWiFiReconnectAttempt >= wifiReconnectInterval)) {
    if (WiFi.SSID() != "") {
      Serial.println("Attempting WiFi reconnect...");
      WiFi.begin();
    }
    lastWiFiReconnectAttempt = now;
  }

  // Detect reconnect event: only fetch schedules once when reconnected
  if (WiFi.status() == WL_CONNECTED && !wifiConnectedLast) {
    Serial.println("WiFi reconnected -> fetching schedules once");
    wifiConnectedLast = true;
    manualMode = false;
    getScheduleFromFirebase();
    lastFirebaseFetchTime = now;
  } else if (WiFi.status() != WL_CONNECTED && wifiConnectedLast) {
    Serial.println("WiFi went offline -> will hold motor state locally");
    wifiConnectedLast = false;
    // Do NOT change motor relays — keep state
  }

  // Send heartbeat periodically only when connected
  if (WiFi.status() == WL_CONNECTED && now - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeatTime = now;
  }

  // Send voltage data periodically only when connected
  if (WiFi.status() == WL_CONNECTED && now - lastVoltageSend >= VOLTAGE_SEND_INTERVAL) {
    lastVoltageSend = now;
    sendVoltageData();
  }

  // Regular schedule fetch every minute if online
  if (WiFi.status() == WL_CONNECTED && !manualMode && motor_status == 0 && now - lastFirebaseFetchTime >= firebaseFetchInterval) {
    getScheduleFromFirebase();
    lastFirebaseFetchTime = now;
  }

  // Determine current time (NTP preferred, RTC fallback)
  int currH = 0, currM = 0;
  if (timeClient.update()) {
    currH = timeClient.getHours();
    currM = timeClient.getMinutes();
  } else {
    tmElements_t tm;
    if (RTC.read(tm)) {
      currH = tm.Hour;
      currM = tm.Minute;
    } else {
      // fallback: keep 0/0 (safe)
      currH = 0; currM = 0;
    }
  }

  // Reset scheduleCompleted/blocked only AFTER schedule end (so blocking remains for the window)
  for (int i = 0; i < numSchedules; i++) {
    int sh, sm, sd; readSchedule(i, sh, sm, sd);
    int startSec = sh*3600 + sm*60;
    int endSec   = startSec + sd*60;
    int nowSec   = currH*3600 + currM*60;
    if (nowSec >= endSec) {
      scheduleCompleted[i] = false;
      scheduleBlocked[i] = false;
    }
    // Note: if nowSec < startSec, we don't touch blocked/completed so next window can run normally
  }

  // Auto-schedule: start only when:
  // - Not in manualMode
  // - There's a matching schedule
  // - That schedule is not completed and not blocked
  // - Motor currently stopped
  int remain = 0;
  int match = (!manualMode) ? findMatchingSchedule(currH, currM, remain) : -1;
  if (!manualMode && match != -1 && !scheduleCompleted[match] && !scheduleBlocked[match] && motor_status == 0) {
    // start with remaining seconds
    startMotorWithSeconds(remain, match);
    scheduleCompleted[match] = true; // mark executed
    Serial.printf("Auto-start schedule %d, remaining %d seconds\n", match+1, remain);
  }

  // Motor countdown (1-second resolution)
  if (motor_status == 1) {
    if (now - lastMotorTimeUpdate >= 1000) {
      lastMotorTimeUpdate = now;
      motor_time--;
    }
    int remMin = motor_time / 60;
    int remSec = motor_time % 60;
    int disp = remMin * 100 + remSec;
    display.showNumberDecEx(disp, 0b01000000);

    // If motor_time reached zero, stop (natural completion)
    if (motor_time <= 0) {
      stopMotor(false); // natural stop -> not blocked (allow schedule to be marked completed but not blocked)
    }
  }

  // Update motor status change (log once when changed)
  if (motor_status != prev_motor_status) {
    Serial.printf("Motor state changed %d -> %d\n", prev_motor_status, motor_status);
    prev_motor_status = motor_status;
    // Already posted in start/stop helper, so nothing else required here.
  }

  delay(10);
}
