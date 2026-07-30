#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>

const char* WIFI_SSID = "VIBHASHREE HONDA2.4";
const char* WIFI_PASS = "VIBHA@123";

const char* SUPABASE_HOST     = "nntytldbmnjraytdmasp.supabase.co"; 
const char* SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im5udHl0bGRibW5qcmF5dGRtYXNwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjEyODc0MTIsImV4cCI6MjA3Njg2MzQxMn0.lcleFnldZ-6E2Qr0UWQqBcMGBHX3j2l_ldApX0gmnfc";
const char* SUPABASE_RPC_PATH = "/rest/v1/rpc/upsert_motor_status";

const uint8_t LED         = D8;

#define LINK_BAUD   115200   
#define DEBUG_BAUD  115200

String lineBuf;

void connectWiFi() {
  digitalWrite(LED, LOW); // NEW: LED off the moment we're (re)connecting, not connected

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial1.print(F("Connecting to WiFi"));
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial1.print(F("."));
    if (millis() - start > 20000UL) {
      Serial1.println(F("\nWiFi timeout, retrying..."));
      WiFi.disconnect();
      digitalWrite(LED, LOW); // still not connected
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      start = millis();
    }
  }

  digitalWrite(LED, HIGH); // NEW: connected -> LED on
  Serial1.print(F("\nWiFi connected, IP: "));
  Serial1.println(WiFi.localIP());
}

bool postToSupabase(const String &jsonPayload) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 1024); 

  HTTPClient http;
  String url = String("https://") + SUPABASE_HOST + SUPABASE_RPC_PATH;

  if (!http.begin(client, url)) {
    Serial1.println(F("HTTPClient begin() failed"));
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", "return=minimal");

  String body = "{\"payload\":" + jsonPayload + "}";
  int code = http.POST(body);

  bool ok = (code >= 200 && code < 300);
  if (ok) {
    Serial1.printf("Supabase insert OK (HTTP %d)\n", code);
  } else {
    Serial1.printf("Supabase insert FAILED (HTTP %d): %s\n", code, http.getString().c_str());
  }
  http.end();
  return ok;
}

void setup() {
  Serial.begin(LINK_BAUD);    
  Serial1.begin(DEBUG_BAUD); 
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW); 
  Serial1.println(F("\nRelay Board (ESP8266) booting..."));
  connectWiFi();
  Serial1.println(F("Relay Board ready - waiting for data from monitor board...\n"));
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi(); // NEW behavior inherited from connectWiFi(): LED off during this call, on again once it succeeds
  }

  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n') {
      lineBuf.trim();
      if (lineBuf.length() > 0) {
        StaticJsonDocument<768> doc;
        DeserializationError err = deserializeJson(doc, lineBuf);
        if (err) {
          Serial1.printf("Bad JSON from monitor board (%s): %s\n", err.c_str(), lineBuf.c_str());
        } else {
          Serial1.println("Received: " + lineBuf);
          postToSupabase(lineBuf);
        }
      }
      lineBuf = "";
    } else if (c != '\r') {
      lineBuf += c;
      if (lineBuf.length() > 1000) {  
        Serial1.println(F("Line buffer overflow, discarding."));
        lineBuf = "";
      }
    }
  }
}
