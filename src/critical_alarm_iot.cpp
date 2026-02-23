#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>

const char* ssid     = "Anupam";
const char* password = "12345678";

const char* supabaseUrl =
  "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/alarms";

const char* supabaseKey =
"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";


String uartBuffer = "";
bool jsonStarted = false;

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\n=== IOT SUPABASE BRIDGE STARTED ===");

  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected");
}

void sendToSupabase(const String& json) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, supabaseUrl);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", supabaseKey);
  http.addHeader("Authorization", String("Bearer ") + supabaseKey);
  http.addHeader("Prefer", "return=minimal");

  int code = http.POST(json);

  Serial.print("Supabase HTTP Code: ");
  Serial.println(code);

  if (code < 200 || code > 299) {
    Serial.println(http.getString());
  }

  http.end();
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '{') {
      uartBuffer = "{";
      jsonStarted = true;
      continue;
    }

    if (jsonStarted) {
      uartBuffer += c;

      if (c == '}') {
        jsonStarted = false;

        Serial.println("\nUART JSON:");
        Serial.println(uartBuffer);

        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, uartBuffer) == DeserializationError::Ok) {
          sendToSupabase(uartBuffer);
        } else {
          Serial.println("JSON INVALID – DROPPED");
        }

        uartBuffer = "";
      }
    }
  }
}
