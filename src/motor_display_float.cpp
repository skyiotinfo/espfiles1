#include <EEPROM.h>
#include <Arduino.h>
#include <TM1637Display.h>

// Pins
#define CLK D3
#define DIO D4
#define ut_sensor D1     // Automatic sensor
#define buzzer D8
#define auto_status D7
#define input1 D9         // Manual button

TM1637Display display(CLK, DIO);

// Display segments
const uint8_t seg_empty[] = { SEG_A | SEG_D | SEG_E | SEG_F | SEG_G, SEG_D, SEG_D, SEG_D };
const uint8_t seg_full[]  = { SEG_A | SEG_G | SEG_F | SEG_E, SEG_A | SEG_G | SEG_F | SEG_E, SEG_D, SEG_D };

// Motor variables
int motor_status = 0;
int motor_status_manual = 0;
int motor_duration = 30; // minutes
int motor_time = 0;

void setup() {
  Serial.begin(115200);

  pinMode(buzzer, OUTPUT);
  pinMode(ut_sensor, INPUT);
  pinMode(auto_status, OUTPUT);
  pinMode(input1, INPUT_PULLUP);

  display.setBrightness(0x0f);
  display.setSegments(seg_empty);

  // Load motor duration
  EEPROM.begin(512);
  motor_duration = EEPROM.read(0);
  if (motor_duration < 1 || motor_duration > 100) motor_duration = 30;
  motor_time = motor_duration * 60;

  digitalWrite(buzzer, LOW);
  digitalWrite(auto_status, LOW);

  Serial.println("Setup complete");
}

void loop() {
  int sensorVal = digitalRead(ut_sensor);

  // --- Manual button handling ---
  static unsigned long buttonPressTime = 0;
  if (digitalRead(input1) == LOW) {
    delay(50); // debounce
    if (buttonPressTime == 0) buttonPressTime = millis();
    if (millis() - buttonPressTime > 2000) { // long press
      motor_duration += 5;
      if (motor_duration > 100) motor_duration = 5;
      EEPROM.write(0, motor_duration);
      EEPROM.commit();
      Serial.print("Motor duration updated: ");
      Serial.println(motor_duration);
      display.showNumberDec(motor_duration, false);
      delay(500);
      buttonPressTime = millis();
    }
  } else {
    // short press -> toggle motor manually
    if (buttonPressTime != 0 && millis() - buttonPressTime < 2000) {
      if (motor_status_manual == 0) {
        motor_status_manual = 1;
        motor_status = 1;
        motor_time = motor_duration * 60;
        digitalWrite(buzzer, HIGH);
        digitalWrite(auto_status, HIGH);
        display.setSegments(seg_full);
        Serial.println("Motor started manually");
      } else {
        motor_status_manual = 0;
        motor_status = 0;
        digitalWrite(buzzer, LOW);
        digitalWrite(auto_status, LOW);
       // display.setSegments(seg_empty);
        Serial.println("Motor stopped manually");
      }
    }
    buttonPressTime = 0;
  }

  // --- Automatic start/stop via sensor ---
  if (sensorVal == HIGH && motor_status_manual == 0 && motor_status == 0) {
    motor_status = 1;
    motor_time = motor_duration * 60;
    digitalWrite(buzzer, HIGH);
    digitalWrite(auto_status, HIGH);
    display.setSegments(seg_full);
    Serial.println("Motor auto-started by sensor");
  } else if (sensorVal == LOW && motor_status_manual == 0 && motor_status == 1) {
    motor_status = 0;
    digitalWrite(buzzer, LOW);
    digitalWrite(auto_status, LOW);
   // display.setSegments(seg_empty);
    Serial.println("Motor auto-stopped by sensor");
  }

  // --- Countdown ---
  if (motor_status == 1) {
    motor_time--;
    display.showNumberDec((motor_time / 60) + 1, false);
    if (motor_time <= 0) {
      motor_status = 0;
      motor_status_manual = 0;
      digitalWrite(buzzer, LOW);
      digitalWrite(auto_status, LOW);
      //display.setSegments(seg_empty);
      motor_time = motor_duration * 60;
      Serial.println("Motor duration ended");
    }
  }

  delay(1000); // loop delay
}
