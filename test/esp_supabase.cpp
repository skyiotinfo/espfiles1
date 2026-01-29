#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPSupabaseRealtime.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

SupabaseRealtime realtime;

void HandleChanges(String result)
{
  JsonDocument doc;
  deserializeJson(doc, result);

  // Example of what you can do with the result
  String tableName = doc["table"];
  String event = doc["type"];
  String changes = doc["record"];

  Serial.print(tableName);
  Serial.print(" : ");
  Serial.println(event);
  Serial.println(changes);
}

void setup()
{
  Serial.begin(115200);

  WiFi.begin("Airtel_9764005401", "air46403");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(100);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  realtime.begin("https://fkgfdgwpqqfxhnyuwtwe.supabase.co", "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU", HandleChanges);
  realtime.login_email("user2@demo.com", "123456"); // Only if you activate RLS in your Supabase Postgres Table

  // Parameter 1 : Table name
  // Parameter 2 : Event type ("*" | "INSERT" | "UPDATE" | "DELETE")
  // Parameter 3 : Your Supabase Table Postgres Schema
  // Parameter 4 : Filter
  //   Please read : https://supabase.com/docs/guides/realtime/postgres-changes?queryGroups=language&language=js#available-filters
  //   empty string if you don't want to filter the result
  realtime.addChangesListener("profiles", "UPDATE", "public", "");
  // You can add multiple table listeners
  realtime.addChangesListener("instruments", "*", "public", "");

  realtime.listen();
}

void loop()
{
  realtime.loop();
}