#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ── WiFi ─────────────────────────────────────────────────────────
const char* ssid     = "anupam";
const char* password = "12345678";

// ── Supabase config ───────────────────────────────────────────────
const char* SUPABASE_URL = "https://nntytldbmnjraytdmasp.supabase.co";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im5udHl0bGRibW5qcmF5dGRtYXNwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjEyODc0MTIsImV4cCI6MjA3Njg2MzQxMn0.lcleFnldZ-6E2Qr0UWQqBcMGBHX3j2l_ldApX0gmnfc";
const int   CUST_ID      = 101001;

// ── Device ID map in node_data table ─────────────────────────────
// 101        = motor
// 201-208    = sdevice[0] to sdevice[7] (tanks)
// 301        = heartbeat
// 401        = motor_command (remote ON/OFF from app)

// ── Device state arrays ──────────────────────────────────────────
int sdevice[8]      = {0};
int prev_sdevice[8] = {0};
String etime[8];
String ftime[8];

// ── Motor state ──────────────────────────────────────────────────
char motor_st      = 'S';
char prev_motor_st = 'S';

// ── Pin definitions ──────────────────────────────────────────────
const int mt_st = D8;
int mt_status   = 0;
const int sled  = D7;

// ── Non-blocking LED blink state ─────────────────────────────────
unsigned long lastLedToggle = 0;
bool ledState               = false;

// ── Motor timeout (5 minutes) ────────────────────────────────────
unsigned long mt_last_active_time = 0;
const unsigned long MT_TIMEOUT    = 5 * 60 * 1000;

// ── Misc globals ─────────────────────────────────────────────────
int wificonnect = 0;

unsigned long lastSerialReceivedTime = 0;
unsigned long lastGetTime            = 0;
const unsigned long GET_INTERVAL     = 20000;

String motorOnTime  = "0000-00-00 00:00:00";
String motorOffTime = "0000-00-00 00:00:00";

const unsigned long HEARTBEAT_INTERVAL = 30000;
unsigned long lastHeartbeatTime        = 0;

// ── Forward declarations ─────────────────────────────────────────
void sendDataToSupabase();
void checkWiFiConnection();
String getCurrentTime();
void readMotorStatusFromSupabase();
void readInitialSupabaseData();
void updateDeviceInSupabase(int deviceIndex);
void sendHeartbeat();
bool hasDeviceOrMotorStateChanged();
int  supabasePatch(int deviceId, const String& jsonBody);
String supabaseGet(int deviceId, const String& selectFields);

// ─────────────────────────────────────────────────────────────────
// HTTP HELPERS
// ─────────────────────────────────────────────────────────────────

int supabasePatch(int deviceId, const String& jsonBody) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = String(SUPABASE_URL)
             + "/rest/v1/node_data?cust_id=eq." + CUST_ID
             + "&device_id=eq." + deviceId;

  https.begin(client, url);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("apikey",        SUPABASE_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  https.addHeader("Prefer",        "return=minimal");

  int code = https.PATCH(jsonBody);
  https.end();
  return code;
}

String supabaseGet(int deviceId, const String& selectFields) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = String(SUPABASE_URL)
             + "/rest/v1/node_data?cust_id=eq." + CUST_ID
             + "&device_id=eq." + deviceId
             + "&select=" + selectFields;

  https.begin(client, url);
  https.addHeader("apikey",        SUPABASE_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  int code = https.GET();
  String payload = "";
  if (code > 0) payload = https.getString();
  https.end();
  return payload;
}

// ─────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(9600);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi.");
  }

  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  pinMode(mt_st, OUTPUT);
  digitalWrite(mt_st, mt_status);

  pinMode(sled, OUTPUT);
  digitalWrite(sled, LOW);

  readInitialSupabaseData();
  readMotorStatusFromSupabase();

  if (mt_status == 1) mt_last_active_time = millis();
}

