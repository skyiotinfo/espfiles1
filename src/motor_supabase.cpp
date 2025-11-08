#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPSupabaseRealtime.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
const int buzzer          = D8;

SupabaseRealtime realtime;

void HandleChanges(String result)
{
  JsonDocument doc;
  deserializeJson(doc, result);

  String tableName = doc["table"];
  String event = doc["type"];
  JsonObject record = doc["record"];
  String changes = doc["record"];

  Serial.println("====================");
  Serial.print("Table: ");
  Serial.println(tableName);
  Serial.print("Event: ");
  Serial.println(event);
   if (tableName == "profiles") {
    Serial.print("Motor State: ");
    Serial.println(record["state"].as<bool>());
    int a=record["state"].as<bool>();
    if(a==1){
      digitalWrite(buzzer,HIGH);
    } else {
      digitalWrite(buzzer,LOW);
    }
   }

  if (tableName == "pump_motor") {
    Serial.print("Motor State: ");
    Serial.println(record["state"].as<bool>());
    Serial.print("Schedule 1 Enabled: ");
    Serial.println(record["sch1_en"].as<bool>());
    Serial.print("Schedule 1 Start: ");
    Serial.println(record["sch1_start"].as<const char*>());
    Serial.print("Schedule 2 Enabled: ");
    Serial.println(record["sch2_en"].as<bool>());
    Serial.print("Schedule 2 Start: ");
    Serial.println(record["sch2_end"].as<const char*>());
    Serial.println("--------------------");
  }
}

void setup()
{
  Serial.begin(115200);
  pinMode(buzzer,OUTPUT);
  digitalWrite(buzzer,LOW);
  WiFi.begin("Anupam", "12345678");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(100);
    Serial.print(".");
  }

  Serial.println("\nConnected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  realtime.begin(
    "https://fkgfdgwpqqfxhnyuwtwe.supabase.co",
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU",
    HandleChanges
  );

  realtime.login_email("user2@demo.com", "123456");

  realtime.addChangesListener("profiles", "UPDATE", "public", "");
  // Listen to changes in the `pump_motor` table in the public schema
  realtime.addChangesListener("pump_motor", "*", "public", "");

  // Start listening
  realtime.listen();
}

void loop()
{
  realtime.loop();
}
