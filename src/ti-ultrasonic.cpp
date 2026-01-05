#include <Arduino.h>

#define TRIG_PIN D5
#define ECHO_PIN D6

const float MIN_DISTANCE = 23.0;    
const float MAX_DISTANCE = 600.0;   

const int SAMPLES = 7;            
const unsigned long PULSE_TIMEOUT = 60000; 

long duration;

void setup() {
  Serial.begin(9600);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  Serial.println("JSN-SR04T Water Level System Started");
  Serial.println("-----------------------------------");
}

int getModeDistance() {
  int readings[SAMPLES];
  int count = 0;

  for (int i = 0; i < SAMPLES; i++) {

    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duration = pulseIn(ECHO_PIN, HIGH, PULSE_TIMEOUT);
    if (duration == 0) continue;

    int distance = (duration * 0.034) / 2;

    if (distance >= MIN_DISTANCE && distance <= MAX_DISTANCE) {
      readings[count++] = distance;
    }

    delay(60);
  }

  if (count < 3) return -1;

  int mode = readings[0];
  int maxCount = 0;

  for (int i = 0; i < count; i++) {
    int freq = 0;
    for (int j = 0; j < count; j++) {
      if (readings[j] == readings[i]) freq++;
    }

    if (freq > maxCount) {
      maxCount = freq;
      mode = readings[i];
    }
  }

  return mode;
}


void loop() {

  static float lastPercent = 0;

  int distance = getModeDistance();

  if (distance == -1) {
    Serial.println("ERROR: No valid sensor reading");
    Serial.println("-----------------------------");
    delay(1000);
    return;
  }

  distance = constrain(distance, MIN_DISTANCE, MAX_DISTANCE);

  float waterPercent =
      (MAX_DISTANCE - distance) * 100.0 /
      (MAX_DISTANCE - MIN_DISTANCE);

  waterPercent = (0.7 * lastPercent) + (0.3 * waterPercent);
  lastPercent = waterPercent;

  Serial.print("Distance: ");
  Serial.print(distance);
  Serial.println(" cm");

  Serial.print("Water Level: ");
  Serial.print(waterPercent, 2);
  Serial.println(" %");

  if (waterPercent <= 10) {
    Serial.println("STATUS: LOW TANK");
  }
  else if (waterPercent >= 90) {
    Serial.println("STATUS: HIGH TANK");
  }
  else {
    Serial.println("STATUS: TANK LEVEL NORMAL");
  }

  Serial.println("-----------------------------");
  delay(1000);
}
