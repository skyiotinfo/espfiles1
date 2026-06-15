#include <Arduino.h>
#include <Servo.h>

#define TRIG1   D1
#define ECHO1   D2
#define TRIG2   D7
#define ECHO2   D6

#define SERVO1_PIN  D4
#define SERVO2_PIN  D5

#define SOUND_SPEED_CM_US  0.034   
#define MAX_DISTANCE_CM    400     
#define MIN_DISTANCE_CM    2       
#define TIMEOUT_US         30000   
#define READ_SAMPLES       3      

#define THRESHOLD_CM       25      
#define SYNC_WINDOW_MS     100     

#define SERVO_MIN_ANGLE    0
#define SERVO_MAX_ANGLE    180
#define SERVO_STEP_DEG     10      
#define STEP_DELAY_MS      20     

Servo servo1;
Servo servo2;

// Global state
float dist1 = -1;
float dist2 = -1;

unsigned long lastDetectTime1 = 0;
unsigned long lastDetectTime2 = 0;

bool sweepActive = false;
int currentAngle = SERVO_MIN_ANGLE;
bool movingForward = true;
unsigned long lastServoMove = 0;

float readUltrasonicCM(int trigPin, int echoPin) {
  float total = 0;
  int valid = 0;

  for (int i = 0; i < READ_SAMPLES; i++) {
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);

    long duration = pulseIn(echoPin, HIGH, TIMEOUT_US);
    if (duration > 0) {
      float distance = (duration * SOUND_SPEED_CM_US) / 2.0;
      if (distance > MIN_DISTANCE_CM && distance < MAX_DISTANCE_CM) {
        total += distance;
        valid++;
      }
    }
    delay(5);  
  }

  if (valid == 0) return -1;
  return total / valid;
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG1, OUTPUT);
  pinMode(ECHO1, INPUT);
  pinMode(TRIG2, OUTPUT);
  pinMode(ECHO2, INPUT);

  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);
  servo1.write(SERVO_MIN_ANGLE);
  servo2.write(SERVO_MIN_ANGLE);

  Serial.println("Dual Ultrasonic + Dual Servo System Ready");
  Serial.println("Both sensors must detect object within 25 cm and 100 ms to start a sweep.");
}

void loop() {
  dist1 = readUltrasonicCM(TRIG1, ECHO1);
  delay(5);                     // small delay between readings
  dist2 = readUltrasonicCM(TRIG2, ECHO2);

  if (dist1 > 0 && dist1 < THRESHOLD_CM) {
    lastDetectTime1 = millis();
  }
  if (dist2 > 0 && dist2 < THRESHOLD_CM) {
    lastDetectTime2 = millis();
  }

  if (!sweepActive) {
    unsigned long timeDiff = (lastDetectTime1 > lastDetectTime2) ?
                              (lastDetectTime1 - lastDetectTime2) :
                              (lastDetectTime2 - lastDetectTime1);

    bool bothNowBelowThreshold = (dist1 > 0 && dist1 < THRESHOLD_CM) &&
                                 (dist2 > 0 && dist2 < THRESHOLD_CM);

    if (bothNowBelowThreshold && timeDiff < SYNC_WINDOW_MS) {
      sweepActive = true;
      movingForward = true;
      currentAngle = SERVO_MIN_ANGLE;
      lastServoMove = 0;    
      Serial.println(">>> SIMULTANEOUS DETECTION – SWEEP START <<<");
    }
    delay(5000);

  }

  if (sweepActive && (millis() - lastServoMove >= STEP_DELAY_MS)) {
    lastServoMove = millis();

    servo1.write(currentAngle);
    servo2.write(currentAngle);

    if (movingForward) {
      currentAngle += SERVO_STEP_DEG;
      if (currentAngle >= SERVO_MAX_ANGLE) {
        currentAngle = SERVO_MAX_ANGLE;
        movingForward = false;
      }
    } else {
      currentAngle -= SERVO_STEP_DEG;
      if (currentAngle <= SERVO_MIN_ANGLE) {
        currentAngle = SERVO_MIN_ANGLE;
        sweepActive = false;          
        lastDetectTime1 = 0;
        lastDetectTime2 = 0;
        Serial.println(">>> SWEEP COMPLETE – SERVOS STOPPED <<<");
      }
    }

  }

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 200) {
    lastPrint = millis();
    Serial.print("D1: ");
    Serial.print(dist1);
    Serial.print(" cm  |  D2: ");
    Serial.print(dist2);
    Serial.print(" cm  |  Servos: ");
    Serial.println(sweepActive ? "SWEEPING" : "IDLE");
  }

}