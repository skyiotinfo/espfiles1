#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPSupabaseRealtime.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

SupabaseRealtime realtime;

// Handle realtime events
void HandleChanges(String result)
{
  DynamicJsonDocument doc(2048);  // ✅ FIXED
  DeserializationError err = deserializeJson(doc, result);

  if (err)
  {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  String tableName = doc["table"] | "unknown";
  String event = doc["type"] | "unknown";

  Serial.print("Table: ");
  Serial.print(tableName);
  Serial.print(" | Event: ");
  Serial.println(event);

  if (doc.containsKey("record"))
  {
    Serial.println("Record:");
    serializeJsonPretty(doc["record"], Serial);
    Serial.println();
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.println();
  Serial.println("Connecting to WiFi...");

  WiFi.begin("Anupam", "12345678");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("Connected to WiFi!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Initialize Supabase Realtime
  realtime.begin(
      "https://fkgfdgwpqqfxhnyuwtwe.supabase.co",
      "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU",
      HandleChanges);

  // Optional login (if Row-Level Security is ON)
  realtime.login_email("user2@demo.com", "123456");

  // Listen to table changes
  realtime.addChangesListener("profiles", "UPDATE", "public", "");
  realtime.addChangesListener("instruments", "*", "public", "");

  // Start realtime listener
  realtime.listen();

  Serial.println("Listening for realtime updates...");
}

void loop()
{
  realtime.loop();
}
