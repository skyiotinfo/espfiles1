#include <SoftwareSerial.h>
#include "pzem_reader.h"

#define RX D5
#define TX D6

SoftwareSerial pzemSerial(RX, TX);
PZEMReader pzem(pzemSerial);

void setup() {
    Serial.begin(115200);
    pzemSerial.begin(9600);
}

void loop() {
    PZEMData d = pzem.readData();

    Serial.printf("Voltage: %.2f V\n", d.voltage);
    Serial.printf("Current: %.2f A\n", d.current);
    Serial.printf("Power  : %.2f W\n", d.power);
    Serial.printf("Energy : %.2f Wh\n", d.energy);
    Serial.printf("Freq   : %.2f Hz\n", d.frequency);
    Serial.printf("PF     : %.2f\n", d.pf);
    Serial.printf("VA     : %.2f\n", d.apparentPower);
    Serial.printf("VAR    : %.2f\n", d.reactivePower);
    Serial.println("------------------");

    delay(2000);
}