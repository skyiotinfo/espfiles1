#include <Arduino.h>
#include <Wire.h>
#include <DS1307RTC.h>
#include <TimeLib.h>

void setup() {
  Serial.begin(115200);
  while (!Serial) ; // wait for Serial on some boards
}

void loop() {
  tmElements_t tm;

  if (RTC.read(tm)) {
    // Print Time
    Serial.print("\nTime: ");
    if (tm.Hour < 10) Serial.print("0");
    Serial.print(tm.Hour);
    Serial.print(":");
    if (tm.Minute < 10) Serial.print("0");
    Serial.print(tm.Minute);
    Serial.print(":");
    if (tm.Second < 10) Serial.print("0");
    Serial.print(tm.Second);

    // Print Date
    Serial.print("\nDate: ");
    if (tm.Day < 10) Serial.print("0");
    Serial.print(tm.Day);
    Serial.print(".");
    if (tm.Month < 10) Serial.print("0");
    Serial.print(tm.Month);
    Serial.print(".");
    Serial.print(tmYearToCalendar(tm.Year)); // convert back to YYYY
    Serial.println();
  } else {
    if (RTC.chipPresent()) {
      Serial.println("RTC stopped! Please set the time.");
    } else {
      Serial.println("RTC not found. Check wiring!");
    }
  }

  delay(1000);
}
