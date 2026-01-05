#include <Arduino.h>

const int buzzer = D8;
const int sled = LED_BUILTIN;
 int i=0;

void tank_blinkall(){
digitalWrite(sled, LOW);
  delay(100);
  digitalWrite(sled, HIGH);
  delay(100);
}

void setup() {
  Serial.begin(115200); // Starts the serial communication
  pinMode(buzzer, OUTPUT);
  pinMode(sled, OUTPUT);
  digitalWrite(sled, HIGH);

  


}

void loop() {
 
  for ( i = 0; i < 100; i++)
  {
  digitalWrite(buzzer, HIGH);
  delay(5000);
  digitalWrite(buzzer, LOW);
  delay(5000);
  }
  
 
 }