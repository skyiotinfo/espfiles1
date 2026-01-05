#include <EEPROM.h>
#include <Arduino.h>
#include <LoRa.h>
#include <SPI.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

#define ss   D8
#define rst  D0
#define dio0 D4  
#define WIFI_SSID "Anupam"
#define WIFI_PASS "12345678"
int led=D8;

#define SUPABASE_URL "https://fkgfdgwpqqfxhnyuwtwe.supabase.co"
#define SUPABASE_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU"

WiFiClientSecure client;
HTTPClient https;
String lastPumpState = "";
String lastTankState = "";


void sendToSupabase(String table, String column, String deviceid, String value) {

  String url = String(SUPABASE_URL) +
               "/rest/v1/" + table +
               "?device_id=eq." + deviceid;

  String payload = "{\"" + column + "\":" + value + "}";

  client.setInsecure();
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

String mapStatus(String raw) {
  if (raw == "11") return "1";
  if (raw == "00") return "0";
  return "";
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(led, OUTPUT);
  digitalWrite(led, LOW);
  Serial.println("BOOT STARTED");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");

  LoRa.setPins(ss, rst, dio0);
  LoRa.setSyncWord(0xA2);
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(62.5E3);

  Serial.print("Initializing LoRa");
  while (!LoRa.begin(433920000)) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nLoRa Ready");
}

void loop() {

  int packetSize = LoRa.parsePacket();
  if (!packetSize) {
    delay(10);
    return;
  }

  Serial.println("==============================");
  Serial.println("LoRa Packet Received");

  String LoRaData = "";
  while (LoRa.available()) {
    LoRaData += (char)LoRa.read();
  }

  Serial.print("Raw LoRa Data: ");
  Serial.println(LoRaData);

  if (LoRaData.length() < 8) {
    Serial.println("Invalid packet");
    return;
  }

  String deviceid   = LoRaData.substring(0, 6);
  String raw_status = LoRaData.substring(6, 8);

  Serial.print("Device ID: ");
  Serial.println(deviceid);

  Serial.print("Raw Status: ");
  Serial.println(raw_status);

  String mapped = mapStatus(raw_status);
  if (mapped == "") {
    Serial.println("Invalid status");
    return;
  }


  if (deviceid == "103101") {   
    if (mapped != lastPumpState) {
      Serial.println("Pump state CHANGED -> updating Supabase");
      sendToSupabase("pump_motor", "state", deviceid, mapped);
      lastPumpState = mapped;
    } else {
      Serial.println("Pump state unchanged -> skip update");
    }
  }
  else if (deviceid == "103102") {  
    if (mapped != lastTankState) {
      Serial.println("Tank state CHANGED -> updating Supabase");
      sendToSupabase("tank", "tank_status", deviceid, mapped);
      lastTankState = mapped;
    } else {
      Serial.println("Tank state unchanged -> skip update");
    }
  }
  else {
    Serial.println("Unknown Device ID");
  }

  // Serial.print("RSSI: ");
  // Serial.println(LoRa.packetRssi());

  Serial.println("==============================");
}
