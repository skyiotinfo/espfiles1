#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

const char* ssid     = "anupam";
const char* password = "12345678";

const char* supabase_url = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1/tank?device_id=eq.103201";
const char* supabase_key = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";

const int device_id = 103201;

const int hsen = D1;
const int lsen = D2;
const int led = LED_BUILTIN;

const int HIGH_DEBOUNCE_COUNT = 3;
const int LOW_DEBOUNCE_COUNT  = 3;

int temp_count1 = 0;
int temp_count2 = 0;
int last_tank_status = -1;   

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

  String payload = "{\"tank_status\": " + String(status) + "}";
  int httpCode = http.PATCH(payload);

  if (httpCode > 0) {
    Serial.printf("Supabase update sent (status=%d), HTTP code: %d\n", status, httpCode);
    if (pending_status == status) pending_status = -1;
  } else {
    Serial.printf("Failed to send update, HTTP error: %d\n", httpCode);
    pending_status = status;
  }
  http.end();
}

void checkAndSendPending() {
  if (pending_status != -1 && WiFi.status() == WL_CONNECTED) {
    Serial.printf("Sending pending status: %d\n", pending_status);
    sendTankStatus(pending_status);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(hsen, INPUT_PULLUP);
  pinMode(lsen, INPUT_PULLUP);
  pinMode(led, OUTPUT);
  digitalWrite(led, HIGH);

  connectWiFi();
  Serial.println("Tank level monitor started. Updates will be stored if WiFi is down.");
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

  bool highActive = (digitalRead(hsen) == LOW);
  bool lowActive  = (digitalRead(lsen) == LOW);

  if (highActive) {
    temp_count1++;
    temp_count2 = 0;
    Serial.printf("Water High... count=%d\n", temp_count1);
    if (temp_count1 >= HIGH_DEBOUNCE_COUNT) {
      if (last_tank_status != 0) {
        last_tank_status = 0;
        sendTankStatus(0);  
        Serial.println("Tank status set to 0 (High water)");
      }
      temp_count1 = 0;
    }
  }
  else if (lowActive) {
    temp_count2++;
    temp_count1 = 0;
    Serial.printf("Water Low... count=%d\n", temp_count2);
    if (temp_count2 >= LOW_DEBOUNCE_COUNT) {
      if (last_tank_status != 1) {
        last_tank_status = 1;
        sendTankStatus(1);
        Serial.println("Tank status set to 1 (Low water)");
      }
      temp_count2 = 0;
    }
  }
  else {
    temp_count1 = 0;
    temp_count2 = 0;
  }

  digitalWrite(led, LOW);
  delay(100);
  digitalWrite(led, HIGH);

  delay(500);
}