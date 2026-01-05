#include <Arduino.h>

#define TRIG_PIN D1
#define ECHO_PIN D2
#define SOUND_SPEED 0.034

long duration;
float distanceCm;

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  Serial.println("JSN-SR04T Ultrasonic Test Starting...");
}

void loop() {
  // Trigger pulse
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Read echo pulse
  duration = pulseIn(ECHO_PIN, HIGH, 30000); // timeout 30 ms

  if (duration == 0) {
    Serial.println("No echo received");
  } else {
    distanceCm = duration * SOUND_SPEED / 2;
    Serial.print("Distance (cm): ");
    Serial.println(distanceCm);
  }

  delay(1000);
}