// ─────────────────────────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────────────────────────
void loop() {
  checkWiFiConnection();
  wificonnect = (WiFi.status() == WL_CONNECTED) ? 1 : 0;

  // ── Read serial data from main controller ──
  if (Serial.available()) {
    delay(100);
    String received = Serial.readStringUntil('\r');
    received.trim();

    bool valid = (received.length() == 9);
    if (valid) {
      for (int i = 0; i < 8; i++) {
        char c = received.charAt(i);
        if (c != '0' && c != '1') { valid = false; break; }
      }
      char lastChar = received.charAt(8);
      if (lastChar != 'R' && lastChar != 'S') valid = false;
    }

    if (valid) {
      lastSerialReceivedTime = millis();

      for (int i = 0; i < 8; i++) {
        int newVal = received.charAt(i) - '0';
        if (newVal != sdevice[i]) {
          sdevice[i] = newVal;
          String currentTime = getCurrentTime();
          if (newVal == 1) etime[i] = currentTime;
          else             ftime[i] = currentTime;
          // ── REALTIME: immediate per-device Supabase update ──
          updateDeviceInSupabase(i);
        }
      }

      char newStatus = received.charAt(8);
      if (newStatus != motor_st) {
        prev_motor_st = motor_st;
        motor_st      = newStatus;
        mt_status     = (motor_st == 'R') ? 1 : 0;
        digitalWrite(mt_st, mt_status);
        mt_last_active_time = millis();

        if (prev_motor_st == 'S' && motor_st == 'R') motorOnTime  = getCurrentTime();
        else if (prev_motor_st == 'R' && motor_st == 'S') motorOffTime = getCurrentTime();

        // ── REALTIME: send motor update immediately on change ──
        sendDataToSupabase();
      }
    }
  }

  // ── REALTIME FIX: non-blocking LED blink ──────────────────────
  if (WiFi.status() == WL_CONNECTED) {
    if (millis() - lastLedToggle >= 300) {
      ledState = !ledState;
      digitalWrite(sled, ledState);
      lastLedToggle = millis();
    }
  } else {
    digitalWrite(sled, LOW);
  }

  // ── Motor timeout watchdog ──
  if (mt_status == 1 && (millis() - mt_last_active_time) > MT_TIMEOUT) {
    Serial.println("Motor timeout. Turning off.");
    mt_status    = 0;
    motor_st     = 'S';
    digitalWrite(mt_st, mt_status);
    motorOffTime = getCurrentTime();
    sendDataToSupabase();
  }

  // ── Poll motor command from Supabase every GET_INTERVAL ──
  if ((millis() - lastGetTime) >= GET_INTERVAL) {
    readMotorStatusFromSupabase();
    lastGetTime = millis();
  }

  // ── Heartbeat every HEARTBEAT_INTERVAL ──
  if ((millis() - lastHeartbeatTime) >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeatTime = millis();
  }

  // ── REALTIME FIX: no blocking delay() here ───────────────────
}

// ─────────────────────────────────────────────────────────────────
// STATE CHANGE CHECK
// ─────────────────────────────────────────────────────────────────
bool hasDeviceOrMotorStateChanged() {
  for (int i = 0; i < 8; i++) {
    if (sdevice[i] != prev_sdevice[i]) return true;
  }
  return motor_st != prev_motor_st;
}

// ─────────────────────────────────────────────────────────────────
// TIME HELPER
// ─────────────────────────────────────────────────────────────────
String getCurrentTime() {
  time_t now = time(nullptr);
  struct tm* p_tm = localtime(&now);
  char timeStr[64];
  if (p_tm) {
    snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d",
             p_tm->tm_year + 1900, p_tm->tm_mon + 1, p_tm->tm_mday,
             p_tm->tm_hour, p_tm->tm_min, p_tm->tm_sec);
    return String(timeStr);
  }
  return "0000-00-00 00:00:00";
}

// ─────────────────────────────────────────────────────────────────
// WIFI RECONNECT
// ─────────────────────────────────────────────────────────────────
void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Reconnecting...");
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
      delay(500);
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) Serial.println("\nReconnected.");
    else Serial.println("\nReconnection failed.");
  }
}

// ─────────────────────────────────────────────────────────────────
// HEARTBEAT  →  device_id = 301
// ─────────────────────────────────────────────────────────────────
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;
  String currentTime = getCurrentTime();
  String jsonBody = "{\"current_data\":\"" + currentTime
                  + "\",\"update_time\":\"" + currentTime + "\"}";
  Serial.println("Heartbeat: " + jsonBody);
  int code = supabasePatch(301, jsonBody);
  Serial.println(code > 0 ? "Heartbeat OK: " + String(code) : "Heartbeat fail: " + String(code));
}

