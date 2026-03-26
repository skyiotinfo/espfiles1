#include <Arduino.h>

#define M1_PIN D5
#define M2_PIN D6
#define B1_PIN D7
#define B2_PIN D8

const unsigned long RUN_TIME = 1UL * 60 * 1000; 

unsigned long motorStartTime = 0;
bool runningFirstPair = true;  

void setupPins();
void startM1B1();
void startM2B2();
void stopAll();

void setup() {
  Serial.begin(115200);
  setupPins();

  Serial.println("System Started");

  motorStartTime = millis();
  startM1B1();   
}

void loop() {

  unsigned long now = millis();

  if (now - motorStartTime >= RUN_TIME) {

    stopAll();
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