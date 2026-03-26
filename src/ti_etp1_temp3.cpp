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

unsigned long RUN_TIME_M1 = 10UL * 60 * 1000;  // adjustable
unsigned long RUN_TIME_M2 = 10UL * 60 * 1000;  // fixed 10 min
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

  display.showNumberDec(motor_duration);

  Serial.print("Saved Duration: ");
  Serial.println(motor_duration);

  // -------- SETTING MODE --------
  if (digitalRead(BUTTON_PIN) == LOW) {

    Serial.println("Duration Setting Mode");
    delay(500);

    while (digitalRead(BUTTON_PIN) == LOW) {

      motor_duration += 5;

      if (motor_duration > 240)
        motor_duration = 5;

      display.showNumberDec(motor_duration);

      Serial.print("New Duration: ");
      Serial.println(motor_duration);

      delay(500);
    }

    // Save ONLY once after release (IMPORTANT)
    EEPROM.write(addr1, motor_duration);
    EEPROM.commit();

    Serial.println("Saved to EEPROM");
  }

  // Assign runtimes
  RUN_TIME_M1 = motor_duration * 60UL * 1000UL;
  RUN_TIME_M2 = 10UL * 60 * 1000UL;

  Serial.print("M1+B1 Run Time: ");
  Serial.print(motor_duration);
  Serial.println(" minutes");

  Serial.println("M2+B2 Run Time: 10 minutes");

  motorStartTime = millis();

  startM1B1();
}

void loop() {

  unsigned long now = millis();

  if (runningFirstPair) {

    if (now - motorStartTime >= RUN_TIME_M1) {

      stopAll();
      Serial.println("M1+B1 Cycle Complete");

      delay(1000);

      runningFirstPair = false;
      startM2B2();
      motorStartTime = millis();
    }

  } else {

    if (now - motorStartTime >= RUN_TIME_M2) {

      stopAll();
      Serial.println("M2+B2 Cycle Complete");

      delay(1000);

      runningFirstPair = true;
      startM1B1();
      motorStartTime = millis();
    }
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