#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

/* ================= WIFI ================= */
#define WIFI_SSID "Anupam"
#define WIFI_PASS "12345678"

/* ================= LED ================= */
#define LED_PIN D8

/* ================= SUPABASE ================= */
#define SUPABASE_URL "https://fkgfdgwpqqfxhnyuwtwe.supabase.co"
#define SUPABASE_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU"

WiFiClientSecure client;
HTTPClient https;

/* ================= LAST STATES ================= */
String lastPumpState = "";
String lastTankState = "";

/* ================= PENDING FLAGS ================= */
volatile bool pumpPending = false;
volatile bool tankPending = false;

String pumpDeviceId, pumpValue;
String tankDeviceId, tankValue;

/* ================= ESP-NOW MESSAGE ================= */
typedef struct struct_message {
  char message[64];
} struct_message;

struct_message incomingmsg;

/* ================= STATUS MAP ================= */
String mapStatus(String raw) {
  if (raw == "11") return "1";
  if (raw == "00") return "0";
  return "";
}

/* ================= SUPABASE UPDATE ================= */
void sendToSupabase(String table, String column, String deviceid, String value) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected → Supabase skipped");
    return;
  }

  String url = String(SUPABASE_URL) +
               "/rest/v1/" + table +
               "?device_id=eq." + deviceid;

  char payload[64];
  snprintf(payload, sizeof(payload),
           "{\"%s\":%s}", column.c_str(), value.c_str());

  client.setInsecure();
  client.setTimeout(5000);

  https.begin(client, url);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));
  https.addHeader("Prefer", "return=minimal");

  int httpCode = https.PATCH(payload);

  Serial.print("Supabase HTTP Code: ");
  Serial.println(httpCode);

  if (httpCode == 204) {
    Serial.println("Supabase update SUCCESS");
  } else {
    Serial.println("Supabase update FAILED");
  }

  https.end();
}

/* ================= WIFI CHECK ================= */
void checkWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("WiFi disconnected → reconnecting...");
  digitalWrite(LED_PIN, LOW);

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi reconnected");
    digitalWrite(LED_PIN, HIGH);
  } else {
    Serial.println("\nWiFi reconnect FAILED");
  }
}

/* ================= ESP-NOW RECEIVE CALLBACK ================= */
void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {

  memcpy(&incomingmsg, incomingData, sizeof(incomingmsg));
  String data = incomingmsg.message;

  Serial.print("ESP-NOW Data: ");
  Serial.println(data);

  if (data.length() < 8) return;

  String deviceid   = data.substring(0, 6);
  String raw_status = data.substring(6, 8);
  String mapped     = mapStatus(raw_status);

  if (mapped == "") return;

  /* ===== PUMP ===== */
  if (deviceid == "103301") {
    if (mapped != lastPumpState) {
      lastPumpState = mapped;
      pumpDeviceId = deviceid;
      pumpValue = mapped;
      pumpPending = true;
      Serial.println("Pump state CHANGED (ESP-NOW)");
    }
  }

  /* ===== TANK ===== */
  else if (deviceid == "103302") {
    if (mapped != lastTankState) {
      lastTankState = mapped;
      tankDeviceId = deviceid;
      tankValue = mapped;
      tankPending = true;
      Serial.println("Tank state CHANGED (ESP-NOW)");
    }
  }
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println("BOOT STARTED");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");
  digitalWrite(LED_PIN, HIGH);

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW INIT FAILED");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(OnDataRecv);

  Serial.println("ESP-NOW Receiver Ready");
}

/* ================= LOOP ================= */
void loop() {
  checkWiFi();

  if (WiFi.status() != WL_CONNECTED) {
    delay(50);
    return;
  }

  /* ===== SAME BEHAVIOR AS LORA ===== */
  if (pumpPending) {
    pumpPending = false;
    Serial.println("Updating Pump → Supabase");
    sendToSupabase("pump_motor", "state", pumpDeviceId, pumpValue);
  }

  if (tankPending) {
    tankPending = false;
    Serial.println("Updating Tank → Supabase");
    sendToSupabase("tank", "tank_status", tankDeviceId, tankValue);
  }

  delay(10);
}
