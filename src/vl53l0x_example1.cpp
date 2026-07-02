#include "Adafruit_VL53L0X.h"

Adafruit_VL53L0X lox = Adafruit_VL53L0X();
void move_motor(int d1);

  // L298N Motor Control with NodeMCU ESP8266

#define IN1 D7
#define IN2 D8
#define ENA D5   // PWM Speed Control
int sp=0;

void setup() {
  Serial.begin(115200);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(ENA, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // Turn off built-in LED

   // wait until serial port opens for native USB devices

  // wait until serial port opens for native USB devices
  while (! Serial) {
    delay(1);
  }
  
  Serial.println("Adafruit VL53L0X test");
  if (!lox.begin()) {
    Serial.println(F("Failed to boot VL53L0X"));
    while(1);
  }
  // power 
  Serial.println(F("VL53L0X API Simple Ranging example\n\n")); 
}


void loop() {
  VL53L0X_RangingMeasurementData_t measure;
    
  Serial.print("Reading a measurement... ");
  lox.rangingTest(&measure, false); // pass in 'true' to get debug data printout!
  int distance = measure.RangeMilliMeter;

  if (measure.RangeStatus != 4) {  // phase failures have incorrect data
    Serial.print("Distance (mm): "); Serial.println(distance);
  } else {
    Serial.println(" out of range ");
  }
  
  move_motor(distance);
}

void move_motor(int d1){
  if(d1 > 300 && d1 <= 800){
    // Motor Forward
    Serial.println("Motor Forward");
    digitalWrite(IN1, HIGH);
    digitalWrite(IN2, LOW);
    analogWrite(ENA, 150);   // Speed (0 - 1023)
    delay(100);
    analogWrite(ENA, 0);
    sp=sp+1;
  }
  else {
    // Motor Stop
    if(sp>0){
      digitalWrite(LED_BUILTIN, LOW);
      delay(2000);
      digitalWrite(LED_BUILTIN, HIGH);
      sp=0;
    }
    Serial.println("Motor Stop");
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
    analogWrite(ENA, 0);   // Speed (0 - 1023)
    delay(500);
  }
  delay(500);
}
