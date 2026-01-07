#include <Arduino.h>
#include <ESPSupabase.h>
#include <ArduinoJson.h>
 
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
 
 
Supabase db;
const char* WIFI_SSID = "Anupam";
const char* WIFI_PASS = "12345678";
const char* SUPABASE_URL = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";
const char* USER_EMAIL = "9999900001@gmail.com";
const char* USER_PASS = "1234";
 
#define TRIG_PIN D5
#define ECHO_PIN D6
 
const float MIN_DISTANCE = 23.0;    
const float MAX_DISTANCE = 600.0;  
 
const int SAMPLES = 5;            
const unsigned long PULSE_TIMEOUT = 60000;
 
long duration;
 
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
 
// Connecting to Wi-Fi
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(100);
    Serial.print(".");
  }
  Serial.println("\nConnected!");


  // Beginning Supabase Connection
  db.begin(SUPABASE_URL, SUPABASE_KEY);
 
  Serial.println("JSN-SR04T Water Level System Started");
  Serial.println("-----------------------------------");
}
 
int getModeDistance() {
  int readings[SAMPLES];
  int count = 0;
 
  for (int i = 0; i < SAMPLES; i++) {
 
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
 
    long duration = pulseIn(ECHO_PIN, HIGH, PULSE_TIMEOUT);
    if (duration == 0) continue;
 
    int distance = (duration * 0.034) / 2;
 
    if (distance >= MIN_DISTANCE && distance <= MAX_DISTANCE) {
      readings[count++] = distance;
    }
 
    delay(30);
  }
 
  if (count < 3) return -1;
 
  int mode = readings[0];
  int maxCount = 0;
 
  for (int i = 0; i < count; i++) {
    int freq = 0;
    for (int j = 0; j < count; j++) {
      if (readings[j] == readings[i]) freq++;
    }
 
    if (freq > maxCount) {
      maxCount = freq;
      mode = readings[i];
    }
  }
 
  return mode;
}
 
 
void loop() {

  static unsigned long lastRun = 0;
  if (millis() - lastRun < 500) return;
  lastRun = millis();

  static float lastPercent = 0;

  int distance = getModeDistance();

  if (distance == -1) {
    Serial.println("ERROR: No valid sensor reading");
    return;
  }

  distance = constrain(distance, MIN_DISTANCE, MAX_DISTANCE);

  float waterPercent =
      (MAX_DISTANCE - distance) * 100.0 /
      (MAX_DISTANCE - MIN_DISTANCE);

  waterPercent = (0.7 * lastPercent) + (0.3 * waterPercent);
  lastPercent = waterPercent;

  int percentInt = static_cast<int>(waterPercent);


  int authCode = db.login_email(USER_EMAIL, USER_PASS);
  Serial.print("Login code: ");
  Serial.println(authCode);
  StaticJsonDocument<32> doc;
  doc["percentage"] = percentInt;

  String payload;
  serializeJson(doc, payload);

  int code = db
    .update("tank")
    .eq("device_id", "103202")
    .doUpdate(payload);

  Serial.print("Update Code: ");
  Serial.println(code);
  db.urlQuery_reset();

  Serial.print("Water Level: ");
  Serial.print(waterPercent, 2);
  Serial.println(" %");

  Serial.println("-----------------------------");
}
