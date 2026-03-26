#include <Arduino.h>
#include <EEPROM.h>
#include <TM1637Display.h>

#define M1_PIN D5
#define M2_PIN D6
#define B1_PIN D7
#define B2_PIN D8

#define BUTTON_PIN D9

#define CLK D3
#define DIO D4

TM1637Display display(CLK, DIO);

int addr1 = 0;

unsigned long RUN_TIME = 10UL * 60 * 1000;
unsigned long motorStartTime = 0;

bool runningFirstPair = true;

int motor_duration = 10; // minutes

void setupPins();
void startM1B1();
void startM2B2();
void stopAll();

void setup() {

  Serial.begin(115200);

  EEPROM.begin(512);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  display.setBrightness(0x0f);
  display.clear();

  setupPins();

  Serial.println("System Started");

  // Read saved duration
  motor_duration = EEPROM.read(addr1);

  if (motor_duration < 1 || motor_duration > 240)
    motor_duration = 10;

  Serial.print("Saved Duration: ");
  Serial.print(motor_duration);
  Serial.println(" minutes");

  display.showNumberDec(motor_duration);

  // Button pressed at startup → change duration
  if (digitalRead(BUTTON_PIN) == LOW) {

    Serial.println("Duration Setting Mode");
    delay(500);

    while (digitalRead(BUTTON_PIN) == LOW) {

      motor_duration += 5;

      if (motor_duration > 240)
        motor_duration = 5;

      Serial.print("New Duration: ");
      Serial.print(motor_duration);
      Serial.println(" minutes");

      display.showNumberDec(motor_duration);

      EEPROM.write(addr1, motor_duration);
      EEPROM.commit();

      delay(600);
    }
  }

  RUN_TIME = motor_duration * 60UL * 1000UL;

  Serial.print("Motor Run Time: ");
  Serial.print(motor_duration);
  Serial.println(" minutes");

  display.showNumberDec(motor_duration);

  motorStartTime = millis();

  startM1B1();
}

void loop() {

  unsigned long now = millis();

  if (now - motorStartTime >= RUN_TIME) {

    stopAll();

    Serial.println("Cycle Complete");

    delay(1000);

    runningFirstPair = !runningFirstPair;

    if (runningFirstPair) {
      startM1B1();
    } else {
      startM2B2();
    }

    motorStartTime = millis();
  }
}

void setupPins() {

  pinMode(M1_PIN, OUTPUT);
  pinMode(M2_PIN, OUTPUT);
  pinMode(B1_PIN, OUTPUT);
  pinMode(B2_PIN, OUTPUT);

  stopAll();
}

void startM1B1() {

  Serial.println("Starting M1 + B1");

  digitalWrite(M1_PIN, HIGH);
  digitalWrite(B1_PIN, HIGH);

  digitalWrite(M2_PIN, LOW);
  digitalWrite(B2_PIN, LOW);
}

void startM2B2() {

  Serial.println("Starting M2 + B2");

  digitalWrite(M2_PIN, HIGH);
  digitalWrite(B2_PIN, HIGH);

  digitalWrite(M1_PIN, LOW);
  digitalWrite(B1_PIN, LOW);
}

void stopAll() {

  digitalWrite(M1_PIN, LOW);
  digitalWrite(M2_PIN, LOW);
  digitalWrite(B1_PIN, LOW);
  digitalWrite(B2_PIN, LOW);
}