#include <Arduino.h>
#include "zmpt101b.h"

#define SENSOR_PIN A0

// Calibration factor (adjust later)
ZMPT101B sensor(SENSOR_PIN, 275.0);

void setup() {
    Serial.begin(115200);
}

void loop() {
    float voltage = sensor.readVoltageRMS();

    Serial.print("AC Voltage: ");
    Serial.print(voltage);
    Serial.println(" V");

    delay(5000);
}