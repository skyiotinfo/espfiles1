#include <Arduino.h>

#define ut_sensor D1  // D2 pin as input

void setup() {
  Serial.begin(115200);       // Start Serial Monitor
  pinMode(ut_sensor, INPUT);  // Set D2 as input
  Serial.println("D2 Sensor Monitor Started");
}

void loop() {
  int sensorVal = digitalRead(ut_sensor);  // Read D2
  Serial.print("D2 Value: ");
  Serial.println(sensorVal);               // Print value (0 or 1)
  delay(500);                              // Print every 500ms
}
