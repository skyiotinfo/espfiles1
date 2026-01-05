#include <ESP8266WiFi.h>
#include <espnow.h>
#include <TM1637Display.h>

#define CLK D3
#define DIO D4
#define buzzer D2
#define input1 D9
#define potPin A0  

uint8_t broadcastAddress[] = {0x3C, 0x61, 0x05, 0xDC, 0x6A, 0x29};

TM1637Display display(CLK, DIO);

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
  Serial.print("Received message: ");
  Serial.println(incomingmsg.message);

  String data = incomingmsg.message;
  if (data.length() >= 8) {
    String networkid = data.substring(0, 4);
    String deviceid = data.substring(4, 6);
    String devicestatus = data.substring(6, 8);

    if (networkid == "2012") {
      if (devicestatus == "00") {
        display.clear();
        display.setSegments(seg_full);
        tcount1++;
        if (tcount1 >= 3) {
          Serial.println("Tank Full Detected");
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
        display.clear();
        display.setSegments(seg_empty);
        tcount2++;
        if (tcount2 >= 3) {
          Serial.println("Tank Empty Detected");
          digitalWrite(buzzer, HIGH);
          motor_status = 1;
          tcount2 = 0;
          sensor_status = 1;
        }
      } else {
        tcount2 = 0;
      }

      if (devicestatus == "22") {
        tcount3++;
        if (tcount3 >= 3) {
          Serial.println("Water Level Normal");
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

  display.setBrightness(0x0f);
  display.clear();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(OnDataRecv);
  esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_COMBO, 1, NULL, 0);


  int potValue = analogRead(potPin);
  motor_duration = map(potValue, 0, 1023, 1, 180);
  motor_time = motor_duration * 60;

  Serial.print("Initial Motor duration (from potentiometer): ");
  Serial.print(motor_duration);
  Serial.println(" minutes");

  Serial.println("ESP-NOW Receiver Ready");
}

void loop() {

  if (digitalRead(input1) == 0) {
    Serial.println("Manual Button Pressed");
    if (digitalRead(buzzer) == 0) {
      digitalWrite(buzzer, HIGH);
      motor_status = 1;
      motor_time = motor_duration * 60;
    } else {
      digitalWrite(buzzer, LOW);
      Serial.println("Motor is now stopped..!");
      motor_status = 0;
    }
    delay(1000);
  }

 
  if (motor_status == 1) {
    motor_time--;
    display.showNumberDec((motor_time / 60) + 1, false);
    if (motor_time <= 0 || sensor_status == 0) {
      temp_count1++;
      if (temp_count1 >= 5) {
        digitalWrite(buzzer, LOW);
        motor_status = 0;
        motor_time = motor_duration * 60;
        temp_count1 = 0;
        display.clear();
      }
    } else {
      temp_count1 = 0;
    }
    delay(1000);
  } else {
  
    int potValue = analogRead(potPin);
    int newMotorDuration = map(potValue, 0, 1023, 1, 180);

    if (newMotorDuration != motor_duration) {
      motor_duration = newMotorDuration;
      motor_time = motor_duration * 60;
      Serial.print("Updated motor duration: ");
      Serial.print(motor_duration);
      Serial.println(" minutes");
    }

    delay(500);
  }
}
