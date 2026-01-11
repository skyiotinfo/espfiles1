#include <Wire.h>
#include <RTClib.h>
#include <EEPROM.h>
RTC_DS1307 rtc;

#include <WiFiClientSecure.h>
#include <ESPSupabase.h> 
#ifdef ESP8266
  #include <ESP8266WiFi.h>  
#else
  #include <WiFi.h>
#endif

Supabase supabase;
#define EEPROM_SIZE 128

#define MOTOR_PIN D8
#define CLK_PIN   D3
#define DIO_PIN   D4

//const int ot_sensor = D1;
//const int ot_status = D2;
const int auto_status = D7;
const int input1 = D9;
int temp_count1 = 0;

int lastDay = -1;

void init_devicedata();
void connect_supabase();
String eeprom_read_data(int addr, int duration, String read_data);
String eeprom_write_data(String data1, int duration, String write_data);

struct Schedule {
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  bool triggered;  // prevents repeat
};

Schedule schedules[3];
uint8_t scheduleCount;

void readSchedulesFromEEPROM() {
  scheduleCount = EEPROM.read(0);
  if (scheduleCount > 3) scheduleCount = 3;

  for (int i = 0; i < scheduleCount; i++) {
    int base = 1 + (i * 3);
    schedules[i].hour   = EEPROM.read(base);
    schedules[i].minute = EEPROM.read(base + 1);
    schedules[i].second = EEPROM.read(base + 2);
    schedules[i].triggered = false;
  }
  EEPROM.end();
}

DateTime getCurrentTime() {
  return rtc.now();
}

void checkSchedules(DateTime now) {
  for (int i = 0; i < scheduleCount; i++) {

    if (!schedules[i].triggered &&
        now.hour()   == schedules[i].hour &&
        now.minute() == schedules[i].minute &&
        now.second() == schedules[i].second) {

      schedules[i].triggered = true;  // fire once

      Serial.print("Schedule ");
      Serial.print(i + 1);
      Serial.println(" matched!");

      // 👉 Place motor / relay logic here
    }
  }
}



void resetDailyTriggers(DateTime now) {
  if (now.day() != lastDay) {
    lastDay = now.day();

    for (int i = 0; i < scheduleCount; i++) {
      schedules[i].triggered = false;
    }
  }
}


void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  pinMode(input1, INPUT_PULLUP);
  //pinMode(ot_sensor, INPUT_PULLUP);
  //pinMode(ot_status, OUTPUT);
  pinMode(auto_status, OUTPUT);
  bool rtc_status = rtc.begin();
  delay(1000);
  Wire.begin();

  if (!rtc.begin()) {
    Serial.println("RTC not found");
    while (1);
  }

  readSchedulesFromEEPROM();
}

void loop() {
  DateTime now = getCurrentTime();

  resetDailyTriggers(now);
  checkSchedules(now);

  delay(200);  // short delay is fine
}
