#include <Wire.h>
#include "RTClib.h"

RTC_DS1307 rtc;

void setup() {
  Serial.begin(115200);
  Wire.begin(D6, D5);   // SDA, SCL for NodeMCU

  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  if (!rtc.isrunning()) {
    Serial.println("RTC not running, setting time...");
    rtc.adjust(DateTime(2025, 11, 22, 15, 30, 00)); // Set time from your PC
  }
}

void set_time(){
    rtc.adjust(DateTime(2025, 11, 22, 18, 00, 00)); // Set time from your PC
}

void loop() {
  DateTime now = rtc.now();

  Serial.print(now.day());
  Serial.print("/");
  Serial.print(now.month());
  Serial.print("/");
  Serial.print(now.year());
  Serial.print("  ");
  
  int hr = now.hour();
  int min = now.minute();
  if (hr != 18)
  {
    Serial.println("Setting time...Hour");
    set_time();
  }
  else
  {
      int diff = min - 0;
      if (diff > 5 || diff < -5)
      {
        Serial.println("Setting time...Hour");
        set_time();
      }
  }
  Serial.print(now.hour());
  Serial.print(":");
  Serial.print(now.minute());
  Serial.print(":");
  Serial.println(now.second());

  delay(1000);
}
