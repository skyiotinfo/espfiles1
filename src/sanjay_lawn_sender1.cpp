#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <LoRa.h>
#include <SPI.h>

#define ss 15
#define rst 16
#define dio0 2
#define networkid "2015"
#define deviceid "01"

#define WIFI_SSID "Anupam"
#define WIFI_PASS "12345678"

#define SUPABASE_URL "https://fkgfdgwpqqfxhnyuwtwe.supabase.co/rest/v1"
#define SUPABASE_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU"

#define TABLE_NAME "lawn"


int motor_status_rx = -1; 
static unsigned long lastSend = 0;
int last_motor_status_sent = -1;
unsigned long lastWifiAttempt = 0;
bool wifiConnected = false;


int counter = 1;
const int hsen = D1;
const int lsen = D2;
const int spin = LED_BUILTIN;

int value = 11;
int state = 0;
int vstate1 = 2;
int vstate2 = 2;
int volt_state = 1;
int temp_count1 = 0;
int temp_count2 = 0;
int temp_count3 = 0;
void sendMotorStatusToSupabase(int motorStatus) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String endpoint = String(SUPABASE_URL) + "/" + TABLE_NAME + "?device_id=eq.01";
  Serial.println("PATCH -> " + endpoint);

  if (!http.begin(client, endpoint)) {
    Serial.println("HTTP begin failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));
  http.addHeader("Prefer", "return=minimal");

  StaticJsonDocument<200> doc;
  doc["motor_status"] = motorStatus;

  String body;
  serializeJson(doc, body);

  int code = http.PATCH(body);
  Serial.printf("Supabase PATCH code: %d\n", code);

  http.end();
}
void setup() {
  Serial.begin(115200);
  pinMode(D0, WAKEUP_PULLUP);
  pinMode(hsen, INPUT_PULLUP);
  pinMode(lsen, INPUT_PULLUP);
  pinMode(spin, OUTPUT);

  while (!Serial);
  Serial.println("LoRa Sender");

  LoRa.setPins(ss, rst, dio0);
  LoRa.setSyncWord(0xA2);
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(62.5E3);

  while (!LoRa.begin(433920000)) {
    Serial.println(".");
    delay(500);
  }
  Serial.println("LoRa Initializing OK!");
WiFi.mode(WIFI_STA);
WiFi.begin(WIFI_SSID, WIFI_PASS);
Serial.println("WiFi connecting (non-blocking)...");

}

void send_data() {
  if (millis() - lastSend < 1500) return;
  lastSend = millis();

  for (int i = 0; i <= 10; i++) {
    LoRa.beginPacket();
    LoRa.print(networkid);
    LoRa.print(deviceid);
    LoRa.print(vstate1);
    LoRa.print(vstate2);
    LoRa.endPacket();

    Serial.print(networkid);
    Serial.print(deviceid);
    Serial.print(vstate1);
    Serial.print(vstate2);
    Serial.print("..");

    delay(100);
  }
  Serial.println();
}


void receive_motor_status() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String rx = LoRa.readString();
    Serial.print("Reply RX: ");
    Serial.println(rx);

    if (rx.startsWith("R")) {
      motor_status_rx = rx.substring(1).toInt();
      
        sendMotorStatusToSupabase(motor_status_rx);
       
      
    }

  }
}
void handleWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      Serial.println("WiFi Connected!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
    }
    return;
  }

  wifiConnected = false;

  // Retry every 10 seconds
  if (millis() - lastWifiAttempt > 10000) {
    lastWifiAttempt = millis();
    Serial.println("Retrying WiFi...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}


void loop() {
   handleWiFi();  

  Serial.print("Sending packet: ");
  Serial.println(counter);


  if (digitalRead(hsen) == 0) {
    temp_count1++;
    Serial.println("Water High...");
    if (temp_count1 >= 3) {
      vstate1 = 0;
      vstate2 = 0;
      temp_count1 = 0;
    }
  } else {
    temp_count1 = 0;
  }

  if (digitalRead(lsen) == 0) {
    temp_count2++;
    Serial.println("Water Low...");
    if (temp_count2 >= 3) {
      vstate1 = 1;
      vstate2 = 1;
      temp_count2 = 0;
    }
  } else {
    temp_count2 = 0;
  }

  if (digitalRead(lsen) != 0 && digitalRead(hsen) != 0) {
    temp_count3++;
    if (temp_count3 >= 10) {
      Serial.println("Normal......");
      vstate1 = 2;
      vstate2 = 2;
      temp_count3 = 0;
    }
  } else {
    temp_count3 = 0;
  }

  counter++;
  if (counter >= 200) counter = 1;

  send_data();

  unsigned long t = millis();
  while (millis() - t < 300) {
    receive_motor_status();
    delay(10);
  }

  digitalWrite(spin, LOW);
  delay(50);
  digitalWrite(spin, HIGH);
  delay(2000);
}
