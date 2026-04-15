// ============================================================
// Uploader.ino – reads motor states from Serial and sends to Supabase
// ============================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>

// ---------- WiFi credentials ----------
const char* ssid     = "anupam";
const char* password = "12345678";

// ---------- Supabase ----------
const char* supabase_url = "https://nntytldbmnjraytdmasp.supabase.co/rest/v1/motor_state_live";
const char* supabase_api_key = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im5udHl0bGRibW5qcmF5dGRtYXNwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjEyODc0MTIsImV4cCI6MjA3Njg2MzQxMn0.lcleFnldZ-6E2Qr0UWQqBcMGBHX3j2l_ldApX0gmnfc";

// ---------- Queue for buffering (in case WiFi is down) ----------
struct Message {
  String id;
  int state;
};
#define QUEUE_SIZE 20
Message queue[QUEUE_SIZE];
volatile int head = 0;
volatile int tail = 0;

// ---------- WiFi connection ----------
bool connectWiFi() {
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
  return WiFi.status() == WL_CONNECTED;
}

// ---------- Enqueue a motor state (called from Serial input) ----------
void enqueue(String id, int state) {
  int next = (head + 1) % QUEUE_SIZE;
  if (next == tail) {
    Serial.println("Queue full, dropping oldest");
    tail = (tail + 1) % QUEUE_SIZE;
  }
  queue[head] = {id, state};
  head = next;
}

// ---------- Send one message to Supabase (UPSERT) ----------
void sendToSupabase(String motorId, int state) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, supabase_url)) return;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", supabase_api_key);
  http.addHeader("Authorization", String("Bearer ") + supabase_api_key);
  http.addHeader("Prefer", "resolution=merge-duplicates");   // UPSERT

  String body = "{\"motor_id\":\"" + motorId + "\",\"motor_state\":" + String(state) + "}";
  int code = http.POST(body);
  if (code == 201 || code == 200 || code == 409) {
    Serial.printf("✅ %s → %d\n", motorId.c_str(), state);
  } else {
    Serial.printf("❌ %s failed, HTTP %d\n", motorId.c_str(), code);
  }
  http.end();
}

// ---------- Process the queue (send oldest first) ----------
void processQueue() {
  if (tail == head) return;
  Message msg = queue[tail];
  tail = (tail + 1) % QUEUE_SIZE;
  sendToSupabase(msg.id, msg.state);
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("Uploader starting...");

  // Connect to WiFi
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi not connected – will retry later");
  }

  Serial.println("Ready to receive motor states from Serial.");
}

// ---------- Main loop ----------
void loop() {
  // Read lines from Serial (coming from the Gateway)
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    // Expected format: "motor_id,state"
    int comma = line.indexOf(',');
    if (comma > 0) {
      String id = line.substring(0, comma);
      int state = line.substring(comma + 1).toInt();
      enqueue(id, state);
      Serial.printf("Queued: %s,%d\n", id.c_str(), state);
    }
  }

  // Maintain WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // Process one queued message per loop (to avoid blocking)
  processQueue();

  delay(10);
}