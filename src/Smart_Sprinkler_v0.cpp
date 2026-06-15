#include <Arduino.h>
#include <Servo.h>

#define TRIG D1
#define ECHO D2
#define SERVO_PIN D4

#define SOUND_SPEED 0.034

Servo myServo;

float distance_cm;
float lastDistance = 0;
// Servo control
unsigned long lastMoveTime = 0;
int servoPos = 0;
bool forward = true;
bool servoActive = false;

const int stepDelay = 1; // ~28ms per step → total ~10 sec

float readUltrasonicCM(int trigPin, int echoPin) {
  float total = 0;
  int validReadings = 0;

  for (int i = 0; i < 5; i++) {
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);

    long duration = pulseIn(echoPin, HIGH, 30000);

    if (duration > 0) {
      float distance = (duration * SOUND_SPEED) / 2.0;

      // ignore unrealistic values
      if (distance > 2 && distance < 400) {
        total += distance;
        validReadings++;
      }
    }

    delay(5);
  }

  if (validReadings == 0) return -1;

  return total / validReadings;
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);

  myServo.attach(SERVO_PIN);
  myServo.write(0);

  Serial.println("Ultrasonic + Servo Test");
}

void loop() {

  if (!servoActive) {
    distance_cm = readUltrasonicCM(TRIG, ECHO);

    if (distance_cm != -1) {

      if (abs(distance_cm - lastDistance) < 20) {

        Serial.print("Distance: ");
        Serial.print(distance_cm);
        Serial.println(" cm");

        if (distance_cm < 50) {
          servoActive = true;
        }
      }

      lastDistance = distance_cm;
    }
  }

  // Servo movement
  if (servoActive && millis() - lastMoveTime >= stepDelay) {
    lastMoveTime = millis();

    myServo.write(servoPos);

    if (forward) {
      servoPos += 10;
      if (servoPos >= 180) forward = false;
    } else {
      servoPos -= 10;
      if (servoPos <= 0) {
        forward = true;
        servoActive = false;
      }
    }
  }
}