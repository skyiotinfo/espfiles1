#include <Arduino.h>
#include <Servo.h>

#define TRIG1 D1
#define ECHO1 D2

#define TRIG2 D7
#define ECHO2 D6

#define SERVO1_PIN D4
#define SERVO2_PIN D5

#define MIN_ANGLE 0
#define MID_ANGLE 90
#define MAX_ANGLE 180
#define SERVO_DEG 0
#define STEP_DELAY 1
#define MIN_DISTANCE 25

float dist1;
float dist2;

Servo servo1;
Servo servo2;

float readDistance(int trigPin, int echoPin)
{

  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);

  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);

  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH);

  float distance = duration * 0.034 / 2;

  return distance;
}

void setup()
{

  Serial.begin(115200);

  pinMode(TRIG1, OUTPUT);
  pinMode(ECHO1, INPUT);

  pinMode(TRIG2, OUTPUT);
  pinMode(ECHO2, INPUT);

  servo1.attach(SERVO1_PIN, 500, 2500);
  servo2.attach(SERVO2_PIN, 500, 2500);

  servo1.write(0);
  servo2.write(0);

  Serial.println("System Ready");
}
void readultrasonicsensor()
{

  dist1 = readDistance(TRIG1, ECHO1);
  delay(10);

  dist2 = readDistance(TRIG2, ECHO2);

  Serial.print("D1: ");
  Serial.print(dist1);

  Serial.print(" cm  |  D2: ");
  Serial.print(dist2);
  Serial.println(" cm");
};

void servoRotate()
{

  servo1.write(MIN_ANGLE);
  servo2.write(MIN_ANGLE);

  delay(1000);

  servo1.write(MID_ANGLE);
  servo2.write(MID_ANGLE);

  delay(1000);
  servo1.write(MAX_ANGLE);
  servo2.write(MAX_ANGLE);
  delay(1000);

  servo1.write(MID_ANGLE);
  servo2.write(MID_ANGLE);

  delay(1000);

  servo1.write(MIN_ANGLE);
  servo2.write(MIN_ANGLE);
};

void loop()
{

  readultrasonicsensor();
  if ((dist1 < MIN_DISTANCE && dist1 > 20) && (dist2 < MIN_DISTANCE && dist2 > 20))
  {

    Serial.println("Object Detected");
    servoRotate();
  }
  delay(5000);
}