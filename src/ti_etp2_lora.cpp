#include <Arduino.h>
#include <LoRa.h>
#include <SPI.h>

#define SS_PIN  15
#define RST_PIN 16
#define DIO0_PIN 2

#define M3_PIN  D1
#define M4_PIN  D2
#define FLOAT_SENSOR D5

#define NETWORK_ID "1023"
#define DEVICE_ID  "08"

const unsigned long LORA_INTERVAL = 2000;
unsigned long lastLoRaSend = 0;

int currentLoRaValue = 0;

void setupPins();
void setupLoRa();
void controlLogic();
void sendLoRa(int value);
void startM3();
void startM4();
void stopMotors();

void setup() {
  Serial.begin(115200);
  setupPins();
  setupLoRa();
  Serial.println("System Ready");
}

void loop() {
  controlLogic();
  delay(5);
}

void setupPins() {
  pinMode(M3_PIN, OUTPUT);
  pinMode(M4_PIN, OUTPUT);
  pinMode(FLOAT_SENSOR, INPUT_PULLUP);
  stopMotors();
}

void setupLoRa() {
  LoRa.setPins(SS_PIN, RST_PIN, DIO0_PIN);
  LoRa.setSyncWord(0xA2);
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(62.5E3);

  while (!LoRa.begin(433920000)) {
    Serial.println("LoRa init failed...");
    delay(500);
  }
  Serial.println("LoRa Initialized");
}

void controlLogic() {

  unsigned long now = millis();

  static bool tankFull = true;
  static unsigned long lastToggle = 0;

  if (now - lastToggle >= 15000) {
    tankFull = !tankFull;
    lastToggle = now;

    Serial.print("Tank Changed To: ");
    Serial.println(tankFull ? "FULL" : "EMPTY");
  }

 
  // bool tankFull = digitalRead(FLOAT_SENSOR);

  static bool lastTankState = true;
  static bool send11Next = true;   

  if (tankFull != lastTankState) {

    if (!tankFull) {

      Serial.println("Tank EMPTY");

      if (send11Next)
        currentLoRaValue = 11;
      else
        currentLoRaValue = 22;
    }

    else {

      Serial.println("Tank FULL");

      currentLoRaValue = 0;   

      if (send11Next)
        startM3();
      else
        startM4();

      send11Next = !send11Next;  
    }

    lastTankState = tankFull;
  }

  if (now - lastLoRaSend >= LORA_INTERVAL) {
    lastLoRaSend = now;
    sendLoRa(currentLoRaValue);
  }
}


void sendLoRa(int value) {

  LoRa.beginPacket();
  LoRa.print(NETWORK_ID);
  LoRa.print(DEVICE_ID);
  LoRa.print(value);
  LoRa.endPacket();

  Serial.print("LoRa Sent: ");
  Serial.print(NETWORK_ID);
  Serial.print(DEVICE_ID);
  Serial.println(value);
}

void startM3() {
  Serial.println("Starting M3");
  digitalWrite(M3_PIN, HIGH);
  digitalWrite(M4_PIN, LOW);
}

void startM4() {
  Serial.println("Starting M4");
  digitalWrite(M4_PIN, HIGH);
  digitalWrite(M3_PIN, LOW);
}

void stopMotors() {
  digitalWrite(M3_PIN, LOW);
  digitalWrite(M4_PIN, LOW);
}