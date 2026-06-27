#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

const char* ssid     = "anupam";
const char* password = "12345678";


const char* SUPABASE_URL = "https://nntytldbmnjraytdmasp.supabase.co";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im5udHl0bGRibW5qcmF5dGRtYXNwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjEyODc0MTIsImV4cCI6MjA3Njg2MzQxMn0.lcleFnldZ-6E2Qr0UWQqBcMGBHX3j2l_ldApX0gmnfc";
const int   CUST_ID      = 101001;


int sdevice[8]      = {0};
int prev_sdevice[8] = {0};
String etime[8];
String ftime[8];

char motor_st      = 'S';
char prev_motor_st = 'S';

const int mt_st = D8;   
int mt_status   = 0;
const int sled  = D7;   

unsigned long mt_last_active_time = 0;
const unsigned long MT_TIMEOUT    = 5 * 60 * 1000;

int wificonnect = 0;

unsigned long lastSerialReceivedTime = 0;
unsigned long lastSendTime           = 0;
unsigned long lastGetTime            = 0;
const unsigned long SEND_INTERVAL    = 5000;
const unsigned long GET_INTERVAL     = 20000;

String motorOnTime  = "0000-00-00 00:00:00";
String motorOffTime = "0000-00-00 00:00:00";

const unsigned long HEARTBEAT_INTERVAL = 30000;
unsigned long lastHeartbeatTime        = 0;

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

  if (mt_status == 1) {
    mt_last_active_time = millis();
  }
}


void loop() {
  checkWiFiConnection();
  wificonnect = (WiFi.status() == WL_CONNECTED) ? 1 : 0;

  if (Serial.available()) {
    delay(100);
    String received = Serial.readStringUntil('\r');
    received.trim();

    bool valid = (received.length() == 9);
    if (valid) {
      for (int i = 0; i < 8; i++) {
        char c = received.charAt(i);
        if (c != '0' && c != '1') {
          valid = false;
          break;
        }
      }
      char lastChar = received.charAt(8);
      if (lastChar != 'R' && lastChar != 'S') {
        valid = false;
      }
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

        if (prev_motor_st == 'S' && motor_st == 'R') {
          motorOnTime = getCurrentTime();
        } else if (prev_motor_st == 'R' && motor_st == 'S') {
          motorOffTime = getCurrentTime();
        }
      }

      if (hasDeviceOrMotorStateChanged()) {
        sendDataToSupabase();
        lastSendTime = millis();
      }
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(sled, HIGH);
    delay(200);
    digitalWrite(sled, LOW);
    delay(200);
  }

  if (mt_status == 1 && (millis() - mt_last_active_time) > MT_TIMEOUT) {
    Serial.println("Motor timeout reached. Turning off motor.");

    mt_status = 0;
    motor_st  = 'S';
    digitalWrite(mt_st, mt_status);
    motorOffTime = getCurrentTime();

    if (hasDeviceOrMotorStateChanged()) {
      sendDataToSupabase();
      lastSendTime = millis();
    }
  }

  if ((millis() - lastGetTime) >= GET_INTERVAL) {
    readMotorStatusFromSupabase();
    lastGetTime = millis();
  }

  if ((millis() - lastHeartbeatTime) >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeatTime = millis();
  }

  delay(500);
}


bool hasDeviceOrMotorStateChanged() {
  for (int i = 0; i < 8; i++) {
    if (sdevice[i] != prev_sdevice[i]) return true;
  }
  return motor_st != prev_motor_st;
}


String getCurrentTime() {
  time_t now = time(nullptr);
  struct tm* p_tm = localtime(&now);
  char timeStr[64];
  if (p_tm) {
    snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d",
             p_tm->tm_year + 1900,
             p_tm->tm_mon + 1,
             p_tm->tm_mday,
             p_tm->tm_hour,
             p_tm->tm_min,
             p_tm->tm_sec);
    return String(timeStr);
  } else {
    return "0000-00-00 00:00:00";
  }
}


void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Reconnecting...");
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nReconnected to WiFi.");
    } else {
      Serial.println("\nReconnection failed.");
    }
  }
}


void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;

  String currentTime = getCurrentTime();
  String jsonBody = "{\"current_data\":\"" + currentTime
                  + "\",\"update_time\":\"" + currentTime + "\"}";

  Serial.println("Sending heartbeat: " + jsonBody);
  int code = supabasePatch(301, jsonBody);

  if (code > 0) {
    Serial.print("Heartbeat response: "); Serial.println(code);
  } else {
    Serial.print("Heartbeat failed: "); Serial.println(code);
  }
}


void sendDataToSupabase() {
  if (!hasDeviceOrMotorStateChanged()) {
    Serial.println("No state change. Skipping Supabase update.");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;

  String motorData = String(motor_st) + "|" + motorOnTime + "|" + motorOffTime;

  String jsonBody = "{\"current_state\":" + String(mt_status)
                  + ",\"current_data\":\"" + motorData
                  + "\",\"update_time\":\"" + getCurrentTime() + "\"}";

  Serial.println("Sending motor update: " + jsonBody);
  int code = supabasePatch(101, jsonBody);

  if (code > 0) {
    Serial.print("Motor update response: "); Serial.println(code);
    for (int i = 0; i < 8; i++) prev_sdevice[i] = sdevice[i];
    prev_motor_st = motor_st;
  } else {
    Serial.print("Error sending motor update: "); Serial.println(code);
  }
}


void updateDeviceInSupabase(int deviceIndex) {
  if (WiFi.status() != WL_CONNECTED) return;

  int deviceId = 201 + deviceIndex; 

  String devData  = etime[deviceIndex] + "|" + ftime[deviceIndex];

  String jsonBody = "{\"current_state\":" + String(sdevice[deviceIndex])
                  + ",\"current_data\":\"" + devData
                  + "\",\"update_time\":\"" + getCurrentTime() + "\"}";

  Serial.println("Updating device " + String(deviceId) + ": " + jsonBody);
  int code = supabasePatch(deviceId, jsonBody);

  if (code > 0) {
    Serial.print("Device update response: "); Serial.println(code);
  } else {
    Serial.print("Error updating device: "); Serial.println(code);
  }
}

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

      Serial.print("Motor command received. New state: ");
      Serial.println(motor_st);
    }
  } else {
    Serial.println("Motor command GET: no data or parse error.");
  }
}

void readInitialSupabaseData() {
  if (WiFi.status() != WL_CONNECTED) return;

  String payload = supabaseGet(101, "current_state,current_data");
  Serial.println("Initial data: " + payload);

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

  Serial.println("Restored motorOnTime:  " + motorOnTime);
  Serial.println("Restored motorOffTime: " + motorOffTime);
}
