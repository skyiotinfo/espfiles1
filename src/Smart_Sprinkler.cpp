#include <Arduino.h>

#define TRIG1 D1
#define ECHO1 D2
#define TRIG2 D5
#define ECHO2 D6
const int buzzer = D8;

#define SOUND_SPEED 0.034 
#define BUZZER_DURATION 5000 

long duration1, duration2;
float distance1_in, distance2_in;

unsigned long buzzerStartTime = 0;
bool buzzerActive = false;

void setup() {
  Serial.begin(115200);

  pinMode(TRIG1, OUTPUT);
  pinMode(ECHO1, INPUT);
  pinMode(TRIG2, OUTPUT);
  pinMode(ECHO2, INPUT);
  pinMode(buzzer, OUTPUT);
  digitalWrite(buzzer, LOW);

  Serial.println("Dual Ultrasonic Sensors Test (Non-blocking)");
}

float readUltrasonicInches(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long startTime = micros();
  while (digitalRead(echoPin) == LOW) {
    if (micros() - startTime > 30000) return -1;
  }
  long echoStart = micros();

  while (digitalRead(echoPin) == HIGH) {
    if (micros() - echoStart > 30000) return -1; 
  }
  long echoEnd = micros();

  long duration = echoEnd - echoStart;
  float distance_cm = duration * SOUND_SPEED / 2.0;
  return distance_cm / 2.54;
}

void loop() {

  distance1_in = readUltrasonicInches(TRIG1, ECHO1);
  distance2_in = readUltrasonicInches(TRIG2, ECHO2);

  if (distance1_in != -1 && distance2_in != -1) {
    Serial.print("Sensor 1: ");
    Serial.print(distance1_in, 2);
    Serial.print(" in, Sensor 2: ");
    Serial.print(distance2_in, 2);
    Serial.println(" in");

  
    if (distance1_in >= 10 && distance1_in <= 15 &&
        distance2_in >= 10 && distance2_in <= 15) {
      if (!buzzerActive) {
        buzzerActive = true;
        buzzerStartTime = millis();
        digitalWrite(buzzer, HIGH);
        Serial.println("Spray - Buzzer ON");
      }
    }
  }

  
  if (buzzerActive && millis() - buzzerStartTime >= BUZZER_DURATION) {
    digitalWrite(buzzer, LOW);
    buzzerActive = false;
    Serial.println("Buzzer OFF");
  }

  delay(50); 
}
