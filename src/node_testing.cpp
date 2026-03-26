#include <ESP8266WiFi.h>

#define HALL_ROT_PIN 12
#define PULSES_PER_REV 1   // Change if more magnets are used

volatile uint32_t rotationCount = 0;

unsigned long lastTime = 0;
uint32_t lastCount = 0;

void IRAM_ATTR rotationISR() {
  rotationCount++;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n===== ESP RESTARTED =====");
  Serial.println("Hall Sensor Speed Test");

  pinMode(HALL_ROT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HALL_ROT_PIN), rotationISR, RISING);

  lastTime = millis();
}

void loop() {

  unsigned long currentTime = millis();

  if (currentTime - lastTime >= 500) {

    uint32_t currentCount = rotationCount;
    uint32_t pulses = currentCount - lastCount;

    float rpm = (pulses * 120.0) / PULSES_PER_REV;

    Serial.print("RPM: ");
    Serial.println(rpm);

    lastCount = currentCount;
    lastTime = currentTime;
  }
}