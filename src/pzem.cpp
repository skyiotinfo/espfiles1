#include <PZEM004Tv30.h>
#include <SoftwareSerial.h>

#define PZEM_RX D5  // GPIO12
#define PZEM_TX D6  // GPIO14

SoftwareSerial pzemSerial(PZEM_RX, PZEM_TX);
PZEM004Tv30 pzem(pzemSerial);

void setup() {
    Serial.begin(115200);
    pzemSerial.begin(9600);

    Serial.println();
    Serial.println("PZEM-004T ESP8266 Started");
}

void loop() {
    float voltage = pzem.voltage();
    float current = pzem.current();
    float power   = pzem.power();
    float energy  = pzem.energy();
    float freq    = pzem.frequency();
    float pf      = pzem.pf();

    if (!isnan(voltage)) {
        Serial.printf("Voltage   : %.2f V\n", voltage);
        Serial.printf("Current   : %.3f A\n", current);
        Serial.printf("Power     : %.2f W\n", power);
        Serial.printf("Energy    : %.2f Wh\n", energy);
        Serial.printf("Frequency : %.2f Hz\n", freq);
        Serial.printf("PF        : %.2f\n", pf);
        Serial.println("------------------------");
    } else {
        Serial.println("⚠️ PZEM not detected");
    }

    delay(2000);
}
