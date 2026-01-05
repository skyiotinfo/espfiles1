#include <Arduino.h>

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define buzzer D2
#define input1 D9
#define potPin A0  // Potentiometer connected to analog pin A0

uint8_t broadcastAddress[] = {0x3C, 0x61, 0x05, 0xDC, 0x6A, 0x29};

LiquidCrystal_I2C lcd(0x27, 16, 2);  // Adjust address if needed

typedef struct struct_message {
  char message[64];
} struct_message;

struct_message incomingmsg;

int motor_status = 0;
int motor_duration = 30;
int motor_time = 0;
int sensor_status = 2;

int tcount1 = 0, tcount2 = 0, tcount3 = 0;
int temp_count1 = 0;

void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  memcpy(&incomingmsg, incomingData, sizeof(incomingmsg));
  String data = incomingmsg.message;

  if (data.length() >= 8) {
    String networkid = data.substring(0, 4);
    String devicestatus = data.substring(6, 8);

    if (networkid == "2012") {
      lcd.clear();
      if (devicestatus == "00") {
        lcd.setCursor(0, 0);
        lcd.print("Tank Full");
        tcount1++;
        if (tcount1 >= 3) {
          digitalWrite(buzzer, LOW);
          motor_status = 0;
          motor_time = motor_duration * 60;
          tcount1 = 0;
          sensor_status = 0;
        }
      } else {
        tcount1 = 0;
      }

      if (devicestatus == "11") {
        lcd.setCursor(0, 0);
        lcd.print("Tank Empty");
        tcount2++;
        if (tcount2 >= 3) {
          digitalWrite(buzzer, HIGH);
          motor_status = 1;
          tcount2 = 0;
          sensor_status = 1;
        }
      } else {
        tcount2 = 0;
      }

      if (devicestatus == "22") {
        lcd.setCursor(0, 0);
        lcd.print("Level Normal");
        tcount3++;
        if (tcount3 >= 3) {
          sensor_status = 2;
          tcount3 = 0;
        }
      } else {
        tcount3 = 0;
      }
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(buzzer, OUTPUT);
  pinMode(input1, INPUT_PULLUP);
  digitalWrite(buzzer, LOW);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Initializing...");



  int potValue = analogRead(potPin);
  motor_duration = map(potValue, 0, 1023, 1, 180);
  motor_time = motor_duration * 60;

  lcd.setCursor(0, 1);
  lcd.print("Time: ");
  lcd.print(motor_duration);
  lcd.print(" min");

  Serial.println("ESP-NOW Receiver Ready");
}

void loop() {
  if (digitalRead(input1) == 0) {
    if (digitalRead(buzzer) == 0) {
      digitalWrite(buzzer, HIGH);
      motor_status = 1;
      motor_time = motor_duration * 60;
    } else {
      digitalWrite(buzzer, LOW);
      motor_status = 0;
    }
    delay(1000);
  }

  if (motor_status == 1) {
    motor_time--;
    lcd.setCursor(0, 1);
    lcd.print("Running: ");
    lcd.print((motor_time / 60) + 1);
    lcd.print(" min   ");
    if (motor_time <= 0 || sensor_status == 0) {
      temp_count1++;
      if (temp_count1 >= 5) {
        digitalWrite(buzzer, LOW);
        motor_status = 0;
        motor_time = motor_duration * 60;
        temp_count1 = 0;
        lcd.clear();
      }
    } else {
      temp_count1 = 0;
    }
    delay(1000);
  } else {
    int potValue = analogRead(potPin);
    int newDuration = map(potValue, 0, 1023, 1, 180);
    if (newDuration != motor_duration) {
      motor_duration = newDuration;
      motor_time = motor_duration * 60;
      lcd.setCursor(0, 1);
      lcd.print("Set Time: ");
      lcd.print(motor_duration);
      lcd.print(" min   ");
      Serial.print("Updated motor duration: ");
      Serial.println(motor_duration);
    }
    delay(500);
  }
}
