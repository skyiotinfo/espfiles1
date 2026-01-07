#include <Arduino.h>

const int input1 = D9;

void setup()

{
  Serial.begin(115200); 
  pinMode(input1, INPUT_PULLUP);
}


void loop(){
     
  digitalWrite(vpin, HIGH);
  delay(3000);
   Serial.println("Blink");
  digitalWrite(vpin, LOW);
 delay(3000);
 Serial.println("Stop");


}
