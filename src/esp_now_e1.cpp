#include "WiFi.h"

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA); // Must be in station mode
  
  Serial.println();
  Serial.print("Receiver ESP32 MAC Address: ");
  Serial.println(WiFi.macAddress());
}

void loop() {
  // Do nothing
}