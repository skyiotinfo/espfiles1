#include <Arduino.h>

void setup() {
  Serial.begin(115200);
}

void loop() {
  Serial.println("HELLO FROM ESP07 anupam here i am testing PZEM-004T");
  delay(2000);
}
