#include <LoRa.h>
#include <SPI.h>

#define ss 15
#define rst 16
#define dio0 2
#define networkid "2022"
#define deviceid "01"

int counter = 10;
const int hpin = D1;
const int sled = LED_BUILTIN;

int vstate1 = 1;
int vstate2 = 1;

void setup()
{
  Serial.begin(115200);
  pinMode(D0, WAKEUP_PULLUP);
  pinMode(hpin, OUTPUT);
  pinMode(sled, OUTPUT);
  digitalWrite(hpin, HIGH);
  digitalWrite(sled, HIGH);

  while (!Serial)
    ;

  Serial.println("LoRa Sender");

  LoRa.setPins(ss, rst, dio0);
  LoRa.setSyncWord(0xA2);
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(62.5E3);

  while (!LoRa.begin(433920000))
  {
    Serial.println(".");
    delay(500);
  }

  Serial.println("LoRa Initializing OK!");
}

void send_data()
{
  for (int i = 0; i <= 10; i++)
  {
    LoRa.beginPacket();
    LoRa.print(networkid);
    LoRa.print(deviceid);
    LoRa.print(vstate1);
    LoRa.print(vstate2);
    LoRa.endPacket();

    Serial.print(networkid);
    Serial.print(deviceid);
    Serial.print(vstate1);
    Serial.print(vstate2);
    Serial.print("..");

    delay(100);
  }
  Serial.println();
}

void loop()
{
  Serial.print("Sending packet: ");
  Serial.println(counter);

  counter++;
  if (counter >= 200)
  {
    counter = 10;
  }

  send_data();

  digitalWrite(sled, LOW);
  delay(50);
  digitalWrite(sled, HIGH);
  delay(50);

  delay(2000);
}
