#include <Arduino.h>

const int input1 = D9;
const int apin = D6;
const int bpin = D7;
const int cpin = D8;
void setup()

{
  Serial.begin(115200); 
  pinMode(input1, INPUT_PULLUP);
  pinMode(apin, OUTPUT);
  digitalWrite(apin, LOW);
}


void loop(){
     
  digitalWrite(apin, HIGH);
  delay(3000);
   Serial.println("Blink");
  digitalWrite(apin, LOW);
 delay(3000);
 Serial.println("Stop");


}
