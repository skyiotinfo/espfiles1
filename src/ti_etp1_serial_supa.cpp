#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <map>

#define LED_PIN   D8

const char* ssid     = "anupam";
const char* password = "12345678";

const char* supabase_url = "https://nntytldbmnjraytdmasp.supabase.co/rest/v1/motor_state_live?on_conflict=motor_id";
const char* supabase_api_key = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im5udHl0bGRibW5qcmF5dGRtYXNwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjEyODc0MTIsImV4cCI6MjA3Njg2MzQxMn0.lcleFnldZ-6E2Qr0UWQqBcMGBHX3j2l_ldApX0gmnfc";

const char* motorIds[] = {"tank1","tank2","M3","M1","B1","M2","B2","dummy"};
const int NUM_MOTORS = 8;

struct QueuedItem {
  String motorId;
  int state;
};

#define QUEUE_SIZE 30
QueuedItem queue[QUEUE_SIZE];
int qHead = 0, qTail = 0;

std::map<String, int> lastQueuedState;
std::map<String, int> lastUploadedState;

bool wifiConnected = false;
unsigned long lastWifiAttempt = 0;
const unsigned long WIFI_RETRY_INTERVAL = 10000;

unsigned long lastRetryTime = 0;
const unsigned long RETRY_DELAY = 15000;  
bool waitingForRetry = false;

void connectWiFi() {
  if (wifiConnected) return;
  if (millis() - lastWifiAttempt < WIFI_RETRY_INTERVAL) return;
  lastWifiAttempt = millis();

  Serial.print("Connecting WiFi");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(200);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\n✅ WiFi Connected");
      digitalWrite(LED_PIN, HIGH);

  } else {
    wifiConnected = false;
    Serial.println("\n❌ WiFi Failed");
      digitalWrite(LED_PIN, LOW);

  }
}

void checkWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      Serial.println("✅ WiFi reconnected");
        digitalWrite(LED_PIN, HIGH);

      waitingForRetry = false;
    }
  } else {
    if (wifiConnected) {
      wifiConnected = false;
      Serial.println("⚠️ WiFi lost");
        digitalWrite(LED_PIN, LOW);

    }
    connectWiFi();
  }
}

bool enqueue(const String& motorId, int state) {
  if (lastQueuedState[motorId] == state) return false;
  lastQueuedState[motorId] = state;

  int nextHead = (qHead + 1) % QUEUE_SIZE;
  if (nextHead == qTail) {
    Serial.println("⚠️ Queue full! Dropping oldest.");
    qTail = (qTail + 1) % QUEUE_SIZE;
  }
  queue[qHead] = {motorId, state};
  qHead = nextHead;
  Serial.printf("📦 Queued: %s=%d\n", motorId.c_str(), state);
  return true;
}

bool peekQueue(QueuedItem& item) {
  if (qTail == qHead) return false;
  item = queue[qTail];
  return true;
}

void popQueue() {
  if (qTail != qHead) {
    qTail = (qTail + 1) % QUEUE_SIZE;
  }
}

bool uploadToSupabase(const String& motorId, int state) {
  if (!wifiConnected) return false;
  if (lastUploadedState[motorId] == state) return true;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  if (!http.begin(client, supabase_url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", supabase_api_key);
  http.addHeader("Authorization", String("Bearer ") + supabase_api_key);
  http.addHeader("Prefer", "resolution=merge-duplicates");

  String body = "{\"motor_id\":\"" + motorId + "\",\"motor_state\":" + String(state) + "}";
  int code = http.POST(body);
  http.end();

  if (code == 200 || code == 201) {
    Serial.printf("📡 Uploaded %s=%d → %d OK\n", motorId.c_str(), state, code);
    lastUploadedState[motorId] = state;
    return true;
  } else {
    Serial.printf("❌ Upload %s=%d → HTTP %d\n", motorId.c_str(), state, code);
    return false;
  }
}

void parseAndQueue(const String& line) {
  if (line.length() < 9) {
    Serial.println("⚠️ Invalid data (too short): " + line);
    return;
  }
  for (int i = 0; i < NUM_MOTORS; i++) {
    char ch = line[i];
    if (ch != '0' && ch != '1') {
      Serial.printf("⚠️ Invalid bit at position %d (char '%c') in line: %s\n", i, ch, line.c_str());
      return;
    }
    int state = (ch == '1');
    enqueue(motorIds[i], state);
  }
}

void readSerial() {
  static String incomingLine = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (incomingLine.length() > 0) {
        incomingLine.trim();
        parseAndQueue(incomingLine);
      }
      incomingLine = "";
    } else if (c != '\r') {
      incomingLine += c;
      if (incomingLine.length() > 64) incomingLine = "";
    }
  }
}

void processQueue() {
  static unsigned long lastUploadTime = 0;
  const unsigned long UPLOAD_INTERVAL = 200;  

  if (waitingForRetry && millis() - lastRetryTime < RETRY_DELAY) {
    return;
  }
  waitingForRetry = false;

  if (millis() - lastUploadTime < UPLOAD_INTERVAL) return;

  QueuedItem item;
  if (!peekQueue(item)) return; 

  if (uploadToSupabase(item.motorId, item.state)) {
    popQueue();
    lastUploadTime = millis();
    waitingForRetry = false;
  } else {
    waitingForRetry = true;
    lastRetryTime = millis();
    Serial.println("↻ Will retry later (5s delay)");

  }
}

void setup() {
  Serial.begin(9600);
  delay(100);
  pinMode(LED_PIN, OUTPUT);
  Serial.println("\n📡 Uploader Started");
  digitalWrite(LED_PIN, LOW);
  connectWiFi();
}

void loop() {
  readSerial();
  checkWiFi();
  processQueue();
  delay(10);   
}