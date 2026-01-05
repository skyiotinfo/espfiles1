#include <Arduino.h>

#define TRIG1 D1
#define ECHO1 D2
#define TRIG2 D5
#define ECHO2 D6
#define PUMP_PIN D7       // use PWM-capable pin via MOSFET

#define SOUND_SPEED 0.034
#define SPRAY_DURATION 5000

float distance1_in, distance2_in;
unsigned long sprayStart = 0;
bool spraying = false;
int sprayPWM = 0;

float readUltrasonicInches(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW); delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long start = micros();
  while (digitalRead(echoPin) == LOW) if (micros() - start > 30000) return -1;
  long echoStart = micros();
  while (digitalRead(echoPin) == HIGH) if (micros() - echoStart > 30000) return -1;
  long duration = micros() - echoStart;

  float cm = duration * SOUND_SPEED / 2.0;
  return cm / 2.54;
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG1, OUTPUT); pinMode(ECHO1, INPUT);
  pinMode(TRIG2, OUTPUT); pinMode(ECHO2, INPUT);
  pinMode(PUMP_PIN, OUTPUT);
  analogWriteFreq(1000);  // 1 kHz PWM frequency
  analogWriteRange(1023); // NodeMCU PWM range (0–1023)
}

void loop() {
  distance1_in = readUltrasonicInches(TRIG1, ECHO1);
  distance2_in = readUltrasonicInches(TRIG2, ECHO2);

  if (distance1_in != -1 && distance2_in != -1) {
    Serial.printf("L: %.1f in  R: %.1f in\n", distance1_in, distance2_in);

    bool tyreNear = (distance1_in >= 8 && distance1_in <= 18 &&
                     distance2_in >= 8 && distance2_in <= 18);

    if (tyreNear && !spraying) {
      spraying = true;
      sprayStart = millis();
      Serial.println("Sprinkler ON");
    }

    if (spraying) {
      // Adjust spray speed based on tyre distance
      float avgDist = (distance1_in + distance2_in) / 2.0;
      sprayPWM = map(avgDist, 8, 18, 1023, 400);  // closer = stronger spray
      sprayPWM = constrain(sprayPWM, 400, 1023);
      analogWrite(PUMP_PIN, sprayPWM);
      Serial.printf("PWM: %d\n", sprayPWM);
    }
  }

  // Turn OFF after fixed duration
  if (spraying && millis() - sprayStart >= SPRAY_DURATION) {
    analogWrite(PUMP_PIN, 0);   // stop pump
    spraying = false;
    Serial.println("Sprinkler OFF");
  }

  delay(50);
}
