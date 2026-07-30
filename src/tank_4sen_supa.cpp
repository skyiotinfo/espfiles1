#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

const char* ssid     = "anupam";
const char* password = "12345678";

const char* supabase_url = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/tank?device_id=eq.123201";
const char* supabase_key = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";

const int device_id = 123201;


const int sen0   = D1;  
const int sen25  = D2;  
const int sen75  = D6; 
const int sen100 = D7;  

const int led = LED_BUILTIN;


const int DEBOUNCE_COUNT = 3;

const int LEVELS[4]     = {0, 25, 75, 100};
const int NUM_LEVELS    = 4;

int debounce_count   = 0;
int candidate_level   = -1;  
int last_percentage = -1;   

int pending_status = -1;
unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 5000;

WiFiClientSecure client;
HTTPClient http;

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi connection failed!");
  }
}

void sendTankStatus(int status) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot send now. Storing as pending.");
    pending_status = status;
    return;
  }

  client.setInsecure();
  http.begin(client, supabase_url);
  http.addHeader("apikey", supabase_key);
  http.addHeader("Authorization", "Bearer " + String(supabase_key));
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");

  String payload = "{\"percentage\": " + String(status) + "}";
  int httpCode = http.PATCH(payload);

  if (httpCode > 0) {
    Serial.printf("Supabase update sent (status=%d%%), HTTP code: %d\n", status, httpCode);
    if (pending_status == status) pending_status = -1;
  } else {
    Serial.printf("Failed to send update, HTTP error: %d\n", httpCode);
    pending_status = status;
  }
  http.end();
}

void checkAndSendPending() {
  if (pending_status != -1 && WiFi.status() == WL_CONNECTED) {
    Serial.printf("Sending pending status: %d%%\n", pending_status);
    sendTankStatus(pending_status);
  }
}


int readTankLevel() {
  bool s100 = (digitalRead(sen100) == LOW);
  bool s75  = (digitalRead(sen75)  == LOW);
  bool s25  = (digitalRead(sen25)  == LOW);
  bool s0   = (digitalRead(sen0)   == LOW);

  if (s100) return 100;
  if (s75)  return 75;
  if (s25)  return 25;
  if (s0)   return 0;   
  return 0;            
}

void setup() {
  Serial.begin(115200);
  pinMode(sen0,   INPUT_PULLUP);
  pinMode(sen25,  INPUT_PULLUP);
  pinMode(sen75,  INPUT_PULLUP);
  pinMode(sen100, INPUT_PULLUP);
  pinMode(led, OUTPUT);
  digitalWrite(led, HIGH);

  connectWiFi();
  Serial.println("Tank level monitor started (0% / 25% / 75% / 100%). Updates stored if WiFi is down.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
      lastWiFiCheck = millis();
      Serial.println("WiFi lost, attempting reconnection...");
      connectWiFi();
      if (WiFi.status() == WL_CONNECTED) {
        checkAndSendPending();
      }
    }
  } else {
    if (millis() - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
      lastWiFiCheck = millis();
      checkAndSendPending();
    }
  }

  int current_level = readTankLevel();
  Serial.printf("Raw read: %d%%\n", current_level);

  if (current_level == candidate_level) {
    debounce_count++;
  } else {
    candidate_level = current_level;
    debounce_count = 1;
  }

  if (debounce_count >= DEBOUNCE_COUNT) {
    if (candidate_level != last_percentage) {
      last_percentage = candidate_level;
      sendTankStatus(last_percentage);
      Serial.printf("Tank status confirmed and sent: %d%%\n", last_percentage);
    }
    debounce_count = DEBOUNCE_COUNT; 
  }

  digitalWrite(led, LOW);
  delay(100);
  digitalWrite(led, HIGH);

  delay(500);
}
