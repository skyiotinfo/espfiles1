#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <ESP8266HTTPClient.h>

extern "C" {
  #include "user_interface.h"
}

const char* ssid     = "anupam";
const char* password = "12345678";
const char* supabase_url = "https://nntytldbmnjraytdmasp.supabase.co/rest/v1/motor_state_live";
const char* supabase_api_key = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im5udHl0bGRibW5qcmF5dGRtYXNwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjEyODc0MTIsImV4cCI6MjA3Njg2MzQxMn0.lcleFnldZ-6E2Qr0UWQqBcMGBHX3j2l_ldApX0gmnfc";

struct Message {
  String id;
  int state;
};
#define QUEUE_SIZE 15
Message queue[QUEUE_SIZE];
volatile int head = 0;
volatile int tail = 0;

int lastTank1 = -1, lastTank2 = -1, lastM3 = -1;
int lastM1 = -1, lastB1 = -1, lastM2 = -1, lastB2 = -1;

bool connectWiFi() {
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
  return WiFi.status() == WL_CONNECTED;
}

void enqueue(String id, int state) {
  int next = (head + 1) % QUEUE_SIZE;
  if (next == tail) {
    Serial.println("Queue full, dropping oldest");
    tail = (tail + 1) % QUEUE_SIZE;
  }
  queue[head] = {id, state};
  head = next;
}

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

void processQueue() {
  if (tail == head) return;
  Message msg = queue[tail];
  tail = (tail + 1) % QUEUE_SIZE;
  sendToSupabase(msg.id, msg.state);
}

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != 2) return;

  uint8_t board = data[0];
  uint8_t status = data[1];

  Serial.printf("Board %d → status %d\n", board, status);

  // Send ACK (0xAA) immediately
  uint8_t ack = 0xAA;
  esp_now_send(mac, &ack, 1);

  if (board == 1) {
    int tank1 = (status >> 2) & 1;
    int tank2 = (status >> 1) & 1;
    int m3    = status & 1;

    if (tank1 != lastTank1) { lastTank1 = tank1; enqueue("tank1", tank1); }
    if (tank2 != lastTank2) { lastTank2 = tank2; enqueue("tank2", tank2); }
    if (m3    != lastM3)    { lastM3    = m3;    enqueue("M3",    m3); }
  } else if (board == 2) {
    int m1=0, b1=0, m2=0, b2=0;
    if (status == 1) { m1 = 1; b1 = 1; }
    else if (status == 2) { m2 = 1; b2 = 1; }

    if (m1 != lastM1) { lastM1 = m1; enqueue("M1", m1); }
    if (b1 != lastB1) { lastB1 = b1; enqueue("B1", b1); }
    if (m2 != lastM2) { lastM2 = m2; enqueue("M2", m2); }
    if (b2 != lastB2) { lastB2 = b2; enqueue("B2", b2); }
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);   // 🔥 SAME CHANNEL as others
  Serial.print("Board4 MAC: ");
  Serial.println(WiFi.macAddress());

  connectWiFi();

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onReceive);

  // Allow broadcast to receive from any device
  uint8_t broadcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_now_add_peer(broadcast, ESP_NOW_ROLE_COMBO, 1, NULL, 0);

  Serial.println("Board4 ready");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  processQueue();
  delay(10);
}