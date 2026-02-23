#include <ESP8266WiFi.h>
#include <espnow.h>

uint8_t receiverMAC[] = {0x2C, 0xF4, 0x32, 0x63, 0xB7, 0x88};  // Receiver ESP
uint8_t iotMAC[]      = {0xFC, 0xF5, 0xC4, 0xBE, 0x79, 0xB3};  // IoT ESP


const long interval = 1000;
unsigned long previousMillis = 0;

#define hsen D1
#define lsen D2

const char* networkid = "1033";
const char* deviceid = "02";

typedef struct struct_message {
  char message[64]; 
} struct_message;

struct_message outgoingmsg;

int vstate1 = 2;  
int vstate2 = 2;

int temp_count1 = 0;
int temp_count2 = 0;
int temp_count3 = 0;

void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
  Serial.print("Last Packet Send Status: ");
  if (sendStatus == 0) {
    Serial.println("Delivery success");
  } else {
    Serial.println("Delivery fail");
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(hsen, INPUT_PULLUP);
  pinMode(lsen, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != 0) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(OnDataSent);
  esp_now_add_peer(receiverMAC, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
  esp_now_add_peer(iotMAC, ESP_NOW_ROLE_COMBO, 1, NULL, 0);

}

void loop() {
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    
    if (digitalRead(hsen) == LOW) {
      temp_count1++;
      if (temp_count1 >= 3) {
        vstate1 = 0;
        vstate2 = 0;
        temp_count1 = 0;
        Serial.println("High Level Triggered");
      }
    } else {
      temp_count1 = 0;
    }

    if (digitalRead(lsen) == LOW) {
      temp_count2++;
      if (temp_count2 >= 3) {
        vstate1 = 1;
        vstate2 = 1;
        temp_count2 = 0;
        Serial.println("Low Level Triggered");
      }
    } else {
      temp_count2 = 0;
    }

    if (digitalRead(hsen) != LOW && digitalRead(lsen) != LOW) {
      temp_count3++;
      if (temp_count3 >= 10) {
        vstate1 = 2;
        vstate2 = 2;
        temp_count3 = 0;
        Serial.println("Water Level Normal");
      }
    } else {
      temp_count3 = 0;
    }

    
    snprintf(outgoingmsg.message, sizeof(outgoingmsg.message), "%s%s%d%d", networkid, deviceid, vstate1, vstate2);

    Serial.print("Sending: ");
    Serial.println(outgoingmsg.message);

    esp_now_send(receiverMAC, (uint8_t *)&outgoingmsg, sizeof(outgoingmsg));
    esp_now_send(iotMAC, (uint8_t *)&outgoingmsg, sizeof(outgoingmsg));

  }
}
