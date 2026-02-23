#include <Arduino.h>

void setup() {
  Serial.begin(115200);
}

void loop() {
  if (Serial.available()) {
    Serial.println(Serial.readStringUntil('\n'));
  }
}
