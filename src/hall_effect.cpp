#include <Arduino.h>

#define HALL_PIN D5   // GPIO14

void setup() {
  Serial.begin(115200);
  pinMode(HALL_PIN, INPUT_PULLUP);
  Serial.println("Hall Test Started");
}

void loop() {
  Serial.print("Pin State: ");
  Serial.println(digitalRead(HALL_PIN));
  delay(500);
}
