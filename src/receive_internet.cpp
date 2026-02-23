#include <ESP8266WiFi.h>
#include <espnow.h>
#include <TM1637Display.h>
#include <EEPROM.h>

#define CLK D3
#define DIO D4
#define buzzer D2
#define input1 D9   // kept as-is (hardware responsibility)

const char* networkid = "1033";
const char* deviceid = "01";

uint8_t broadcastAddress[] = {0xFC, 0xF5, 0xC4, 0xBE, 0x79, 0xB3};

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
struct_message outgoingmsg;

/* ===== CHANGED ===== */
String motor_status = "00";   // "00" OFF, "11" ON

int motor_duration = 30;
int motor_time = 0;
int sensor_status = 2; 
int tcount1 = 0, tcount2 = 0, tcount3 = 0;
int temp_count1 = 0;

/* ===== ADDED (SEND CONTROL) ===== */
bool sendingActive = false;
int sendCount = 0;
const int maxSendCount = 10;
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 1000;
/* ===== ADDED (1 MIN PERIODIC RESEND) ===== */
unsigned long lastResendTime = 0;
const unsigned long resendInterval = 60000; // 1 minute


/* ===== ADDED SEND FUNCTION ===== */
void send_motor_status() {
  if (!sendingActive) return;

  if (millis() - lastSendTime >= sendInterval) {
    snprintf(outgoingmsg.message, sizeof(outgoingmsg.message), "%s%s%s", networkid, deviceid, motor_status);
    esp_now_send(broadcastAddress, (uint8_t *)&outgoingmsg, sizeof(outgoingmsg));

    Serial.print("ESP-NOW Sent: ");
    Serial.println(outgoingmsg.message);

    lastSendTime = millis();
    sendCount++;

    if (sendCount >= maxSendCount) {
      sendingActive = false;
      Serial.println("Send cycle complete");
    }
  }
}

void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  memcpy(&incomingmsg, incomingData, sizeof(incomingmsg));
  Serial.print("Received message: ");
  Serial.println(incomingmsg.message);

  String data = incomingmsg.message;
  if (data.length() >= 8) {
    String networkid = data.substring(0, 6);
    String devicestatus = data.substring(6, 8);

    if (networkid == "103302") {

      if (devicestatus == "00") {
        display.clear();
        display.setSegments(seg_full);
        tcount1++;
        if (tcount1 >= 3 && motor_status != "00") {
          digitalWrite(buzzer, LOW);
          motor_status = "00";
          motor_time = motor_duration * 60;
          sensor_status = 0;

          sendingActive = true;
          sendCount = 0;
          lastResendTime = millis();


          tcount1 = 0;
        }
      } else tcount1 = 0;

      if (devicestatus == "11") {
        display.clear();
        display.setSegments(seg_empty);
        tcount2++;
        if (tcount2 >= 3 && motor_status != "11") {
          digitalWrite(buzzer, HIGH);
          motor_status = "11";
          motor_time = motor_duration * 60;
          sensor_status = 1;

          sendingActive = true;
          sendCount = 0;
          lastResendTime = millis();


          tcount2 = 0;
        }
      } else tcount2 = 0;

      if (devicestatus == "22") {
        tcount3++;
        if (tcount3 >= 3) {
          sensor_status = 2;
          tcount3 = 0;
        }
      } else tcount3 = 0;
    }
  }
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);

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

  motor_duration = EEPROM.read(0);
  if (motor_duration < 1 || motor_duration > 180) {
    motor_duration = 30;
  }
  motor_time = motor_duration * 60;

  Serial.println("ESP-NOW Receiver Ready");
}

void loop() {

  if (digitalRead(input1) == 0) {
    if (motor_status == "00") {
      digitalWrite(buzzer, HIGH);
      motor_status = "11";
      motor_time = motor_duration * 60;
    } else {
      digitalWrite(buzzer, LOW);
      motor_status = "00";
    }
    sendingActive = true;
    sendCount = 0;
    lastResendTime = millis();

    delay(1000);
  }

  if (motor_status == "11") {
    motor_time--;
    display.showNumberDec((motor_time / 60) + 1, false);

    if (motor_time <= 0 || sensor_status == 0) {
      temp_count1++;
      if (temp_count1 >= 5) {
        digitalWrite(buzzer, LOW);
        motor_status = "00";
        motor_time = motor_duration * 60;
        display.clear();
        temp_count1 = 0;

        sendingActive = true;
        sendCount = 0;
        lastResendTime = millis();

      }
    } else temp_count1 = 0;

    delay(1000);
  } else {
    delay(500);
  }

  /* ===== SEND MOTOR STATUS (10 TIMES) ===== */
  send_motor_status();

  /* ===== PERIODIC 1 MIN RESEND ===== */
if (!sendingActive) {
  if (millis() - lastResendTime >= resendInterval) {
    Serial.println("1 minute resend triggered");

    sendingActive = true;
    sendCount = 0;
    lastResendTime = millis();
  }
}

}
