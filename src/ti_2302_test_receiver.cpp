#include <EEPROM.h>
#include <Arduino.h>
#include <TM1637Display.h>
#include <LoRa.h>
#include <SPI.h>

#define CLK D3
#define DIO D4

#define ss D8
#define rst D0
#define dio0 D4

int addr1 = 0;
byte memval1;

int sensor_status = 2;

const int buzzer = D2;
const int input1 = D9;

int tcount1 = 0;
int tcount2 = 0;
int tcount3 = 0;

int motor_status = 0;
int motor_time = 0;
int motor_duration = 30;
bool full_display = false;
int value_count = 0;
int count = 0;
int temp_count1 = 0;

int signal_timeout = 0;
int timeout_val = 15;
bool manual_mode = false;

const uint8_t seg_empty[] = {
    0x00,
    SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,
    0x00};

const uint8_t seg_full[] = {
    0x00,
    SEG_A | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_E | SEG_F | SEG_G,
    0x00};

const uint8_t seg_nodata[] = {
    SEG_A | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_E | SEG_F | SEG_G,
    SEG_D | SEG_E | SEG_F,
    SEG_A | SEG_D | SEG_E | SEG_F | SEG_G};

TM1637Display display(CLK, DIO);

uint8_t blank[] = {0x00, 0x00, 0x00, 0x00};

void setup()
{

  Serial.begin(115200);

  EEPROM.begin(512);

  pinMode(input1, INPUT_PULLUP);

  pinMode(buzzer, OUTPUT);

  digitalWrite(buzzer, LOW);

  delay(1000);

  memval1 = EEPROM.read(addr1);

  motor_duration = memval1;

  if (motor_duration < 1 || motor_duration > 180)
  {
    motor_duration = 30;
  }

  motor_time = motor_duration * 60;

  display.setBrightness(0x0f);

  display.setSegments(blank);

  int temp_count = 100;

  if (digitalRead(input1) == 0)
  {

    while (temp_count >= 1)
    {

      if (digitalRead(input1) == 0)
      {

        if (motor_duration <= 180)
        {
          motor_duration += 5;
          temp_count++;
        }
        else
        {
          motor_duration = 0;
        }
      }

      temp_count--;

      delay(200);

      EEPROM.write(addr1, motor_duration);

      if (EEPROM.commit())
      {

        display.showNumberDec(5, false, 1, 0);

        display.showNumberDec(motor_duration, false);
      }
      else
      {

        Serial.println("EEPROM Commit Failed");
      }
    }
  }

  for (int i = 0; i < 8; i++)
  {

    display.showNumberDec(motor_duration, false);

    delay(200);

    display.clear();

    delay(200);
  }

  display.showNumberDec(0, false);

  LoRa.setPins(ss, rst, dio0);

  LoRa.setSyncWord(0xA2);

  LoRa.setSpreadingFactor(12);

  LoRa.setSignalBandwidth(62.5E3);

  int lora_retry = 0;

  while (!LoRa.begin(433920000))
  {

    Serial.println("LoRa Starting Failed");

    lora_retry++;

    if (lora_retry >= 30)
    {
      break;
    }

    delay(500);
  }

  Serial.println("LoRa Started");
}

void loop()
{

  int packetSize = LoRa.parsePacket();

  if (packetSize)
  {

    String LoRaData = LoRa.readString();
    Serial.print("Received Packet : ");
    Serial.println(LoRaData);

    String deviceid = LoRaData.substring(0, 6);
    String devicestatus = LoRaData.substring(6, 8);

    if (deviceid.equals("202201"))
    {
      signal_timeout = timeout_val;
      full_display = false;
    }

    if (deviceid.equals("202201") && devicestatus.equals("00"))
    {

      display.clear();
      display.setSegments(seg_full);
      delay(500);
      tcount1++;

      if (tcount1 >= 4)
      {
        digitalWrite(buzzer, LOW);
        motor_status = 0;
        manual_mode = false;
        display.showNumberDec(0, false);
        motor_time = motor_duration * 60;
        tcount1 = 0;
        sensor_status = 0;
        Serial.println("Tank Full - Motor OFF");
      }
    }
    else
    {
      tcount1 = 0;
    }

    if (deviceid.equals("202201") && devicestatus.equals("11"))
    {

      tcount2++;
      display.clear();
      display.setSegments(seg_empty);

      if (tcount2 >= 4)
      {
        if (!manual_mode)
        {
          digitalWrite(buzzer, HIGH);
          motor_status = 1;
        }
        tcount2 = 0;
        sensor_status = 1;
        Serial.println("Tank Empty - Motor ON");
      }
    }
    else
    {
      tcount2 = 0;
    }

    if (deviceid.equals("202201") && devicestatus.equals("22"))
    {
      tcount3++;
      if (tcount3 >= 2)
      {
        sensor_status = 2;
        tcount3 = 0;
        Serial.println("Sensor No Data");
      }
    }
    else
    {
      tcount3 = 0;
    }

    Serial.print("RSSI : ");
    Serial.println(LoRa.packetRssi());
  }

  if (signal_timeout <= 0)
  {

    sensor_status = 2;

    if (!manual_mode)
    {
      
    if (!full_display) {

      for (int i = 0; i < 3; i++) {

        display.clear();

        display.setSegments(seg_full);

        delay(500);

        display.clear();

        delay(300);
      }

      full_display = true;
    }

    // CONTINUOUSLY SHOW 0

    display.showNumberDec(0, false);
      digitalWrite(buzzer, LOW);
      motor_status = 0;
      motor_time = motor_duration * 60;
    }

    Serial.println("LoRa Signal Lost");
  }

  if (digitalRead(input1) == 0)
  {

    delay(200);

    if (motor_status == 0)
    {
      digitalWrite(buzzer, HIGH);
      motor_status = 1;
      manual_mode = true;
       full_display = false;
      motor_time = motor_duration * 60;
      value_count = 15;
      Serial.println("Manual Motor ON");
    }
    else
    {
      digitalWrite(buzzer, LOW);
      display.showNumberDec(0, false);
      motor_status = 0;
      manual_mode = false;
      value_count = 0;
      Serial.println("Manual Motor OFF");
    }

    while (digitalRead(input1) == 0)
    {
      delay(10);
    }
  }

  if (motor_status == 1)
  {

    motor_time--;

    display.showNumberDec(1, false, 1, 0);
    display.showNumberDec((motor_time / 60) + 1, false);

    if (motor_time <= 0 || (!manual_mode && sensor_status == 0))
    {
      temp_count1++;
      if (temp_count1 >= 5)
      {
        display.clear();
        digitalWrite(buzzer, LOW);
        motor_status = 0;
        manual_mode = false;
        temp_count1 = 0;
        display.showNumberDec(0, false);
        motor_time = motor_duration * 60;
        Serial.println("Motor Stopped");
      }
    }
    else
    {
      temp_count1 = 0;
    }
  }

  Serial.println("==============================");
  Serial.print("Motor Status : ");
  Serial.println(motor_status);
  Serial.print("Motor Time : ");
  Serial.println(motor_time);
  Serial.print("Sensor Status : ");
  Serial.println(sensor_status);
  Serial.print("Signal Timeout : ");
  Serial.println(signal_timeout);
  Serial.print("Manual Mode : ");
  Serial.println(manual_mode);

  if (signal_timeout > 0)
    signal_timeout--;

  delay(1000);
  count++;
}