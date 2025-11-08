#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPSupabaseRealtime.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#else
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#endif

#include <EEPROM.h>

const char* WIFI_SSID = "Anupam";
const char* WIFI_PASS = "12345678";

const char* SUPABASE_URL = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";
const char* TABLE_NAME = "pump_motor";
const char* DEVICE_ID = "1001"; 

const int MOTOR_PIN = D5;
const int BUZZER_PIN = D8;

int motor_state = 0;  
SupabaseRealtime realtime;

#define EEPROM_SIZE 8
#define EEPROM_ADDR 0

void saveMotorStateToEEPROM(int state);
int loadMotorStateFromEEPROM();
void sendMotorStateToSupabase(int state);
void setMotorState(int newState);
void handleRealtime(String result);

void saveMotorStateToEEPROM(int state) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_ADDR, state);
  EEPROM.commit();
  Serial.printf("Saved state %d to EEPROM\n", state);
}

int loadMotorStateFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  int state = EEPROM.read(EEPROM_ADDR);
  Serial.printf("Loaded state %d from EEPROM\n", state);
  return state;
}

void sendMotorStateToSupabase(int state) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot update Supabase");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_NAME + "?id=eq." + DEVICE_ID;
  if (https.begin(client, url)) {
    https.addHeader("apikey", SUPABASE_KEY);
    https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
    https.addHeader("Content-Type", "application/json");

    String body = "{\"state\":" + String(state) + "}";
    int code = https.PATCH(body);
    Serial.printf("📤 Supabase update: %s | Code: %d\n", body.c_str(), code);
    https.end();
  } else {
    Serial.println("HTTPS begin failed");
  }
}

void setMotorState(int newState) {
  if (newState == motor_state) return; 

  motor_state = newState;
  saveMotorStateToEEPROM(motor_state);

  if (motor_state == 1) {
    digitalWrite(MOTOR_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    Serial.println("🟢 Motor Started");
  } else {
    digitalWrite(MOTOR_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    Serial.println("🔴 Motor Stopped");
  }

  sendMotorStateToSupabase(motor_state);
}

void handleRealtime(String result) {
  JsonDocument doc;
  deserializeJson(doc, result);

  String tableName = doc["table"];
  String event = doc["type"];
  JsonObject record = doc["record"];

  if (tableName == TABLE_NAME && event == "UPDATE") {
    int newState = record["state"];
    Serial.printf("📡 Realtime update received: %d\n", newState);
    setMotorState(newState);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(MOTOR_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  motor_state = loadMotorStateFromEEPROM();
  setMotorState(motor_state);

  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi Connected");
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  realtime.begin(SUPABASE_URL, SUPABASE_KEY, handleRealtime);
  realtime.addChangesListener(TABLE_NAME, "UPDATE", "public", "");
  realtime.listen();

  Serial.println("Listening to Supabase changes...");
}

void loop() {
  realtime.loop();
}