// ─────────────────────────────────────────────────────────────────
// MOTOR ROW  →  device_id = 101
// ─────────────────────────────────────────────────────────────────
void sendDataToSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;

  String motorData = String(motor_st) + "|" + motorOnTime + "|" + motorOffTime;
  String jsonBody  = "{\"current_state\":" + String(mt_status)
                   + ",\"current_data\":\"" + motorData
                   + "\",\"update_time\":\"" + getCurrentTime() + "\"}";

  Serial.println("Motor update: " + jsonBody);
  int code = supabasePatch(101, jsonBody);

  if (code > 0) {
    Serial.println("Motor update OK: " + String(code));
    for (int i = 0; i < 8; i++) prev_sdevice[i] = sdevice[i];
    prev_motor_st = motor_st;
  } else {
    Serial.println("Motor update fail: " + String(code));
  }
}

// ─────────────────────────────────────────────────────────────────
// DEVICE ROW  →  device_id = 201-208
// ─────────────────────────────────────────────────────────────────
void updateDeviceInSupabase(int deviceIndex) {
  if (WiFi.status() != WL_CONNECTED) return;

  int deviceId    = 201 + deviceIndex;
  String devData  = etime[deviceIndex] + "|" + ftime[deviceIndex];
  String jsonBody = "{\"current_state\":" + String(sdevice[deviceIndex])
                  + ",\"current_data\":\"" + devData
                  + "\",\"update_time\":\"" + getCurrentTime() + "\"}";

  Serial.println("Device " + String(deviceId) + ": " + jsonBody);
  int code = supabasePatch(deviceId, jsonBody);
  Serial.println(code > 0 ? "Device OK: " + String(code) : "Device fail: " + String(code));
}

// ─────────────────────────────────────────────────────────────────
// READ MOTOR COMMAND  →  device_id = 401
// ─────────────────────────────────────────────────────────────────
void readMotorStatusFromSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;

  String payload = supabaseGet(401, "current_state");
  int index = payload.indexOf("\"current_state\":");
  if (index != -1) {
    int startIndex = index + 16;
    int endIndex   = payload.indexOf("}", startIndex);
    String val     = payload.substring(startIndex, endIndex);
    val.replace("\"", "");
    val.trim();

    int new_mt_status = val.toInt();
    if (new_mt_status != mt_status) {
      prev_motor_st = motor_st;
      mt_status     = new_mt_status;
      motor_st      = (mt_status == 1) ? 'R' : 'S';
      digitalWrite(mt_st, mt_status);
      mt_last_active_time = millis();

      if (motor_st == 'R') motorOnTime  = getCurrentTime();
      else                  motorOffTime = getCurrentTime();

      Serial.print("Remote motor command. New state: ");
      Serial.println(motor_st);
      // ── REALTIME: push update immediately after remote command ──
      sendDataToSupabase();
    }
  } else {
    Serial.println("Motor command: no data or parse error.");
  }
}

// ─────────────────────────────────────────────────────────────────
// BOOT RESTORE  →  device_id = 101
// ─────────────────────────────────────────────────────────────────
void readInitialSupabaseData() {
  if (WiFi.status() != WL_CONNECTED) return;

  String payload = supabaseGet(101, "current_state,current_data");
  Serial.println("Boot restore: " + payload);

  int stIdx = payload.indexOf("\"current_state\":");
  if (stIdx != -1) {
    int s      = stIdx + 16;
    String val = payload.substring(s, payload.indexOf(",", s));
    val.trim();
    mt_status = val.toInt();
    motor_st  = (mt_status == 1) ? 'R' : 'S';
    digitalWrite(mt_st, mt_status);
  }

  int cdIdx = payload.indexOf("\"current_data\":\"");
  if (cdIdx != -1) {
    int start = cdIdx + 16;
    int end   = payload.indexOf("\"", start);
    String cd = payload.substring(start, end);
    int pipe1 = cd.indexOf("|");
    int pipe2 = cd.indexOf("|", pipe1 + 1);
    if (pipe1 != -1 && pipe2 != -1) {
      motorOnTime  = cd.substring(pipe1 + 1, pipe2);
      motorOffTime = cd.substring(pipe2 + 1);
    }
  }

  Serial.println("motorOnTime:  " + motorOnTime);
  Serial.println("motorOffTime: " + motorOffTime);
}