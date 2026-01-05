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
#define networkid "1031"
#define device "01"

int addr1 = 0;
int value1 = 1;
byte memval1;
int sensor_status = 2;

const int buzzer = D2;
const int input1 = D9;
int lora_pac_count = 0;
int tcount1 = 0;
int tcount2 = 0;
int tcount3 = 0;

String motor_status = "00";
String last_sent_status = "";

int motor_time = 0;
int empty_start = 0;
int value_count = 0;
int motor_duration = 30;
int motor_stoptime = 0;
int count = 0;
int sound = 0;
int temp_count1 = 0;

unsigned long lastLoRaReceiveTime = 0;
unsigned long loRaTimeout = 30000;  
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 300;   
int sendCount = 0;
const int maxSendCount = 5;
bool sendingActive = false;


const uint8_t seg_empty[] = {
  0x00,
  SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,
  SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,
  0x00
};

const uint8_t seg_full[] = {
  0x00,
  SEG_A | SEG_E | SEG_F | SEG_G,
  SEG_A | SEG_E | SEG_F | SEG_G,
  0x00
};

const uint8_t seg_nodata[] = {
  SEG_A | SEG_E | SEG_F | SEG_G,             
  SEG_A | SEG_B | SEG_E | SEG_F | SEG_G,       
  SEG_D | SEG_E | SEG_F,                      
  SEG_A | SEG_D | SEG_E | SEG_F | SEG_G        
};

TM1637Display display(CLK, DIO);
uint8_t blank[] = { 0x00, 0x00, 0x00, 0x00 };

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  pinMode(input1, INPUT_PULLUP);
  pinMode(buzzer, OUTPUT);
  digitalWrite(buzzer, LOW);
  motor_status = "00";
  sendingActive = true;
  sendCount = 0;
  delay(1000);

  memval1 = EEPROM.read(addr1);
  motor_duration = memval1;

  display.setBrightness(0x0f);
  display.setSegments(blank);

  int temp_count = 100;
  if (digitalRead(input1) == 0) {
    while (temp_count >= 1) {
      if (digitalRead(input1) == 0) {
        if (motor_duration <= 180) {
          motor_duration += 5;
          temp_count++;
        } else {
          motor_duration = 0;
        }
      }
      temp_count--;
      delay(200);
      Serial.print("Temp Count:");
      Serial.println(temp_count);
      Serial.print("Motor Duration:");
      Serial.println(motor_duration);

      EEPROM.write(addr1, motor_duration);
      if (EEPROM.commit()) {
        Serial.println("EEPROM successfully committed");
        display.showNumberDec(5, false, 1, 0);
        display.showNumberDec(motor_duration, false);
      } else {
        Serial.println("ERROR! EEPROM commit failed");
      }
    }
  }

  if (motor_duration < 1 || motor_duration > 180) {
    motor_duration = 30;
  }

  motor_time = motor_duration * 60;
  empty_start = 0;

  for (int i = 0; i < 8; i++) {
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

  int temp_count1 = 0;
  while (!LoRa.begin(433920000)) {
    Serial.println(".");
    temp_count1++;
    if (temp_count1 >= 30) break;
    delay(500);
  }

  lastLoRaReceiveTime = millis();  
}

void send_data() {
  if (!sendingActive) return;

  if (millis() - lastSendTime >= sendInterval) {

    LoRa.beginPacket();
    LoRa.print(networkid);
    LoRa.print(device);
    LoRa.print(motor_status);
    LoRa.endPacket();

    Serial.print("LoRa Sent: ");
    Serial.print(networkid);
    Serial.print(device);
    Serial.println(motor_status);

    lastSendTime = millis();
    sendCount++;

    if (sendCount >= maxSendCount) {
      sendingActive = false;
      last_sent_status = motor_status;
      Serial.println("LoRa send cycle complete");
    }
  }


}


void loop() {
    
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String LoRaData = "";
    while (LoRa.available()) {
      LoRaData += (char)LoRa.read();
    }

    Serial.print("Received packet: ");
    Serial.println(LoRaData);

    String deviceid = LoRaData.substring(0, 4);
    String devicestatus = LoRaData.substring(6, 8);

    // Serial.println(deviceid);
    // Serial.println(devicestatus);

    if (deviceid.equals("1031") && devicestatus.equals("00")) {
      display.clear();
      display.setSegments(seg_full);
      // delay(500);
      tcount1++;
      if (tcount1 >= 3 && digitalRead(buzzer) == 1) {
        digitalWrite(buzzer, LOW);
        motor_status = "00";
        sendingActive = true;
        sendCount = 0;
        display.showNumberDec(0, false);
        motor_time = motor_duration * 60;
        tcount1 = 0;
        sensor_status = 0;
      }
    } else {
      tcount1 = 0;
    }

    if (deviceid.equals("1031") && devicestatus.equals("11")) {
      tcount2++;
      display.clear();
      display.setSegments(seg_empty);
      if (tcount2 >= 3) {
        digitalWrite(buzzer, HIGH);
        motor_status = "11";
        sendingActive = true;
        sendCount = 0;
        tcount2 = 0;
        sensor_status = 1;
      }
    } else {
      tcount2 = 0;
    }

    if (deviceid.equals("1031") && devicestatus.equals("22")) {
      tcount3++;
      if (tcount3 >= 2) {
        Serial.println("Tank No Data.........");
        tcount3 = 0;
        sensor_status = 2;
      }
    } else {
      tcount3 = 0;
    }

    lastLoRaReceiveTime = millis();  
    Serial.print("RSSI :");
    Serial.println(LoRa.packetRssi());
  }

  Serial.println("=========================================");
  Serial.print("Motor Status: ");
  Serial.println(motor_status);
  Serial.print("Motor Duration: ");
  Serial.println(motor_duration);
  Serial.print("Motor Time: ");
  Serial.println(motor_time);
  Serial.print("Tank Status: ");
  Serial.println(sensor_status);

  if (digitalRead(input1) == 0) {
    if (digitalRead(buzzer) == 0) {
      digitalWrite(buzzer, HIGH);
      motor_status = "11";
      sendingActive = true;
      sendCount = 0;
      motor_time = motor_duration * 60;
      value_count = 15;
    } else {
      digitalWrite(buzzer, LOW);
      display.showNumberDec(0, false);
      motor_status = "00";
      sendingActive = true;
      sendCount = 0;
      value_count = 0;
    }
  }

  if (digitalRead(buzzer) == 1) {
  static unsigned long lastMotorTick = 0;
  if (millis() - lastMotorTick >= 1000) {
    lastMotorTick = millis();

    motor_time--;

    display.showNumberDec(1, false, 1, 0);
    display.showNumberDec((motor_time / 60) + 1, false);

    if (motor_time <= 0 || sensor_status == 0) {
      temp_count1++;
      if (temp_count1 >= 5) {
        display.clear();
        digitalWrite(buzzer, LOW);
        motor_status = "00";
        sendingActive = true;
        sendCount = 0;
        temp_count1 = 0;
        display.showNumberDec(0, false);
        motor_time = motor_duration * 60;
      }
    } else {
      temp_count1 = 0;
    }
  }
}

  if (millis() - lastLoRaReceiveTime > loRaTimeout && sensor_status != 2) {
    Serial.println("No LoRa data received. Switching to default (No Data) mode.");
    sensor_status = 2;
    display.clear();
    display.setSegments(seg_nodata);
    digitalWrite(buzzer, LOW);
    motor_status = "00";
    sendingActive = true;
    sendCount = 0;
    motor_time = motor_duration * 60;
  }
 send_data();
delay(200);
  count++;
}
