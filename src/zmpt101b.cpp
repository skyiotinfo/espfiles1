#include <EmonLib.h>   // Include Emon Library

EnergyMonitor emon1;

#define VOLTAGE_PIN A0   // ZMPT101B output

void setup() {
  Serial.begin(115200);

  // Voltage(pin, calibration, phase_shift)
  // Calibration value needs tuning
  emon1.voltage(VOLTAGE_PIN, 533.70, 1.7);

  Serial.println("ZMPT101B Voltage Measurement Started");
}

void loop() {
  emon1.calcVI(20, 2000);   // crossings, timeout
  float voltage = emon1.Vrms;

  if (voltage < 10) voltage = 0; // noise filtering

  Serial.print("Voltage: ");
  Serial.print(voltage);
  Serial.println(" V");

  delay(1000);
}
