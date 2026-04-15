#include <Arduino.h>
#include <Wire.h>
#include <DS1307RTC.h>
#include <TimeLib.h>

uint8_t set_Sec   = 0;     /* Set the Seconds */
uint8_t set_Minute=20;    /* Set the Minutes */
uint8_t set_Hour  = 13;     /* Set the Hours */
uint8_t set_Day   = 10;     /* Set the Day */
uint8_t set_Month = 4;    /* Set the Month */
uint16_t set_Year = 2026;  /* Set the Year */

void setup() {
  Serial.begin(115200);

  tmElements_t tm;

  // Set time into structure
  tm.Second = set_Sec;
  tm.Minute = set_Minute;
  tm.Hour   = set_Hour;
  tm.Day    = set_Day;
  tm.Month  = set_Month;
  tm.Year   = CalendarYrToTm(set_Year); // converts 2025 → internal year format

  // Write to RTC
  if (RTC.write(tm)) {
    Serial.println("RTC time set successfully!");
  } else {
    Serial.println("Error: Unable to set RTC time.");
  }

  delay(1000);

  // Read back to confirm
  if (RTC.read(tm)) {
    Serial.print("You have set: ");
    Serial.print("\nTime: ");
    Serial.print(tm.Hour);
    Serial.print(":");
    Serial.print(tm.Minute);
    Serial.print(":");
    Serial.print(tm.Second);

    Serial.print("\nDate: ");
    Serial.print(tm.Day);
    Serial.print(".");
    Serial.print(tm.Month);
    Serial.print(".");
    Serial.print(tmYearToCalendar(tm.Year));
    Serial.println();
  } else {
    Serial.println("Error: Unable to read RTC!");
  }
}

void loop() {
  tmElements_t tm;
  if (RTC.read(tm)) {
    Serial.print("Current Time: ");
    Serial.print(tm.Hour);
    Serial.print(":");
    Serial.print(tm.Minute);
    Serial.print(":");
    Serial.print(tm.Second);
    Serial.print("  Date: ");
    Serial.print(tm.Day);
    Serial.print("/");
    Serial.print(tm.Month);
    Serial.print("/");
    Serial.println(tmYearToCalendar(tm.Year));
  } else {
    Serial.println("RTC read error!");
  }
  delay(1000);
}
